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

#include "codex_app_server_client.h"
#include "codex_paths.h"
#include "codex_protocol_log.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include <wx/filename.h>
#include <wx/log.h>
#include <wx/intl.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>


class CODEX_PROCESS : public wxProcess
{
public:
    explicit CODEX_PROCESS( wxEvtHandler* aParent ) : wxProcess( aParent ), m_selfDelete( false ) {}

    void SelfDeleteOnTerminate()
    {
        m_selfDelete = true;
        Detach();
    }

    void OnTerminate( int aPid, int aStatus ) override
    {
        if( m_selfDelete )
            delete this;
        else
            wxProcess::OnTerminate( aPid, aStatus );
    }

private:
    bool m_selfDelete;
};


CODEX_APP_SERVER_CLIENT::CODEX_APP_SERVER_CLIENT() :
        m_process( nullptr ),
        m_pid( 0 ),
        m_pollTimer( this ),
        m_nextRequestId( 1 ),
        m_outboundQueuedBytes( 0 ),
        m_outputDeferred( false )
{
    Bind( wxEVT_TIMER, &CODEX_APP_SERVER_CLIENT::onPoll, this, m_pollTimer.GetId() );
    Bind( wxEVT_END_PROCESS, &CODEX_APP_SERVER_CLIENT::onProcessTerminated, this );
}


CODEX_APP_SERVER_CLIENT::~CODEX_APP_SERVER_CLIENT()
{
    Stop();
    Unbind( wxEVT_TIMER, &CODEX_APP_SERVER_CLIENT::onPoll, this, m_pollTimer.GetId() );
    Unbind( wxEVT_END_PROCESS, &CODEX_APP_SERVER_CLIENT::onProcessTerminated, this );
}


bool CODEX_APP_SERVER_CLIENT::Start()
{
    if( IsRunning() )
        return true;

    wxString executable;

    if( !wxGetEnv( wxS( "KICHAD_CODEX_EXECUTABLE" ), &executable ) || executable.IsEmpty() )
    {
#ifdef __WXMSW__
        const wxString codexName = wxS( "codex.exe" );
#else
        const wxString codexName = wxS( "codex" );
#endif
        wxFileName runningExecutable( wxStandardPaths::Get().GetExecutablePath() );
        wxFileName bundledCodex( runningExecutable.GetPath(), codexName );

        // Distribution packages place Codex next to the real KiChad executable.  Prefer that
        // exact executable over PATH so another Codex installation cannot silently change the
        // app-server protocol underneath KiChad.  Developer builds retain the PATH fallback.
        if( bundledCodex.FileExists() && bundledCodex.IsFileExecutable() )
            executable = bundledCodex.GetFullPath();
        else
            executable = codexName;
    }

    wxString codexHome;

    if( !wxGetEnv( wxS( "KICHAD_CODEX_HOME" ), &codexHome ) || codexHome.IsEmpty() )
        codexHome = KICHAD::CODEX_PATHS::AppServerHome();

    if( !wxFileName::DirName( codexHome ).IsAbsolute() )
    {
        setState( false, _( "KICHAD_CODEX_HOME must be an absolute path." ) );
        return false;
    }

    if( !wxFileName::Mkdir( codexHome, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL )
        && !wxFileName::DirExists( codexHome ) )
    {
        setState( false, wxString::Format( _( "Could not create the Codex state directory '%s'." ),
                                           codexHome ) );
        return false;
    }

    KICHAD::CODEX_PROTOCOL_LOG::Event(
            "app_server_starting",
            { { "executable", std::string( executable.ToUTF8() ) },
              { "codexHome", std::string( codexHome.ToUTF8() ) } } );

    // Keep the owned inference process isolated from arbitrary MCP servers and broad Codex tools
    // in the user's global configuration.  KiChad supplies its complete tool surface through
    // app-server dynamic tools on this same stdio connection.
    std::vector<wxString> argumentStorage = {
        executable,
        wxS( "app-server" ),
        wxS( "-c" ),
        wxS( "mcp_servers={}" ),
        wxS( "-c" ),
        wxS( "web_search=\"live\"" ),
        wxS( "-c" ),
        wxS( "tools.web_search.context_size=\"high\"" ),
        wxS( "--disable" ),
        wxS( "shell_tool" ),
        wxS( "--disable" ),
        wxS( "unified_exec" ),
        wxS( "--disable" ),
        wxS( "apps" ),
        wxS( "--disable" ),
        wxS( "browser_use" ),
        wxS( "--disable" ),
        wxS( "computer_use" ),
        wxS( "--disable" ),
        wxS( "image_generation" ),
        wxS( "--disable" ),
        wxS( "multi_agent" ),
        wxS( "--disable" ),
        wxS( "multi_agent_v2" ),
        wxS( "--disable" ),
        wxS( "plugins" ),
        wxS( "--disable" ),
        wxS( "tool_suggest" ),
        wxS( "--disable" ),
        wxS( "enable_mcp_apps" ),
        wxS( "--disable" ),
        wxS( "hooks" ),
        wxS( "--disable" ),
        wxS( "skill_mcp_dependency_install" ),
        wxS( "--disable" ),
        wxS( "workspace_dependencies" )
    };
    std::vector<const wchar_t*> arguments;

    for( const wxString& argument : argumentStorage )
        arguments.emplace_back( argument.wc_str() );

    arguments.emplace_back( nullptr );

    m_process = new CODEX_PROCESS( this );
    m_process->Redirect();

    wxExecuteEnv environment;
    wxGetEnvMap( &environment.env );
    environment.env[wxS( "CODEX_HOME" )] = codexHome;
    m_pid = wxExecute( arguments.data(), wxEXEC_ASYNC, m_process, &environment );

    if( m_pid == 0 )
    {
        KICHAD::CODEX_PROTOCOL_LOG::Event(
                "app_server_start_failed",
                { { "executable", std::string( executable.ToUTF8() ) } } );
        delete m_process;
        m_process = nullptr;
        setState( false, wxString::Format( _( "Could not start Codex executable '%s'." ),
                                           executable ) );
        return false;
    }

    m_stdoutBuffer.clear();
    m_stderrBuffer.clear();
    m_outboundFrames.clear();
    m_outboundQueuedBytes = 0;
    m_outputDeferred = false;
    m_pendingRequests.clear();
    m_pollTimer.Start( POLL_INTERVAL_MS );
    KICHAD::CODEX_PROTOCOL_LOG::Event( "app_server_started", { { "pid", m_pid } } );
    setState( true, _( "Connecting to Codex..." ) );
    return true;
}


