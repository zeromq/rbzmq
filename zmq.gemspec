Gem::Specification.new do |s|
  s.name = %q{zmq}
  s.version = '2.0.7'
  s.date = %q{2010-06-04}
  s.summary = 'Ruby 0MQ bindings'
  s.extensions = 'extconf.rb'
  s.files = Dir['Makefile'] + Dir['*.c']
  s.has_rdoc = true
  s.extra_rdoc_files = Dir['*.c']
end
