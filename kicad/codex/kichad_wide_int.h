/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KICHAD_WIDE_INT_H
#define KICHAD_WIDE_INT_H

/**
 * 128-bit integer types for the design-script analyzers.
 *
 * Several codex passes widen nanometre and nanoamp arithmetic to 128 bits so an
 * intermediate product cannot overflow before the result is range-checked back
 * down to int64_t.  `__int128` is a GCC/Clang extension that MSVC does not
 * provide, so Windows borrows the same range from Boost.Multiprecision.
 *
 * The qualified Linux platform keeps the builtin type, so its arithmetic and
 * codegen are unchanged by the Windows port.
 *
 * WIDE_UINT is the unsigned counterpart, used where a magnitude is taken before
 * decimal conversion.  Note that on MSVC these are class types: an expression
 * such as `'0' + value % 10` needs an explicit narrowing cast to an integer
 * type before it can be used as a character.
 */

#if defined( _MSC_VER )

#include <boost/multiprecision/cpp_int.hpp>

using WIDE_INT = boost::multiprecision::int128_t;
using WIDE_UINT = boost::multiprecision::uint128_t;

#else

using WIDE_INT = __int128;
using WIDE_UINT = unsigned __int128;

#endif

#endif  // KICHAD_WIDE_INT_H
