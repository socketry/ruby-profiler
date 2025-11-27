// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "profiler.h"
#include "state.h"

#include <ruby/internal/core/rhash.h>
#include <stdlib.h>
#include <string.h>

// Thread-local pointer to current state (public symbol for BPF access)
_Thread_local struct Ruby_Profiler_State *ruby_profiler_state = NULL;

VALUE Ruby_Profiler_State = Qnil;

// Cached ID for @ruby_profiler_state instance variable
ID id_ruby_profiler_state;

static void Ruby_Profiler_State_mark(void *ptr) {
	struct Ruby_Profiler_State *state = (struct Ruby_Profiler_State*)ptr;
	
	// Handle NULL (deferred allocation)
	if (!state) {
		return;
	}
	
	// Mark all VALUEs in pairs (iterate through capacity to find all non-empty slots)
	for (size_t i = 0; i < state->capacity; i++) {
		if (state->pairs[i].key != 0) {
			rb_gc_mark_movable(state->pairs[i].value);
		}
	}
}

static void Ruby_Profiler_State_compact(void *ptr) {
	struct Ruby_Profiler_State *state = (struct Ruby_Profiler_State*)ptr;
	
	// Handle NULL (deferred allocation)
	if (!state) {
		return;
	}
	
	// Update all VALUE locations after GC compaction (iterate through capacity)
	for (size_t i = 0; i < state->capacity; i++) {
		if (state->pairs[i].key != 0) {
			state->pairs[i].value = rb_gc_location(state->pairs[i].value);
		}
	}
}

static void Ruby_Profiler_State_free(void *ptr) {
	struct Ruby_Profiler_State *state = (struct Ruby_Profiler_State*)ptr;
	
	// Handle NULL (deferred allocation):
	if (!state) {
		return;
	}
	
	// If this state is currently active, clear the thread-local pointer
	if (ruby_profiler_state == state) {
		ruby_profiler_state = NULL;
	}
	
	free(state);
}

static size_t Ruby_Profiler_State_memsize(const void *ptr) {
	const struct Ruby_Profiler_State *state = (const struct Ruby_Profiler_State*)ptr;
	
	// Handle NULL (deferred allocation)
	if (!state) {
		return 0;
	}
	
	return sizeof(*state) + (state->capacity * sizeof(struct Ruby_Profiler_Pair));
}

const rb_data_type_t Ruby_Profiler_State_Type = {
	.wrap_struct_name = "Ruby::Profiler::State",
	.function = {
		.dmark = Ruby_Profiler_State_mark,
		.dcompact = Ruby_Profiler_State_compact,
		.dfree = Ruby_Profiler_State_free,
		.dsize = Ruby_Profiler_State_memsize,
	},
	.flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED,
};

struct Ruby_Profiler_State *Ruby_Profiler_State_get(VALUE self) {
	struct Ruby_Profiler_State *state;
	TypedData_Get_Struct(self, struct Ruby_Profiler_State, &Ruby_Profiler_State_Type, state);
	return state;
}

// Round up to next power of 2
static size_t round_capacity_to_power_of_2(size_t capacity) {
	if (capacity == 0) return 1;
	capacity--;
	capacity |= capacity >> 1;
	capacity |= capacity >> 2;
	capacity |= capacity >> 4;
	capacity |= capacity >> 8;
	capacity |= capacity >> 16;
#if SIZEOF_SIZE_T == 8
	capacity |= capacity >> 32;
#endif
	return capacity + 1;
}

static VALUE Ruby_Profiler_State_allocate(VALUE klass) {
	// Defer allocation until initialize when we know the required capacity
	return TypedData_Wrap_Struct(klass, &Ruby_Profiler_State_Type, NULL);
}

// Find a pair by key using hash table lookup with linear probing
static struct Ruby_Profiler_Pair *Ruby_Profiler_State_find_pair(struct Ruby_Profiler_State *state, ID key) {
	if (key == 0 || state->capacity == 0) {
		return NULL;
	}
	
	size_t mask = state->capacity - 1;  // Assumes power of 2
	size_t idx = (size_t)key & mask;
	
	for (size_t i = 0; i < state->capacity; i++) {
		size_t pos = (idx + i) & mask;
		
		if (state->pairs[pos].key == key) {
			return &state->pairs[pos];
		}
		if (state->pairs[pos].key == 0) {
			return NULL;  // Empty slot means not found
		}
	}
	
	return NULL;  // Table full, key not found
}

