/*********************************************************
 * Copyright (C) 2022 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/*
 * sigc++2to3.h --
 *
 *     A header file that contain definations that will allow some sigc++2 APIs
 *     to work when builing with sigc++3.
 */

#pragma once


#include <type_traits>
#include <sigc++config.h>
#include <sigc++/sigc++.h>


#if SIGCXX_MAJOR_VERSION >= 3

namespace sigc {


// Add old mem_fun API for pointer
template<typename T_return, typename T_obj, typename... T_arg>
inline decltype(auto)
mem_fun(T_obj *obj, T_return (T_obj::*func)(T_arg...))
{
   return bound_mem_functor<T_return (T_obj::*)(T_arg...), T_arg...>(*obj, func);
}

template<typename T_return, typename T_obj, typename... T_arg>
inline decltype(auto)
mem_fun(const T_obj *obj, T_return (T_obj::*func)(T_arg...) const)
{
   return bound_mem_functor<T_return (T_obj::*)(T_arg...) const, T_arg...>(*obj, func);
}


} //namespace sigc

#endif // SIGCXX_MAJOR_VERSION >= 3
