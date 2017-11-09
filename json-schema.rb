#!/usr/bin/env ruby
$VERBOSE = true

require 'json'
require 'json-schema'
require 'ipaddr'
require 'pp'

if ARGV.size != 2
  puts "Usage: #{__FILE__} <schema.json> <file.json>"
  exit(1)
end
schema = JSON.parse(File.read(ARGV[0]))
file = JSON.parse(File.read(ARGV[1]))

format_ip = lambda do |value|
  addr = IPAddr.new(value) rescue false
  return if addr
  raise JSON::Schema::CustomFormatError, 'must be a valid IP address'
end

format_ifname = lambda do |value|
  error = nil
  if value.length < 1 then error = 'was not of a minimum string length of 1'
  elsif value.length > 16 then error = 'was not of a maximum string length of 16'
  elsif value !~ /^[^ ]+$/ then error = 'must not contain spaces'
  end
  return unless error
  raise JSON::Schema::CustomFormatError, error
end

JSON::Validator.schema_reader = JSON::Schema::Reader.new(accept_ur: false,
                                                         accept_file: true)
JSON::Validator.register_format_validator('ip', format_ip)
JSON::Validator.register_format_validator('interface-name', format_ifname)

begin
  ret = JSON::Validator.fully_validate(schema, file, errors_as_objects: true)
rescue JSON::Schema::ValidationError => e
  e.message
end

pp ret
