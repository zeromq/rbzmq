Gem::Specification.new do |s|
  s.name = 'zmq2'
  s.version = '2.0.7'
  s.date = '2010-06-17'
  s.authors = ['Martin Sustrik', 'Brian Buchanan']
  s.email = ['sustrik@250bpm.com', 'bwb@holo.org']
  s.description = 'This gem provides bindings for the zeromq library (2.x version series)'
  s.homepage = 'http://www.zeromq.org/bindings:ruby'
  s.summary = 'Ruby API for ZeroMQ 2.x'
  s.extensions = 'extconf.rb'
  s.files = Dir['Makefile'] + Dir['*.c']
  s.has_rdoc = true
  s.extra_rdoc_files = Dir['*.c']
end
