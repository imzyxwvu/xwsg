#include <iostream>
#include <unistd.h>
#include <cstring>
#include <cstdarg>
#include "xyfiber.h"

using namespace std;

#ifdef _WIN32
# include <windows.h>
# include <ctime>
fiber_context_t fiber::maincontext = NULL;
#else
# include <sys/mman.h>
# include <execinfo.h>
fiber_context_t fiber::maincontext;
queue<P<fiber::stack_mem>> fiber::stack_pool;
#endif

P<fiber> fiber::_current;

std::string fmt(const char *f, ...) {
    va_list ap, ap2;
    va_start(ap, f);
    va_copy(ap2, ap);
    const int len = vsnprintf(NULL, 0, f, ap2);
    va_end(ap2);
    vector<char> zc(len + 1);
    vsnprintf(zc.data(), zc.size(), f, ap);
    va_end(ap);
    return string(zc.data(), len);
}

std::string timelabel() {
    time_t now = ::time(NULL);
    char tmlabel[32];
    int len = strftime(tmlabel, sizeof(tmlabel),
                       "%Y-%m-%d %H:%M:%S", ::localtime(&now));
    return string(tmlabel, len);
}

extended_runtime_error::extended_runtime_error
        (const char *fname, int lno, const string &wh) :
        _filename(fname), _lineno(lno), runtime_error(wh), _depth(0) {
#ifndef _WIN32
    _depth = backtrace(_btbuf, 20) - 1;
#endif
}

const char *extended_runtime_error::filename() {
    return strrchr(_filename, '/') ?
           strrchr(_filename, '/') + 1 : _filename;
}

int extended_runtime_error::lineno() {
    return _lineno;
}

int extended_runtime_error::tracedepth() {
    return _depth;
}

#ifndef _WIN32

char **extended_runtime_error::stacktrace() {
    return backtrace_symbols(_btbuf + 1, _depth);
}

#else

char **extended_runtime_error::stacktrace() {
    auto buf = (char **)malloc(sizeof(char *));
    *buf = NULL;
    return buf;
}

#endif

int fiber::stack_pool_target = 32;

#ifndef _WIN32

fiber::stack_mem::stack_mem(int siz) : _size(siz) {
    _base = (char *)mmap(NULL, _size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
    if(!_base) throw std::bad_alloc();
    mprotect(_base, getpagesize(), PROT_NONE);
}

fiber::stack_mem::~stack_mem() {
    munmap(_base, _size);
}

fiber::fiber(function<void()> func)
: _entry(move(func)) {
    if(stack_pool.empty()) {
        _stack = make_shared<stack_mem>(0x200000);
    } else {
        _stack = stack_pool.front();
        stack_pool.pop();
    }
    getcontext(&context);
    context.uc_stack.ss_sp = _stack->base();
    context.uc_stack.ss_size = _stack->size();
    context.uc_link = NULL;
    makecontext(&context, (void(*)(void))fiber::wrapper, 1, this);
    _terminated = false;
}

fiber::~fiber() {
    if(stack_pool.size() < stack_pool_target)
        stack_pool.push(_stack);
}

void fiber::wrapper(fiber *f) {
    try {
        f->_entry();
    }
    catch(exception &ex) {
        cerr<<"["<<timelabel()<<" "<<f<<"] "<<ex.what()<<endl;
    }
    f->_terminated = true;
    f->self.reset();
    swapcontext(&f->context,
                !f->_prev ? &maincontext : &f->_prev->context);
}

int fiber::yield() {
    if(!_current)
        throw RTERR("yielding outside a fiber");
    P<fiber> self = _current;
    _current = move(self->_prev);
    swapcontext(&self->context,
                !_current ? &maincontext : &_current->context);
    if(self->_err) {
        runtime_error ex(*self->_err);
        self->_err.reset();
        throw ex;
    }
    return self->_event;
}

void fiber::resume(int event) {
    if(_terminated || _current == self)
        throw RTERR("resuming terminated or current fiber");
    _prev = move(_current);
    _current = self;
    _event = event;
    swapcontext(_prev ? &_prev->context : &maincontext, &context);
    if(_current && _current->_terminated)
        _current = move(_current->_prev);
}

#else

fiber::fiber(function<void()> func)
        : _entry(move(func)) {
    if(!maincontext)
        maincontext = ConvertThreadToFiber(NULL);
    context = CreateFiber(0x200000, (LPFIBER_START_ROUTINE)fiber::wrapper, this);
    if(!context) throw runtime_error("failed to create fiber");
    _terminated = false;
}

fiber::~fiber() {
    DeleteFiber(context);
}

void WINAPI fiber::wrapper(fiber *lpFiberParameter) {
    fiber *f = (fiber *)GetFiberData();
    try {
        f->_entry();
    }
    catch(exception &ex) {
        cerr<<"["<<timelabel()<<" "<<f<<"] "<<ex.what()<<endl;
    }
    f->_terminated = true;
    f->self.reset();
    SwitchToFiber(!f->_prev ? maincontext : f->_prev->context);
}

shared_ptr<wakeup_event> fiber::yield() {
    if(!_current)
        throw RTERR("yielding outside a fiber");
    shared_ptr<fiber> self = _current;
    _current = move(self->_prev);
    SwitchToFiber(!_current ? maincontext : _current->context);
    if(self->_err) {
        runtime_error ex(*self->_err);
        self->_err.reset();
        throw ex;
    }
    return move(self->_event);
}

void fiber::resume() {
    if(_terminated || _current == self)
        throw RTERR("resuming terminated or current fiber");
    _prev = move(_current);
    _current = self;
    SwitchToFiber(context);
    if(_current && _current->_terminated)
        _current = move(_current->_prev);
}

#endif

P<fiber> fiber::launch(function<void()> entry) {
    auto f = make_shared<fiber>(move(entry));
    f->self = f;
    f->resume(0);
    return f;
}

void fiber::raise(const string &ex) {
    _err = make_shared<extended_runtime_error>("@fiber", 0, ex);
    resume(-1);
}

fiber::preserve::preserve(P<fiber> &f) : _f(f) {
    _f = fiber::current();
}

fiber::preserve::~preserve() {
    _f.reset();
}