void CODEX_APP_SERVER_CLIENT::Stop()
{
    m_pollTimer.Stop();
    m_pendingRequests.clear();
    m_outboundFrames.clear();
    m_outboundQueuedBytes = 0;
    m_outputDeferred = false;

    if( !m_process )
        return;

    KICHAD::CODEX_PROTOCOL_LOG::Event( "app_server_stopping", { { "pid", m_pid } } );

    CODEX_PROCESS* process = static_cast<CODEX_PROCESS*>( m_process );
    process->SelfDeleteOnTerminate();

    // Closing stdin asks app-server to finish cleanly.  Terminate only this exact child if it does
    // not observe EOF before the host is torn down.
    process->CloseOutput();

    if( m_pid > 0 && wxProcess::Exists( m_pid ) )
        wxProcess::Kill( static_cast<int>( m_pid ), wxSIGTERM, wxKILL_NOCHILDREN );

    m_process = nullptr;
    m_pid = 0;
}


int64_t CODEX_APP_SERVER_CLIENT::SendRequest( const std::string& aMethod, const JSON& aParams,
                                              RESPONSE_HANDLER aHandler )
{
    const int64_t requestId = m_nextRequestId++;
    JSON          message = { { "id", requestId }, { "method", aMethod }, { "params", aParams } };

    if( !writeMessage( message ) )
        return 0;

    if( aHandler )
        m_pendingRequests.emplace( requestId, std::move( aHandler ) );

    return requestId;
}


bool CODEX_APP_SERVER_CLIENT::SendNotification( const std::string& aMethod, const JSON& aParams )
{
    JSON message = { { "method", aMethod } };

    if( !aParams.empty() )
        message["params"] = aParams;

    return writeMessage( message );
}


bool CODEX_APP_SERVER_CLIENT::SendResponse( const JSON& aId, const JSON& aResult,
                                            bool aSensitive )
{
    return writeMessage( { { "id", aId }, { "result", aResult } }, aSensitive );
}


bool CODEX_APP_SERVER_CLIENT::SendError( const JSON& aId, int aCode, const std::string& aMessage )
{
    return writeMessage( { { "id", aId },
                           { "error", { { "code", aCode }, { "message", aMessage } } } } );
}


