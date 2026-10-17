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

#define LUABIND_BUILDING

#include <luabind/detail/object_rep.hpp>
#include <luabind/detail/class_rep.hpp>

#include <atomic>

#if LUA_VERSION_NUM < 502
# define lua_getuservalue lua_getfenv
# define lua_setuservalue lua_setfenv
#endif

namespace luabind {
	namespace detail {

		// dest is a function that is called to delete the c++ object this struct holds
		object_rep::object_rep(instance_holder* instance, class_rep* crep)
			: m_instance(instance)
			, m_classrep(crep)
		{
			// GlobalWar: no tag of use_trailing_storage() (stale memory of an earlier userdata)
			object_rep* const no_tag = nullptr;
			std::memcpy(&m_instance_buffer, &no_tag, sizeof(no_tag));
		}

		object_rep::~object_rep()
		{
			if(!m_instance)
				return;
			m_instance->~instance_holder();
			deallocate(m_instance);
		}

		void object_rep::add_dependency(lua_State* L, int index)
		{
			if(!m_dependency_ref.is_valid())
			{
				lua_newtable(L);
				m_dependency_ref.set(L);
			}
			m_dependency_ref.get(L);

			lua_pushvalue(L, index);
			lua_pushnumber(L, 0);
			lua_rawset(L, -3);
			lua_pop(L, 1);
		}

		int destroy_instance(lua_State* L)
		{
			object_rep* instance = static_cast<object_rep*>(lua_touserdata(L, 1));

			lua_pushstring(L, "__finalize");
			lua_gettable(L, 1);

			if(lua_isnil(L, -1))
			{
				lua_pop(L, 1);
			}
			else
			{
				lua_pushvalue(L, 1);
				lua_call(L, 1, 0);
			}

			instance->~object_rep();

			lua_pushnil(L);
			lua_setmetatable(L, 1);
			return 0;
		}

		namespace
		{

			int set_instance_value(lua_State* L)
			{
				lua_getuservalue(L, 1);
				lua_pushvalue(L, 2);
				lua_rawget(L, -2);

				if(lua_isnil(L, -1) && lua_getmetatable(L, -2))
				{
					lua_pushvalue(L, 2);
					lua_rawget(L, -2);
					lua_replace(L, -3);
					lua_pop(L, 1);
				}

				if(lua_tocfunction(L, -1) == &property_tag)
				{
					// this member is a property, extract the "set" function and call it.
					lua_getupvalue(L, -1, 2);

					if(lua_isnil(L, -1))
					{
						lua_pushfstring(L, "property '%s' is read only", lua_tostring(L, 2));
						lua_error(L);
					}

					lua_pushvalue(L, 1);
					lua_pushvalue(L, 3);
					lua_call(L, 2, 0);
					return 0;
				}

				lua_pop(L, 1);

				if(!lua_getmetatable(L, 4))
				{
					lua_newtable(L);
					lua_pushvalue(L, -1);
					lua_setuservalue(L, 1);
					lua_pushvalue(L, 4);
					lua_setmetatable(L, -2);
				}
				else
				{
					lua_pop(L, 1);
				}

				lua_pushvalue(L, 2);
				lua_pushvalue(L, 3);
				lua_rawset(L, -3);

				return 0;
			}

			int get_instance_value(lua_State* L)
			{
				lua_getuservalue(L, 1);
				lua_pushvalue(L, 2);
				lua_rawget(L, -2);

				if(lua_isnil(L, -1) && lua_getmetatable(L, -2))
				{
					lua_pushvalue(L, 2);
					lua_rawget(L, -2);
				}

				if(lua_tocfunction(L, -1) == &property_tag)
				{
					// this member is a property, extract the "get" function and call it.
					lua_getupvalue(L, -1, 1);
					lua_pushvalue(L, 1);
					lua_call(L, 1, 1);
				}

				return 1;
			}

