= ZeroMQ Ruby Bindings

ZeroMQ http://www.zeromq.org/

ØMQ looks like an embeddable networking library but acts like a concurrency framework. It gives you sockets that
carry whole messages across various transports like inproc, IPC, TCP, and multicast. You can connect sockets N-to-N
with patterns like fanout, pubsub, task distribution, and request-reply.

== Example

  require "zmq"

  context = ZMQ::Context.new(1)

  puts "Opening connection for READ"
  inbound = context.socket(ZMQ::UPSTREAM)
  inbound.bind("tcp://127.0.0.1:9000")

  outbound = context.socket(ZMQ::DOWNSTREAM)
  outbound.connect("tcp://127.0.0.1:9000")
  p outbound.send("Hello World!")
  p outbound.send("QUIT")

  loop do
    data = inbound.recv
    p data
    break if data == "QUIT"
  end

== License

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
