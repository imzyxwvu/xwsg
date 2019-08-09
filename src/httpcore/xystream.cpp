#include "xystream.h"
#include <cstring>
#include <iostream>

const char *int_status::strerror() {
    return uv_strerror(_status);
}

int_status::~int_status() {}

stream::stream() : _timeout(30000) {
    _timeOuter = mem_alloc<uv_timer_t>();
    if(uv_timer_init(uv_default_loop(), _timeOuter) < 0) {
        free(_timeOuter);
        throw runtime_error("failed to setup timeout timer");
    }
    _timeOuter->data = this;
}

stream::~stream() {
    if(handle)
        uv_close((uv_handle_t *)handle, (uv_close_cb) free);
    uv_close((uv_handle_t *)_timeOuter, (uv_close_cb) free);
}

void stream::accept(uv_stream_t *svr) {
    int r = uv_accept(svr, handle);
    if(r < 0)
        throw IOERR(r);
}

stream::write_request::write_request() {
    write_req.data = this;
    _fiber = fiber::current();
}

class stream::callbacks {
public:
    static void on_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
        stream *self = (stream *)handle->data;
        buf->base = self->buffer.prepare(suggested_size);
        buf->len = buf->base ? suggested_size : 0;
    }

    static void on_data(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf) {
        stream *self = (stream *)handle->data;
        if(nread != 0)
            self->_commit_rx(buf->base, nread);
    }

    static void pump_on_shutdown(uv_shutdown_t* req, int status) {
        stream *self = (stream *)req->data;
        delete req;
        auto pipe_sink = move(self->_pipe_src->_pipe_sink);
        self->_pipe_src.reset();
    }

    static void pump_on_write(uv_write_t *req, int status)
    {
        stream *self = (stream *)req->data;
        delete req;
        if(status == 0) {
            if(uv_read_start(self->_pipe_src->handle,
                    callbacks::on_alloc, callbacks::on_data) < 0) {
                uv_shutdown_t *req = new uv_shutdown_t;
                req->data = self;
                if(uv_shutdown(req, self->handle, callbacks::pump_on_shutdown) < 0) {
                    delete req;
                    auto pipe_sink = move(self->_pipe_src->_pipe_sink);
                    self->_pipe_src.reset();
                }
            }
        } else {
            auto pipe_sink = move(self->_pipe_src->_pipe_sink);
            self->_pipe_src.reset();
        }
    }
};

shared_ptr<message> stream::read(const shared_ptr<decoder> &decoder) {
    if(reading_fiber || _pipe_sink)
        throw RTERR("stream is read-busy");
    if(buffer.size() > 0)
        if(decoder->decode(buffer))
            return decoder->msg();
    _decoder = decoder;
    auto s = _do_read();
    _decoder.reset();
    if(s->status() >= 0) {
        return decoder->msg();
    } else {
        if(s->status() == UV_EOF)
            return nullptr;
        throw IOERR(s->status());
    }
}

static void stream_on_write(uv_write_t *req, int status)
{
    auto *self = (stream::write_request *)req->data;
    shared_ptr<fiber> f = move(self->_fiber);
    delete self;
    f->resume(int_status::make(status));
}

void stream::write(const char *chunk, int length)
{
    if(_pipe_src) throw runtime_error("stream is a sink");
    uv_buf_t buf;
    buf.base = (char *)chunk;
    buf.len = length;
    write_request *wreq = new write_request;
    int r = uv_write(&wreq->write_req, handle, &buf, 1, stream_on_write);
    if(r < 0) {
        delete wreq;
        throw IOERR(r);
    }
    auto s = fiber::yield<int_status>();
    if(s->status() != 0)
        throw IOERR(s->status());
}

void stream::write(const shared_ptr<message> &msg) {
    int size = msg->serialize_size();
    char *buf = new char[size];
    msg->serialize(buf);
    write(buf, size);
    delete[] buf;
}

void stream::write(const string &str) {
    write(str.data(), str.size());
}

