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

#include "kicad_ipc_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#ifdef _WIN32
// NOMINMAX keeps windows.h from replacing std::min below with its own macro.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <api/common/commands/editor_commands.pb.h>
#include <kinng.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>


namespace
{

std::u8string utf8Path( const wxString& aPath )
{
    const std::string utf8( aPath.ToUTF8() );
    return std::u8string( reinterpret_cast<const char8_t*>( utf8.data() ), utf8.size() );
}


std::filesystem::path normalizedPath( const wxString& aPath )
{
    std::error_code error;
    std::filesystem::path path =
            std::filesystem::weakly_canonical( std::filesystem::path( utf8Path( aPath ) ), error );

    if( error )
        return {};

    return path;
}


std::string pathUtf8( const std::filesystem::path& aPath )
{
    const std::u8string utf8 = aPath.generic_u8string();
    return std::string( reinterpret_cast<const char*>( utf8.data() ), utf8.size() );
}


std::string loweredPath( std::string aValue )
{
    std::transform( aValue.begin(), aValue.end(), aValue.begin(),
                    []( unsigned char aCharacter )
                    {
                        return static_cast<char>( std::tolower( aCharacter ) );
                    } );
    return aValue;
}


/**
 * Collect the endpoints that look like an open KiCad editor socket.
 *
 * nng's ipc:// transport is a Unix domain socket on Linux and macOS, so every open editor
 * leaves an api*.sock file behind and enumerating the socket directory finds them.  On
 * Windows the same transport is a named pipe whose name is the socket path string, and
 * nothing is created on disk: the directory is always empty, so a filesystem scan reports
 * that no editor is open no matter how many are running.  Enumerate the pipe namespace
 * there instead and match the same api*.sock naming, so both platforms discover the same
 * endpoints and the caller's ipc:// URLs stay identical.
 */
bool collectSocketCandidates( const wxString& aSocketDirectory,
                              std::vector<std::filesystem::path>& aPaths, std::string& aError )
{
    constexpr size_t MAX_CANDIDATES = 32;

    auto isEditorSocket = []( const std::filesystem::path& aCandidate )
    {
        const std::string name = pathUtf8( aCandidate.filename() );
        return name.starts_with( "api" ) && name.ends_with( ".sock" );
    };

#ifdef _WIN32
    WIN32_FIND_DATAW findData;
    HANDLE           find = FindFirstFileW( L"\\\\.\\pipe\\*", &findData );

    if( find == INVALID_HANDLE_VALUE )
    {
        aError = "the Windows named pipe namespace is unavailable";
        return false;
    }

    // Pipe names carry the whole socket path, so match the directory by name.  Windows
    // paths are case-insensitive, and pathUtf8() normalizes the separators for both sides.
    const std::string wanted = loweredPath( pathUtf8( normalizedPath( aSocketDirectory ) ) );

    do
    {
        const std::filesystem::path candidate( findData.cFileName );

        if( !isEditorSocket( candidate ) )
            continue;

        if( loweredPath( pathUtf8( candidate.parent_path() ) ) != wanted )
            continue;

        aPaths.emplace_back( candidate );
    } while( aPaths.size() < MAX_CANDIDATES && FindNextFileW( find, &findData ) );

    FindClose( find );
    return true;
#else
    std::error_code                     directoryError;
    std::filesystem::directory_iterator directory( std::filesystem::path( utf8Path( aSocketDirectory ) ),
                                                   directoryError );

    if( directoryError )
    {
        aError = "the KiCad IPC socket directory is unavailable";
        return false;
    }

    for( const std::filesystem::directory_entry& entry : directory )
    {
        if( !isEditorSocket( entry.path() ) )
            continue;

        aPaths.emplace_back( entry.path() );

        if( aPaths.size() == MAX_CANDIDATES )
            break;
    }

    return true;
#endif
}


bool samePath( const wxString& aLeft, const wxString& aRight )
{
    std::filesystem::path left = normalizedPath( aLeft );
    std::filesystem::path right = normalizedPath( aRight );

    if( left.empty() || right.empty() )
        return false;

#ifdef __WXMSW__
    wxString leftString = wxString::FromUTF8( left.generic_string() );
    wxString rightString = wxString::FromUTF8( right.generic_string() );
    return leftString.CmpNoCase( rightString ) == 0;
#else
    return left == right;
#endif
}


bool sameFilename( const wxString& aLeft, const wxString& aRight )
{
#ifdef __WXMSW__
    return aLeft.CmpNoCase( aRight ) == 0;
#else
    return aLeft == aRight;
#endif
}

} // namespace