// Insert or update a pair using hash table with linear probing
static int Ruby_Profiler_State_insert_pair(struct Ruby_Profiler_State *state, ID key, VALUE value) {
	if (key == 0) {
		return 0;  // Invalid key
	}
	
	size_t mask = state->capacity - 1;  // Assumes power of 2
	size_t idx = (size_t)key & mask;
	
	// First, check if key already exists (update case)
	for (size_t i = 0; i < state->capacity; i++) {
		size_t pos = (idx + i) & mask;
		
		if (state->pairs[pos].key == key) {
			// Update existing pair (doesn't require capacity check)
			state->pairs[pos].value = value;
			return 1;
		}
		if (state->pairs[pos].key == 0) {
			// Found empty slot, check capacity before inserting
			if (state->size >= state->capacity) {
				return 0;  // Table full
			}
			// Insert here
			state->pairs[pos].key = key;
			state->pairs[pos].value = value;
			state->size++;
			return 1;
		}
	}
	
	return 0;  // Table full (no empty slot found)
}

// Callback for rb_hash_foreach to insert pairs into state
static int Ruby_Profiler_State_foreach_insert(VALUE key, VALUE value, VALUE data) {
	struct Ruby_Profiler_State *state = (struct Ruby_Profiler_State*)data;
	
	// Keys must be symbols - raise TypeError if not
	if (!RB_TYPE_P(key, T_SYMBOL)) {
		rb_raise(rb_eTypeError, "State keys must be symbols, got %s", rb_obj_classname(key));
	}
	
	ID id = rb_sym2id(key);
	
	// Insert using hash table (will update if key exists, insert if new)
	if (!Ruby_Profiler_State_insert_pair(state, id, value)) {
		rb_raise(rb_eArgError, "State capacity exceeded (%zu pairs)!", state->capacity);
	}
	
	return ST_CONTINUE;
}

// Helper struct for counting new keys
struct Ruby_Profiler_State_CountData {
	struct Ruby_Profiler_State *old_state;
	size_t new_count;
};

// Callback for rb_hash_foreach to count new keys (keys not in old_state)
static int Ruby_Profiler_State_foreach_count_new(VALUE key, VALUE value, VALUE data) {
	struct Ruby_Profiler_State_CountData *count_data = (struct Ruby_Profiler_State_CountData*)data;
	
	// Keys must be symbols
	if (!RB_TYPE_P(key, T_SYMBOL)) {
		return ST_CONTINUE;  // Skip non-symbols, will be caught during insert
	}
	
	ID id = rb_sym2id(key);
	
	// Count if key doesn't exist in old_state
	if (!count_data->old_state || !Ruby_Profiler_State_find_pair(count_data->old_state, id)) {
		count_data->new_count++;
	}
	
	return ST_CONTINUE;
}

static VALUE Ruby_Profiler_State_initialize(int argc, VALUE *argv, VALUE self) {
	struct Ruby_Profiler_State *state;
	TypedData_Get_Struct(self, struct Ruby_Profiler_State, &Ruby_Profiler_State_Type, state);
	
	if (state) {
		rb_raise(rb_eRuntimeError, "State already initialized!");
	}
	
	VALUE options = Qnil;
	rb_scan_args(argc, argv, ":", &options);
	
	// Determine required capacity based on number of pairs
	size_t required_capacity = 0;
	
	if (!RB_NIL_P(options)) {
		// Get hash size directly without allocating temporary array
		size_t keys_count = RHASH_SIZE(options);
		
		// Calculate required capacity (next power of 2)
		required_capacity = round_capacity_to_power_of_2(keys_count);
	} else {
		return self;
	}
	
	// Allocate state with correct capacity (or reallocate if already allocated)
	size_t size = sizeof(struct Ruby_Profiler_State) + (required_capacity * sizeof(struct Ruby_Profiler_Pair));
	state = (struct Ruby_Profiler_State*)calloc(1, size);
	
	if (!state) {
		rb_raise(rb_eNoMemError, "Failed to allocate state!");
	}
	
	// Initialize state:
	state->size = 0;
	state->capacity = required_capacity;
	
	// Update TypedData pointer:
	DATA_PTR(self) = state;

	// Now insert all pairs using rb_hash_foreach (more efficient than allocating keys array):
	rb_hash_foreach(options, Ruby_Profiler_State_foreach_insert, (VALUE)state);
	
	return self;
}

