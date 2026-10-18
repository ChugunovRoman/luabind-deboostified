// Copyright Daniel Wallin 2008. Use, modification and distribution is
// subject to the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef LUABIND_DETAIL_CONSTRUCTOR_081018_HPP
#define LUABIND_DETAIL_CONSTRUCTOR_081018_HPP

#include <luabind/get_main_thread.hpp>
#include <luabind/lua_argument_proxy.hpp>
#include <luabind/wrapper_base.hpp>
#include <luabind/detail/inheritance.hpp>
#include <luabind/detail/object_rep.hpp>
#include <type_traits>
#include <utility>

namespace luabind {
	namespace detail {

		inline void inject_backref(lua_State*, void*, void*)
		{}

		template <class T>
		void inject_backref(lua_State* L, T* p, wrap_base*)
		{
			weak_ref(get_main_thread(L), L, 1).swap(wrap_access::ref(*p));
		}

		template< class T, class Pointer, class Signature, class Arguments, class ArgumentIndices >
		struct construct_aux_helper;

		template< class T, class Pointer, class Signature, typename... Arguments, unsigned int... ArgumentIndices >
		struct construct_aux_helper< T, Pointer, Signature, meta::type_list< Arguments... >, meta::index_list< ArgumentIndices... > >
		{
			using holder_type = pointer_holder<Pointer, T>;
			using value_holder_type = value_holder<T>;

			// GlobalWar (47-luabind-value-nogc.md, object_rep.hpp set_value_instance_nogc): a trivially destructible
			// T with the default holder whose value fits m_instance_buffer of object_rep (constructor_dispatcher made the
			// userdata already, its size is fixed) may be kept by value in an instance without __gc
			static constexpr bool value_nogc = std::is_same<Pointer, luabind::unique_ptr<T>>::value
				&& !std::is_base_of<wrap_base, T>::value
				&& std::is_trivially_destructible<T>::value
				&& sizeof(value_holder_type) <= 32
				&& alignof(value_holder_type) <= alignof(void*)
				&& alignof(value_holder_type) <= alignof(object_rep);

			void operator()(argument const& self_, Arguments... args) const
			{
				object_rep* self = touserdata<object_rep>(self_);
				lua_State* const L = self_.interpreter();

				if constexpr(value_nogc)
				{
					self_.push(L);
					const bool nogc = value_construct_nogc(L, -1, typeid(T));
					lua_pop(L, 1);
					if(nogc)
					{
						// No __gc: the value in the userdata, nothing to free (value_holder of a trivially destructible T)
						void* storage = self->allocate(sizeof(value_holder_type));
						self->set_instance(new (storage) value_holder_type(L, std::in_place, args...));
						return;
					}
				}
				else
				{
					// An instance without __gc gets a holder that must be destroyed (a base class constructor, a smart
					// pointer holder): back to the metatable with __gc
					self_.push(L);
					restore_instance_finalizer(L, -1);
					lua_pop(L, 1);
				}

                luabind::unique_ptr<T> instance(luabind_new<T>(args...));
				inject_backref(self_.interpreter(), instance.get(), instance.get());

				void* naked_ptr = instance.get();
				Pointer ptr(instance.release());

				void* storage = self->allocate(sizeof(holder_type));

				self->set_instance(new (storage) holder_type(std::move(ptr), registered_class<T>::id, naked_ptr));
			}
		};


		template< class T, class Pointer, class Signature >
		struct construct :
			public construct_aux_helper <
			T,
			Pointer,
			Signature, typename meta::sub_range< Signature, 2, meta::size<Signature>::value >::type,
			typename meta::make_index_range<0, meta::size<Signature>::value - 2>::type >
		{
		};

	}	// namespace detail

}	// namespace luabind

# endif // LUABIND_DETAIL_CONSTRUCTOR_081018_HPP

