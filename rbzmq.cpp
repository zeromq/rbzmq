/*
    Copyright (c) 2007-2010 iMatix Corporation

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the Lesser GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Lesser GNU General Public License for more details.

    You should have received a copy of the Lesser GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <string.h>
#include <ruby.h>
#ifdef HAVE_RUBY_IO_H
#include <ruby/io.h>
#else
#include <rubyio.h>
#endif
#include <zmq.h>

#if defined _MSC_VER
#ifndef int8_t
typedef __int8 int8_t;
#endif
#ifndef int16_t
typedef __int16 int16_t;
#endif
#ifndef int32_t
typedef __int32 int32_t;
#endif
#ifndef int64_t
typedef __int64 int64_t;
#endif
#ifndef uint8_t
typedef unsigned __int8 uint8_t;
#endif
#ifndef uint16_t
typedef unsigned __int16 uint16_t;
#endif
#ifndef uint32_t
typedef unsigned __int32 uint32_t;
#endif
#ifndef uint64_t
typedef unsigned __int64 uint64_t;
#endif
#else
#include <stdint.h>
#endif

#define Check_Socket(__socket) \
    do {\
        if ((__socket) == NULL)\
            rb_raise (rb_eIOError, "closed socket");\
    } while(0)

VALUE socket_type;

static VALUE module_version (VALUE self_)
{
    int major, minor, patch;
    
    zmq_version(&major, &minor, &patch);
    
    return rb_ary_new3 (3, INT2NUM (major), INT2NUM (minor), INT2NUM (patch));
}

static void context_free (void *ctx)
{
    if (ctx) {
       int rc = zmq_term (ctx);
       assert (rc == 0);
    }
}

static VALUE context_alloc (VALUE class_)
{
    return rb_data_object_alloc (class_, NULL, 0, context_free);
}

static VALUE context_initialize (int argc_, VALUE* argv_, VALUE self_)
{
    VALUE io_threads;
    rb_scan_args (argc_, argv_, "01", &io_threads);

    assert (!DATA_PTR (self_));
    void *ctx = zmq_init (NIL_P (io_threads) ? 1 : NUM2INT (io_threads));
    if (!ctx) {
        rb_raise (rb_eRuntimeError, "%s", zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    DATA_PTR (self_) = (void*) ctx;
    return self_;
}

static VALUE context_close (VALUE self_)
{
    void * ctx = NULL;
    Data_Get_Struct (self_, void, ctx);
    
    if (ctx != NULL) {
        int rc = zmq_term (ctx);
        assert (rc == 0);

        DATA_PTR (self_) = NULL;
    }

    return Qnil;
}

struct poll_state {
    int event;
    int nitems;
    zmq_pollitem_t *items;
    VALUE io_objects;  
};

typedef VALUE(*iterfunc)(...);

static VALUE poll_add_item(VALUE io_, void *ps_) {
    struct poll_state *state = (struct poll_state *)ps_;

    long i;
    
    for (i = 0; i < RARRAY_LEN (state->io_objects); i++) {
        if (RARRAY_PTR (state->io_objects)[i] == io_) {
#ifdef HAVE_RUBY_IO_H
            state->items[i].events |= state->event;
            return Qnil;
#else
            if (CLASS_OF (io_) == socket_type) {
                state->items[i].events |= state->event;
                return Qnil;
            }

            OpenFile *fptr;
            GetOpenFile (io_, fptr);
            
            if (state->event == ZMQ_POLLOUT &&
                GetWriteFile (fptr) != NULL &&
                fileno (GetWriteFile (fptr)) != state->items[i].fd) {
                break;
            }
            else {
                state->items[i].events |= state->event;
                return Qnil;
            }
#endif
        }
    }
    
    /* Not found in array.  Add a new poll item. */
    rb_ary_push (state->io_objects, io_);
    
    zmq_pollitem_t *item = &state->items[state->nitems];
    state->nitems++;

    item->events = state->event;

    if (CLASS_OF (io_) == socket_type) {
        item->socket = DATA_PTR (io_);
        item->fd = -1;
    }
    else {
        item->socket = NULL;

#ifdef HAVE_RUBY_IO_H
        rb_io_t *fptr;

        GetOpenFile (io_, fptr);
        item->fd = fileno (rb_io_stdio_file (fptr));
#else
        OpenFile *fptr;
        
        GetOpenFile (io_, fptr);
        
        if (state->event == ZMQ_POLLIN && GetReadFile (fptr) != NULL) {
            item->fd = fileno (GetReadFile (fptr));
        }
        else if (state->event == ZMQ_POLLOUT && GetWriteFile (fptr) != NULL) {
            item->fd = fileno (GetWriteFile (fptr));
        }
        else if (state->event == ZMQ_POLLERR) {
            if (GetReadFile(fptr) != NULL)
                item->fd = fileno (GetReadFile (fptr));
            else
                item->fd = fileno (GetWriteFile (fptr));
        }
#endif
    }
    
    return Qnil;
}

