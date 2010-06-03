Gem::Specification.new do |s|
  s.name = %q{zmq}
  s.version = '2.0.6'
  s.date = %q{2010-06-03}
  s.summary = 'Ruby 0MQ bindings'
  s.extensions = 'extconf.rb'
  s.files = Dir['Makefile'] + Dir['*.cpp']
end