KICHAD_IPC_CLIENT::KICHAD_IPC_CLIENT( std::string aClientName, wxString aSocketDirectory,
                                      std::chrono::milliseconds aTimeout ) :
        m_clientName( std::move( aClientName ) ),
        m_socketDirectory( std::move( aSocketDirectory ) ),
        m_timeout( aTimeout )
{
    if( m_socketDirectory.IsEmpty() )
        m_socketDirectory = DefaultSocketDirectory();
}


wxString KICHAD_IPC_CLIENT::DefaultSocketDirectory()
{
    wxFileName socketDirectory;

#ifdef __WXMAC__
    socketDirectory.AssignDir( wxS( "/tmp" ) );
#else
    socketDirectory.AssignDir( wxStandardPaths::Get().GetTempDir() );
#endif

    socketDirectory.AppendDir( wxS( "kicad" ) );
    return socketDirectory.GetFullPath();
}


bool KICHAD_IPC_CLIENT::FindOpenPcb( const wxString& aProjectPath, const wxString& aBoardPath,
                                     KICHAD_IPC_TARGET& aTarget, std::string& aError ) const
{
    wxString boardName = wxFileName( aBoardPath ).GetFullName();
    auto     deadline = std::chrono::steady_clock::now() + m_timeout;

    do
    {
        std::vector<std::filesystem::path> socketPaths;
        socketPaths.reserve( 32 );

        if( !collectSocketCandidates( m_socketDirectory, socketPaths, aError ) )
            return false;

        std::sort( socketPaths.begin(), socketPaths.end() );

        for( size_t socketIndex = 0; socketIndex < socketPaths.size(); ++socketIndex )
        {
            const std::filesystem::path& socketPath = socketPaths[socketIndex];
            const auto now = std::chrono::steady_clock::now();

            if( now >= deadline )
                break;

            std::string socketUrl = "ipc://" + pathUtf8( socketPath );
            kiapi::common::commands::GetOpenDocuments request;
            request.set_type( kiapi::common::types::DOCTYPE_PCB );

            kiapi::common::ApiResponse response;
            std::string                 ignoredError;

            auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>( deadline - now );

            // A dead socket may accept the transport connection and never reply.  Give every
            // discovered KiCad instance a fair share of the remaining discovery budget so one
            // stale endpoint cannot hide a live PCB Editor later in the directory.
            const size_t remainingSockets = socketPaths.size() - socketIndex;
            auto candidateTimeout = remaining / static_cast<int64_t>( remainingSockets );
            candidateTimeout = std::min( candidateTimeout, std::chrono::milliseconds( 250 ) );

            if( candidateTimeout <= std::chrono::milliseconds::zero() )
                candidateTimeout = std::chrono::milliseconds( 1 );

            if( !callSocket( socketUrl, "", request, response, ignoredError,
                             candidateTimeout ) )
                continue;

            kiapi::common::commands::GetOpenDocumentsResponse openDocuments;

            if( !response.message().UnpackTo( &openDocuments ) )
                continue;

            for( const kiapi::common::types::DocumentSpecifier& document :
                 openDocuments.documents() )
            {
                if( document.type() != kiapi::common::types::DOCTYPE_PCB
                    || !samePath( wxString::FromUTF8( document.project().path() ), aProjectPath )
                    || ( !boardName.IsEmpty()
                         && !sameFilename( wxString::FromUTF8( document.board_filename() ),
                                           boardName ) ) )
                {
                    continue;
                }

                if( response.header().kicad_token().empty() )
                    continue;

                aTarget.socketUrl = std::move( socketUrl );
                aTarget.kicadToken = response.header().kicad_token();
                aTarget.document = document;
                return true;
            }
        }

        if( std::chrono::steady_clock::now() < deadline )
            std::this_thread::sleep_for( std::chrono::milliseconds( 25 ) );
    } while( std::chrono::steady_clock::now() < deadline );

    aError = "the requested PCB is not open in a KiCad 10 PCB Editor";
    return false;
}


