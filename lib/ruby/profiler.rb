# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require_relative "profiler/version"
require_relative "profiler/native"

# Define fiber-local accessor for profiler state:
Fiber.attr_accessor :ruby_profiler_state
