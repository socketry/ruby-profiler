# BPF Integration Guide

This guide explains how to integrate `ruby-profiler` with BPF/eBPF programs to read profiler state from Ruby fibers.

## Overview

`ruby-profiler` exposes a thread-local pointer `ruby_profiler_state` that BPF programs can read to access the current fiber's state. This allows BPF programs to correlate system-level events (like syscalls, network I/O, or CPU samples) with application-level context (like request IDs, user IDs, or operation names).

## Why BPF Integration?

When profiling Ruby applications, you need to correlate low-level system events with high-level application context. BPF programs run in the kernel and can observe system calls, network traffic, and CPU usage, but they don't have direct access to Ruby application state.

`ruby-profiler` bridges this gap by:

- **Exposing state via thread-local pointer**: BPF programs can read the pointer directly from memory
- **Automatic synchronization**: State is automatically updated on fiber switches
- **BPF-friendly data structure**: Simple, flat structure that's easy to read from BPF

## State Structure

The state structure is designed to be BPF-friendly and is considered a public interface:

```c
struct Ruby_Profiler_Pair {
	ID key;      // Ruby ID (unsigned long)
	VALUE value; // Ruby VALUE (unsigned long)
};

struct Ruby_Profiler_State {
	size_t size;           // Number of active pairs
	size_t capacity;       // Total slots (power of 2)
	struct Ruby_Profiler_Pair pairs[]; // Array of pairs
};
```

### Important Notes

- **Public interface**: This structure is considered a public interface for BPF programs. Changes will be avoided unless absolutely necessary.
- **Empty slots**: Pairs with `key == 0` are empty slots (ID 0 is invalid in Ruby).
- **Enumeration**: Iterate through `capacity` slots and skip empty ones (`key != 0`).
- **Power of 2 capacity**: Capacity is always a power of 2 for efficient hashing.
- **Hash function**: `key & (capacity - 1)` computes the initial index (bitwise AND is faster than modulo).
- **Linear probing**: If the initial slot is occupied, check subsequent slots: `(idx + i) & (capacity - 1)`.

## Accessing State from BPF

### Thread-Local Pointer

The state is accessible via a thread-local pointer:

```c
// Thread-local pointer (public symbol for BPF access)
extern _Thread_local struct Ruby_Profiler_State *ruby_profiler_state;
```

### Basic BPF Program Example

Here's a simple BPF program that reads the state:

```c
#include <bpf/bpf_helpers.h>
#include <linux/ptrace.h>

// Define the state structure (must match ruby-profiler's state.h)
struct Ruby_Profiler_Pair {
	unsigned long key;
	unsigned long value;
};

struct Ruby_Profiler_State {
	unsigned long size;
	unsigned long capacity;
	struct Ruby_Profiler_Pair pairs[];
};

// Thread-local pointer (in BPF, accessed via thread-local storage)
struct Ruby_Profiler_State *ruby_profiler_state;

SEC("uprobe/ruby")
int read_profiler_state(struct pt_regs *ctx) {
	struct Ruby_Profiler_State *state = ruby_profiler_state;
	
	if (!state) {
		// No state attached to current fiber
		return 0;
	}
	
	// Enumerate pairs (iterate through capacity, skip empty slots):
	for (unsigned long i = 0; i < state->capacity; i++) {
		if (state->pairs[i].key != 0) {
			// Found a valid pair:
			unsigned long key = state->pairs[i].key;
			unsigned long value = state->pairs[i].value;
			
			// Process key-value pair...
			// Note: VALUE is a Ruby object pointer - you'll need
			// additional BPF programs to dereference Ruby objects
		}
	}
	
	return 0;
}
```

### Efficient Key Lookup Using Hash Function

For efficient lookups, use the hash function to compute the initial index instead of scanning from the beginning:

```c
// Helper function to find a value by key using hash table lookup:
static inline unsigned long lookup_value(struct Ruby_Profiler_State *state, unsigned long target_key) {
	if (!state || target_key == 0 || state->capacity == 0) {
		return 0;
	}
	
	// Compute initial hash index: key & (capacity - 1)
	// Since capacity is a power of 2, bitwise AND is equivalent to modulo:
	unsigned long mask = state->capacity - 1;
	unsigned long idx = target_key & mask;
	
	// Linear probing: start at hash index and scan forward:
	for (unsigned long i = 0; i < state->capacity; i++) {
		unsigned long pos = (idx + i) & mask;  // Wrap around using mask
		
		if (state->pairs[pos].key == target_key) {
			return state->pairs[pos].value;
		}
		
		// Empty slot means key not found (early termination):
		if (state->pairs[pos].key == 0) {
			break;
		}
	}
	
	return 0; // Not found
}
```

### Reading Ruby IDs

Ruby IDs are unsigned integers that represent symbols. To convert an ID to a string, you'll need to:

1. Read the ID from the state.
2. Use a BPF map or userspace helper to resolve the ID to a symbol name.
3. Or use Ruby's internal symbol table (requires additional BPF programs).

Example of storing IDs in a BPF map:

```c
// BPF map to store ID -> string mappings
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, unsigned long);    // Ruby ID
	__type(value, char[64]);        // Symbol name
} symbol_map SEC(".maps");

SEC("uprobe/ruby")
int track_request_id(struct pt_regs *ctx) {
	struct Ruby_Profiler_State *state = ruby_profiler_state;
	
	if (!state) {
		return 0;
	}
	
	// Look for request_id key (you'll need to know the ID value):
	unsigned long request_id_key = 12345; // Example ID
	unsigned long request_id_value = 0;
	
	// Compute initial hash index using bitwise AND (capacity is power of 2):
	unsigned long mask = state->capacity - 1;
	unsigned long idx = request_id_key & mask;
	
	// Linear probing: start at hash index and scan forward:
	for (unsigned long i = 0; i < state->capacity; i++) {
		unsigned long pos = (idx + i) & mask;  // Wrap around using mask
		
		if (state->pairs[pos].key == request_id_key) {
			request_id_value = state->pairs[pos].value;
			break;
		}
		
		// Empty slot means key not found (early termination):
		if (state->pairs[pos].key == 0) {
			break;
		}
	}
	
	if (request_id_value) {
		// Store in BPF map for correlation:
		bpf_map_update_elem(&events_map, &request_id_value, &event_data, BPF_ANY);
	}
	
	return 0;
}
```
