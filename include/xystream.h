#ifndef XYHTTPD_STREAM_H
#define XYHTTPD_STREAM_H

#include <uv.h>
#include <functional>

#include "xybuffer.h"
#include "xyfiber.h"

#define IOERR(r) RTERR("I/O Error: %s", uv_strerror(r))

class int_status : public wakeup_event {
public:
    inline int_status(int s) : _status(s) {};
    inline int_status(const int_status &s)
        : _status(s._status) {}
    static inline P<int_status> make(int s) {
        return std::make_shared<int_status>(s);
    }
    inline int status() {
        return _status;
    }
    const char *strerror();
    virtual ~int_status();
private:
    int _status;
};

class stream : public std::enable_shared_from_this<stream> {
public:
    class write_request {
    public:
        write_request();

        union {
            uv_write_t write_req;
            uv_shutdown_t shutdown_req;
            uv_connect_t connect_req;
        };
        P<fiber> _fiber;
    };

    P<fiber> reading_fiber;

    class callbacks;
    friend class callbacks;

    virtual void accept(uv_stream_t *);
    template<class T>
    inline P<T> read(const P<decoder> &dec) {
        auto msg = read(dec);
        return msg ? std::dynamic_pointer_cast<T>(msg) : nullptr;
    }
    virtual P<message> read(const P<decoder> &dec);
    virtual void write(const char *buf, int length);
    virtual void pipe(const P<stream> &sink);
    virtual bool has_tls();
    void write(const P<message> &msg);
    void write(const chunk &str);
    void shutdown();
    void set_timeout(int timeout);
    virtual ~stream();
protected:
    P<int_status> _do_read();
    virtual void _commit_rx(char *base, int nread);
    uv_stream_t *handle;
    uv_timer_t *_timeOuter;
    int _timeout;
    stream_buffer buffer;
    P<decoder> _decoder;
    P<stream> _pipe_src, _pipe_sink;
    stream();
private:
    stream(const stream &);
};

class ip_endpoint {
public:
    ip_endpoint(struct sockaddr_storage *_sa);
    ip_endpoint(const char *addr, int p);
    ip_endpoint(const std::string &addr, int p);
    int port();
    std::string straddr();
    const sockaddr *sa();
private:
    union {
        struct sockaddr_storage _sa;
        struct sockaddr_in _sa_in;
        struct sockaddr_in6 _sa_in6;
    };
};

class tcp_stream : public stream {
public:
    tcp_stream();
    virtual void connect(const std::string &host, int port);
    virtual void connect(P<ip_endpoint> ep);
    void nodelay(bool enable);
    P<ip_endpoint> getpeername();
private:
    virtual void connect(const sockaddr *sa);
};

class unix_stream : public stream {
public:
    unix_stream();
    virtual void connect(const std::string &path);
};

class tcp_server {
public:
    std::function<void(P<tcp_stream>)> service_func;

    tcp_server(const char *addr, int port);
    virtual ~tcp_server();

    void serve(std::function<void(P<tcp_stream>)> f);

    tcp_server(const tcp_server &) = delete;
    tcp_server &operator=(const tcp_server &) = delete;
protected:
    uv_tcp_t *_server;
};


#endif
