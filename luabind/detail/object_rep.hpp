// Copyright (c) 2003 Daniel Wallin and Arvid Norberg

// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
// OR OTHER DEALINGS IN THE SOFTWARE.


#ifndef LUABIND_OBJECT_REP_HPP_INCLUDED
#define LUABIND_OBJECT_REP_HPP_INCLUDED

#include <luabind/config.hpp>
#include <luabind/detail/class_rep.hpp>
#include <luabind/detail/instance_holder.hpp>
#include <luabind/detail/ref.hpp>
#include <type_traits>	// std::aligned_storage
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace luabind {
	namespace detail {

		void finalize(lua_State* L, class_rep* crep);

		// this class is allocated inside lua for each pointer.
		// it contains the actual c++ object-pointer.
		// it also tells if it is const or not.
		class LUABIND_API object_rep
		{
		public:
			object_rep(instance_holder* instance, class_rep* crep);
			~object_rep();

			const class_rep* crep() const { return m_classrep; }
			class_rep* crep() { return m_classrep; }

			void set_instance(instance_holder* instance) { m_instance = instance; }

			void add_dependency(lua_State* L, int index);

			std::pair<void*, int> get_instance(class_id target) const
			{
				if(m_instance == 0)
					return std::pair<void*, int>(nullptr, -1);
				return m_instance->get(m_classrep->casts(), target);
			}

			bool is_const() const
			{
				return m_instance && m_instance->pointee_const();
			}

			void release()
			{
				if(m_instance)
					m_instance->release();
			}

			void* allocate(std::size_t size)
			{
				if(size <= 32) {
					return &m_instance_buffer;
				}
				else {
					return std::malloc(size);
				}

			}

			void deallocate(void* storage)
			{
				if(storage == &m_instance_buffer) {
					return;
				}
				else if(is_trailing_storage(storage)) {
					return; // GlobalWar: freed with the userdata itself
				}
				else {
					std::free(storage);
				}
			}

			// GlobalWar (lua_wrapper_nogc, see push_new_pointer_instance): the holder of a non-owning instance made
			// without __gc lives in the userdata itself, right after the object_rep (never malloc'ed: nothing to free).
			// m_instance_buffer is unused then and keeps this as the tag, so deallocate() never takes for it a
			// malloc'ed holder that happens to start at the same address; the constructor clears the tag.
			void* use_trailing_storage()
			{
				object_rep* const self = this;
				std::memcpy(&m_instance_buffer, &self, sizeof(self));
				return this + 1;
			}

			bool is_trailing_storage(void const* storage) const
			{
				if(storage != static_cast<void const*>(this + 1))
					return false;
				object_rep const* tag;
				std::memcpy(&tag, &m_instance_buffer, sizeof(tag));
				return tag == this;
			}

		private:
			object_rep(object_rep const&) = delete;
			void operator=(object_rep const&) = delete;

			instance_holder* m_instance;
			std::aligned_storage<32>::type m_instance_buffer;
			class_rep* m_classrep; // the class information about this object's type
			detail::lua_reference m_dependency_ref; // reference to lua table holding dependency references
		};

		template<class T>
		struct delete_s
		{
			static void apply(void* ptr)
			{
				luabind_delete(static_cast<T*>(ptr));
			}
		};

		template<class T>
		struct destruct_only_s
		{
			static void apply(void* ptr)
			{
				// Removes unreferenced formal parameter warning on VC7.
				(void)ptr;
#ifndef NDEBUG
				int completeness_check[sizeof(T)];
				(void)completeness_check;
#endif
				static_cast<T*>(ptr)->~T();
			}
		};

		LUABIND_API object_rep* get_instance(lua_State* L, int index);
		LUABIND_API void push_instance_metatable(lua_State* L);
		LUABIND_API object_rep* push_new_instance(lua_State* L, class_rep* cls);

		// GlobalWar, diagnostics only (xrGame lua_udata_profile): called by push_new_instance() after it created the
		// userdata of an instance of cls (on the top of the stack of L, its holder not set yet) - every class instance
		// luabind makes: a C++ object pushed by pointer or by value, a constructor called from Lua. Must not raise a
		// Lua error nor throw. nullptr (default) = off: push_new_instance pays one load and a branch.
		using instance_create_hook = void (*)(lua_State* L, class_rep* cls);
		LUABIND_API void set_instance_create_hook(instance_create_hook hook);

		// GlobalWar (plans/optimization_mt/32-frame-spikes.md, xrGame cvar lua_wrapper_nogc): instances of C++ classes
		// pushed by a raw pointer (pointer_holder<T*>: game_object, alife(), device(), a reference returned by a method)
		// own nothing - their __gc (destroy_instance) only runs an empty holder destructor and frees the holder. On,
		// make_pointer_instance makes them with the holder inside the userdata and a twin of the instance metatable
		// without __gc: LuaJIT then frees them in the cycle they die in instead of queueing them for a finalizer (the
		// mmudata list of the atomic phase) and freeing them in the next one. Off (default): as before. Only the
		// instances made after the switch are affected; any thread (atomic).
		LUABIND_API void set_nonowning_instance_nogc(bool enable);
		// GlobalWar: how many instances were made without __gc since the start (a running total for the metrics)
		LUABIND_API std::uint64_t nonowning_instance_nogc_created();
		// GlobalWar: push_new_instance for make_pointer_instance over a raw pointer. storage receives where to construct
		// a holder of holder_size bytes (inside the userdata when made without __gc; object_rep::allocate otherwise).
		LUABIND_API object_rep* push_new_pointer_instance(lua_State* L, class_rep* cls, std::size_t holder_size, void*& storage);
		// GlobalWar: the instance at index gets back the instance metatable with __gc if it was made without one. For
		// dependency_policy: the dependency table of an object_rep is released by its __gc (~lua_reference).
		LUABIND_API void restore_instance_finalizer(lua_State* L, int index);
		// GlobalWar: integer keys (key 1 is the instance tag, see get_instance): the instance metatable keeps its twin
		// without __gc (made with the first instance without __gc in the Lua state), the twin keeps the instance metatable
		constexpr int instance_metatable_nogc_twin_key = 2;
		constexpr int instance_metatable_gc_original_key = 3;

	}	// namespace detail

}	// namespace luabind

#endif // LUABIND_OBJECT_REP_HPP_INCLUDED