bool KICHAD_IPC_CLIENT::Call( const KICHAD_IPC_TARGET& aTarget,
                              const google::protobuf::Message& aRequest,
                              kiapi::common::ApiResponse& aResponse,
                              std::string& aError ) const
{
    // Every condition that makes KiCad report AS_BUSY ends on its own: a zone fill finishes, a
    // track gets routed, a point edit is released, a transient tool returns to selection.  Wait
    // for them rather than failing the request, because the caller's only other recourse is to
    // ask the user to clear a state that was about to clear itself.  A guarded apply keeps its
    // recovery journal, so arriving late is safe.
    //
    // Tool dispatch runs on a worker thread, so blocking here leaves the UI responsive and lets
    // the user finish whatever is holding the editor.
    constexpr auto BUSY_BUDGET = std::chrono::seconds( 30 );
    constexpr auto BUSY_POLL_INTERVAL = std::chrono::milliseconds( 200 );

    const auto deadline = std::chrono::steady_clock::now() + BUSY_BUDGET;

    for( ;; )
    {
        if( callSocket( aTarget.socketUrl, aTarget.kicadToken, aRequest, aResponse, aError,
                        m_timeout ) )
        {
            return true;
        }

        // callSocket() reports every non-OK status as a failure, so the response carries the
        // reason.  Only a busy editor is worth waiting on; anything else is final.
        if( aResponse.status().status() != kiapi::common::AS_BUSY
            || std::chrono::steady_clock::now() >= deadline )
        {
            return false;
        }

        std::this_thread::sleep_for( BUSY_POLL_INTERVAL );
    }
}


bool KICHAD_IPC_CLIENT::BeginCommit( const KICHAD_IPC_TARGET& aTarget,
                                     std::string& aCommitId, std::string& aError ) const
{
    kiapi::common::commands::BeginCommit request;
    kiapi::common::ApiResponse           response;

    if( !Call( aTarget, request, response, aError ) )
        return false;

    kiapi::common::commands::BeginCommitResponse beginResponse;

    if( !response.message().UnpackTo( &beginResponse ) || beginResponse.id().value().empty() )
    {
        aError = "KiCad returned an invalid begin-commit response";
        return false;
    }

    aCommitId = beginResponse.id().value();
    return true;
}


bool KICHAD_IPC_CLIENT::EndCommit( const KICHAD_IPC_TARGET& aTarget,
                                   const std::string& aCommitId, bool aCommit,
                                   const std::string& aMessage, std::string& aError ) const
{
    kiapi::common::commands::EndCommit request;
    request.mutable_id()->set_value( aCommitId );
    request.set_action( aCommit ? kiapi::common::commands::CMA_COMMIT
                                : kiapi::common::commands::CMA_DROP );
    request.set_message( aMessage );

    kiapi::common::ApiResponse response;
    return Call( aTarget, request, response, aError );
}


bool KICHAD_IPC_CLIENT::callSocket( const std::string& aSocketUrl,
                                    const std::string& aKicadToken,
                                    const google::protobuf::Message& aRequest,
                                    kiapi::common::ApiResponse& aResponse,
                                    std::string& aError,
                                    std::chrono::milliseconds aTimeout ) const
{
    if( aSocketUrl.empty() || m_clientName.empty() )
    {
        aError = "the KiCad IPC target or client name is empty";
        return false;
    }

    kiapi::common::ApiRequest envelope;
    envelope.mutable_header()->set_client_name( m_clientName );
    envelope.mutable_header()->set_kicad_token( aKicadToken );
    envelope.mutable_message()->PackFrom( aRequest );

    KINNG_REQUEST_RESULT transport = KINNG_REQUEST_CLIENT::Request(
            aSocketUrl, envelope.SerializeAsString(), aTimeout );

    if( !transport.success )
    {
        aError = transport.errorMessage;
        return false;
    }

    if( !aResponse.ParseFromString( transport.response ) )
    {
        aError = "KiCad returned an invalid protobuf response";
        return false;
    }

    if( !aKicadToken.empty() && aResponse.header().kicad_token() != aKicadToken )
    {
        aError = "the KiCad IPC instance token changed";
        return false;
    }

    if( aResponse.status().status() != kiapi::common::AS_OK )
    {
        aError = aResponse.status().error_message();

        if( aError.empty() )
            aError = "KiCad rejected the IPC request with status "
                     + std::to_string( aResponse.status().status() );

        return false;
    }

    return true;
}