			int dispatch_operator(lua_State* L)
			{
				for(int i = 0; i < 2; ++i)
				{
					if(get_instance(L, 1 + i))
					{
						int nargs = lua_gettop(L);

						lua_pushvalue(L, lua_upvalueindex(1));
						lua_gettable(L, 1 + i);

						if(lua_isnil(L, -1))
						{
							lua_pop(L, 1);
							continue;
						}

						lua_insert(L, 1); // move the function to the bottom

						nargs = lua_toboolean(L, lua_upvalueindex(2)) ? 1 : nargs;

						if(lua_toboolean(L, lua_upvalueindex(2))) // remove trailing nil
							lua_remove(L, 3);

						lua_call(L, nargs, 1);
						return 1;
					}
				}

				lua_pop(L, lua_gettop(L));
				lua_pushstring(L, "No such operator defined");
				lua_error(L);

				return 0;
			}

		} // namespace unnamed

		LUABIND_API void push_instance_metatable(lua_State* L)
		{
			lua_newtable(L);

			// This is used as a tag to determine if a userdata is a luabind
			// instance. We use a numeric key and a cclosure for fast comparison.
			lua_pushnumber(L, 1);
			lua_pushcclosure(L, get_instance_value, 0);
			lua_rawset(L, -3);

			lua_pushcclosure(L, destroy_instance, 0);
			lua_setfield(L, -2, "__gc");

			lua_pushcclosure(L, get_instance_value, 0);
			lua_setfield(L, -2, "__index");

			lua_pushcclosure(L, set_instance_value, 0);
			lua_setfield(L, -2, "__newindex");

			for(int op = 0; op < number_of_operators; ++op)
			{
				lua_pushstring(L, get_operator_name(op));
				lua_pushvalue(L, -1);
				lua_pushboolean(L, op == op_unm || op == op_len);
				lua_pushcclosure(L, &dispatch_operator, 2);
				lua_settable(L, -3);
			}
		}

		LUABIND_API object_rep* get_instance(lua_State* L, int index)
		{
			object_rep* result = static_cast<object_rep*>(lua_touserdata(L, index));

			if(!result || !lua_getmetatable(L, index))
				return 0;

			lua_rawgeti(L, -1, 1);

			if(lua_tocfunction(L, -1) != &get_instance_value)
				result = 0;

			lua_pop(L, 2);

			return result;
		}

		namespace {
			// GlobalWar: see set_instance_create_hook (object_rep.hpp)
			std::atomic<instance_create_hook> g_instance_create_hook{ nullptr };
		}

		LUABIND_API void set_instance_create_hook(instance_create_hook hook)
		{
			g_instance_create_hook.store(hook, std::memory_order_release);
		}

		LUABIND_API object_rep* push_new_instance(lua_State* L, class_rep* cls)
		{
			void* storage = lua_newuserdata(L, sizeof(object_rep));
			object_rep* result = new (storage) object_rep(0, cls);
			cls->get_table(L);
			lua_setuservalue(L, -2);
			lua_rawgeti(L, LUA_REGISTRYINDEX, cls->metatable_ref());
			lua_setmetatable(L, -2);
			if(const instance_create_hook hook = g_instance_create_hook.load(std::memory_order_acquire))
				hook(L, cls);
			return result;
		}

		namespace {
			// GlobalWar: see set_nonowning_instance_nogc (object_rep.hpp)
			std::atomic<bool> g_nonowning_nogc{ false };
			std::atomic<std::uint64_t> g_nonowning_nogc_created{ 0 };

			// object_rep::allocate keeps a holder of up to this many bytes in m_instance_buffer
			constexpr std::size_t inline_holder_size = 32;

			constexpr int nogc_twin_key = instance_metatable_nogc_twin_key;
			constexpr int gc_original_key = instance_metatable_gc_original_key;

			bool is_gc_key(lua_State* L, int index)
			{
				if(lua_type(L, index) != LUA_TSTRING)
					return false;
				size_t len = 0;
				const char* key = lua_tolstring(L, index, &len);
				return len == 4 && std::memcmp(key, "__gc", 4) == 0;
			}

			// __newindex of the metatable of both the instance metatable and its twin (upvalues: the instance metatable,
			// the twin): a new key set on either goes to both, __gc only to the table it was set on. So the twin keeps
			// what scripts add later, e.g. _g.script getmetatable(game.CTime()).__persist (lmarshal reads it raw).
			// A key that already exists is set raw on that table only, as before (luabind sets none after creation).
			int mirror_new_key(lua_State* L)
			{
				lua_pushvalue(L, 2);
				lua_pushvalue(L, 3);
				lua_rawset(L, 1);
				if(!is_gc_key(L, 2))
				{
					const int other = lua_rawequal(L, 1, lua_upvalueindex(1)) ? lua_upvalueindex(2) : lua_upvalueindex(1);
					lua_pushvalue(L, 2);
					lua_pushvalue(L, 3);
					lua_rawset(L, other);
				}
				return 0;
			}

