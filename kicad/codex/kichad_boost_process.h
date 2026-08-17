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

#ifndef KICHAD_BOOST_PROCESS_H
#define KICHAD_BOOST_PROCESS_H

/**
 * Boost.Process v1 access for the codex child-process helpers.
 *
 * Boost 1.86 introduced Process v2, and later releases repoint
 * `<boost/process.hpp>` at it.  v2 drops the `child` / `args` / `start_dir` /
 * `std_out` / `std_err` API that these helpers are written against, so a current
 * vcpkg Boost fails to compile them while Ubuntu 24.04's Boost 1.83 succeeds.
 *
 * Include the explicit v1 header wherever it exists and fall back to the plain
 * header on releases that predate the split.  Use the KICHAD_BP alias rather
 * than naming `boost::process` directly, so both layouts resolve to v1.
 */

#if defined( __has_include )
#if __has_include( <boost/process/v1.hpp> )
#define KICHAD_HAVE_BOOST_PROCESS_V1 1
#endif
#endif

#if defined( KICHAD_HAVE_BOOST_PROCESS_V1 )

#include <boost/process/v1.hpp>
namespace KICHAD_BP = boost::process::v1;

#else

#include <boost/process.hpp>
namespace KICHAD_BP = boost::process;

#endif

#endif  // KICHAD_BOOST_PROCESS_H
