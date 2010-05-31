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

VALUE socket_type;

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

static VALUE context_initialize (VALUE self_, VALUE app_threads_,
    VALUE io_threads_, VALUE flags_)
{
    assert (!DATA_PTR (self_));
    void *ctx = zmq_init (NUM2INT (app_threads_), NUM2INT (io_threads_),
        NUM2INT (flags_));
    if (!ctx) {
        rb_raise (rb_eRuntimeError, zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    DATA_PTR (self_) = (void*) ctx;
    return self_;
}

static void socket_free (void *s)
{
    if (s) {
       int rc = zmq_close (s);
       assert (rc == 0);
    }
}

static VALUE socket_alloc (VALUE class_)
{
    return rb_data_object_alloc (class_, NULL, 0, socket_free);
}

static VALUE context_socket (VALUE self_, VALUE type_)
{
    void * c = NULL;
    Data_Get_Struct(self_, void, c);
    void * s = zmq_socket(c, NUM2INT(type_));
    if (!s) {
        rb_raise(rb_eRuntimeError, zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    return Data_Wrap_Struct(socket_type, 0, socket_free, s);
}

#ifdef ZMQ_RCVMORE
static VALUE socket_getsockopt (VALUE self_, VALUE option_)
{
    int rc = 0;
    VALUE retval;
  
    switch (NUM2INT (option_)) {
    case ZMQ_RCVMORE:
    case ZMQ_HWM:
#ifdef ZMQ_LWM
    case ZMQ_LWM:
#endif
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

            rc = zmq_getsockopt (DATA_PTR (self_), NUM2INT (option_), (void *)&optval,
                                 &optvalsize);

            if (rc != 0) {
              rb_raise (rb_eRuntimeError, "%s", zmq_strerror (zmq_errno ()));
              return Qnil;
            }

            if (NUM2INT (option_) == ZMQ_RCVMORE)
                retval = optval ? Qtrue : Qfalse;
            else
                retval = INT2NUM(optval);
        }
        break;
    case ZMQ_IDENTITY:
        {
            char identity[255];
            size_t optvalsize = sizeof (identity);

            rc = zmq_getsockopt (DATA_PTR (self_), NUM2INT (option_), (void *)identity,
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
#endif

static VALUE socket_setsockopt (VALUE self_, VALUE option_,
    VALUE optval_)
{

    int rc = 0;

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
            rc = zmq_setsockopt (DATA_PTR (self_), NUM2INT (option_),
                (void*) &optval, sizeof (optval));
        }
        break;

    case ZMQ_IDENTITY:
    case ZMQ_SUBSCRIBE:
    case ZMQ_UNSUBSCRIBE:

        //  Forward the code to native 0MQ library.
        rc = zmq_setsockopt (DATA_PTR (self_), NUM2INT (option_),
	    (void *) StringValueCStr (optval_), RSTRING_LEN (optval_));
        break;

    default:
        rb_raise (rb_eRuntimeError, zmq_strerror (EINVAL));
        return Qnil;
    }

    if (rc != 0) {
        rb_raise (rb_eRuntimeError, zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    return self_;
}


static VALUE socket_bind (VALUE self_, VALUE addr_)
{
    assert (DATA_PTR (self_));

    int rc = zmq_bind (DATA_PTR (self_), rb_string_value_cstr (&addr_));
    if (rc != 0) {
        rb_raise (rb_eRuntimeError, zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    return Qnil;
}

static VALUE socket_connect (VALUE self_, VALUE addr_)
{
    assert (DATA_PTR (self_));

    int rc = zmq_connect (DATA_PTR (self_), rb_string_value_cstr (&addr_));
    if (rc != 0) {
        rb_raise (rb_eRuntimeError, zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    return Qnil;
}

#ifdef HAVE_RUBY_INTERN_H
static VALUE socket_send_blocking (void* args_)
{
    VALUE self_ = RARRAY_PTR(args_)[0];
    VALUE msg_ = RARRAY_PTR(args_)[1];
    VALUE flags_ = RARRAY_PTR(args_)[2];
#else
static VALUE socket_send (VALUE self_, VALUE msg_, VALUE flags_)
{
#endif
    assert (DATA_PTR (self_));

    Check_Type (msg_, T_STRING);

    zmq_msg_t msg;
    int rc = zmq_msg_init_size (&msg, RSTRING_LEN (msg_));
    if (rc != 0) {
        rb_raise (rb_eRuntimeError, zmq_strerror (zmq_errno ()));
        return Qnil;
    }
    memcpy (zmq_msg_data (&msg), RSTRING_PTR (msg_), RSTRING_LEN (msg_));

    rc = zmq_send (DATA_PTR (self_), &msg, NUM2INT (flags_));
    if (rc != 0 && zmq_errno () == EAGAIN) {
        rc = zmq_msg_close (&msg);
        assert (rc == 0);
        return Qfalse;
    }

    if (rc != 0) {
        rb_raise (rb_eRuntimeError, zmq_strerror (zmq_errno ()));
        rc = zmq_msg_close (&msg);
        assert (rc == 0);
        return Qnil;
    }

    rc = zmq_msg_close (&msg);
    assert (rc == 0);
    return Qtrue;
}

#ifdef HAVE_RUBY_INTERN_H
static VALUE socket_recv_blocking (void* args_)
{
    VALUE self_ = RARRAY_PTR(args_)[0];
    VALUE flags_ = RARRAY_PTR(args_)[1];
#else
static VALUE socket_recv (VALUE self_, VALUE flags_)
{
#endif
    assert (DATA_PTR (self_));

    zmq_msg_t msg;
    int rc = zmq_msg_init (&msg);
    assert (rc == 0);

    rc = zmq_recv (DATA_PTR (self_), &msg, NUM2INT (flags_));
    if (rc != 0 && zmq_errno () == EAGAIN) {
        rc = zmq_msg_close (&msg);
        assert (rc == 0);
        return Qnil;
    }

    if (rc != 0) {
        rb_raise (rb_eRuntimeError, zmq_strerror (zmq_errno ()));
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

#ifdef HAVE_RUBY_INTERN_H
static VALUE socket_send (VALUE self_, VALUE msg_, VALUE flags_)
{
  VALUE args = rb_ary_new3(3, self_, msg_, flags_);
  return rb_thread_blocking_region(socket_send_blocking, (void*)args, NULL, NULL);
}

static VALUE socket_recv (VALUE self_, VALUE flags_)
{
    VALUE args = rb_ary_new3(2, self_, flags_);
    return rb_thread_blocking_region(socket_recv_blocking, (void*)args, NULL, NULL);
}
#endif

static VALUE socket_close (VALUE self_)
{
    void * s = NULL;
    Data_Get_Struct(self_, void, s);
    int rc = zmq_close(s);
    if (rc != 0) {
        rb_raise (rb_eRuntimeError, zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    return Qnil;
}

extern "C" void Init_zmq ()
{
    VALUE zmq_module = rb_define_module ("ZMQ");
    VALUE context_type = rb_define_class_under (zmq_module, "Context",
        rb_cObject);
    rb_define_alloc_func (context_type, context_alloc);
    rb_define_method (context_type, "initialize",
        (VALUE(*)(...)) context_initialize, 3);
    rb_define_method (context_type, "socket",
        (VALUE(*)(...)) context_socket, 1);

    socket_type = rb_define_class_under (zmq_module, "Socket", rb_cObject);
    rb_undef_alloc_func(socket_type);
#ifdef ZMQ_RCVMORE
    /* zeromq 2.0.7 and higher */
    rb_define_method (socket_type, "getsockopt",
        (VALUE(*)(...)) socket_getsockopt, 1);
#endif
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
#ifdef ZMQ_LWM
    /* ZMQ_LWM is deprecated. */
    rb_define_const (zmq_module, "LWM", INT2NUM (ZMQ_LWM));
#endif
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
#ifdef ZMQ_SNDMORE
    /* zeromq 2.0.7 and higher */
    rb_define_const (zmq_module, "SNDMORE", INT2NUM (ZMQ_SNDMORE));
#endif
#ifdef ZMQ_RCVMORE
    /* zeromq 2.0.7 and higher */
    rb_define_const (zmq_module, "RCVMORE", INT2NUM (ZMQ_RCVMORE));
#endif

    rb_define_const (zmq_module, "NOBLOCK", INT2NUM (ZMQ_NOBLOCK));

#ifdef ZMQ_P2P
    /* ZMQ_P2P is deprecated. */
    rb_define_const (zmq_module, "P2P", INT2NUM (ZMQ_P2P));
#endif
#ifdef ZMQ_PAIR
    /* zeromq 2.0.7 and higher */
    rb_define_const (zmq_module, "PAIR", INT2NUM (ZMQ_PAIR));
#endif
    rb_define_const (zmq_module, "SUB", INT2NUM (ZMQ_SUB));
    rb_define_const (zmq_module, "PUB", INT2NUM (ZMQ_PUB));
    rb_define_const (zmq_module, "REQ", INT2NUM (ZMQ_REQ));
    rb_define_const (zmq_module, "REP", INT2NUM (ZMQ_REP));
    rb_define_const (zmq_module, "XREQ", INT2NUM (ZMQ_XREQ));
    rb_define_const (zmq_module, "XREP", INT2NUM (ZMQ_XREP));
    rb_define_const (zmq_module, "UPSTREAM", INT2NUM (ZMQ_UPSTREAM));
    rb_define_const (zmq_module, "DOWNSTREAM", INT2NUM (ZMQ_DOWNSTREAM));

    rb_define_const (zmq_module, "POLL", INT2NUM (ZMQ_POLL));
}