			// The instance metatable is on the top of the stack: replaces it by its twin without __gc. The twin is made
			// on the first call in a Lua state (all luabind instances share one instance metatable, class_registry):
			// a raw copy of every key but __gc, the same closures (the tag, __index, __newindex, the operators).
			void replace_with_nogc_twin(lua_State* L)
			{
				lua_rawgeti(L, -1, nogc_twin_key);
				if(lua_istable(L, -1))
				{
					lua_remove(L, -2);
					return;
				}
				lua_pop(L, 1);

				const int original = lua_gettop(L);
				lua_newtable(L);
				const int twin = original + 1;
				lua_pushnil(L);
				while(lua_next(L, original))
				{
					if(is_gc_key(L, -2))
					{
						lua_pop(L, 1);
						continue;
					}
					lua_pushvalue(L, -2);
					lua_insert(L, -2);
					lua_rawset(L, twin);
				}

				lua_pushvalue(L, twin);
				lua_rawseti(L, original, nogc_twin_key);
				lua_pushvalue(L, original);
				lua_rawseti(L, twin, gc_original_key);

				if(!lua_getmetatable(L, original))
				{
					lua_newtable(L);
					lua_pushvalue(L, original);
					lua_pushvalue(L, twin);
					lua_pushcclosure(L, &mirror_new_key, 2);
					lua_setfield(L, -2, "__newindex");
					lua_pushvalue(L, -1);
					lua_setmetatable(L, original);
					lua_setmetatable(L, twin);
				}
				else
				{
					lua_pop(L, 1); // not luabind's: left as it is, no mirroring
				}

				lua_remove(L, original);
			}
		}

		LUABIND_API void set_nonowning_instance_nogc(bool enable)
		{
			g_nonowning_nogc.store(enable, std::memory_order_relaxed);
		}

		LUABIND_API std::uint64_t nonowning_instance_nogc_created()
		{
			return g_nonowning_nogc_created.load(std::memory_order_relaxed);
		}

		LUABIND_API object_rep* push_new_pointer_instance(lua_State* L, class_rep* cls, std::size_t holder_size, void*& storage)
		{
			// A Lua class may have __finalize (destroy_instance calls it): only C++ classes
			if(!g_nonowning_nogc.load(std::memory_order_relaxed) || cls->get_class_type() != class_rep::cpp_class)
			{
				object_rep* result = push_new_instance(L, cls);
				storage = result->allocate(holder_size);
				return result;
			}

			const bool trailing = holder_size > inline_holder_size;
			void* mem = lua_newuserdata(L, sizeof(object_rep) + (trailing ? holder_size : 0));
			object_rep* result = new (mem) object_rep(0, cls);
			cls->get_table(L);
			lua_setuservalue(L, -2);
			lua_rawgeti(L, LUA_REGISTRYINDEX, cls->metatable_ref());
			replace_with_nogc_twin(L);
			lua_setmetatable(L, -2);
			storage = trailing ? result->use_trailing_storage() : result->allocate(holder_size);
			// Lua runs on one thread at a time: no read-modify-write needed
			g_nonowning_nogc_created.store(g_nonowning_nogc_created.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
			if(const instance_create_hook hook = g_instance_create_hook.load(std::memory_order_acquire))
				hook(L, cls);
			return result;
		}

		LUABIND_API void restore_instance_finalizer(lua_State* L, int index)
		{
			if(index < 0 && index > LUA_REGISTRYINDEX)
				index = lua_gettop(L) + index + 1;
			if(lua_type(L, index) != LUA_TUSERDATA || !lua_getmetatable(L, index))
				return;
			lua_rawgeti(L, -1, gc_original_key);
			if(lua_istable(L, -1))
				lua_setmetatable(L, index);
			else
				lua_pop(L, 1);
			lua_pop(L, 1);
		}

	}
}