#ifdef HAVE_RUBY_INTERN_H

struct zmq_poll_args {
    zmq_pollitem_t *items;
    int nitems;
    long timeout_usec;
    int rc;
};

static VALUE zmq_poll_blocking (void* args_)
{
    struct zmq_poll_args *poll_args = (struct zmq_poll_args *)args_;
    
    poll_args->rc = zmq_poll (poll_args->items, poll_args->nitems, poll_args->timeout_usec);
    
    return Qnil;
}

#endif

static VALUE module_select (int argc_, VALUE* argv_, VALUE self_)
{
    VALUE readset, writeset, errset, timeout;
    rb_scan_args (argc_, argv_, "13", &readset, &writeset, &errset, &timeout);

    long timeout_usec;
    int rc, nitems, i;
    zmq_pollitem_t *items, *item;

    if (!NIL_P (readset)) Check_Type (readset, T_ARRAY);
    if (!NIL_P (writeset)) Check_Type (writeset, T_ARRAY);
    if (!NIL_P (errset)) Check_Type (errset, T_ARRAY);
    
    if (NIL_P (timeout))
        timeout_usec = -1;
    else
        timeout_usec = (long)(NUM2DBL (timeout) * 1000000);
    
    /* Conservative estimate for nitems before we traverse the lists. */
    nitems = (NIL_P (readset) ? 0 : RARRAY_LEN (readset)) +
             (NIL_P (writeset) ? 0 : RARRAY_LEN (writeset)) +
             (NIL_P (errset) ? 0 : RARRAY_LEN (errset));
    items = (zmq_pollitem_t*)ruby_xmalloc(sizeof(zmq_pollitem_t) * nitems);

    struct poll_state ps;
    ps.nitems = 0;
    ps.items = items;
    ps.io_objects = rb_ary_new ();

    if (!NIL_P (readset)) {
        ps.event = ZMQ_POLLIN;
        rb_iterate(rb_each, readset, (iterfunc)poll_add_item, (VALUE)&ps);
    }

    if (!NIL_P (writeset)) {
        ps.event = ZMQ_POLLOUT;
        rb_iterate(rb_each, writeset, (iterfunc)poll_add_item, (VALUE)&ps);
    }

    if (!NIL_P (errset)) {
        ps.event = ZMQ_POLLERR;
        rb_iterate(rb_each, errset, (iterfunc)poll_add_item, (VALUE)&ps);
    }
    
    /* Reset nitems to the actual number of zmq_pollitem_t records we're sending. */
    nitems = ps.nitems;

#ifdef HAVE_RUBY_INTERN_H
    if (timeout_usec != 0) {
        struct zmq_poll_args poll_args;
        poll_args.items = items;
        poll_args.nitems = nitems;
        poll_args.timeout_usec = timeout_usec;

        rb_thread_blocking_region (zmq_poll_blocking, (void*)&poll_args, NULL, NULL);
        rc = poll_args.rc;
    }
    else
#endif
        rc = zmq_poll (items, nitems, timeout_usec);
    
    if (rc == -1) {
        rb_raise(rb_eRuntimeError, "%s", zmq_strerror (zmq_errno ()));
        return Qnil;
    }
    else if (rc == 0)
        return Qnil;
    
    VALUE read_active = rb_ary_new ();
    VALUE write_active = rb_ary_new ();
    VALUE err_active = rb_ary_new ();
    
    for (i = 0, item = &items[0]; i < nitems; i++, item++) {
        if (item->revents != 0) {
            VALUE io = RARRAY_PTR (ps.io_objects)[i];
            
            if (item->revents & ZMQ_POLLIN)
                rb_ary_push (read_active, io);
            if (item->revents & ZMQ_POLLOUT)
                rb_ary_push (write_active, io);
            if (item->revents & ZMQ_POLLERR)
                rb_ary_push (err_active, io);
        }
    }
    
    ruby_xfree (items);
    
    return rb_ary_new3 (3, read_active, write_active, err_active);
}