bool CODEX_APP_SERVER_CLIENT::writeMessage( const JSON& aMessage, bool aSensitive )
{
    const JSON loggedMessage = aSensitive
                                       ? JSON( { { "id", aMessage.value( "id", JSON() ) },
                                                 { "result", "[REDACTED]" } } )
                                       : aMessage;

    if( !IsRunning() || !m_process->GetOutputStream() )
    {
        KICHAD::CODEX_PROTOCOL_LOG::Event( "protocol_write_failed",
                                           { { "reason", "app-server input unavailable" },
                                             { "message", loggedMessage } } );
        return false;
    }

    const std::string serialized = aMessage.dump() + "\n";

    if( serialized.size() > MAX_JSONRPC_MESSAGE_BYTES )
    {
        KICHAD::CODEX_PROTOCOL_LOG::Event(
                "protocol_write_failed",
                { { "reason", "JSON-RPC message exceeds bounded transport size" },
                  { "bytes", serialized.size() } } );
        return false;
    }

    if( serialized.size() > MAX_OUTBOUND_QUEUE_BYTES - m_outboundQueuedBytes )
    {
        KICHAD::CODEX_PROTOCOL_LOG::Event(
                "protocol_write_failed",
                { { "reason", "bounded app-server output queue is full" },
                  { "queuedBytes", m_outboundQueuedBytes },
                  { "messageBytes", serialized.size() },
                  { "maximumQueuedBytes", MAX_OUTBOUND_QUEUE_BYTES } } );
        setState( false, _( "Codex app-server output queue is full." ) );
        return false;
    }

    KICHAD::CODEX_PROTOCOL_LOG::Protocol( "outbound", loggedMessage );
    m_outboundQueuedBytes += serialized.size();
    m_outboundFrames.push_back(
            { serialized, 0, std::chrono::steady_clock::now() } );
    return drainOutput();
}


bool CODEX_APP_SERVER_CLIENT::drainOutput()
{
    if( m_outboundFrames.empty() )
        return true;

    if( !IsRunning() || !m_process->GetOutputStream() )
    {
        KICHAD::CODEX_PROTOCOL_LOG::Event(
                "protocol_write_failed",
                { { "reason", "app-server input unavailable while output was queued" },
                  { "queuedBytes", m_outboundQueuedBytes } } );
        return false;
    }

    wxOutputStream* stream = m_process->GetOutputStream();
    size_t          writeBudget = MAX_WRITE_BYTES_PER_POLL;

    while( !m_outboundFrames.empty() && writeBudget > 0 )
    {
        OUTBOUND_FRAME& frame = m_outboundFrames.front();
        const size_t remaining = frame.payload.size() - frame.offset;
        const size_t requested = std::min( { remaining, MAX_WRITE_CHUNK_BYTES,
                                             writeBudget } );

        // wx's Unix pipe stream records EAGAIN as a generic write error.  Resetting only the
        // stream status is safe: frame.offset remains the authoritative byte position and the
        // process pipe itself stays open.
        stream->Reset();
        stream->Write( frame.payload.data() + frame.offset, requested );
        const size_t written = stream->LastWrite();
        const bool   writeError = !stream->IsOk();

        if( written > requested )
        {
            KICHAD::CODEX_PROTOCOL_LOG::Event(
                    "protocol_write_failed",
                    { { "reason", "app-server pipe reported an invalid write count" },
                      { "requestedBytes", requested }, { "writtenBytes", written } } );
            setState( false, _( "Codex app-server transport returned an invalid write count." ) );
            return false;
        }

        if( written > 0 )
        {
            frame.offset += written;
            m_outboundQueuedBytes -= written;
            writeBudget -= written;
            frame.lastProgress = std::chrono::steady_clock::now();
        }

        if( frame.offset == frame.payload.size() )
        {
            m_outboundFrames.pop_front();

            if( writeError )
                stream->Reset();

            continue;
        }

        if( written == 0 || writeError )
        {
            stream->Reset();
            const auto stalledFor = std::chrono::steady_clock::now() - frame.lastProgress;

            if( stalledFor >= std::chrono::seconds( 30 ) )
            {
                KICHAD::CODEX_PROTOCOL_LOG::Event(
                        "protocol_write_failed",
                        { { "reason", "app-server input remained blocked for 30 seconds" },
                          { "frameBytesWritten", frame.offset },
                          { "frameBytes", frame.payload.size() },
                          { "queuedBytes", m_outboundQueuedBytes } } );
                setState( false, _( "Codex app-server stopped accepting input." ) );
                return false;
            }

            if( !m_outputDeferred )
            {
                KICHAD::CODEX_PROTOCOL_LOG::Event(
                        "protocol_write_deferred",
                        { { "reason", "app-server pipe backpressure" },
                          { "frameBytesWritten", frame.offset },
                          { "frameBytes", frame.payload.size() },
                          { "queuedBytes", m_outboundQueuedBytes } } );
                m_outputDeferred = true;
            }

            return true;
        }
    }

    if( m_outboundFrames.empty() && m_outputDeferred )
    {
        KICHAD::CODEX_PROTOCOL_LOG::Event( "protocol_write_resumed",
                                           { { "queuedBytes", 0 } } );
        m_outputDeferred = false;
    }

    return true;
}


