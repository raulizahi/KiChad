/*
 * This program source code file is part of KiChad, a Codex-integrated downstream of KiCad.
 *
 * Copyright (C) 2026 KiChad Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "managed_footprint_library_io.h"
#include "kichad_remove_file.h"

#include <kiid.h>

#include <algorithm>
#include "kichad_boost_process.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <thread>
#include <vector>

#include <wx/file.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>


namespace
{

using FILES = KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::FILES;

constexpr size_t MAX_FOOTPRINT_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_LIBRARY_BYTES = 64 * 1024 * 1024;
constexpr size_t MAX_FOOTPRINTS = 4096;
constexpr auto MAX_VALIDATION_WAIT = std::chrono::seconds( 30 );
constexpr std::string_view PROJECT_PREFIX = "${KIPRJMOD}/";


std::filesystem::path nativePath( const wxString& aPath )
{
    wxString normalized = aPath;

    while( normalized.length() > 1
           && ( normalized.EndsWith( wxS( "/" ) ) || normalized.EndsWith( wxS( "\\" ) ) ) )
    {
        normalized.RemoveLast();
    }

    const std::string utf8( normalized.ToUTF8() );
    const std::u8string value( reinterpret_cast<const char8_t*>( utf8.data() ), utf8.size() );
    return std::filesystem::path( value );
}


wxString wxPath( const std::filesystem::path& aPath )
{
    const std::u8string value = aPath.generic_u8string();
    return wxString::FromUTF8( reinterpret_cast<const char*>( value.data() ), value.size() );
}


bool canonicalDirectory( wxFileName& aDirectory )
{
    std::error_code error;
    const std::filesystem::path canonical =
            std::filesystem::canonical( nativePath( aDirectory.GetFullPath() ), error );

    if( error || !std::filesystem::is_directory( canonical, error ) || error )
        return false;

    aDirectory.AssignDir( wxPath( canonical ) );
    return true;
}


bool validFilename( const std::string& aFilename )
{
    if( aFilename.size() <= std::strlen( ".kicad_mod" ) || aFilename.size() > 1024
        || !aFilename.ends_with( ".kicad_mod" )
        || aFilename.find_first_of( "/\\\r\n" ) != std::string::npos
        || aFilename.find( '\0' ) != std::string::npos )
    {
        return false;
    }

    const std::string stem = aFilename.substr( 0, aFilename.size() - std::strlen( ".kicad_mod" ) );
    return stem != "." && stem != "..";
}


bool boundedFiles( const FILES& aFiles, std::string& aError )
{
    if( aFiles.empty() || aFiles.size() > MAX_FOOTPRINTS )
    {
        aError = "managed footprint library requires 1 through 4096 footprints";
        return false;
    }

    size_t bytes = 0;

    for( const auto& [filename, source] : aFiles )
    {
        if( !validFilename( filename ) || source.empty()
            || source.size() > MAX_FOOTPRINT_BYTES
            || source.size() > MAX_LIBRARY_BYTES - bytes
            || source.find( '\0' ) != std::string::npos )
        {
            aError = "managed footprint library has an invalid name or exceeds bounded size limits";
            return false;
        }

        bytes += source.size();
    }

    return true;
}


wxFileName siblingDirectory( const wxFileName& aPath, const wxString& aSuffix )
{
    wxFileName sibling = aPath;
    const wxString leaf = sibling.GetDirs().Last();
    sibling.RemoveLastDir();
    sibling.AppendDir( leaf + aSuffix );
    return sibling;
}


bool findCli( wxFileName& aCli )
{
    wxFileName executable( wxStandardPaths::Get().GetExecutablePath() );
#ifdef __WXMSW__
    const wxString cliName = wxS( "kicad-cli.exe" );
#else
    const wxString cliName = wxS( "kicad-cli" );
#endif
    wxFileName sibling( executable.GetPath(), cliName );

    if( sibling.FileExists() && sibling.IsFileExecutable() )
    {
        aCli = sibling;
        return true;
    }

    wxString fromBuild;

    if( !wxGetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ), &fromBuild )
        || fromBuild != wxS( "1" ) )
    {
        return false;
    }

    wxFileName root = wxFileName::DirName( executable.GetPath() );

    for( int depth = 0; depth < 6 && root.GetDirCount() > 0; ++depth )
    {
        wxFileName candidate = root;
        candidate.AppendDir( wxS( "kicad" ) );
        candidate.SetFullName( cliName );

        if( candidate.FileExists() && candidate.IsFileExecutable() )
        {
            aCli = candidate;
            return true;
        }

        root.RemoveLastDir();
    }

    return false;
}


void appendBoundedError( const wxFileName& aPath, std::string& aError )
{
    if( !aPath.FileExists() )
        return;

    wxFile file( aPath.GetFullPath(), wxFile::read );
    const wxFileOffset length = file.IsOpened() ? file.Length() : 0;

    if( length <= 0 )
        return;

    const size_t bounded = std::min<size_t>( static_cast<size_t>( length ), 4096 );
    std::string detail( bounded, '\0' );

    if( file.Read( detail.data(), detail.size() )
        == static_cast<wxFileOffset>( detail.size() ) )
    {
        aError += ": " + detail;
    }
}


bool writeDirectory( const wxFileName& aPath, const FILES& aFiles, std::string& aError )
{
    if( !wxFileName::Mkdir( aPath.GetFullPath(), 0700 ) )
    {
        aError = "could not create managed footprint staging directory";
        return false;
    }

    for( const auto& [filename, source] : aFiles )
    {
        wxFileName output( aPath.GetFullPath(), wxString::FromUTF8( filename ) );
        wxFile file;

        if( !file.Create( output.GetFullPath(), false )
            || file.Write( source.data(), source.size() ) != source.size()
            || !file.Flush() )
        {
            file.Close();
            aError = "could not durably write managed footprint " + filename;
            return false;
        }
    }

    return true;
}


void removeDirectory( const wxFileName& aPath )
{
    std::error_code ignored;
    std::filesystem::remove_all( nativePath( aPath.GetFullPath() ), ignored );
}

} // namespace


namespace KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO
{

bool Resolve( const wxString& aProjectPath, const std::string& aUri,
              wxFileName& aResolved, std::string& aRelativePath, std::string& aError )
{
    if( !aUri.starts_with( PROJECT_PREFIX ) || aUri.size() <= PROJECT_PREFIX.size()
        || aUri.size() > 4096 || !aUri.ends_with( ".pretty" )
        || aUri.find( '\0' ) != std::string::npos )
    {
        aError = "managed footprint library URI is malformed";
        return false;
    }

    aRelativePath = aUri.substr( PROJECT_PREFIX.size() );
    wxFileName candidate = wxFileName::DirName( wxString::FromUTF8( aRelativePath ) );

    if( candidate.IsAbsolute() || candidate.GetDirCount() == 0 )
    {
        aError = "managed footprint library path must be project-relative";
        return false;
    }

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalDirectory( root ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    candidate.MakeAbsolute( root.GetFullPath() );
    candidate.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    wxFileName parent = candidate;
    const wxString leaf = parent.GetDirs().Last();
    parent.RemoveLastDir();

    if( !canonicalDirectory( parent ) )
    {
        aError = "managed footprint library parent directory does not exist";
        return false;
    }

    wxString parentPath = parent.GetPathWithSep();
    wxString rootPath = root.GetPathWithSep();

#ifdef __WXMSW__
    parentPath.MakeLower();
    rootPath.MakeLower();
#endif

    if( !parentPath.StartsWith( rootPath ) && parent.GetFullPath() != root.GetFullPath() )
    {
        aError = "managed footprint library resolves outside the active project";
        return false;
    }

    aResolved = parent;
    aResolved.AppendDir( leaf );

    std::error_code targetError;
    const std::filesystem::file_status targetStatus =
            std::filesystem::symlink_status( nativePath( aResolved.GetFullPath() ), targetError );

    if( !targetError && targetStatus.type() != std::filesystem::file_type::not_found )
    {
        if( !std::filesystem::is_directory( targetStatus )
            || std::filesystem::is_symlink( targetStatus ) )
        {
            aError = "managed footprint library must be a confined directory, not a symlink";
            return false;
        }
    }
    else if( targetError && targetError != std::errc::no_such_file_or_directory )
    {
        aError = "managed footprint library path could not be inspected: "
                 + targetError.message();
        return false;
    }

    return true;
}


bool ReadOptional( const wxFileName& aPath, bool& aPresent, FILES& aFiles,
                   std::string& aError )
{
    aFiles.clear();
    std::error_code error;
    const std::filesystem::path directory = nativePath( aPath.GetFullPath() );
    const std::filesystem::file_status directoryStatus =
            std::filesystem::symlink_status( directory, error );

    if( error )
    {
        if( error == std::errc::no_such_file_or_directory )
        {
            aPresent = false;
            return true;
        }

        aError = "managed footprint library path could not be inspected: " + error.message();
        return false;
    }

    if( directoryStatus.type() == std::filesystem::file_type::not_found )
    {
        aPresent = false;
        return true;
    }

    aPresent = true;

    if( !std::filesystem::is_directory( directoryStatus )
        || std::filesystem::is_symlink( directoryStatus ) )
    {
        aError = "managed footprint library is not a regular owned directory";
        return false;
    }

    size_t totalBytes = 0;

    for( std::filesystem::directory_iterator iterator( directory, error ), end;
         !error && iterator != end; iterator.increment( error ) )
    {
        const std::filesystem::directory_entry& entry = *iterator;
        const std::filesystem::file_status status = entry.symlink_status( error );

        if( error )
            break;

        const std::u8string utf8Name = entry.path().filename().generic_u8string();
        const std::string filename( reinterpret_cast<const char*>( utf8Name.data() ),
                                    utf8Name.size() );

        if( !std::filesystem::is_regular_file( status ) || std::filesystem::is_symlink( status )
            || !validFilename( filename ) || aFiles.size() >= MAX_FOOTPRINTS )
        {
            aError = "managed footprint library contains an unsafe or unowned entry: " + filename;
            return false;
        }

        const uintmax_t length = entry.file_size( error );

        if( error || length == 0 || length > MAX_FOOTPRINT_BYTES
            || length > MAX_LIBRARY_BYTES - totalBytes )
        {
            aError = "managed footprint library exceeds bounded size limits";
            return false;
        }

        wxFile input( wxPath( entry.path() ), wxFile::read );
        std::string source( static_cast<size_t>( length ), '\0' );

        if( !input.IsOpened()
            || input.Read( source.data(), source.size() )
                       != static_cast<wxFileOffset>( source.size() ) )
        {
            aError = "could not read complete managed footprint " + filename;
            return false;
        }

        totalBytes += source.size();
        aFiles.emplace( filename, std::move( source ) );
    }

    if( error )
    {
        aError = "could not enumerate complete managed footprint library: " + error.message();
        return false;
    }

    if( aFiles.empty() )
    {
        aError = "managed footprint library directory is empty";
        return false;
    }

    return true;
}


bool InstallAtomically( const wxFileName& aPath, bool aPresent, const FILES& aFiles,
                        std::string& aError )
{
    if( !aPresent )
    {
        if( !aFiles.empty() )
        {
            aError = "absent managed footprint library cannot contain files";
            return false;
        }

        bool currentPresent = false;
        FILES currentFiles;

        if( !ReadOptional( aPath, currentPresent, currentFiles, aError ) )
            return false;

        if( !currentPresent )
            return true;

        wxFileName removed = siblingDirectory(
                aPath, wxS( ".remove-" ) + KIID().AsString() );

        if( removed.DirExists() || wxFileExists( removed.GetFullPath() ) )
        {
            aError = "managed footprint removal staging path already exists";
            return false;
        }

        std::error_code error;
        std::filesystem::rename( nativePath( aPath.GetFullPath() ),
                                 nativePath( removed.GetFullPath() ), error );

        if( error )
        {
            aError = "could not atomically remove managed footprint library: " + error.message();
            return false;
        }

        std::filesystem::remove_all( nativePath( removed.GetFullPath() ), error );

        if( error || aPath.DirExists() )
        {
            aError = "managed footprint library removal verification failed";
            return false;
        }

        return true;
    }

    if( !boundedFiles( aFiles, aError ) )
        return false;

    bool previousPresent = false;
    FILES previousFiles;

    if( !ReadOptional( aPath, previousPresent, previousFiles, aError ) )
        return false;

    wxFileName staging = siblingDirectory(
            aPath, wxS( ".stage-" ) + KIID().AsString() );
    wxFileName backup = siblingDirectory(
            aPath, wxS( ".backup-" ) + KIID().AsString() );

    if( staging.DirExists() || wxFileExists( staging.GetFullPath() )
        || backup.DirExists() || wxFileExists( backup.GetFullPath() ) )
    {
        aError = "managed footprint staging or backup path already exists";
        return false;
    }

    if( !writeDirectory( staging, aFiles, aError ) )
    {
        removeDirectory( staging );
        return false;
    }

    bool stagedPresent = false;
    FILES stagedFiles;

    if( !ReadOptional( staging, stagedPresent, stagedFiles, aError )
        || !stagedPresent || stagedFiles != aFiles )
    {
        removeDirectory( staging );
        aError = "managed footprint staging verification failed";
        return false;
    }

    std::error_code error;

    if( previousPresent )
    {
        std::filesystem::rename( nativePath( aPath.GetFullPath() ),
                                 nativePath( backup.GetFullPath() ), error );

        if( error )
        {
            removeDirectory( staging );
            aError = "could not stage the prior managed footprint library: " + error.message();
            return false;
        }
    }

    std::filesystem::rename( nativePath( staging.GetFullPath() ),
                             nativePath( aPath.GetFullPath() ), error );

    if( error )
    {
        if( previousPresent )
        {
            std::error_code restoreError;
            std::filesystem::rename( nativePath( backup.GetFullPath() ),
                                     nativePath( aPath.GetFullPath() ), restoreError );

            if( restoreError )
                aError = "managed footprint install and prior-library restore both failed";
            else
                aError = "could not atomically install managed footprint library";
        }
        else
        {
            aError = "could not atomically install managed footprint library";
        }

        removeDirectory( staging );
        return false;
    }

    bool installedPresent = false;
    FILES installedFiles;

    if( !ReadOptional( aPath, installedPresent, installedFiles, aError )
        || !installedPresent || installedFiles != aFiles )
    {
        const std::string verificationError =
                "managed footprint library verification failed after installation";
        wxFileName rejected = siblingDirectory(
                aPath, wxS( ".rejected-" ) + KIID().AsString() );
        std::error_code rejectError;
        std::filesystem::rename( nativePath( aPath.GetFullPath() ),
                                 nativePath( rejected.GetFullPath() ), rejectError );
        std::error_code restoreError;

        if( previousPresent && !rejectError )
        {
            std::filesystem::rename( nativePath( backup.GetFullPath() ),
                                     nativePath( aPath.GetFullPath() ), restoreError );
        }

        removeDirectory( rejected );
        aError = verificationError;

        if( rejectError || ( previousPresent && restoreError ) )
            aError += "; prior-library restore also failed";

        return false;
    }

    if( previousPresent )
    {
        removeDirectory( backup );

        if( backup.DirExists() )
        {
            aError = "could not remove managed footprint library backup";
            return false;
        }
    }

    return true;
}


bool ValidateNative( const wxFileName& aPath, std::string& aError )
{
    namespace bp = KICHAD_BP;
    bool inputPresent = false;
    FILES inputFiles;

    if( !ReadOptional( aPath, inputPresent, inputFiles, aError ) || !inputPresent )
        return false;

    wxFileName cli;

    if( !findCli( cli ) )
    {
        aError = "the sibling kicad-cli native footprint validator is unavailable";
        return false;
    }

    const wxString token = KIID().AsString();
    wxFileName temporaryRoot = wxFileName::DirName( wxFileName::GetTempDir() );
    temporaryRoot.AppendDir( wxS( "kichad-kds-footprint-" ) + token );

    if( !wxFileName::Mkdir( temporaryRoot.GetFullPath(), 0700 ) )
    {
        aError = "could not create private native footprint validation directory";
        return false;
    }

    wxFileName output = temporaryRoot;
    output.AppendDir( wxS( "validated.pretty" ) );
    wxFileName stdoutLog( temporaryRoot.GetFullPath(), wxS( "stdout.log" ) );
    wxFileName stderrLog( temporaryRoot.GetFullPath(), wxS( "stderr.log" ) );
    bool finished = false;
    int exitCode = -1;

    try
    {
        bp::environment childEnvironment = boost::this_process::environment();
        childEnvironment["KICAD_CONFIG_HOME"] = temporaryRoot.GetFullPath().ToStdString();
        childEnvironment["KICAD_CONFIG_HOME_IS_QA"] = "1";
        bp::child process(
                cli.GetFullPath().ToStdString(),
                bp::args( std::vector<std::string>{ "fp", "upgrade", "--force", "--output",
                                                    output.GetFullPath().ToStdString(),
                                                    aPath.GetFullPath().ToStdString() } ),
                bp::std_out > stdoutLog.GetFullPath().ToStdString(),
                bp::std_err > stderrLog.GetFullPath().ToStdString(), childEnvironment );
        const auto deadline = std::chrono::steady_clock::now() + MAX_VALIDATION_WAIT;
        std::error_code processError;

        while( process.running( processError ) && !processError
               && std::chrono::steady_clock::now() < deadline )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }

        finished = !process.running( processError ) && !processError;

        if( !finished )
        {
            process.terminate();
            process.wait();

            if( processError )
                aError = "native footprint validation process failed: " + processError.message();
        }
        else
        {
            exitCode = process.exit_code();
        }
    }
    catch( const std::exception& error )
    {
        aError = std::string( "could not run native footprint validation: " ) + error.what();
    }

    bool outputPresent = false;
    FILES outputFiles;
    std::string outputError;
    const bool outputValid = finished && exitCode == 0
                             && ReadOptional( output, outputPresent, outputFiles, outputError )
                             && outputPresent && outputFiles.size() == inputFiles.size()
                             && std::equal( inputFiles.begin(), inputFiles.end(), outputFiles.begin(),
                                            []( const auto& aLeft, const auto& aRight )
                                            {
                                                return aLeft.first == aRight.first;
                                            } );

    if( aError.empty() && !outputValid )
    {
        aError = !finished ? "native footprint validation exceeded 30 seconds"
                           : "native KiCad rejected the generated footprint library";
        appendBoundedError( stderrLog, aError );
    }

    // Best effort; see the matching comment in managed_symbol_library_io.cpp.
    KICHAD::RemoveDirectoryWithRetry( temporaryRoot.GetFullPath() );

    return aError.empty();
}

} // namespace KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO
