// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#pragma once

#include <ruby.h>

struct Ruby_Profiler_Pair {
	// ID 0 indicates empty slot:
	ID key;
	VALUE value;
};

// This state is considered a public interface for BPF programs to read. Therefore, we will endeavour not to change it without an extremely good reason.
struct Ruby_Profiler_State {
	// Number of active pairs:
	size_t size;

	// Total slots (must be power of 2 for efficient hashing):
	size_t capacity;
	
	// Array of pairs:
	struct Ruby_Profiler_Pair pairs[];
};

// Hash Table Design:
//
// For small hash tables (< 16 items) with integer keys like your Ruby profiler,
// hash + linear probing at 100% load factor is optimal: computing key % capacity
// (especially with power-of-2 capacity using bitwise AND) is essentially free, and
// even in the worst case where you scan all slots, you're no worse off than a pure
// linear scan from index 0, while on average you start closer to your target and
// find items faster—giving you all the benefits of hashing with zero memory overhead
// and no downside, making it strictly better than either pure linear scan or traditional
// linear probing with lower load factors.
//
// Implementation details:
// - Capacity must be a power of 2 (enforced at allocation).
// - Hash function: key & (capacity - 1) (fast bitwise AND).
// - Linear probing: (hash + i) & (capacity - 1) for i = 0, 1, 2, ...
// - Empty slots: key == 0 (ID 0 is invalid in Ruby).
// - BPF-friendly: Can enumerate by iterating capacity slots and skipping empty ones.

// Thread-local pointer to current state (public symbol for BPF access)
extern _Thread_local struct Ruby_Profiler_State *ruby_profiler_state;

// Typed data type (defined in state.c)
extern const rb_data_type_t Ruby_Profiler_State_Type;

// Cached ID for @ruby_profiler_state instance variable (defined in state.c)
extern ID id_ruby_profiler_state;

// Get state for fiber from fiber-local storage
struct Ruby_Profiler_State *Ruby_Profiler_State_for(VALUE fiber);

void Init_Ruby_Profiler_State(VALUE Ruby_Profiler);