static VALUE Ruby_Profiler_State_apply(VALUE self) {
	struct Ruby_Profiler_State *state = Ruby_Profiler_State_get(self);
	
	// Update the thread-local pointer (NULL if state not initialized)
	ruby_profiler_state = state;
	
	// Store state in fiber-local storage using Fiber#ruby_profiler_state=
	// This is fiber-local storage that persists across fiber switches
	VALUE fiber;

#ifdef HAVE_RB_FIBER_CURRENT
	fiber = rb_fiber_current();
#else
	fiber = rb_funcall(rb_cFiber, rb_intern("current"), 0);
#endif
	
	rb_ivar_set(fiber, id_ruby_profiler_state, self);
	
	return self;
}

static VALUE Ruby_Profiler_State_size(VALUE self) {
	struct Ruby_Profiler_State *state = Ruby_Profiler_State_get(self);
	
	if (!state) {
		return SIZET2NUM(0);  // Uninitialized state has size 0
	}
	
	return SIZET2NUM(state->size);
}

static VALUE Ruby_Profiler_State_with(int argc, VALUE *argv, VALUE self) {
	struct Ruby_Profiler_State *old_state;
	TypedData_Get_Struct(self, struct Ruby_Profiler_State, &Ruby_Profiler_State_Type, old_state);
	
	VALUE options = Qnil;
	rb_scan_args(argc, argv, ":", &options);
	
	if (RB_NIL_P(options)) {
		// No updates, return self:
		return self;
	}
	
	// Count how many keys in options are NOT in old_state (new keys)
	size_t old_size = old_state ? old_state->size : 0;
	struct Ruby_Profiler_State_CountData count_data = {old_state, 0};
	
	rb_hash_foreach(options, Ruby_Profiler_State_foreach_count_new, (VALUE)&count_data);
	
	size_t required_capacity = round_capacity_to_power_of_2(old_size + count_data.new_count);
	
	// Allocate a new state with the required capacity
	VALUE klass = rb_obj_class(self);
	VALUE new_state_value = Ruby_Profiler_State_allocate(klass);
	
	// Allocate the new state struct
	size_t size = sizeof(struct Ruby_Profiler_State) + (required_capacity * sizeof(struct Ruby_Profiler_Pair));
	struct Ruby_Profiler_State *new_state = (struct Ruby_Profiler_State*)calloc(1, size);
	
	if (!new_state) {
		rb_raise(rb_eNoMemError, "Failed to allocate Ruby_Profiler_State");
	}
	
	new_state->size = 0;
	new_state->capacity = required_capacity;
	DATA_PTR(new_state_value) = new_state;
	
	// Copy all existing pairs from old_state to new_state (if old_state exists)
	if (old_state) {
		for (size_t i = 0; i < old_state->capacity; i++) {
			if (old_state->pairs[i].key != 0) {
				if (!Ruby_Profiler_State_insert_pair(new_state, old_state->pairs[i].key, old_state->pairs[i].value)) {
					rb_raise(rb_eArgError, "State capacity exceeded while copying state");
				}
			}
		}
	}
	
	// Apply updates from options hash using rb_hash_foreach
	rb_hash_foreach(options, Ruby_Profiler_State_foreach_insert, (VALUE)new_state);
	
	return new_state_value;
}

// Get state for fiber from fiber-local storage
struct Ruby_Profiler_State *Ruby_Profiler_State_for(VALUE fiber) {
	VALUE state_value = rb_ivar_get(fiber, id_ruby_profiler_state);
	
	if (RB_NIL_P(state_value)) {
		return NULL;
	}
	
	// Extract the state pointer from the VALUE
	struct Ruby_Profiler_State *state;
	if (!rb_typeddata_is_kind_of(state_value, &Ruby_Profiler_State_Type)) {
		return NULL;
	}
	
	TypedData_Get_Struct(state_value, struct Ruby_Profiler_State, &Ruby_Profiler_State_Type, state);
	return state;
}

void Init_Ruby_Profiler_State(VALUE Ruby_Profiler) {
	Ruby_Profiler_State = rb_define_class_under(Ruby_Profiler, "State", rb_cObject);
	rb_define_alloc_func(Ruby_Profiler_State, Ruby_Profiler_State_allocate);
	
	// Cache the ID for @ruby_profiler_state instance variable
	id_ruby_profiler_state = rb_intern("@ruby_profiler_state");
	
	rb_define_method(Ruby_Profiler_State, "initialize", Ruby_Profiler_State_initialize, -1);
	rb_define_method(Ruby_Profiler_State, "apply!", Ruby_Profiler_State_apply, 0);
	rb_define_method(Ruby_Profiler_State, "with", Ruby_Profiler_State_with, -1);
	rb_define_method(Ruby_Profiler_State, "size", Ruby_Profiler_State_size, 0);
}

