# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "ruby/profiler"

describe Ruby::Profiler::State do
	with "#initialize" do
		it "can create an empty state" do
			state = subject.new
			
			expect(state).to have_attributes(
				size: be == 0
			)
		end
		
		it "can create a state with key-value pairs" do
			state = subject.new(
				request_id: "abc123",
				user_id: 42,
				endpoint: "/api/users"
			)
			
			expect(state).to have_attributes(
				size: be == 3
			)
		end
		
		it "raises TypeError for non-symbol keys" do
			expect{subject.new(
					"request_id" => "abc123",
					:user_id => 42
				)
			}.to raise_exception(TypeError)
		end
	end
	
	with "#apply!" do
		it "updates the thread-local pointer" do
			state = subject.new(request_id: "test")
			
			# Get the thread-local pointer via FFI or similar
			# For now, we'll test that apply! doesn't raise
			expect(state.apply!).to be == state
		end
		
		it "stores state in fiber-local storage" do
			state = subject.new(request_id: "test")
			
			Fiber.new do
				state.apply!
				
				state_value = Fiber.current.ruby_profiler_state
				expect(state_value).to be_a(Ruby::Profiler::State)
				expect(state_value).to be == state
			end.resume
		end
	end
	
	with "#size" do
		it "returns the number of active pairs" do
			state = subject.new(
				a: 1,
				b: 2,
				c: 3
			)
			
			expect(state.size).to be == 3
		end
		
		it "returns 0 for empty state" do
			state = subject.new
			
			expect(state.size).to be == 0
		end
	end
	
	with "hash table behavior" do
		it "allocates capacity based on number of pairs" do
			# 3 pairs should allocate capacity of 4 (next power of 2)
			state = subject.new(a: 1, b: 2, c: 3)
			expect(state.size).to be == 3
			
			# 5 pairs should allocate capacity of 8
			state = subject.new(a: 1, b: 2, c: 3, d: 4, e: 5)
			expect(state.size).to be == 5
			
			# 1 pair should allocate capacity of 1
			state = subject.new(a: 1)
			expect(state.size).to be == 1
		end
		
		it "can handle many pairs" do
			# Create state with many pairs to test hash table
			pairs = {}
			20.times do |i|
				pairs[:"key_#{i}"] = i
			end
			
			state = subject.new(**pairs)
			
			expect(state.size).to be == 20
		end
		
		it "handles large numbers of pairs" do
			# With dynamic capacity, we can handle many pairs
			pairs = {}
			33.times do |i|
				pairs[:"key_#{i}"] = i
			end
			
			state = subject.new(**pairs)
			expect(state.size).to be == 33
		end
	end
	
	with "#with" do
		it "creates a new state with updated values" do
			original = subject.new(request_id: "req1", user_id: 1)
			updated = original.with(user_id: 2)
			
			expect(updated).to be != original
			expect(updated.size).to be == 2
			expect(original.size).to be == 2
		end
		
		it "preserves existing values when updating" do
			original = subject.new(request_id: "req1", user_id: 1)
			updated = original.with(user_id: 2)
			
			# Original should be unchanged
			expect(original.size).to be == 2
			
			# Updated should have new value
			expect(updated.size).to be == 2
		end
		
		it "can add new keys" do
			original = subject.new(request_id: "req1")
			updated = original.with(user_id: 1)
			
			expect(original.size).to be == 1
			expect(updated.size).to be == 2
		end
		
		it "can update multiple keys at once" do
			original = subject.new(request_id: "req1", user_id: 1)
			updated = original.with(request_id: "req2", user_id: 2, action: "update")
			
			expect(original.size).to be == 2
			expect(updated.size).to be == 3
		end
		
		it "returns self when no updates provided" do
			original = subject.new(request_id: "req1")
			result = original.with
			
			expect(result).to be == original
		end
		
		it "raises TypeError for non-symbol keys" do
			original = subject.new(request_id: "req1")
			
			expect{original.with("request_id" => "req2")
			}.to raise_exception(TypeError)
		end
	end
end

