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

#ifndef KICHAD_REMOVE_FILE_H
#define KICHAD_REMOVE_FILE_H

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include <wx/filefn.h>
#include <wx/log.h>
#include <wx/string.h>

namespace KICHAD
{

/**
 * Remove a file, tolerating a transient Windows sharing violation.
 *
 * POSIX allows a file to be unlinked while handles to it are still open, so removing a
 * scratch file immediately after the process that wrote it always succeeds.  Windows
 * refuses with ERROR_SHARING_VIOLATION until every handle is closed, and a file that was
 * just written is routinely held for a short spell by a virus scanner or the search
 * indexer, as well as by a child process that has not finished exiting.  The redirected
 * stdout and stderr of the kicad-cli helpers hit exactly that window.
 *
 * Retry briefly instead of failing on the first attempt.  wxRemoveFile() logs a system
 * error each time it fails, which reaches the user as an error dialog about a scratch
 * file they can do nothing about, so the attempts are made with logging suppressed and
 * only the final outcome is reported to the caller.
 *
 * @param aPath is the file to remove.
 * @return true if the file is gone, either because it was removed or because it was
 *         already absent.
 */
inline bool RemoveFileWithRetry( const wxString& aPath )
{
    constexpr int  MAX_ATTEMPTS = 10;
    constexpr auto RETRY_DELAY = std::chrono::milliseconds( 20 );

    for( int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt )
    {
        {
            wxLogNull suppressTransientFailures;

            if( wxRemoveFile( aPath ) )
                return true;
        }

        // A concurrent remover, or a delete that Windows has already committed, leaves
        // nothing to retry.
        if( !wxFileExists( aPath ) )
            return true;

        if( attempt + 1 < MAX_ATTEMPTS )
            std::this_thread::sleep_for( RETRY_DELAY );
    }

    return !wxFileExists( aPath );
}


/**
 * Remove a directory tree, tolerating a transient Windows sharing violation.
 *
 * The directory counterpart of RemoveFileWithRetry().  The native symbol and footprint
 * validators point KICAD_CONFIG_HOME at a scratch directory and let kicad-cli write into
 * it, so on Windows the tree routinely stays locked for a moment after the child exits.
 *
 * @param aPath is the directory to remove.
 * @return true if the directory is gone, either because it was removed or because it was
 *         already absent.
 */
inline bool RemoveDirectoryWithRetry( const wxString& aPath )
{
    constexpr int  MAX_ATTEMPTS = 10;
    constexpr auto RETRY_DELAY = std::chrono::milliseconds( 20 );

    const std::string   utf8( aPath.ToUTF8() );
    const std::u8string encoded( reinterpret_cast<const char8_t*>( utf8.data() ), utf8.size() );
    const std::filesystem::path path( encoded );

    for( int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt )
    {
        std::error_code removeError;
        std::filesystem::remove_all( path, removeError );

        if( !removeError )
            return true;

        std::error_code existsError;

        if( !std::filesystem::exists( path, existsError ) )
            return true;

        if( attempt + 1 < MAX_ATTEMPTS )
            std::this_thread::sleep_for( RETRY_DELAY );
    }

    std::error_code existsError;
    return !std::filesystem::exists( path, existsError );
}

} // namespace KICHAD

#endif // KICHAD_REMOVE_FILE_H
