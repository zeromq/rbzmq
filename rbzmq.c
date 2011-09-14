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
VALUE exception_type;

/*
 * Document-class: ZMQ
 *
 * Ruby interface to the zeromq messaging library.
 */

/*
 * call-seq:
 *   ZMQ.version() -> [major, minor, patch]
 *
 * Returns the version of the zeromq library.
 */
static VALUE module_version (VALUE self_)
{
    int major, minor, patch;
    
    zmq_version(&major, &minor, &patch);
    
    return rb_ary_new3 (3, INT2NUM (major), INT2NUM (minor), INT2NUM (patch));
}

/*
 * Document-class: ZMQ::Context
 *
 * ZeroMQ library context.
 */

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

/*
 * Document-method: new
 *
 * call-seq:
 *   new(io_threads=1)
 *
 * Initializes a new 0MQ context. The io_threads argument specifies the size
 * of the 0MQ thread pool to handle I/O operations. If your application is
 * using only the _inproc_ transport for you may set this to zero; otherwise,
 * set it to at least one.
 */

static VALUE context_initialize (int argc_, VALUE* argv_, VALUE self_)
{
    VALUE io_threads;
    rb_scan_args (argc_, argv_, "01", &io_threads);

    assert (!DATA_PTR (self_));
    void *ctx = zmq_init (NIL_P (io_threads) ? 1 : NUM2INT (io_threads));
    if (!ctx) {
        rb_raise (exception_type, "%s", zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    DATA_PTR (self_) = (void*) ctx;
    return self_;
}

/*
 * call-seq:
 *   zmq.close() -> nil
 *
 * Terminates the 0MQ context.
 *
 * Context termination is performed in the following steps:
 *
 * 1. Any blocking operations currently in progress on sockets open
 *    within context shall return immediately with an error code of
 *    ETERM. With the exception of ZMQ::Socket#close(), any further operations on
 *    sockets open within context shall fail with an error code of ETERM.
 *
 * 2. After interrupting all blocking calls, zmq_term() shall block until
 *    the following conditions are satisfied:
 *    * All sockets open within context have been closed with ZMQ::Socket#close().
 *    * For each socket within context, all messages sent by the
 *      application with ZMQ::Socket#send() have either been physically
 *      transferred to a network peer, or the socket’s linger period
 *      set with the ZMQ::LINGER socket option has expired.
 *
 * For further details regarding socket linger behaviour refer to the
 * ZMQ::LINGER option in ZMQ::Socket#setsockopt().
 */
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

typedef VALUE(*iterfunc)(ANYARGS);

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

struct select_arg {
    VALUE readset;
    VALUE writeset;
    VALUE errset;
    long timeout_usec;
    zmq_pollitem_t *items;
};

static VALUE internal_select(VALUE argval)
{
    struct select_arg *arg = (struct select_arg *)argval;

    int rc, nitems, i;
    zmq_pollitem_t *item;

    struct poll_state ps;
    ps.nitems = 0;
    ps.items = arg->items;
    ps.io_objects = rb_ary_new ();

    if (!NIL_P (arg->readset)) {
        ps.event = ZMQ_POLLIN;
        rb_iterate(rb_each, arg->readset, (iterfunc)poll_add_item, (VALUE)&ps);
    }

    if (!NIL_P (arg->writeset)) {
        ps.event = ZMQ_POLLOUT;
        rb_iterate(rb_each, arg->writeset, (iterfunc)poll_add_item, (VALUE)&ps);
    }

    if (!NIL_P (arg->errset)) {
        ps.event = ZMQ_POLLERR;
        rb_iterate(rb_each, arg->errset, (iterfunc)poll_add_item, (VALUE)&ps);
    }
    
    /* Reset nitems to the actual number of zmq_pollitem_t records we're sending. */
    nitems = ps.nitems;

#ifdef HAVE_RUBY_INTERN_H
    if (arg->timeout_usec != 0) {
        struct zmq_poll_args poll_args;
        poll_args.items = ps.items;
        poll_args.nitems = ps.nitems;
        poll_args.timeout_usec = arg->timeout_usec;

        rb_thread_blocking_region (zmq_poll_blocking, (void*)&poll_args, NULL, NULL);
        rc = poll_args.rc;
    }
    else
#endif
        rc = zmq_poll (ps.items, ps.nitems, arg->timeout_usec);
    
    if (rc == -1) {
        rb_raise(exception_type, "%s", zmq_strerror (zmq_errno ()));
        return Qnil;
    }
    else if (rc == 0)
        return Qnil;
    
    VALUE read_active = rb_ary_new ();
    VALUE write_active = rb_ary_new ();
    VALUE err_active = rb_ary_new ();
    
    for (i = 0, item = &ps.items[0]; i < nitems; i++, item++) {
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
    
    return rb_ary_new3 (3, read_active, write_active, err_active);
}

static VALUE module_select_internal(VALUE readset, VALUE writeset, VALUE errset, long timeout_usec)
{
    size_t nitems;
    struct select_arg arg;

    /* Conservative estimate for nitems before we traverse the lists. */
    nitems = (NIL_P (readset) ? 0 : RARRAY_LEN (readset)) +
             (NIL_P (writeset) ? 0 : RARRAY_LEN (writeset)) +
             (NIL_P (errset) ? 0 : RARRAY_LEN (errset));
    arg.items = (zmq_pollitem_t*)ruby_xmalloc(sizeof(zmq_pollitem_t) * nitems);

    arg.readset = readset;
    arg.writeset = writeset;
    arg.errset = errset;
    arg.timeout_usec = timeout_usec;

    return rb_ensure(internal_select, (VALUE)&arg, (VALUE (*)())ruby_xfree, (VALUE)arg.items);
}

/*
 * call-seq:
 *   ZMQ.select(in, out=[], err=[], timeout=nil) -> [in, out, err] | nil
 *
 * Like IO.select, but also works with 0MQ sockets.
 */
static VALUE module_select (int argc_, VALUE* argv_, VALUE self_)
{
    VALUE readset, writeset, errset, timeout;
    rb_scan_args (argc_, argv_, "13", &readset, &writeset, &errset, &timeout);

    long timeout_usec;

    if (!NIL_P (readset)) Check_Type (readset, T_ARRAY);
    if (!NIL_P (writeset)) Check_Type (writeset, T_ARRAY);
    if (!NIL_P (errset)) Check_Type (errset, T_ARRAY);
    
    if (NIL_P (timeout))
        timeout_usec = -1;
    else
        timeout_usec = (long)(NUM2DBL (timeout) * 1000000);

    return module_select_internal(readset, writeset, errset, timeout_usec);
}

static void socket_free (void *s)
{
    if (s) {
       int rc = zmq_close (s);
       assert (rc == 0);
    }
}

/*
 * Document-method: socket
 *
 * call-seq:
 *   zmq.socket(socket_type)
 *
 * Creates a new 0MQ socket.  The socket_type argument specifies the socket
 * type, which determines the semantics of communication over the socket.
 *
 * The newly created socket is initially unbound, and not associated with any
 * endpoints. In order to establish a message flow a socket must first be
 * connected to at least one endpoint with connect(), or at least one
 * endpoint must be created for accepting incoming connections with
 * bind().
 *
 * For a description of the various socket types, see ZMQ::Socket.
 */
static VALUE context_socket (VALUE self_, VALUE type_)
{
    void * c = NULL;
    Data_Get_Struct (self_, void, c);
    void * s = zmq_socket (c, NUM2INT (type_));
    if (!s) {
        rb_raise (exception_type, "%s", zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    return Data_Wrap_Struct(socket_type, 0, socket_free, s);
}

/*
 * Document-class: ZMQ::Socket
 *
 * ZeroMQ message socket.
 *
 * = Description
 * == Key differences to conventional sockets
 * Generally speaking, conventional sockets present a _synchronous_ interface
 * to either connection-oriented reliable byte streams (SOCK_STREAM), or
 * connection-less unreliable datagrams (SOCK_DGRAM). In comparison, 0MQ
 * sockets present an abstraction of an asynchronous <em>message queue</em>, with the
 * exact queueing semantics depending on the socket type in use. Where
 * conventional sockets transfer streams of bytes or discrete datagrams, 0MQ
 * sockets transfer discrete _messages_.
 *
 * 0MQ sockets being _asynchronous_ means that the timings of the physical
 * connection setup and teardown, reconnect and effective delivery are
 * transparent to the user and organized by 0MQ itself. Further, messages
 * may be _queued_ in the event that a peer is unavailable to receive them.
 *
 * Conventional sockets allow only strict one-to-one (two peers),
 * many-to-one (many clients, one server), or in some cases one-to-many
 * (multicast) relationships. With the exception of ZMQ::PAIR, 0MQ sockets
 * may be connected <b>to multiple endpoints</b> using connect(), while
 * simultaneously accepting incoming connections <b>from multiple endpoints</b>
 * bound to the socket using bind(), thus allowing many-to-many relationships.
 *
 * == Socket Types
 *
 * The following sections present the socket types defined by 0MQ, grouped by
 * the general <em>messaging pattern</em> which is built from related
 * socket types.
 *
 * = Request-reply pattern
 * The request-reply pattern is used for sending requests from a _client_ to one
 * or more instances of a _service_, and receiving subsequent replies to each
 * request sent.
 *
 * == ZMQ::REQ
 * A socket of type ZMQ::REQ is used by a _client_ to send requests to and receive
 * replies from a _service_. This socket type allows only an alternating sequence
 * of send(request) and subsequent recv(reply) calls. Each request sent
 * is load-balanced among all _services_, and each reply received is matched with
 * the last issued request.
 *
 * When a ZMQ::REQ socket enters an exceptional state due to having reached the
 * high water mark for all _services_, or if there are no _services_ at all, then
 * any send() operations on the socket shall block until the exceptional
 * state ends or at least one _service_ becomes available for sending; messages
 * are not discarded.
 *
 * === Summary of ZMQ::REQ characteristics
 * [Compatible peer sockets] ZMQ::REP
 * [Direction] Bidirectional
 * [Send/receive pattern] Send, Receive, Send, Receive, ...
 * [Outgoing routing strategy] Load-balanced
 * [Incoming routing strategy] Last peer
 * [ZMQ::HWM option action] Block
 *
 * == ZMQ::REP
 * A socket of type ZMQ::REP is used by a _service_ to receive requests from and
 * send replies to a _client_. This socket type allows only an alternating
 * sequence of recv(request) and subsequent send(reply) calls. Each
 * request received is fair-queued from among all _clients_, and each reply sent
 * is routed to the _client_ that issued the last request.
 *
 * When a ZMQ::REP socket enters an exceptional state due to having reached the
 * high water mark for a _client_, then any replies sent to the _client_ in
 * question shall be dropped until the exceptional state ends.
 *
 * === Summary of ZMQ::REP characteristics
 * [Compatible peer sockets] ZMQ::REQ
 * [Direction] Bidirectional
 * [Send/receive pattern] Receive, Send, Receive, Send, ...
 * [Incoming routing strategy] Fair-queued
 * [Outgoing routing stratagy] Last peer
 * [ZMQ::HWM option action] Drop
 *
 *
 * = Publish-subscribe pattern
 * The publish-subscribe pattern is used for one-to-many distribution of data
 * from a single _publisher_ to multiple _subscribers_ in a fanout fashion.
 *
 * == ZMQ::PUB
 * A socket of type ZMQ::PUB is used by a publisher to distribute data. Messages
 * sent are distributed in a fanout fashion to all connected peers. The
 * recv() function is not implemented for this socket type.
 *
 * When a ZMQ::PUB socket enters an exceptional state due to having reached the
 * high water mark for a _subscriber_, then any messages that would be sent to the
 * subscriber in question shall instead be dropped until the exceptional state
 * ends.
 *
 * === Summary of ZMQ::PUB characteristics
 * [Compatible peer sockets] ZMQ::SUB
 * [Direction] Unidirectional
 * [Send/receive pattern] Send only
 * [Incoming routing strategy] N/A
 * [Outgoing routing strategy] Fanout
 * [ZMQ::HWM option action] Drop
 *
 * == ZMQ::SUB
 *
 * A socket of type ZMQ::SUB is used by a _subscriber_ to subscribe to data
 * distributed by a _publisher_. Initially a ZMQ::SUB socket is not subscribed to
 * any messages, use the ZMQ::SUBSCRIBE option of setsockopt() to specify which
 * messages to subscribe to. The send() function is not implemented for this
 * socket type.
 *
 * === Summary of ZMQ::SUB characteristics
 * [Compatible peer sockets] ZMQ::PUB
 * [Direction] Unidirectional
 * [Send/receive pattern] Receive only
 * [Incoming routing strategy] Fair-queued
 * [Outgoing routing strategy] N/A
 * [ZMQ::HWM option action] N/A
 *
 * = Pipeline pattern
 * The pipeline pattern is used for distributing data to _nodes_ arranged in a
 * pipeline. Data always flows down the pipeline, and each stage of the pipeline
 * is connected to at least one _node_. When a pipeline stage is connected to
 * multiple _nodes_ data is load-balanced among all connected _nodes_.
 *
 * == ZMQ::PUSH
 *
 * A socket of type ZMQ::PUSH is used by a pipeline node to send messages
 * to downstream pipeline nodes. Messages are load-balanced to all connected
 * downstream nodes. The ZMQ::recv() function is not implemented for this socket
 * type.
 *
 * When a ZMQ::PUSH socket enters an exceptional state due to having
 * reached the high water mark for all downstream _nodes_, or if there are no
 * downstream _nodes_ at all, then any send() operations on the socket shall
 * block until the exceptional state ends or at least one downstream _node_
 * becomes available for sending; messages are not discarded.
 *
 * === Summary of ZMQ::PUSH characteristics
 * [Compatible peer sockets] ZMQ::PULL
 * [Direction] Unidirectional
 * [Send/receive pattern] Send only
 * [Incoming routing strategy] N/A
 * [Outgoing routing strategy] Load-balanced
 * [ZMQ::HWM option action] Block
 *
 * == ZMQ::PULL
 *
 * A socket of type ZMQ::PULL is used by a pipeline _node_ to receive messages
 * from upstream pipeline _nodes_. Messages are fair-queued from among all
 * connected upstream nodes. The send() function is not implemented for
 * this socket type.
 *
 * === Summary of ZMQ::PULL characteristics
 * [Compatible peer sockets]  ZMQ::PUSH
 * [Direction] Unidirectional
 * [Send/receive pattern] Receive only
 * [Incoming routing strategy] Fair-queued
 * [Outgoing routing strategy] N/A
 * [ZMQ::HWM option action] N/A
 *
 * = Exclusive pair pattern
 *
 * The exclusive pair is an advanced pattern used for communicating exclusively
 * between two peers.
 *
 * == ZMQ::PAIR
 *
 * A socket of type ZMQ::PAIR can only be connected to a single peer at any one
 * time. No message routing or filtering is performed on messages sent over a
 * ZMQ::PAIR socket.
 *
 * When a ZMQ::PAIR socket enters an exceptional state due to having reached the
 * high water mark for the connected peer, or if no peer is connected, then any
 * send() operations on the socket shall block until the peer becomes
 * available for sending; messages are not discarded.
 *
 * *NOTE* ZMQ::PAIR sockets are experimental, and are currently missing several
 * features such as auto-reconnection.
 *
 * === Summary of ZMQ::PAIR characteristics
 * [Compatible peer sockets] ZMQ::PAIR
 * [Direction] Bidirectional
 * [Send/receive pattern] Unrestricted
 * [Incoming routing strategy] N/A
 * [Outcoming routing strategy] N/A
 * [ZMQ::HWM option action] Block
 */

/*
 * call-seq:
 *   socket.getsockopt(option)
 *
 * Retrieves the value of the specified 0MQ socket option.
 *
 * The following options can be retrievesd with the getsockopt() function:
 *
 * == ZMQ::RCVMORE: More message parts to follow
 * The ZMQ::RCVMORE option shall return a boolean value indicating if the
 * multi-part message currently being read from the specified socket has more
 * message parts to follow. If there are no message parts to follow or if the
 * message currently being read is not a multi-part message a value of false
 * shall be returned. Otherwise, a value of true shall be returned.
 *
 * Refer to send() and recv() for a detailed description of sending/receiving
 * multi-part messages.
 *
 * [Option value type] Boolean
 * [Option value unit] N/A
 * [Default value] N/A
 * [Applicable socket types] all
 *
 * == ZMQ::HWM: Retrieve high water mark
 * The ZMQ::HWM option shall retrieve the high water mark for the specified
 * _socket_. The high water mark is a hard limit on the maximum number of
 * outstanding messages 0MQ shall queue in memory for any single peer that the
 * specified _socket_ is communicating with.
 *
 * If this limit has been reached the socket shall enter an exceptional state
 * and depending on the socket type, 0MQ shall take appropriate action such as
 * blocking or dropping sent messages. Refer to the individual socket
 * descriptions in ZMQ::Socket for details on the exact action taken for each
 * socket type.
 *
 * The default ZMQ::HWM value of zero means "no limit".
 *
 * [Option value type] Integer
 * [Option value unit] messages
 * [Default value] 0
 * [Applicable socket types] all
 *
 * == ZMQ::SWAP: Retrieve disk offload size
 * The ZMQ::SWAP option shall retrieve the disk offload (swap) size for the
 * specified _socket_. A socket which has ZMQ::SWAP set to a non-zero value may
 * exceed it’s high water mark; in this case outstanding messages shall be
 * offloaded to storage on disk rather than held in memory.
 *
 * The value of ZMQ::SWAP defines the maximum size of the swap space in bytes.
 *
 * [Option value type] Integer
 * [Option value unit] bytes
 * [Default value] 0
 * [Applicable socket types] all
 *
 * == ZMQ::AFFINITY: Retrieve I/O thread affinity
 * The ZMQ::AFFINITY option shall retrieve the I/O thread affinity for newly
 * created connections on the specified _socket_.
 *
 * Affinity determines which threads from the 0MQ I/O thread pool associated
 * with the socket’s _context_ shall handle newly created connections. A value of
 * zero specifies no affinity, meaning that work shall be distributed fairly
 * among all 0MQ I/O threads in the thread pool. For non-zero values, the lowest
 * bit corresponds to thread 1, second lowest bit to thread 2 and so on. For
 * example, a value of 3 specifies that subsequent connections on _socket_ shall
 * be handled exclusively by I/O threads 1 and 2.
 *
 * See also ZMQ::Context#new for details on allocating the number of
 * I/O threads for a specific _context_.
 *
 * [Option value type] Integer
 * [Option value unit] N/A (bitmap)
 * [Default value] 0
 * [Applicable socket types] all
 *
 * == ZMQ::IDENTITY: Retrieve socket identity
 * The ZMQ::IDENTITY option shall retrieve the identity of the specified _socket_.
 * Socket identity determines if existing 0MQ infastructure (<em>message queues</em>,
 * <em>forwarding devices</em>) shall be identified with a specific application and
 * persist across multiple runs of the application.
 *
 * If the socket has no identity, each run of an application is completely
 * separate from other runs. However, with identity set the socket shall re-use
 * any existing 0MQ infrastructure configured by the previous run(s). Thus the
 * application may receive messages that were sent in the meantime, <em>message
 * queue</em> limits shall be shared with previous run(s) and so on.
 *
 * Identity can be at least one byte and at most 255 bytes long. Identities
 * starting with binary zero are reserved for use by 0MQ infrastructure.
 *
 * [Option value type] String
 * [Option value unit] N/A
 * [Default value] nil
 * [Applicable socket types] all
 *
 * == ZMQ::RATE: Retrieve multicast data rate
 *
 * The ZMQ::Rate option shall retrieve the maximum send or receive data
 * rate for multicast transports using the specified _socket_.
 * 
 * [Option value type] Integer
 * [Option value unit] kilobits per second
 * [Default value] 100
 * [Applicable socket types] all, when using multicast transports
 *
 * == ZMQ::RECOVERY_IVL: Get multicast recovery interval
 *
 * The ZMQ::RECOVERY_IVL option shall retrieve the recovery interval for
 * multicast transports using the specified _socket_. The recovery interval
 * determines the maximum time in seconds that a receiver can be absent from a
 * multicast group before unrecoverable data loss will occur.
 *
 * [Option value type] Integer
 * [Option value unit] seconds
 * [Default value] 10
 * [Applicable socket types] all, when using multicast transports
 *
 * == ZMQ::RECOVERY_IVL_MSEC: Get multicast recovery interval in milliseconds
 * The ZMQ::RECOVERY_IVL_MSEC option shall retrieve the recovery interval, in
 * milliseconds (ms) for multicast transports using the specified socket. The
 * recovery interval determines the maximum time in milliseconds that a receiver
 * can be absent from a multicast group before unrecoverable data loss will occur.
 *
 * For backward compatibility, the default value of ZMQ::RECOVERY_IVL_MSEC is -1
 * indicating that the recovery interval should be obtained from the
 * ZMQ::RECOVERY_IVL option. However, if the ZMQ::RECOVERY_IVL_MSEC value is not
 * zero, then it will take precedence, and be used.
 *
 * [Option value type] Integer
 * [Option value unit] milliseconds
 * [Default value] -1
 * [Applicable socket types] all, when using multicast transports
 *
 * == ZMQ::MCAST_LOOP: Control multicast loopback
 * The ZMQ::MCAST_LOOP option controls whether data sent via multicast transports
 * can also be received by the sending host via loopback. A value of zero
 * indicates that the loopback functionality is disabled, while the default
 * value of 1 indicates that the loopback functionality is enabled. Leaving
 * multicast loopback enabled when it is not required can have a negative impact
 * on performance. Where possible, disable ZMQ::MCAST_LOOP in production
 * environments.
 *
 * [Option value type] Boolean
 * [Option value unit] N/A
 * [Default value] true
 * [Applicable socket types] all, when using multicast transports
 *
 * == ZMQ::SNDBUF: Retrieve kernel transmit buffer size
 * The ZMQ::SNDBUF option shall retrieve the underlying kernel transmit buffer
 * size for the specified _socket_. A value of zero means that the OS default is
 * in effect. For details refer to your operating system documentation for the
 * SO_SNDBUF socket option.
 * 
 * [Option value type] Integer
 * [Option value unit] bytes
 * [Default value] 0
 * [Applicable socket types] all
 *
 * == ZMQ::RCVBUF: Retrieve kernel receive buffer size
 * The ZMQ::RCVBUF option shall retrieve the underlying kernel receive buffer
 * size for the specified _socket_. A value of zero means that the OS default is
 * in effect. For details refer to your operating system documentation for the
 * SO_RCVBUF socket option.
 * 
 * [Option value type] Integer
 * [Option value unit] bytes
 * [Default value] 0
 * [Applicable socket types] all
 *
 * == ZMQ::LINGER: Retrieve linger period for socket shutdown
 * The ZMQ::LINGER option shall retrieve the linger period for the specified
 * socket. The linger period determines how long pending messages which have
 * yet to be sent to a peer shall linger in memory after a socket is closed
 * with ZMQ::Socket#close(), and further affects the termination of the
 * socket’s context with ZMQ#close(). The following outlines the different
 * behaviours:
 *
 * * The default value of −1 specifies an infinite linger period.
 *   Pending messages shall not be discarded after a call to ZMQ::Socket#close();
 *   attempting to terminate the socket’s context with ZMQ::Context#close() shall block
 *   until all pending messages have been sent to a peer.
 *
 * * The value of 0 specifies no linger period. Pending messages shall be
 *   discarded immediately when the socket is closed with ZMQ::Socket#close.
 *
 * * Positive values specify an upper bound for the linger period in
 *   milliseconds. Pending messages shall not be discarded after a call to
 *   ZMQ::Socket#close(); attempting to terminate the socket’s context with
 *   ZMQ::Context#close() shall block until either all pending messages have been sent
 *   to a peer, or the linger period expires, after which any pending messages
 *   shall be discarded.
 *
 * [Option value type] Integer
 * [Option value unit] milliseconds
 * [Default value] -1 (infinite)
 * [Applicable socket types] all
 *
 * == ZMQ::RECONNECT_IVL: Retrieve reconnection interval
 * The ZMQ::RECONNECT_IVL option shall retrieve the reconnection interval for
 * the specified socket. The reconnection interval is the maximum period 0MQ
 * shall wait between attempts to reconnect disconnected peers when using
 * connection−oriented transports.
 *
 * [Option value type] Integer
 * [Option value unit] milliseconds
 * [Default value] 100
 * [Applicable socket types] all, only for connection-oriented transports
 *
 * == ZMQ::RECONNECT_IVL_MAX: Retrieve maximum reconnection interval
 * The ZMQ::RECONNECT_IVL_MAX option shall set the maximum reconnection interval
 * for the specified socket. This is the maximum period ØMQ shall wait between
 * attempts to reconnect. On each reconnect attempt, the previous interval
 * shall be doubled untill ZMQ::RECONNECT_IVL_MAX is reached. This allows for
 * exponential backoff strategy. Default value means no exponential backoff
 * is performed and reconnect interval calculations are only based on
 * ZMQ::RECONNECT_IVL.
 *
 * Values less than ZMQ::RECONNECT_IVL will be ignored.
 *
 * [Option value type] Integer
 * [Option value unit] milliseconds
 * [Default value] 0 (only use RECONNECT_IVL)
 * [Applicable socket types] all, only for connection-oriented transports
 *
 * == ZMQ::BACKLOG: Retrieve maximum length of the queue of outstanding connections
 * The ZMQ::BACKLOG option shall retrieve the maximum length of the queue of
 * outstanding peer connections for the specified socket; this only applies to
 * connection−oriented transports. For details refer to your operating system
 * documentation for the listen function.
 *
 * [Option value type] Integer
 * [Option value unit] connections
 * [Default value] 100
 * [Applicable socket types] all, only for connection-oriented transports
 *
 * == ZMQ::FD: Retrieve file descriptor associated with the socket
 * The ZMQ::FD option shall retrieve the file descriptor associated with the
 * specified socket. The returned file descriptor can be used to integrate the
 * socket into an existing event loop; the 0MQ library shall signal any pending
 * events on the socket in an edge−triggered fashion by making the file
 * descriptor become ready for reading.
 *
 * === Note
 * The ability to read from the returned file descriptor does not necessarily
 * indicate that messages are available to be read from, or can be written to,
 * the underlying socket; applications must retrieve the actual event state
 * with a subsequent retrieval of the ZMQ::EVENTS option.
 *
 * === Caution
 * The returned file descriptor is intended for use with a poll or similar
 * system call only. Applications must never attempt to read or write data
 * to it directly.
 *
 * [Option value type] int on POSIX systems, SOCKT on Windows
 * [Option value unit] N/A
 * [Default value] N/A
 * [Applicable socket types] all
 *
 * == ZMQ::EVENTS: Retrieve socket event state
 * The ZMQ::EVENTS option shall retrieve the event state for the specified
 * socket. The returned value is a bit mask constructed by OR’ing a combination
 * of the following event flags:
 *
 * === ZMQ::POLLIN
 * Indicates that at least one message may be received from the specified
 * socket without blocking.
 *
 * == ZMQ::POLLOUT
 * Indicates that at least one message may be sent to the specified socket
 * without blocking.
 *
 * The combination of a file descriptor returned by the ZMQ::FD option being
 * ready for reading but no actual events returned by a subsequent retrieval of
 * the ZMQ::EVENTS option is valid; applications should simply ignore this case
 * and restart their polling operation/event loop.
 *
 * [Option value type] uint32_t
 * [Option value unit] N/A (flags)
 * [Default value] N/A
 * [Applicable socket types] all
 *
 */
static VALUE socket_getsockopt (VALUE self_, VALUE option_)
{
    int rc = 0;
    VALUE retval;
    void * s;
    
    Data_Get_Struct (self_, void, s);
    Check_Socket (s);
  
    switch (NUM2INT (option_)) {
#if ZMQ_VERSION >= 20100
	case ZMQ_FD:
        {
#ifdef _WIN32
			SOCKET optval;
#else
			int optval;
#endif
            size_t optvalsize = sizeof(optval);

            rc = zmq_getsockopt (s, NUM2INT (option_), (void *)&optval,
                                 &optvalsize);

            if (rc != 0) {
              rb_raise (exception_type, "%s", zmq_strerror (zmq_errno ()));
              return Qnil;
            }

            if (NUM2INT (option_) == ZMQ_RCVMORE)
                retval = optval ? Qtrue : Qfalse;
            else
                retval = INT2NUM (optval);
        }
        break;
	case ZMQ_EVENTS:
        {
            uint32_t optval;
            size_t optvalsize = sizeof(optval);

            rc = zmq_getsockopt (s, NUM2INT (option_), (void *)&optval,
                                 &optvalsize);

            if (rc != 0) {
              rb_raise (exception_type, "%s", zmq_strerror (zmq_errno ()));
              return Qnil;
            }

            if (NUM2INT (option_) == ZMQ_RCVMORE)
                retval = optval ? Qtrue : Qfalse;
            else
                retval = INT2NUM (optval);
        }
        break;
	case ZMQ_TYPE:
	case ZMQ_LINGER:
	case ZMQ_RECONNECT_IVL:
	case ZMQ_BACKLOG:
#if ZMQ_VERSION >= 20101
	case ZMQ_RECONNECT_IVL_MAX:
	case ZMQ_RECOVERY_IVL_MSEC:
#endif
        {
            int optval;
            size_t optvalsize = sizeof(optval);

            rc = zmq_getsockopt (s, NUM2INT (option_), (void *)&optval,
                                 &optvalsize);

            if (rc != 0) {
              rb_raise (exception_type, "%s", zmq_strerror (zmq_errno ()));
              return Qnil;
            }

            if (NUM2INT (option_) == ZMQ_RCVMORE)
                retval = optval ? Qtrue : Qfalse;
            else
                retval = INT2NUM (optval);
        }
        break;
#endif
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
              rb_raise (exception_type, "%s", zmq_strerror (zmq_errno ()));
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
              rb_raise (exception_type, "%s", zmq_strerror (zmq_errno ()));
              return Qnil;
            }

            if (optvalsize > sizeof (identity))
                optvalsize = sizeof (identity);

            retval = rb_str_new (identity, optvalsize);
        }
        break;
    default:
        rb_raise (exception_type, "%s", zmq_strerror (EINVAL));
        return Qnil;
    }
  
    return retval;
}

/*
 * call-seq:
 *   socket.setsockopt(option, value) -> nil
 *
 * Sets the value of a 0MQ socket option.
 *
 * The following socket options can be set with the setsockopt() function:
 *
 * == ZMQ::HWM: Set high water mark
 * The ZMQ::HWM option shall set the high water mark for the specified _socket_.
 * The high water mark is a hard limit on the maximum number of outstanding
 * messages 0MQ shall queue in memory for any single peer that the specified
 * _socket_ is communicating with.
 *
 * If this limit has been reached the socket shall enter an exceptional state
 * and depending on the socket type, 0MQ shall take appropriate action such as
 * blocking or dropping sent messages. Refer to the individual socket
 * descriptions in ZMQ::Socket for details on the exact action taken for each
 * socket type.
 *
 * The default ZMQ::HWM value of zero means "no limit".
 *
 * [Option value type] Integer
 * [Option value unit] messages
 * [Default value] 0
 * [Applicable socket types] all
 *
 * == ZMQ::SWAP: Set disk offload size
 * The ZMQ::SWAP option shall set the disk offload (swap) size for the specified
 * socket. A socket which has ZMQ::SWAP set to a non-zero value may exceed it’s
 * high water mark; in this case outstanding messages shall be offloaded to
 * storage on disk rather than held in memory.
 *
 * The value of ZMQ::SWAP defines the maximum size of the swap space in bytes.
 *
 * [Option value type] Integer
 * [Option value unit] bytes
 * [Default value] 0
 * [Applicable socket types] all
 *
 * == ZMQ::AFFINITY: Set I/O thread affinity
 * The ZMQ::AFFINITY option shall set the I/O thread affinity for newly created
 * connections on the specified socket.
 *
 * Affinity determines which threads from the 0MQ I/O thread pool associated
 * with the socket’s _context_ shall handle newly created connections. A value of
 * zero specifies no affinity, meaning that work shall be distributed fairly
 * among all 0MQ I/O threads in the thread pool. For non-zero values, the lowest
 * bit corresponds to thread 1, second lowest bit to thread 2 and so on. For
 * example, a value of 3 specifies that subsequent connections on socket shall
 * be handled exclusively by I/O threads 1 and 2.
 *
 * See also ZMQ::Context#new for details on allocating the number of I/O threads
 * for a specific _context_.
 *
 * [Option value type] Integer
 * [Option value unit] N/A (bitmap)
 * [Default value] 0
 * [Applicable socket types] all
 *
 * == ZMQ::IDENTITY: Set socket identity
 * The ZMQ::IDENTITY option shall set the identity of the specified socket.
 * Socket identity determines if existing 0MQ infastructure (<em>message queues</em>,
 * <em>forwarding devices</em>) shall be identified with a specific application and
 * persist across multiple runs of the application.
 *
 * If the socket has no identity, each run of an application is completely
 * separate from other runs. However, with identity set the socket shall re-use
 * any existing 0MQ infrastructure configured by the previous run(s). Thus the
 * application may receive messages that were sent in the meantime, <em>message
 * queue</em> limits shall be shared with previous run(s) and so on.
 *
 * Identity should be at least one byte and at most 255 bytes long. Identities
 * starting with binary zero are reserved for use by 0MQ infrastructure.
 *
 * [Option value type] String
 * [Option value unit] N/A
 * [Default value] nil
 * [Applicable socket types] all
 *
 * ZMQ::SUBSCRIBE: Establish message filter
 * The ZMQ::SUBSCRIBE option shall establish a new message filter on a ZMQ::SUB
 * socket. Newly created ZMQ::SUB sockets shall filter out all incoming messages,
 * therefore you should call this option to establish an initial message filter.
 *
 * An empty _value_ of length zero shall subscribe to all incoming messages. A
 * non-empty _value_ shall subscribe to all messages beginning with the
 * specified prefix. Mutiple filters may be attached to a single ZMQ::SUB socket,
 * in which case a message shall be accepted if it matches at least one filter.
 *
 * [Option value type] String
 * [Option value unit] N/A
 * [Default value] N/A
 * [Applicable socket types] ZMQ::SUB
 *
 * == ZMQ::UNSUBSCRIBE: Remove message filter
 * The ZMQ::UNSUBSCRIBE option shall remove an existing message filter on a
 * ZMQ::SUB socket. The filter specified must match an existing filter
 * previously established with the ZMQ::SUBSCRIBE option. If the socket has
 * several instances of the same filter attached the ZMQ::UNSUBSCRIBE option
 * shall remove only one instance, leaving the rest in place and functional.
 *
 * [Option value type] String
 * [Option value unit] N/A
 * [Default value] nil
 * [Applicable socket types] all
 *
 * == ZMQ::RATE: Set multicast data rate
 * The ZMQ::RATE option shall set the maximum send or receive data rate for
 * multicast transports such as _pgm_ using the specified socket.
 *
 * [Option value type] Integer
 * [Option value unit] kilobits per second
 * [Default value] 100
 * [Applicable socket types] all, when using multicast transports
 *
 * == ZMQ::RECOVERY_IVL: Set multicast recovery interval
 * The ZMQ::RECOVERY_IVL option shall set the recovery interval for multicast
 * transports using the specified _socket_. The recovery interval determines the
 * maximum time in seconds that a receiver can be absent from a multicast group
 * before unrecoverable data loss will occur.
 *
 * <bCaution:</b> Exercise care when setting large recovery intervals as the data
 * needed for recovery will be held in memory. For example, a 1 minute recovery
 * interval at a data rate of 1Gbps requires a 7GB in-memory buffer.
 *
 * [Option value type] Integer
 * [Option value unit] seconds
 * [Default value] 10
 * [Applicable socket types] all, when using multicast transports
 *
 * == ZMQ::RECOVERY_IVL_MSEC: Set multicast recovery interval in milliseconds
 * The ZMQ::RECOVERY_IVL_MSEC option shall set the recovery interval, specified
 * in milliseconds (ms) for multicast transports using the specified socket. The
 * recovery interval determines the maximum time in milliseconds that a receiver
 * can be absent from a multicast group before unrecoverable data loss will occur.
 *
 * A non-zero value of the ZMQ::RECOVERY_IVL_MSEC option will take precedence over
 * the ZMQ::RECOVERY_IVL option, but since the default for the
 * ZMQ::RECOVERY_IVL_MSEC is -1, the default is to use the ZMQ::RECOVERY_IVL option
 * value.
 *
 * <bCaution:</b> Exercise care when setting large recovery intervals as the data
 * needed for recovery will be held in memory. For example, a 1 minute recovery
 * interval at a data rate of 1Gbps requires a 7GB in-memory buffer.
 *
 * [Option value type] Integer
 * [Option value unit] milliseconds
 * [Default value] -1
 * [Applicable socket types] all, when using multicast transports
 *
 * == ZMQ::MCAST_LOOP: Control multicast loopback
 * The ZMQ::MCAST_LOOP option shall control whether data sent via multicast
 * transports using the specified _socket_ can also be received by the sending
 * host via loopback. A value of zero disables the loopback functionality, while
 * the default value of 1 enables the loopback functionality. Leaving multicast
 * loopback enabled when it is not required can have a negative impact on
 * performance. Where possible, disable ZMQ::MCAST_LOOP in production
 * environments.
 *
 * [Option value type] Boolean
 * [Option value unit] N/A
 * [Default value] true
 * [Applicable socket types] all, when using multicast transports
 *
 * == ZMQ::SNDBUF: Set kernel transmit buffer size
 * The ZMQ::SNDBUF option shall set the underlying kernel transmit buffer size
 * for the socket to the specified size in bytes. A value of zero means leave
 * the OS default unchanged. For details please refer to your operating system
 * documentation for the SO_SNDBUF socket option.
 *
 * [Option value type] Integer
 * [Option value unit] bytes
 * [Default value] 0
 * [Applicable socket types] all
 *
 * == ZMQ::RCVBUF: Set kernel receive buffer size
 * The ZMQ::RCVBUF option shall set the underlying kernel receive buffer size
 * for the socket to the specified size in bytes. A value of zero means leave
 * the OS default unchanged. For details refer to your operating system
 * documentation for the SO_RCVBUF socket option.
 *
 * [Option value type] Integer
 * [Option value unit] bytes
 * [Default value] 0
 * [Applicable socket types] all
 *
 * == ZMQ::LINGER: Set linger period for socket shutdown
 * The ZMQ::LINGER option shall set the linger period for the specified
 * socket. The linger period determines how long pending messages which have
 * yet to be sent to a peer shall linger in memory after a socket is closed
 * with ZMQ::Socket#close(), and further affects the termination of the
 * socket’s context with ZMQ#close(). The following outlines the different
 * behaviours:
 *
 * * The default value of −1 specifies an infinite linger period.
 *   Pending messages shall not be discarded after a call to ZMQ::Socket#close();
 *   attempting to terminate the socket’s context with ZMQ::Context#close() shall block
 *   until all pending messages have been sent to a peer.
 *
 * * The value of 0 specifies no linger period. Pending messages shall be
 *   discarded immediately when the socket is closed with ZMQ::Socket#close.
 *
 * * Positive values specify an upper bound for the linger period in
 *   milliseconds. Pending messages shall not be discarded after a call to
 *   ZMQ::Socket#close(); attempting to terminate the socket’s context with
 *   ZMQ::Context#close() shall block until either all pending messages have been sent
 *   to a peer, or the linger period expires, after which any pending messages
 *   shall be discarded.
 *
 * [Option value type] Integer
 * [Option value unit] milliseconds
 * [Default value] -1 (infinite)
 * [Applicable socket types] all
 *
 * == ZMQ::RECONNECT_IVL: Set reconnection interval
 * The ZMQ::RECONNECT_IVL option shall set the reconnection interval for
 * the specified socket. The reconnection interval is the maximum period 0MQ
 * shall wait between attempts to reconnect disconnected peers when using
 * connection−oriented transports.
 *
 * [Option value type] Integer
 * [Option value unit] milliseconds
 * [Default value] 100
 * [Applicable socket types] all, only for connection-oriented transports
 *
 * == ZMQ::BACKLOG: Set maximum length of the queue of outstanding connections
 * The ZMQ::BACKLOG option shall set the maximum length of the queue of
 * outstanding peer connections for the specified socket; this only applies to
 * connection−oriented transports. For details refer to your operating system
 * documentation for the listen function.
 *
 * [Option value type] Integer
 * [Option value unit] connections
 * [Default value] 100
 * [Applicable socket types] all, only for connection-oriented transports
 *
 */
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

#if ZMQ_VERSION >= 20100
	case ZMQ_LINGER:
	case ZMQ_RECONNECT_IVL:
	case ZMQ_BACKLOG:
#if ZMQ_VERSION >= 20101
    case ZMQ_RECONNECT_IVL_MAX:
    case ZMQ_RECOVERY_IVL_MSEC:
#endif
        {
            int optval = FIX2INT (optval_);

            //  Forward the code to native 0MQ library.
            rc = zmq_setsockopt (s, NUM2INT (option_),
                (void*) &optval, sizeof (optval));
        }
        break;
#endif

    case ZMQ_IDENTITY:
    case ZMQ_SUBSCRIBE:
    case ZMQ_UNSUBSCRIBE:

        //  Forward the code to native 0MQ library.
        rc = zmq_setsockopt (s, NUM2INT (option_),
	    (void *) StringValueCStr (optval_), RSTRING_LEN (optval_));
        break;

    default:
        rb_raise (exception_type, "%s", zmq_strerror (EINVAL));
        return Qnil;
    }

    if (rc != 0) {
        rb_raise (exception_type, "%s", zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    return self_;
}

/*
 * call-seq:
 *   socket.bind(endpoint) -> nil
 *
 * Creates an endpoint for accepting connections and binds it to the socket.
 *
 * The _endpoint_ argument is a string consisting of two parts as follows:
 * _transport://address_.  The _transport_ part specifies the underlying
 * transport protocol to use.  The meaning of the _address_ part is specific
 * to the underlying transport protocol selected.
 *
 * The following transports are defined:
 *
 * [_inproc_] local in-process (inter-thread) communication transport
 * [_ipc_] local inter-process communication transport
 * [_tcp_] unicast transport using TCP
 * [_pgm_, _epgm_] reliable multicast transport using PGM
 *
 * With the exception of ZMQ:PAIR sockets, a single socket may be connected to
 * multiple endpoints using connect(), while simultaneously accepting
 * incoming connections from multiple endpoints bound to the socket using
 * bind(). Refer to ZMQ::Socket for a description of the exact semantics
 * involved when connecting or binding a socket to multiple endpoints.
 */
static VALUE socket_bind (VALUE self_, VALUE addr_)
{
    void * s;
    Data_Get_Struct (self_, void, s);
    Check_Socket (s);

    int rc = zmq_bind (s, rb_string_value_cstr (&addr_));
    if (rc != 0) {
        rb_raise (exception_type, "%s", zmq_strerror (zmq_errno ()));
        return Qnil;
    }

    return Qnil;
}

/*
 * call-seq:
 *   socket.connect(endpoint) -> nil
 *
 * Connects the socket to the endpoint specified by the _endpoint_ argument.
 *
 * The _endpoint_ argument is a string consisting of two parts as follows:
 * _transport://address_.  The _transport_ part specifies the underlying
 * transport protocol to use.  The meaning of the _address_ part is specific
 * to the underlying transport protocol selected.
 *
 * The following transports are defined:
 *
 * [_inproc_] local in-process (inter-thread) communication transport
 * [_ipc_] local inter-process communication transport
 * [_tcp_] unicast transport using TCP
 * [_pgm_, _epgm_] reliable multicast transport using PGM
 *
 * With the exception of ZMQ:PAIR sockets, a single socket may be connected to
 * multiple endpoints using connect(), while simultaneously accepting
 * incoming connections from multiple endpoints bound to the socket using
 * bind(). Refer to ZMQ::Socket for a description of the exact semantics
 * involved when connecting or binding a socket to multiple endpoints.
 *
 * <b>NOTE:</b> The connection will not be performed immediately, but as needed by
 * 0MQ.  Thus, a successful invocation of connect() does not indicate that
 * a physical connection was or can actually be established.
 */
static VALUE socket_connect (VALUE self_, VALUE addr_)
{
    void * s;
    Data_Get_Struct (self_, void, s);
    Check_Socket (s);

    int rc = zmq_connect (s, rb_string_value_cstr (&addr_));
    if (rc != 0) {
        rb_raise (exception_type, "%s", zmq_strerror (zmq_errno ()));
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

/*
 * call-seq:
 *   socket.send(message, flags=0) -> true | false
 *
 * Queue the message referenced by the _msg_ argument to be send to the
 * _socket_.  The _flags_ argument is a combination of the flags defined
 * below:
 *
 * [ZMQ::NOBLOCK] Specifies that the operation should be performed in
 * non-blocking mode. If the message cannot be queued on the _socket_,
 * the function shall fail and return _false_.
 * [ZMQ::SNDMORE] Specifies that the message being sent is a multi-part message,
 * and that further message parts are to follow. Refer to the section regarding
 * multi-part messages below for a detailed description.
 *
 * <b>NOTE:</b> A successful invocation of send() does not indicate that the
 * message has been transmitted to the network, only that it has been queued on
 * the socket and 0MQ has assumed responsibility for the message.
 *
 * == Multi-part messages
 * A 0MQ message is composed of 1 or more message parts. 0MQ ensures atomic
 * delivery of messages; peers shall receive either all <em>message parts</em> of a
 * message or none at all.
 *
 * The total number of message parts is unlimited.
 *
 * An application wishing to send a multi-part message does so by specifying the
 * ZMQ::SNDMORE flag to send(). The presence of this flag indicates to 0MQ
 * that the message being sent is a multi-part message and that more message
 * parts are to follow. When the application wishes to send the final message
 * part it does so by calling send() without the ZMQ::SNDMORE flag; this
 * indicates that no more message parts are to follow.
 *
 * This function returns _true_ if successful, _false_ if not.
 */
static VALUE socket_send (int argc_, VALUE* argv_, VALUE self_)
{
    VALUE msg_, flags_;
    
    rb_scan_args (argc_, argv_, "11", &msg_, &flags_);

    void * s;
    Data_Get_Struct (self_, void, s);
    Check_Socket (s);

    Check_Type (msg_, T_STRING);

    int flags = NIL_P (flags_) ? 0 : NUM2INT (flags_);

    zmq_msg_t msg;
    int rc = zmq_msg_init_size (&msg, RSTRING_LEN (msg_));
    if (rc != 0) {
        rb_raise (exception_type, "%s", zmq_strerror (zmq_errno ()));
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
        rb_raise (exception_type, "%s", zmq_strerror (zmq_errno ()));
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

/*
 * call-seq:
 *   socket.recv(flags=0) -> message | nil
 *
 * Receives a message from the _socket_.  If there are no messages available
 * on the _socket_, the recv() function shall block until the request can be
 * satisfied.  The _flags_ argument is a combination of the flags defined
 * below:
 *
 * [ZMQ::NOBLOCK] Specifies that the operation should be performed in
 * non-blocking mode.  If there are no messages available on the _socket_,
 * the recv() function shall fail and return _nil_.
 *
 * == Multi-part messages
 * A 0MQ message is composed of 1 or more message parts. 0MQ ensures atomic
 * delivery of messages; peers shall receive either all <em>message parts</em> of a
 * message or none at all.
 *
 * The total number of message parts is unlimited.
 *
 * An application wishing to determine if a message is composed of multiple
 * parts does so by retrieving the value of the ZMQ::RCVMORE socket option on the
 * socket it is receiving the message from, using getsockopt(). If there are no
 * message parts to follow, or if the message is not composed of multiple parts,
 * ZMQ::RCVMORE shall report a value of false. Otherwise, ZMQ::RCVMORE shall
 * report a value of true, indicating that more message parts are to follow.
 */
static VALUE socket_recv (int argc_, VALUE* argv_, VALUE self_)
{
    VALUE flags_;
    
    rb_scan_args (argc_, argv_, "01", &flags_);

    void * s;
    Data_Get_Struct (self_, void, s);
    Check_Socket (s);

    int flags = NIL_P (flags_) ? 0 : NUM2INT (flags_);

    zmq_msg_t msg;
    int rc = zmq_msg_init (&msg);
    assert (rc == 0);

#ifdef HAVE_RUBY_INTERN_H
    if (!(flags & ZMQ_NOBLOCK)) {
        struct zmq_send_recv_args recv_args;
        recv_args.socket = s;
        recv_args.msg = &msg;
        recv_args.flags = flags;
        rb_thread_blocking_region (zmq_recv_blocking, (void*) &recv_args,
            NULL, NULL);
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
        rb_raise (exception_type, "%s", zmq_strerror (zmq_errno ()));
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

/*
 * call-seq:
 *   socket.close() -> nil
 *
 * Destroys the 0MQ socket.  Any outstanding messages physically received from
 * the network but not yet received by the application with ZMQ::Socket#recv()
 * shall be discarded. The behaviour for discarding messages sent by the
 * application with ZMQ::Socket#send() but not yet physically transferred to
 * the network depends on the value of the ZMQ::LINGER socket option for the
 * socket.
 */
static VALUE socket_close (VALUE self_)
{
    void * s = NULL;
    Data_Get_Struct (self_, void, s);
    if (s != NULL) {
        int rc = zmq_close (s);
        if (rc != 0) {
            rb_raise (exception_type, "%s", zmq_strerror (zmq_errno ()));
            return Qnil;
        }

        DATA_PTR (self_) = NULL;
    }
    return Qnil;
}

void Init_zmq ()
{
    VALUE zmq_module = rb_define_module ("ZMQ");
    rb_define_singleton_method (zmq_module, "version", module_version, 0);
    rb_define_singleton_method (zmq_module, "select", module_select, -1);

    exception_type = rb_define_class_under (zmq_module, "Error", rb_eRuntimeError );

    VALUE context_type = rb_define_class_under (zmq_module, "Context",
        rb_cObject);
    rb_define_alloc_func (context_type, context_alloc);
    rb_define_method (context_type, "initialize", context_initialize, -1);
    rb_define_method (context_type, "socket", context_socket, 1);
    rb_define_method (context_type, "close", context_close, 0);

    socket_type = rb_define_class_under (zmq_module, "Socket", rb_cObject);
    rb_undef_alloc_func(socket_type);
    rb_define_method (socket_type, "getsockopt", socket_getsockopt, 1);
    rb_define_method (socket_type, "setsockopt", socket_setsockopt, 2);
    rb_define_method (socket_type, "bind", socket_bind, 1);
    rb_define_method (socket_type, "connect", socket_connect, 1);
    rb_define_method (socket_type, "send", socket_send, -1);
    rb_define_method (socket_type, "recv", socket_recv, -1);
    rb_define_method (socket_type, "close", socket_close, 0);

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
#if ZMQ_VERSION >= 20100
    rb_define_const (zmq_module, "FD", INT2NUM (ZMQ_FD));
    rb_define_const (zmq_module, "EVENTS", INT2NUM (ZMQ_EVENTS));
    rb_define_const (zmq_module, "TYPE", INT2NUM (ZMQ_TYPE));
    rb_define_const (zmq_module, "LINGER", INT2NUM (ZMQ_LINGER));
    rb_define_const (zmq_module, "RECONNECT_IVL", INT2NUM (ZMQ_RECONNECT_IVL));
    rb_define_const (zmq_module, "BACKLOG", INT2NUM (ZMQ_BACKLOG));
#endif
#if ZMQ_VERSION >= 20101
    rb_define_const (zmq_module, "RECONNECT_IVL_MAX", INT2NUM (ZMQ_RECONNECT_IVL_MAX));
    rb_define_const (zmq_module, "RECOVERY_IVL_MSEC", INT2NUM (ZMQ_RECOVERY_IVL_MSEC));
#endif

    rb_define_const (zmq_module, "NOBLOCK", INT2NUM (ZMQ_NOBLOCK));

    rb_define_const (zmq_module, "PAIR", INT2NUM (ZMQ_PAIR));
    rb_define_const (zmq_module, "SUB", INT2NUM (ZMQ_SUB));
    rb_define_const (zmq_module, "PUB", INT2NUM (ZMQ_PUB));
    rb_define_const (zmq_module, "REQ", INT2NUM (ZMQ_REQ));
    rb_define_const (zmq_module, "REP", INT2NUM (ZMQ_REP));
#ifdef ZMQ_DEALER
    rb_define_const (zmq_module, "XREQ", INT2NUM (ZMQ_DEALER));
    rb_define_const (zmq_module, "XREP", INT2NUM (ZMQ_ROUTER));
    
    // Deprecated
    rb_define_const (zmq_module, "DEALER", INT2NUM (ZMQ_DEALER));
    rb_define_const (zmq_module, "ROUTER", INT2NUM (ZMQ_ROUTER));
#else
    rb_define_const (zmq_module, "DEALER", INT2NUM (ZMQ_XREQ));
    rb_define_const (zmq_module, "ROUTER", INT2NUM (ZMQ_XREP));

    // Deprecated
    rb_define_const (zmq_module, "XREQ", INT2NUM (ZMQ_XREQ));
    rb_define_const (zmq_module, "XREP", INT2NUM (ZMQ_XREP));
#endif

#ifdef ZMQ_PUSH
    rb_define_const (zmq_module, "PUSH", INT2NUM (ZMQ_PUSH));
    rb_define_const (zmq_module, "PULL", INT2NUM (ZMQ_PULL));

    //  Deprecated
    rb_define_const (zmq_module, "UPSTREAM", INT2NUM (ZMQ_PULL));
    rb_define_const (zmq_module, "DOWNSTREAM", INT2NUM (ZMQ_PUSH));
#else
    rb_define_const (zmq_module, "PUSH", INT2NUM (ZMQ_DOWNSTREAM));
    rb_define_const (zmq_module, "PULL", INT2NUM (ZMQ_UPSTREAM));

    //  Deprecated
    rb_define_const (zmq_module, "UPSTREAM", INT2NUM (ZMQ_UPSTREAM));
    rb_define_const (zmq_module, "DOWNSTREAM", INT2NUM (ZMQ_DOWNSTREAM));
#endif

}