void CODEX_APP_SERVER_CLIENT::consumeStream( wxInputStream* aStream, std::string& aBuffer,
                                             bool aParseJson )
{
    if( !aStream )
        return;

    char buffer[4096];

    while( aStream->CanRead() )
    {
        aStream->Read( buffer, sizeof( buffer ) );
        aBuffer.append( buffer, aStream->LastRead() );

        if( aBuffer.size() > MAX_JSONRPC_MESSAGE_BYTES
            && aBuffer.find( '\n' ) == std::string::npos )
        {
            KICHAD::CODEX_PROTOCOL_LOG::Event(
                    "protocol_read_failed",
                    { { "reason", "unterminated JSON-RPC message exceeds bounded transport size" },
                      { "bytes", aBuffer.size() } } );
            aBuffer.clear();
            setState( false, _( "Codex app-server returned an oversized protocol message." ) );
            return;
        }
    }

    size_t newline = 0;

    while( ( newline = aBuffer.find( '\n' ) ) != std::string::npos )
    {
        std::string line = aBuffer.substr( 0, newline );
        aBuffer.erase( 0, newline + 1 );

        if( !line.empty() && line.back() == '\r' )
            line.pop_back();

        if( line.empty() )
            continue;

        if( line.size() > MAX_JSONRPC_MESSAGE_BYTES )
        {
            KICHAD::CODEX_PROTOCOL_LOG::Event(
                    "protocol_read_failed",
                    { { "reason", "JSON-RPC message exceeds bounded transport size" },
                      { "bytes", line.size() } } );
            setState( false, _( "Codex app-server returned an oversized protocol message." ) );
            continue;
        }

        if( aParseJson )
            dispatchLine( line );
        else
        {
            KICHAD::CODEX_PROTOCOL_LOG::Text( "app_server_stderr", line );
            wxLogTrace( wxS( "KICHAD_CODEX" ), wxS( "app-server: %s" ), wxString::FromUTF8( line ) );
        }
    }
}


void CODEX_APP_SERVER_CLIENT::dispatchLine( const std::string& aLine )
{
    JSON message = JSON::parse( aLine, nullptr, false );

    if( message.is_discarded() )
    {
        KICHAD::CODEX_PROTOCOL_LOG::Text( "protocol_malformed", aLine );
        setState( false, _( "Codex app-server returned malformed JSON." ) );
        wxLogTrace( wxS( "KICHAD_CODEX" ), wxS( "Malformed app-server JSON: %s" ),
                    wxString::FromUTF8( aLine ) );
        return;
    }

    try
    {
        KICHAD::CODEX_PROTOCOL_LOG::Protocol( "inbound", message );

        if( message.contains( "id" ) && !message.contains( "method" )
            && message["id"].is_number_integer() )
        {
            int64_t requestId = message["id"].get<int64_t>();
            auto    pending = m_pendingRequests.find( requestId );

            if( pending != m_pendingRequests.end() )
            {
                RESPONSE_HANDLER handler = std::move( pending->second );
                m_pendingRequests.erase( pending );
                handler( message );
            }

            return;
        }

        if( m_messageHandler )
            m_messageHandler( message );
    }
    catch( const std::exception& error )
    {
        KICHAD::CODEX_PROTOCOL_LOG::Event( "protocol_dispatch_failed",
                                           { { "error", error.what() },
                                             { "message", message } } );
        setState( IsRunning(), _( "Codex app-server returned an invalid protocol message." ) );
        wxLogTrace( wxS( "KICHAD_CODEX" ), wxS( "Invalid app-server message: %s (%s)" ),
                    wxString::FromUTF8( aLine ), wxString::FromUTF8( error.what() ) );
    }
}


void CODEX_APP_SERVER_CLIENT::setState( bool aRunning, const wxString& aDetail )
{
    if( m_stateHandler )
        m_stateHandler( aRunning, aDetail );
}


void CODEX_APP_SERVER_CLIENT::onPoll( wxTimerEvent& aEvent )
{
    if( !m_process )
        return;

    consumeStream( m_process->GetInputStream(), m_stdoutBuffer, true );
    consumeStream( m_process->GetErrorStream(), m_stderrBuffer, false );
    drainOutput();
}


void CODEX_APP_SERVER_CLIENT::onProcessTerminated( wxProcessEvent& aEvent )
{
    if( !m_process || aEvent.GetPid() != m_pid )
        return;

    consumeStream( m_process->GetInputStream(), m_stdoutBuffer, true );
    consumeStream( m_process->GetErrorStream(), m_stderrBuffer, false );
    m_pollTimer.Stop();

    wxProcess* finished = m_process;
    m_process = nullptr;
    m_pid = 0;
    m_pendingRequests.clear();
    m_outboundFrames.clear();
    m_outboundQueuedBytes = 0;
    m_outputDeferred = false;
    delete finished;

    KICHAD::CODEX_PROTOCOL_LOG::Event(
            "app_server_exited",
            { { "pid", aEvent.GetPid() }, { "exitStatus", aEvent.GetExitCode() } } );

    setState( false, wxString::Format( _( "Codex app-server exited with status %d." ),
                                      aEvent.GetExitCode() ) );
}
