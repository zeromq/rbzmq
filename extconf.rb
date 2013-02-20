#
#    Copyright (c) 2007-2010 iMatix Corporation
#
#    This file is part of 0MQ.
#
#    0MQ is free software; you can redistribute it and/or modify it under
#    the terms of the Lesser GNU General Public License as published by
#    the Free Software Foundation; either version 3 of the License, or
#    (at your option) any later version.
#
#    0MQ is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    Lesser GNU General Public License for more details.
#
#    You should have received a copy of the Lesser GNU General Public License
#    along with this program.  If not, see <http://www.gnu.org/licenses/>.

require 'mkmf'
dir_config('zmq')

$CFLAGS += ' -std=c99'
CONFIG['warnflags'].gsub!(/-Wdeclaration-after-statement|-Wunused-parameter/, '') if CONFIG['warnflags']

def header?
  have_header('zmq.h') ||
    find_header('zmq.h', '/opt/local/include', '/usr/local/include', '/usr/include')
end

def library?
  have_library('zmq', 'zmq_init') ||
    find_library('zmq', 'zmq_init', '/opt/local/lib', '/usr/local/lib', '/usr/lib')
end

if header? && library?
  puts "Cool, I found your zmq install..."
  create_makefile("zmq")
else
  raise "Couldn't find zmq library. try setting --with-zmq-dir=<path> to tell me where it is."
end