static void socket_free (void *s)
{
    if (s) {
       int rc = zmq_close (s);
       assert (rc == 0);
    }
}

static VALUE context_socket (VALUE self_, VALUE type_)
{
    void * c = NULL;
    Data_Get_Struct (self_, void, c);
    void * s = zmq_socket (c, NUM2INT (type_));
    if (!s) {
        rb_raise (rb_eRuntimeError, "%s", zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    return Data_Wrap_Struct(socket_type, 0, socket_free, s);
}

static VALUE socket_getsockopt (VALUE self_, VALUE option_)
{
    int rc = 0;
    VALUE retval;
    void * s;
    
    Data_Get_Struct (self_, void, s);
    Check_Socket (s);
  
    switch (NUM2INT (option_)) {
    case ZMQ_RCVMORE:
    case ZMQ_HWM:
    case ZMQ_SWAP:
    case ZMQ_AFFINITY:
    case ZMQ_RATE:
    case ZMQ_RECOVERY_IVL:
    case ZMQ_MCAST_LOOP:
    case ZMQ_SNDBUF:
    case ZMQ_RCVBUF:
        {
            int64_t optval;
            size_t optvalsize = sizeof(optval);

            rc = zmq_getsockopt (s, NUM2INT (option_), (void *)&optval,
                                 &optvalsize);

            if (rc != 0) {
              rb_raise (rb_eRuntimeError, "%s", zmq_strerror (zmq_errno ()));
              return Qnil;
            }

            if (NUM2INT (option_) == ZMQ_RCVMORE)
                retval = optval ? Qtrue : Qfalse;
            else
                retval = INT2NUM (optval);
        }
        break;
    case ZMQ_IDENTITY:
        {
            char identity[255];
            size_t optvalsize = sizeof (identity);

            rc = zmq_getsockopt (s, NUM2INT (option_), (void *)identity,
                                 &optvalsize);

            if (rc != 0) {
              rb_raise (rb_eRuntimeError, "%s", zmq_strerror (zmq_errno ()));
              return Qnil;
            }

            if (optvalsize > sizeof (identity))
                optvalsize = sizeof (identity);

            retval = rb_str_new (identity, optvalsize);
        }
        break;
    default:
        rb_raise (rb_eRuntimeError, "%s", zmq_strerror (EINVAL));
        return Qnil;
    }
  
    return retval;
}

static VALUE socket_setsockopt (VALUE self_, VALUE option_,
    VALUE optval_)
{

    int rc = 0;
    void * s;

    Data_Get_Struct (self_, void, s);
    Check_Socket (s);

    switch (NUM2INT (option_)) {
    case ZMQ_HWM:
    case ZMQ_SWAP:
    case ZMQ_AFFINITY:
    case ZMQ_RATE:
    case ZMQ_RECOVERY_IVL:
    case ZMQ_MCAST_LOOP:
    case ZMQ_SNDBUF:
    case ZMQ_RCVBUF:
        {
            uint64_t optval = FIX2LONG (optval_);

            //  Forward the code to native 0MQ library.
            rc = zmq_setsockopt (s, NUM2INT (option_),
                (void*) &optval, sizeof (optval));
        }
        break;

    case ZMQ_IDENTITY:
    case ZMQ_SUBSCRIBE:
    case ZMQ_UNSUBSCRIBE:

        //  Forward the code to native 0MQ library.
        rc = zmq_setsockopt (s, NUM2INT (option_),
	    (void *) StringValueCStr (optval_), RSTRING_LEN (optval_));
        break;

    default:
        rb_raise (rb_eRuntimeError, "%s", zmq_strerror (EINVAL));
        return Qnil;
    }

    if (rc != 0) {
        rb_raise (rb_eRuntimeError, "%s", zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    return self_;
}


static VALUE socket_bind (VALUE self_, VALUE addr_)
{
    void * s;
    Data_Get_Struct (self_, void, s);
    Check_Socket (s);

    int rc = zmq_bind (s, rb_string_value_cstr (&addr_));
    if (rc != 0) {
        rb_raise (rb_eRuntimeError, "%s", zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    return Qnil;
}

static VALUE socket_connect (VALUE self_, VALUE addr_)
{
    void * s;
    Data_Get_Struct (self_, void, s);
    Check_Socket (s);

    int rc = zmq_connect (s, rb_string_value_cstr (&addr_));
    if (rc != 0) {
        rb_raise (rb_eRuntimeError, "%s", zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    return Qnil;
}

#ifdef HAVE_RUBY_INTERN_H
struct zmq_send_recv_args {
    void *socket;
    zmq_msg_t *msg;
    int flags;
    int rc;
};

static VALUE zmq_send_blocking (void* args_)
{
    struct zmq_send_recv_args *send_args = (struct zmq_send_recv_args *)args_;

    send_args->rc = zmq_send(send_args->socket, send_args->msg, send_args->flags);
    
    return Qnil;
}
#endif

static VALUE socket_send (VALUE self_, VALUE msg_, VALUE flags_)
{
    void * s;
    Data_Get_Struct (self_, void, s);
    Check_Socket (s);

    Check_Type (msg_, T_STRING);

    int flags = NUM2INT (flags_);

    zmq_msg_t msg;
    int rc = zmq_msg_init_size (&msg, RSTRING_LEN (msg_));
    if (rc != 0) {
        rb_raise (rb_eRuntimeError, "%s", zmq_strerror (zmq_errno ()));
        return Qnil;
    }
    memcpy (zmq_msg_data (&msg), RSTRING_PTR (msg_), RSTRING_LEN (msg_));

#ifdef HAVE_RUBY_INTERN_H
    if (!(flags & ZMQ_NOBLOCK)) {
        struct zmq_send_recv_args send_args;
        send_args.socket = s;
        send_args.msg = &msg;
        send_args.flags = flags;
        rb_thread_blocking_region (zmq_send_blocking, (void*) &send_args, NULL, NULL);
        rc = send_args.rc;
    }
    else
#endif
        rc = zmq_send (s, &msg, flags);
    if (rc != 0 && zmq_errno () == EAGAIN) {
        rc = zmq_msg_close (&msg);
        assert (rc == 0);
        return Qfalse;
    }

    if (rc != 0) {
        rb_raise (rb_eRuntimeError, "%s", zmq_strerror (zmq_errno ()));
        rc = zmq_msg_close (&msg);
        assert (rc == 0);
        return Qnil;
    }

    rc = zmq_msg_close (&msg);
    assert (rc == 0);
    return Qtrue;
}

#ifdef HAVE_RUBY_INTERN_H
static VALUE zmq_recv_blocking (void* args_)
{
    struct zmq_send_recv_args *recv_args = (struct zmq_send_recv_args *)args_;

    recv_args->rc = zmq_recv(recv_args->socket, recv_args->msg, recv_args->flags);
    
    return Qnil;
}
#endif

static VALUE socket_recv (VALUE self_, VALUE flags_)
{
    void * s;
    Data_Get_Struct (self_, void, s);
    Check_Socket (s);

    int flags = NUM2INT (flags_);

    zmq_msg_t msg;
    int rc = zmq_msg_init (&msg);
    assert (rc == 0);

#ifdef HAVE_RUBY_INTERN_H
    if (!(flags & ZMQ_NOBLOCK)) {
        struct zmq_send_recv_args recv_args;
        recv_args.socket = s;
        recv_args.msg = &msg;
        recv_args.flags = flags;
        rb_thread_blocking_region (zmq_recv_blocking, (void*) &recv_args, NULL, NULL);
        rc = recv_args.rc;
    }
    else
#endif
        rc = zmq_recv (s, &msg, flags);
    if (rc != 0 && zmq_errno () == EAGAIN) {
        rc = zmq_msg_close (&msg);
        assert (rc == 0);
        return Qnil;
    }

    if (rc != 0) {
        rb_raise (rb_eRuntimeError, "%s", zmq_strerror (zmq_errno ()));
        rc = zmq_msg_close (&msg);
        assert (rc == 0);
        return Qnil;
    }

    VALUE message = rb_str_new ((char*) zmq_msg_data (&msg),
        zmq_msg_size (&msg));
    rc = zmq_msg_close (&msg);
    assert (rc == 0);
    return message;
}

static VALUE socket_close (VALUE self_)
{
    void * s = NULL;
    Data_Get_Struct (self_, void, s);
    if (s != NULL) {
        int rc = zmq_close (s);
        if (rc != 0) {
            rb_raise (rb_eRuntimeError, "%s", zmq_strerror (zmq_errno ()));
            return Qnil;
        }

        DATA_PTR (self_) = NULL;
    }
    return Qnil;
}

extern "C" void Init_zmq ()
{
    VALUE zmq_module = rb_define_module ("ZMQ");
    rb_define_singleton_method (zmq_module, "version",
        (VALUE(*)(...)) module_version, 0);
    rb_define_singleton_method (zmq_module, "select",
        (VALUE(*)(...)) module_select, -1);

    VALUE context_type = rb_define_class_under (zmq_module, "Context",
        rb_cObject);
    rb_define_alloc_func (context_type, context_alloc);
    rb_define_method (context_type, "initialize",
        (VALUE(*)(...)) context_initialize, -1);
    rb_define_method (context_type, "socket",
        (VALUE(*)(...)) context_socket, 1);
    rb_define_method (context_type, "close",
        (VALUE(*)(...)) context_close, 0);

    socket_type = rb_define_class_under (zmq_module, "Socket", rb_cObject);
    rb_undef_alloc_func(socket_type);
    rb_define_method (socket_type, "getsockopt",
        (VALUE(*)(...)) socket_getsockopt, 1);
    rb_define_method (socket_type, "setsockopt",
        (VALUE(*)(...)) socket_setsockopt, 2);
    rb_define_method (socket_type, "bind",
        (VALUE(*)(...)) socket_bind, 1);
    rb_define_method (socket_type, "connect",
        (VALUE(*)(...)) socket_connect, 1);
    rb_define_method (socket_type, "send",
        (VALUE(*)(...)) socket_send, 2);
    rb_define_method (socket_type, "recv",
        (VALUE(*)(...)) socket_recv, 1);
    rb_define_method (socket_type, "close",
        (VALUE(*)(...)) socket_close, 0);

    rb_define_const (zmq_module, "HWM", INT2NUM (ZMQ_HWM));
    rb_define_const (zmq_module, "SWAP", INT2NUM (ZMQ_SWAP));
    rb_define_const (zmq_module, "AFFINITY", INT2NUM (ZMQ_AFFINITY));
    rb_define_const (zmq_module, "IDENTITY", INT2NUM (ZMQ_IDENTITY));
    rb_define_const (zmq_module, "SUBSCRIBE", INT2NUM (ZMQ_SUBSCRIBE));
    rb_define_const (zmq_module, "UNSUBSCRIBE", INT2NUM (ZMQ_UNSUBSCRIBE));
    rb_define_const (zmq_module, "RATE", INT2NUM (ZMQ_RATE));
    rb_define_const (zmq_module, "RECOVERY_IVL", INT2NUM (ZMQ_RECOVERY_IVL));
    rb_define_const (zmq_module, "MCAST_LOOP", INT2NUM (ZMQ_MCAST_LOOP));
    rb_define_const (zmq_module, "SNDBUF", INT2NUM (ZMQ_SNDBUF));
    rb_define_const (zmq_module, "RCVBUF", INT2NUM (ZMQ_RCVBUF));
    rb_define_const (zmq_module, "SNDMORE", INT2NUM (ZMQ_SNDMORE));
    rb_define_const (zmq_module, "RCVMORE", INT2NUM (ZMQ_RCVMORE));

    rb_define_const (zmq_module, "NOBLOCK", INT2NUM (ZMQ_NOBLOCK));

    rb_define_const (zmq_module, "PAIR", INT2NUM (ZMQ_PAIR));
    rb_define_const (zmq_module, "SUB", INT2NUM (ZMQ_SUB));
    rb_define_const (zmq_module, "PUB", INT2NUM (ZMQ_PUB));
    rb_define_const (zmq_module, "REQ", INT2NUM (ZMQ_REQ));
    rb_define_const (zmq_module, "REP", INT2NUM (ZMQ_REP));
    rb_define_const (zmq_module, "XREQ", INT2NUM (ZMQ_XREQ));
    rb_define_const (zmq_module, "XREP", INT2NUM (ZMQ_XREP));
    rb_define_const (zmq_module, "UPSTREAM", INT2NUM (ZMQ_UPSTREAM));
    rb_define_const (zmq_module, "DOWNSTREAM", INT2NUM (ZMQ_DOWNSTREAM));
}
