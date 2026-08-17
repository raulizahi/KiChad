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

#include "managed_symbol_library_io.h"
#include "kichad_remove_file.h"

#include <kiid.h>

#include <algorithm>
#include "kichad_boost_process.h"
#include <chrono>
#include <filesystem>
#include <string_view>
#include <thread>
#include <vector>

#include <wx/file.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>


namespace
{

constexpr size_t MAX_LIBRARY_BYTES = 16 * 1024 * 1024;
constexpr auto MAX_VALIDATION_WAIT = std::chrono::seconds( 30 );
constexpr std::string_view PROJECT_PREFIX = "${KIPRJMOD}/";


bool canonicalDirectory( wxFileName& aDirectory )
{
    std::error_code error;
    const std::string utf8( aDirectory.GetFullPath().ToUTF8() );
    const std::u8string input( reinterpret_cast<const char8_t*>( utf8.data() ), utf8.size() );
    const std::filesystem::path canonical =
            std::filesystem::canonical( std::filesystem::path( input ), error );

    if( error || !std::filesystem::is_directory( canonical, error ) || error )
        return false;

    const std::u8string output = canonical.generic_u8string();
    aDirectory.AssignDir( wxString::FromUTF8(
            reinterpret_cast<const char*>( output.data() ), output.size() ) );
    return true;
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

} // namespace


namespace KICHAD::MANAGED_SYMBOL_LIBRARY_IO
{

bool Resolve( const wxString& aProjectPath, const std::string& aUri,
              wxFileName& aResolved, std::string& aRelativePath, std::string& aError )
{
    if( !aUri.starts_with( PROJECT_PREFIX ) || aUri.size() <= PROJECT_PREFIX.size()
        || aUri.size() > 4096 || !aUri.ends_with( ".kicad_sym" )
        || aUri.find( '\0' ) != std::string::npos )
    {
        aError = "managed symbol library URI is malformed";
        return false;
    }

    aRelativePath = aUri.substr( PROJECT_PREFIX.size() );
    wxFileName candidate( wxString::FromUTF8( aRelativePath ) );

    if( candidate.IsAbsolute() )
    {
        aError = "managed symbol library path must be project-relative";
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
    wxFileName parent = wxFileName::DirName( candidate.GetPath() );

    if( !canonicalDirectory( parent ) )
    {
        aError = "managed symbol library parent directory does not exist";
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
        aError = "managed symbol library resolves outside the active project";
        return false;
    }

    aResolved.Assign( parent.GetFullPath(), candidate.GetFullName() );

    if( aResolved.FileExists() )
    {
        std::error_code error;
        const std::string utf8( aResolved.GetFullPath().ToUTF8() );
        const std::u8string input( reinterpret_cast<const char8_t*>( utf8.data() ), utf8.size() );
        const std::filesystem::path path( input );
        const std::filesystem::file_status status = std::filesystem::symlink_status( path, error );

        if( error || !std::filesystem::is_regular_file( status )
            || std::filesystem::is_symlink( status ) )
        {
            aError = "managed symbol library must be a confined regular file, not a symlink";
            return false;
        }
    }

    return true;
}


bool ReadOptional( const wxFileName& aPath, bool& aPresent, std::string& aSource,
                   std::string& aError )
{
    aPresent = aPath.FileExists();
    aSource.clear();

    if( !aPresent )
        return true;

    wxFile file( aPath.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
    {
        aError = "could not open managed symbol library";
        return false;
    }

    const wxFileOffset length = file.Length();

    if( length < 0 || length > static_cast<wxFileOffset>( MAX_LIBRARY_BYTES ) )
    {
        aError = "managed symbol library exceeds 16 MiB";
        return false;
    }

    aSource.assign( static_cast<size_t>( length ), '\0' );

    if( length > 0 && file.Read( aSource.data(), aSource.size() ) != length )
    {
        aSource.clear();
        aError = "could not read complete managed symbol library";
        return false;
    }

    return true;
}


bool InstallAtomically( const wxFileName& aPath, bool aPresent, const std::string& aSource,
                        std::string& aError )
{
    if( !aPresent )
    {
        if( aPath.FileExists() && !KICHAD::RemoveFileWithRetry( aPath.GetFullPath() ) )
        {
            aError = "could not remove newly created managed symbol library during rollback";
            return false;
        }

        if( aPath.FileExists() )
        {
            aError = "managed symbol library remained after rollback";
            return false;
        }

        return true;
    }

    if( aSource.empty() || aSource.size() > MAX_LIBRARY_BYTES )
    {
        aError = "managed symbol library must contain 1 byte to 16 MiB";
        return false;
    }

    const wxString temporaryPath =
            aPath.GetFullPath() + wxS( ".tmp-" ) + KIID().AsString();
    wxFile temporary;

    if( !temporary.Create( temporaryPath, true )
        || temporary.Write( aSource.data(), aSource.size() ) != aSource.size()
        || !temporary.Flush() )
    {
        temporary.Close();
        KICHAD::RemoveFileWithRetry( temporaryPath );
        aError = "could not durably write managed symbol library";
        return false;
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, aPath.GetFullPath(), true ) )
    {
        KICHAD::RemoveFileWithRetry( temporaryPath );
        aError = "could not atomically install managed symbol library";
        return false;
    }

    bool installedPresent = false;
    std::string installedSource;

    if( !ReadOptional( aPath, installedPresent, installedSource, aError )
        || !installedPresent || installedSource != aSource )
    {
        aError = "managed symbol library verification failed after installation";
        return false;
    }

    return true;
}


bool ValidateNative( const wxFileName& aPath, std::string& aError )
{
    namespace bp = KICHAD_BP;
    wxFileName cli;

    if( !findCli( cli ) )
    {
        aError = "the sibling kicad-cli native symbol validator is unavailable";
        return false;
    }

    const wxString token = KIID().AsString();
    wxFileName temporaryRoot = wxFileName::DirName( wxFileName::GetTempDir() );
    temporaryRoot.AppendDir( wxS( "kichad-kds-symbol-" ) + token );

    if( !wxFileName::Mkdir( temporaryRoot.GetFullPath(), 0700 ) )
    {
        aError = "could not create private native symbol validation directory";
        return false;
    }

    wxFileName output( temporaryRoot.GetFullPath(), wxS( "validated.kicad_sym" ) );
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
                bp::args( std::vector<std::string>{ "sym", "upgrade", "--force", "--output",
                                                    output.GetFullPath().ToStdString(),
                                                    aPath.GetFullPath().ToStdString() } ),
                bp::std_out > stdoutLog.GetFullPath().ToStdString(),
                bp::std_err > stderrLog.GetFullPath().ToStdString(),
                childEnvironment );
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
                aError = "native symbol validation process failed: " + processError.message();
        }
        else
        {
            exitCode = process.exit_code();
        }
    }
    catch( const std::exception& error )
    {
        aError = std::string( "could not run native symbol validation: " ) + error.what();
    }

    bool outputPresent = false;
    std::string outputSource;
    std::string outputError;
    const bool outputValid = finished && exitCode == 0
                             && ReadOptional( output, outputPresent, outputSource, outputError )
                             && outputPresent && !outputSource.empty();

    if( aError.empty() && !outputValid )
    {
        aError = !finished ? "native symbol validation exceeded 30 seconds"
                           : "native KiCad rejected the generated symbol library";
        appendBoundedError( stderrLog, aError );
    }

    std::error_code cleanupError;
    const std::string utf8( temporaryRoot.GetFullPath().ToUTF8() );
    const std::u8string cleanupPath( reinterpret_cast<const char8_t*>( utf8.data() ),
                                     utf8.size() );
    std::filesystem::remove_all( std::filesystem::path( cleanupPath ), cleanupError );

    if( aError.empty() && cleanupError )
        aError = "could not remove private native symbol validation directory";

    return aError.empty();
}

} // namespace KICHAD::MANAGED_SYMBOL_LIBRARY_IO