void stream::_commit_rx(char *base, int nread) {
    if(!_pipe_sink) {
        if(nread < 0) {
            reading_fiber->resume(int_status::make(nread));
            return;
        }
        buffer.commit(nread);
        try {
            if(_decoder->decode(buffer))
                reading_fiber->resume(int_status::make(nread));
        }
        catch(runtime_error &ex) {
            uv_read_stop(handle);
            uv_timer_stop(_timeOuter);
            _decoder.reset();
            reading_fiber->raise(ex.what());
        }
    } else { // Stream is piped to another
        uv_read_stop(handle);
        if(nread > 0) {
            uv_buf_t wbuf;
            wbuf.base = base;
            wbuf.len = nread;
            uv_write_t *wreq = new uv_write_t;
            wreq->data = _pipe_sink.get();
            if(uv_write(wreq, _pipe_sink->handle,
                        &wbuf, 1, callbacks::pump_on_write) < 0) {
                delete wreq;
                auto pipe_src = move(_pipe_sink->_pipe_src);
                _pipe_sink.reset();
            }
        } else {
            uv_shutdown_t *req = new uv_shutdown_t;
            req->data = _pipe_sink.get();
            if(uv_shutdown(req, _pipe_sink->handle, callbacks::pump_on_shutdown) < 0) {
                delete req;
                auto pipe_src = move(_pipe_sink->_pipe_src);
                _pipe_sink.reset();
            }
        }
    }
}

static void stream_on_read_timeout(uv_timer_t* handle) {
    stream *self = (stream *)handle->data;
    self->reading_fiber->resume(int_status::make(UV_ETIMEDOUT));
}

shared_ptr<int_status> stream::_do_read() {
    int r;
    if(!fiber::current()) throw logic_error("outside-fiber read");
    if((r = uv_read_start(handle, callbacks::on_alloc, callbacks::on_data)) < 0)
        throw IOERR(r);
    if(_timeout > 0)
        uv_timer_start(_timeOuter, stream_on_read_timeout, _timeout, 0);
    fiber::preserve p(reading_fiber);
    auto s = fiber::yield<int_status>();
    uv_read_stop(handle);
    uv_timer_stop(_timeOuter);
    return s;
}

void stream::set_timeout(int timeout) {
    _timeout = timeout;
}

void stream::pipe(const shared_ptr<stream> &sink) {
    if(reading_fiber) throw runtime_error("stream is read-busy");
    if(sink->_pipe_src) throw runtime_error("sink stream already has a source");
    if(has_tls() || sink->has_tls())
        throw RTERR("pipe to TLS stream is not implemented");
    if(buffer.size() > 0) {
        sink->write(buffer.data(), buffer.size());
        buffer.pull(buffer.size());
    }
    _pipe_sink = sink;
    int r = uv_read_start(handle, callbacks::on_alloc, callbacks::on_data);
    if(r < 0) {
        _pipe_sink.reset();
        throw IOERR(r);
    }
    _pipe_sink->_pipe_src = shared_from_this();
}

static void stream_on_shutdown(uv_shutdown_t* req, int status) {
    auto self = (stream::write_request *)req->data;
    shared_ptr<fiber> f = move(self->_fiber);
    delete self;
    f->resume(int_status::make(status));
}

void stream::shutdown() {
    if(_pipe_src) throw runtime_error("stream is a sink");
    write_request *req = new write_request;
    int r = uv_shutdown(&req->shutdown_req, handle, stream_on_shutdown);
    if(r < 0) {
        delete req;
        throw IOERR(r);
    }
    fiber::yield<int_status>();
}

bool stream::has_tls() {
    return false;
}

tcp_stream::tcp_stream() {
    uv_tcp_t *h = mem_alloc<uv_tcp_t>();
    if(uv_tcp_init(uv_default_loop(), h) < 0) {
        free(h);
        throw runtime_error("failed to initialize libuv TCP stream");
    }
    handle = (uv_stream_t *)h;
    handle->data = this;
}

static void stream_on_connect(uv_connect_t* req, int status) {
    auto self = (stream::write_request *)req->data;
    shared_ptr<fiber> f = move(self->_fiber);
    delete self;
    f->resume(int_status::make(status));
}

void tcp_stream::connect(const string &host, int port)
{
    ip_endpoint ep(host, port);
    connect(ep.sa());
}

void tcp_stream::connect(shared_ptr<ip_endpoint> ep) {
    connect(ep->sa());
}

