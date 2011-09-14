Gem::Specification.new do |s|
  s.name = 'zmq'
  s.version = '2.1.4'
  s.date = '2011-09-14'
  s.authors = ['Martin Sustrik', 'Brian Buchanan']
  s.email = ['sustrik@250bpm.com', 'bwb@holo.org']
  s.description = 'This gem provides a Ruby API for the ZeroMQ messaging library.'
  s.homepage = 'http://www.zeromq.org/bindings:ruby'
  s.summary = 'Ruby API for ZeroMQ'
  s.extensions = 'extconf.rb'
  s.files = Dir['Makefile'] + Dir['*.c']
  s.has_rdoc = true
  s.extra_rdoc_files = Dir['*.c']
end
