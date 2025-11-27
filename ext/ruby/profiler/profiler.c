// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "profiler.h"
#include "state.h"

#include <ruby/debug.h>

#ifndef HAVE_RB_FIBER_CURRENT
static ID id_current;

static VALUE Ruby_Profiler_Fiber_current(void) {
	return rb_funcall(rb_cFiber, id_current, 0);
}
#else
static VALUE Ruby_Profiler_Fiber_current(void) {
	return rb_fiber_current();
}
#endif

// Fiber switch callback - updates thread-local pointer based on fiber-local storage
static void Ruby_Profiler_fiber_switch_callback(rb_event_flag_t event_flag, VALUE data, VALUE self, ID id, VALUE klass) {
	VALUE fiber = Ruby_Profiler_Fiber_current();
	
	struct Ruby_Profiler_State *state = Ruby_Profiler_State_for(fiber);
	
	// Update thread-local pointer
	ruby_profiler_state = state;
}

void Init_Ruby_Profiler(void)
{
#ifdef HAVE_RB_EXT_RACTOR_SAFE
	rb_ext_ractor_safe(true);
#endif

#ifndef HAVE_RB_FIBER_CURRENT
	id_current = rb_intern("current");
#endif
	
	// Get or create Ruby module:
	VALUE Ruby;
	if (rb_const_defined(rb_cObject, rb_intern("Ruby"))) {
		Ruby = rb_const_get(rb_cObject, rb_intern("Ruby"));
	} else {
		Ruby = rb_define_module("Ruby");
	}
	
	VALUE Ruby_Profiler = rb_define_module_under(Ruby, "Profiler");
	
	Init_Ruby_Profiler_State(Ruby_Profiler);
	
	// Register fiber switch event hook automatically:
	// This updates the thread-local pointer whenever a fiber switch occurs.
	rb_add_event_hook(
		Ruby_Profiler_fiber_switch_callback,
		RUBY_EVENT_FIBER_SWITCH,
		Qnil  // No data needed, callback is stateless.
	);
	
	// Also update state immediately for current fiber:
	VALUE fiber = Ruby_Profiler_Fiber_current();
	struct Ruby_Profiler_State *state = Ruby_Profiler_State_for(fiber);
	ruby_profiler_state = state;
}

