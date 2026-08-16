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

#ifndef KICHAD_CODEX_APP_SERVER_CLIENT_H
#define KICHAD_CODEX_APP_SERVER_CLIENT_H

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <string>

#include <nlohmann/json.hpp>
#include <wx/event.h>
#include <wx/process.h>
#include <wx/timer.h>


/**
 * Small JSON-RPC transport for the Codex app-server stdio protocol.
 *
 * The app-server is the inference/session process required by Codex.  KiChad does not launch a
 * separate tool server: server-initiated dynamic tool calls arrive through this transport and are
 * handled by the KiChad host.
 */
class CODEX_APP_SERVER_CLIENT : public wxEvtHandler
{
public:
    using JSON = nlohmann::json;
    using RESPONSE_HANDLER = std::function<void( const JSON& )>;
    using MESSAGE_HANDLER = std::function<void( const JSON& )>;
    using STATE_HANDLER = std::function<void( bool, const wxString& )>;

    CODEX_APP_SERVER_CLIENT();
    ~CODEX_APP_SERVER_CLIENT() override;

    bool Start();
    void Stop();

    bool IsRunning() const { return m_process != nullptr && m_pid > 0; }

    int64_t SendRequest( const std::string& aMethod, const JSON& aParams,
                         RESPONSE_HANDLER aHandler = {} );
    bool SendNotification( const std::string& aMethod, const JSON& aParams = JSON::object() );
    bool SendResponse( const JSON& aId, const JSON& aResult, bool aSensitive = false );
    bool SendError( const JSON& aId, int aCode, const std::string& aMessage );

    void SetMessageHandler( MESSAGE_HANDLER aHandler ) { m_messageHandler = std::move( aHandler ); }
    void SetStateHandler( STATE_HANDLER aHandler ) { m_stateHandler = std::move( aHandler ); }

private:
    struct OUTBOUND_FRAME
    {
        std::string                                  payload;
        size_t                                       offset;
        std::chrono::steady_clock::time_point        lastProgress;
    };

    static constexpr int POLL_INTERVAL_MS = 25;
    static constexpr size_t MAX_JSONRPC_MESSAGE_BYTES = 16 * 1024 * 1024;
    static constexpr size_t MAX_OUTBOUND_QUEUE_BYTES = 64 * 1024 * 1024;
    static constexpr size_t MAX_WRITE_CHUNK_BYTES = 16 * 1024;
    static constexpr size_t MAX_WRITE_BYTES_PER_POLL = 64 * 1024;

    bool writeMessage( const JSON& aMessage, bool aSensitive = false );
    bool drainOutput();
    void consumeStream( wxInputStream* aStream, std::string& aBuffer, bool aParseJson );
    void dispatchLine( const std::string& aLine );
    void setState( bool aRunning, const wxString& aDetail );
    void onPoll( wxTimerEvent& aEvent );
    void onProcessTerminated( wxProcessEvent& aEvent );

    wxProcess*                         m_process;
    long                               m_pid;
    wxTimer                            m_pollTimer;
    int64_t                            m_nextRequestId;
    std::string                        m_stdoutBuffer;
    std::string                        m_stderrBuffer;
    std::deque<OUTBOUND_FRAME>         m_outboundFrames;
    size_t                             m_outboundQueuedBytes;
    bool                               m_outputDeferred;
    std::map<int64_t, RESPONSE_HANDLER> m_pendingRequests;
    MESSAGE_HANDLER                    m_messageHandler;
    STATE_HANDLER                      m_stateHandler;
};

#endif // KICHAD_CODEX_APP_SERVER_CLIENT_H