void tcp_stream::connect(const sockaddr *sa) {
    write_request *req = new write_request;
    int r = uv_tcp_connect(&req->connect_req, (uv_tcp_t *)handle, sa, stream_on_connect);
    if(r < 0) {
        delete req;
        throw IOERR(r);
    }
    auto s = fiber::yield<int_status>();
    if(s->status() != 0)
        throw IOERR(s->status());
}

void tcp_stream::nodelay(bool enable) {
    int r = uv_tcp_nodelay((uv_tcp_t *)handle, enable);
    if(r < 0) throw IOERR(r);
}

shared_ptr<ip_endpoint> tcp_stream::getpeername() {
    struct sockaddr_storage address;
    int addrlen = sizeof(address);
    int r = uv_tcp_getpeername(
        (uv_tcp_t *)handle, (struct sockaddr*)&address, &addrlen);
    if(r < 0) throw IOERR(r);
    return make_shared<ip_endpoint>(&address);
}

unix_stream::unix_stream() {
    uv_pipe_t *h = mem_alloc<uv_pipe_t>();
    if(uv_pipe_init(uv_default_loop(), h, 0) < 0) {
        free(h);
        throw runtime_error("failed to initialize libuv UNIX stream");
    }
    handle = (uv_stream_t *)h;
    handle->data = this;
}

void unix_stream::connect(const shared_ptr<string> &path)
{
    write_request *req = new write_request;
    uv_pipe_connect(&req->connect_req, (uv_pipe_t *)handle, path->c_str(), stream_on_connect);
    auto s = fiber::yield<int_status>();
    if(s->status() != 0)
        throw IOERR(s->status());
}

ip_endpoint::ip_endpoint(struct sockaddr_storage *sa) {
    if (sa->ss_family == AF_INET)
        memcpy(&_sa, sa, sizeof(struct sockaddr_in));
    else if (sa->ss_family == AF_INET6)
        memcpy(&_sa, sa, sizeof(struct sockaddr_in6));
}

ip_endpoint::ip_endpoint(const char *addr, int p) {
    if(uv_ip4_addr(addr, p, &_sa_in) && uv_ip6_addr(addr, p, &_sa_in6))
        throw RTERR("invalid IP address or port");
}

ip_endpoint::ip_endpoint(const string &addr, int p)
    : ip_endpoint(addr.c_str(), p) {}

int ip_endpoint::port() {
    if (_sa.ss_family == AF_INET)
        return ntohs(_sa_in.sin_port);
    else if (_sa.ss_family == AF_INET6)
        return ntohs(_sa_in6.sin6_port);
}

const sockaddr *ip_endpoint::sa() {
    return (sockaddr *)&_sa;
}

shared_ptr<string> ip_endpoint::straddr() {
    char ip[20];
    if (_sa.ss_family == AF_INET)
        uv_inet_ntop(AF_INET, &_sa_in.sin_addr, ip, sizeof(ip));
    else if (_sa.ss_family == AF_INET6)
        uv_inet_ntop(AF_INET6, &_sa_in6.sin6_addr, ip, sizeof(ip));
    return make_shared<string>(ip);
}

tcp_server::tcp_server(const char *addr, int port) {
    ip_endpoint ep(addr, port);
    _server = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));
    if(uv_tcp_init(uv_default_loop(), _server) < 0) {
        free(_server);
        throw runtime_error("failed to initialize libuv TCP stream");
    }
    _server->data = this;
    int r = uv_tcp_bind(_server, ep.sa(), 0);
    if(r < 0) {
        uv_close((uv_handle_t *)_server, (uv_close_cb)free);
        throw runtime_error(uv_strerror(r));
    }
}

tcp_server::~tcp_server() {
    uv_close((uv_handle_t *)_server, (uv_close_cb) free);
}

static void tcp_server_on_connection(uv_stream_t* strm, int status) {
    tcp_server *self = (tcp_server *)strm->data;
    if(status >= 0) {
        auto client = make_shared<tcp_stream>();
        try {
            client->accept(strm);
            client->nodelay(true);
            fiber::launch(bind(self->service_func, client));
        }
        catch(exception &ex) {}
    }
}

void tcp_server::serve(function<void(shared_ptr<tcp_stream>)> f) {
    service_func = f;
    int r = uv_listen((uv_stream_t *)_server, 32, tcp_server_on_connection);
    if(r < 0)
        throw runtime_error(uv_strerror(r));
}