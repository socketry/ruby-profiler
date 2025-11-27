# Getting Started

This guide explains how to get started with `ruby-profiler`, a profiler state manager for Ruby fibers designed for BPF/eBPF integration.

## Installation

Add the gem to your project:

```bash
$ bundle add ruby-profiler
```

## Core Concepts

`ruby-profiler` provides a mechanism to attach key-value state to Ruby fibers and make it accessible via a thread-local pointer that can be read by BPF programs. The state is automatically synchronized across fiber switches.

The gem has one main concept:

- **{ruby Ruby::Profiler::State}**: Represents a collection of key-value pairs that can be attached to a fiber. The state is immutable - once created, you cannot modify it. Use the `with` method to create new states with updated values.

## Usage

### Creating and Applying State

When profiling Ruby applications, you often need to attach contextual information (like request IDs, user IDs, or operation names) to fibers so that BPF programs can correlate events with application context.

```ruby
require "ruby/profiler"

# Create a state with key-value pairs:
state = Ruby::Profiler::State.new(
	request_id: "abc123",
	user_id: 42,
	endpoint: "/api/users"
)

# Apply the state to the current fiber:
state.apply!

# The state is now accessible via the thread-local pointer
# `ruby_profiler_state` which can be read by BPF programs
```

### Automatic Fiber Switch Tracking

Fiber switch tracking is automatically enabled when the extension loads. Whenever a fiber switch occurs, the thread-local pointer is automatically updated to point to the state stored in the current fiber's instance variables.

You don't need to manually enable or configure anything - it just works:

```ruby
require "ruby/profiler"

# In your application code:
Fiber.new do
	state = Ruby::Profiler::State.new(request_id: "req-123")
	state.apply!
	
	# Do work...
	# The state pointer is automatically updated on fiber switches
end.resume
```

### Creating Updated States

Since states are immutable, use the `with` method to create new states with updated values:

```ruby
# Create initial state:
state = Ruby::Profiler::State.new(request_id: "req-1", user_id: 1)

# Create updated state with new user_id:
updated_state = state.with(user_id: 2)

# Original state is unchanged:
state.size        # => 2
updated_state.size # => 2

# You can add new keys:
extended_state = state.with(action: "update", timestamp: Time.now.to_i)
extended_state.size # => 4
```
