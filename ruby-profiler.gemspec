# frozen_string_literal: true

require_relative "lib/ruby/profiler/version"

Gem::Specification.new do |spec|
	spec.name = "ruby-profiler"
	spec.version = Ruby::Profiler::VERSION
	
	spec.summary = "A profiler state manager for Ruby fibers."
	spec.authors = ["Samuel Williams"]
	spec.license = "MIT"
	
	spec.homepage = "https://github.com/socketry/ruby-profiler"
	
	spec.metadata = {
		"documentation_uri" => "https://socketry.github.io/ruby-profiler/",
		"source_code_uri" => "https://github.com/socketry/ruby-profiler.git",
	}
	
	spec.files = Dir["{ext,lib}/**/*", "*.md", base: __dir__]
	spec.require_paths = ["lib"]
	
	spec.extensions = ["ext/extconf.rb"]
	
	spec.required_ruby_version = ">= 3.2"
end
