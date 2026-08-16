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

#include "codex_panel.h"

#include "codex_agent_policy.h"
#include "codex_tool_internal.h"
#include "codex_user_input_dialog.h"

#include <bitmaps.h>
#include <build_version.h>

#include <algorithm>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/datetime.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/intl.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/thread.h>
#include <wx/utils.h>
#include <settings/kicad_settings.h>
#include <settings/settings_manager.h>


wxDECLARE_EVENT( KICHAD_CODEX_TOOL_COMPLETED, wxThreadEvent );
wxDEFINE_EVENT( KICHAD_CODEX_TOOL_COMPLETED, wxThreadEvent );
wxDECLARE_EVENT( KICHAD_CODEX_DEPENDENCY_REQUESTED, wxThreadEvent );
wxDEFINE_EVENT( KICHAD_CODEX_DEPENDENCY_REQUESTED, wxThreadEvent );


namespace
{

using JSON = nlohmann::json;


wxString responseErrorMessage( const JSON& aResponse, const wxString& aFallback )
{
    if( !aResponse.contains( "error" ) || !aResponse["error"].is_object() )
        return aFallback;

    const JSON& error = aResponse["error"];

    if( error.contains( "message" ) && error["message"].is_string() )
    {
        const std::string message = error["message"].get<std::string>();

        if( !message.empty() )
            return wxString::FromUTF8( message );
    }

    return aFallback;
}


bool nativeToolFailureSummary( const JSON& aResult, wxString& aCode, wxString& aMessage,
                               wxString& aDetails, wxString& aRecovery,
                               wxString& aStateChanged )
{
    if( !aResult.is_object() || aResult.value( "success", true )
        || !aResult.contains( "contentItems" ) || !aResult["contentItems"].is_array()
        || aResult["contentItems"].empty()
        || !aResult["contentItems"][0].contains( "text" )
        || !aResult["contentItems"][0]["text"].is_string() )
    {
        return false;
    }

    JSON envelope = JSON::parse(
            aResult["contentItems"][0]["text"].get<std::string>(), nullptr, false );

    if( !envelope.is_object() || !envelope.contains( "error" )
        || !envelope["error"].is_object() )
    {
        return false;
    }

    const JSON& error = envelope["error"];
    aCode = wxString::FromUTF8( error.value( "code", "tool_failed" ) );
    aMessage = wxString::FromUTF8( error.value( "message", "Native KiChad tool failed" ) );
    aStateChanged = wxString::FromUTF8( error.value( "stateChanged", "unknown" ) );

    if( error.contains( "details" ) )
    {
        std::string details = error["details"].dump();

        if( details.size() > 2000 )
            details = details.substr( 0, 2000 ) + "...";

        aDetails = wxString::FromUTF8( details );
    }

    if( error.contains( "recovery" ) && error["recovery"].is_object() )
    {
        aRecovery = wxString::FromUTF8(
                error["recovery"].value( "summary", "Inspect the failure before retrying." ) );
    }

    return true;
}


std::vector<CODEX_THREAD_STORE::MESSAGE> conversationMessages( const JSON& aTurns )
{
    std::vector<CODEX_THREAD_STORE::MESSAGE> messages;

    if( !aTurns.is_array() )
        return messages;

    for( const JSON& turn : aTurns )
    {
        if( !turn.is_object() )
            continue;

        for( const JSON& item : turn.value( "items", JSON::array() ) )
        {
            const std::string type = item.value( "type", "" );

            if( type == "userMessage" )
            {
                std::string text;

                for( const JSON& content : item.value( "content", JSON::array() ) )
                {
                    if( content.value( "type", "" ) != "text" )
                        continue;

                    if( !text.empty() )
                        text += "\n";

                    text += content.value( "text", "" );
                }

                if( !text.empty() )
                    messages.push_back( { "user", std::move( text ) } );
            }
            else if( type == "agentMessage" )
            {
                std::string text = item.value( "text", "" );

                if( !text.empty() )
                    messages.push_back( { "assistant", std::move( text ) } );
            }
        }
    }

    return messages;
}

} // namespace


CODEX_PANEL::CODEX_PANEL( wxWindow* aParent, std::function<wxString()> aProjectPathProvider,
                          SNAPSHOT_PROVIDER aSnapshotProvider, RESTORE_HANDLER aRestoreHandler,
                          wxString aPreferredModel, wxString aPreferredReasoningEffort,
                          PREFERENCE_SAVER aPreferenceSaver,
                          APPLICATION_OPENER aApplicationOpener ) :
        wxPanel( aParent ),
        m_projectPathProvider( std::move( aProjectPathProvider ) ),
        m_snapshotProvider( std::move( aSnapshotProvider ) ),
        m_restoreHandler( std::move( aRestoreHandler ) ),
        m_preferenceSaver( std::move( aPreferenceSaver ) ),
        m_applicationOpener( std::move( aApplicationOpener ) ),
        m_toolRegistry( m_projectPathProvider,
                        [this]() { return !m_turnSnapshotHash.IsEmpty(); },
                        {},
                        []( const wxFileName& aRoot, const JSON& aCompilerIr,
                            const JSON& aResolvedSymbols, std::string& aError )
                        {
                            return KICHAD::CODEX_TOOLS::ValidateNativeSchematicHierarchy(
                                    aRoot, aCompilerIr, aResolvedSymbols, aError );
                        } ),
        m_status( nullptr ),
        m_processStatus( nullptr ),
        m_loginButton( nullptr ),
        m_deviceLoginButton( nullptr ),
        m_cancelLoginButton( nullptr ),
        m_modelChoice( nullptr ),
        m_reasoningChoice( nullptr ),
        m_transcript( nullptr ),
        m_activity( nullptr ),
        m_input( nullptr ),
        m_sendButton( nullptr ),
        m_stopButton( nullptr ),
        m_revertButton( nullptr ),
        m_newConversationButton( nullptr ),
        m_userInputDialog( nullptr ),
        m_externalLayoutMode( false ),
        m_preferredModel( std::move( aPreferredModel ) ),
        m_preferredReasoningEffort( std::move( aPreferredReasoningEffort ) ),
        m_shuttingDown( false ),
        m_nextToolTaskId( 1 ),
        m_nextSteerMessageId( 1 ),
        m_initialized( false ),
        m_authenticated( false ),
        m_conversationLoaded( false ),
        m_threadPreparing( false ),
        m_reasoningSummaryOpen( false ),
        m_agentResponseOpen( false )
{
    wxBoxSizer* root = new wxBoxSizer( wxVERTICAL );

    wxBoxSizer* statusRow = new wxBoxSizer( wxHORIZONTAL );
    m_status = new wxStaticText( this, wxID_ANY, _( "Codex is starting..." ) );
    m_loginButton = new wxButton( this, wxID_ANY, _( "Sign in" ) );
    m_deviceLoginButton = new wxButton( this, wxID_ANY, _( "Device code" ) );
    m_cancelLoginButton = new wxButton( this, wxID_ANY, _( "Cancel" ) );
    m_loginButton->Disable();
    m_deviceLoginButton->Disable();
    m_cancelLoginButton->Disable();
    statusRow->Add( m_status, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 8 ) );
    statusRow->Add( m_loginButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 6 ) );
    statusRow->Add( m_deviceLoginButton, 0,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 6 ) );
    statusRow->Add( m_cancelLoginButton, 0, wxALIGN_CENTER_VERTICAL );
    root->Add( statusRow, 0, wxEXPAND | wxALL, FromDIP( 8 ) );

    wxBoxSizer* conversationRow = new wxBoxSizer( wxHORIZONTAL );
    m_processStatus = new wxStaticText( this, wxID_ANY, _( "Codex service: connecting..." ) );
    m_revertButton = new wxButton( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                   wxDefaultSize, wxBU_EXACTFIT );
    m_revertButton->SetBitmap( KiBitmapBundle( BITMAPS::undo ) );
    m_revertButton->SetToolTip( _( "Restore the project to before the last Codex turn" ) );
    m_revertButton->SetName( _( "Undo Codex changes" ) );
    m_revertButton->Disable();
    m_newConversationButton = new wxButton( this, wxID_ANY, wxS( "+" ), wxDefaultPosition,
                                            wxDefaultSize, wxBU_EXACTFIT );
    m_newConversationButton->SetMinSize( wxSize( FromDIP( 28 ), FromDIP( 28 ) ) );
    m_newConversationButton->SetToolTip( _( "Start a new Codex conversation" ) );
    m_newConversationButton->SetName( _( "New conversation" ) );
    m_newConversationButton->Disable();
    conversationRow->Add( m_processStatus, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 8 ) );
    conversationRow->Add( m_revertButton, 0,
                          wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 4 ) );
    conversationRow->Add( m_newConversationButton, 0, wxALIGN_CENTER_VERTICAL );
    root->Add( conversationRow, 0,
               wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    wxBoxSizer* modelRow = new wxBoxSizer( wxHORIZONTAL );
    m_modelChoice = new wxChoice( this, wxID_ANY );
    m_reasoningChoice = new wxChoice( this, wxID_ANY );
    m_modelChoice->Append( _( "Loading models..." ) );
    m_modelChoice->SetSelection( 0 );
    m_modelChoice->Disable();
    m_reasoningChoice->Append( _( "Default reasoning" ) );
    m_reasoningChoice->SetSelection( 0 );
    m_reasoningChoice->Disable();
    modelRow->Add( m_modelChoice, 1, wxRIGHT, FromDIP( 6 ) );
    modelRow->Add( m_reasoningChoice, 1 );
    root->Add( modelRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    root->Add( new wxStaticLine( this ), 0, wxEXPAND );

    wxNotebook* transcriptBook = new wxNotebook( this, wxID_ANY );
    m_transcript = new wxTextCtrl( transcriptBook, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                   wxDefaultSize,
                                   wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_BESTWRAP );
    m_activity = new wxTextCtrl( transcriptBook, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                 wxDefaultSize,
                                 wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_BESTWRAP );
    transcriptBook->AddPage( m_transcript, _( "Conversation" ), true );
    transcriptBook->AddPage( m_activity, _( "Activity" ) );
    root->Add( transcriptBook, 1, wxEXPAND | wxALL, FromDIP( 8 ) );

    m_input = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              FromDIP( wxSize( -1, 90 ) ), wxTE_MULTILINE | wxTE_PROCESS_ENTER );
    m_input->SetHint( _( "Describe the board you want Codex to design..." ) );
    root->Add( m_input, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    wxBoxSizer* actionRow = new wxBoxSizer( wxHORIZONTAL );
    m_stopButton = new wxButton( this, wxID_ANY, _( "Stop" ) );
    m_sendButton = new wxButton( this, wxID_ANY, _( "Send" ) );
    m_stopButton->Disable();
    m_sendButton->Disable();
    actionRow->AddStretchSpacer();
    actionRow->Add( m_stopButton, 0, wxRIGHT, FromDIP( 6 ) );
    actionRow->Add( m_sendButton );
    root->Add( actionRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    SetSizer( root );

    m_loginButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onLogin, this );
    m_deviceLoginButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onDeviceLogin, this );
    m_cancelLoginButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onCancelLogin, this );
    m_sendButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onSend, this );
    m_stopButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onStop, this );
    m_revertButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onRevertTurn, this );
    m_newConversationButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onNewConversation, this );
    m_modelChoice->Bind( wxEVT_CHOICE, &CODEX_PANEL::onModelChanged, this );
    m_reasoningChoice->Bind( wxEVT_CHOICE, &CODEX_PANEL::onReasoningChanged, this );
    Bind( KICHAD_CODEX_TOOL_COMPLETED, &CODEX_PANEL::onToolCompleted, this );
    Bind( KICHAD_CODEX_DEPENDENCY_REQUESTED,
          &CODEX_PANEL::onRuntimeDependencyRequested, this );

    RefreshExternalLayoutSettings();

    m_client.SetMessageHandler( [this]( const JSON& aMessage ) { onAppServerMessage( aMessage ); } );
    m_client.SetStateHandler(
            [this]( bool aRunning, const wxString& aDetail )
            {
                onAppServerState( aRunning, aDetail );
            } );

    if( m_client.Start() )
        initializeAppServer();
}


CODEX_PANEL::~CODEX_PANEL()
{
    m_shuttingDown.store( true );
    clearUserInputRequests();
    m_pendingSteers.clear();
    m_client.SetMessageHandler( {} );
    m_client.SetStateHandler( {} );

    {
        std::lock_guard<std::mutex> dependencyLock( m_dependencyRequestMutex );

        if( m_pendingDependencyRequest )
        {
            std::lock_guard<std::mutex> requestLock( m_pendingDependencyRequest->mutex );
            m_pendingDependencyRequest->completed = true;
            m_pendingDependencyRequest->success = false;
            m_pendingDependencyRequest->detail = _( "KiChad is shutting down" );
            m_pendingDependencyRequest->condition.notify_all();
        }
    }

    // Synchronize with the worker's final shutdown check before wxPanel destruction begins.
    {
        std::lock_guard<std::mutex> lock( m_toolEventMutex );
    }

    for( auto& entry : m_toolWorkers )
    {
        if( entry.second.joinable() )
            entry.second.join();
    }

    m_toolWorkers.clear();
    m_toolRequestIds.clear();
    DeletePendingEvents();
    Unbind( KICHAD_CODEX_TOOL_COMPLETED, &CODEX_PANEL::onToolCompleted, this );
    Unbind( KICHAD_CODEX_DEPENDENCY_REQUESTED,
            &CODEX_PANEL::onRuntimeDependencyRequested, this );
}


void CODEX_PANEL::initializeAppServer()
{
    JSON params = {
        { "clientInfo",
          { { "name", "kichad" }, { "title", "KiChad" },
            { "version", std::string( GetBuildVersion().ToUTF8() ) } } },
        { "capabilities", { { "experimentalApi", true }, { "requestAttestation", false } } }
    };

    m_client.SendRequest(
            "initialize", params,
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "error" ) )
                {
                    setStatus( _( "Codex protocol initialization failed." ) );
                    return;
                }

                m_initialized = true;
                m_client.SendNotification( "initialized" );
                setStatus( _( "Checking Codex account..." ) );
                readAccount();
                readModels();
            } );
}


void CODEX_PANEL::readAccount( bool aRefreshToken )
{
    m_client.SendRequest(
            "account/read", { { "refreshToken", aRefreshToken } },
            [this]( const JSON& aResponse )
            {
                if( !aResponse.contains( "result" ) )
                {
                    setStatus( _( "Could not read the Codex account." ) );
                    setLoginPending( false );
                    return;
                }

                const JSON& account = aResponse["result"].value( "account", JSON() );
                m_authenticated = account.is_object()
                                  && account.value( "type", "" ) == "chatgpt";

                if( m_authenticated )
                {
                    std::string email;

                    if( account.contains( "email" ) && account["email"].is_string() )
                        email = account["email"].get<std::string>();

                    m_loginId.clear();
                    setStatus( email.empty() ? _( "Signed in with ChatGPT" )
                                             : wxString::Format( _( "ChatGPT: %s" ),
                                                                 wxString::FromUTF8( email ) ) );
                    m_loginButton->SetLabel( _( "Sign out" ) );
                    loadSavedConversation();
                }
                else
                {
                    setStatus( _( "Sign in with ChatGPT to use Codex." ) );
                    m_loginButton->SetLabel( _( "Sign in" ) );
                    m_sendButton->Disable();
                }

                setLoginPending( false );
                setBusy( !m_turnId.empty() );
            } );
}


void CODEX_PANEL::readModels()
{
    m_client.SendRequest(
            "model/list", { { "includeHidden", false } },
            [this]( const JSON& aResponse )
            {
                m_models.clear();
                m_modelChoice->Clear();

                if( aResponse.contains( "result" ) && aResponse["result"].contains( "data" ) )
                {
                    for( const JSON& model : aResponse["result"]["data"] )
                    {
                        if( !model.value( "hidden", false ) )
                        {
                            m_models.emplace_back( model );
                            m_modelChoice->Append( wxString::FromUTF8( model.value( "displayName",
                                                                                  model.value( "model", "" ) ) ) );
                        }
                    }
                }

                if( m_models.empty() )
                {
                    m_modelChoice->Append( _( "No models available" ) );
                    m_modelChoice->SetSelection( 0 );
                    m_modelChoice->Disable();
                    return;
                }

                size_t defaultIndex = 0;

                for( size_t i = 0; i < m_models.size(); ++i )
                {
                    const wxString modelId =
                            wxString::FromUTF8( m_models[i].value( "model", "" ) );

                    if( !m_preferredModel.IsEmpty() && modelId == m_preferredModel )
                    {
                        defaultIndex = i;
                        break;
                    }

                    if( m_preferredModel.IsEmpty() && m_models[i].value( "isDefault", false ) )
                    {
                        defaultIndex = i;
                        break;
                    }
                }

                m_modelChoice->SetSelection( static_cast<int>( defaultIndex ) );
                m_modelChoice->Enable();
                updateReasoningChoices();
            } );
}


void CODEX_PANEL::updateReasoningChoices()
{
    m_reasoningChoice->Clear();

    int selection = m_modelChoice->GetSelection();

    if( selection < 0 || static_cast<size_t>( selection ) >= m_models.size() )
    {
        m_reasoningChoice->Append( _( "Default reasoning" ) );
        m_reasoningChoice->SetSelection( 0 );
        m_reasoningChoice->Disable();
        return;
    }

    const JSON& model = m_models[selection];
    std::string defaultEffort = model.value( "defaultReasoningEffort", "" );
    int         defaultSelection = 0;
    bool        preferredFound = false;

    for( const JSON& option : model.value( "supportedReasoningEfforts", JSON::array() ) )
    {
        std::string effort = option.value( "reasoningEffort", "" );

        if( effort.empty() )
            continue;

        int index = m_reasoningChoice->Append( wxString::FromUTF8( effort ) );

        if( !m_preferredReasoningEffort.IsEmpty()
            && wxString::FromUTF8( effort ) == m_preferredReasoningEffort )
        {
            defaultSelection = index;
            preferredFound = true;
        }
        else if( !preferredFound && effort == defaultEffort )
        {
            defaultSelection = index;
        }
    }

    if( m_reasoningChoice->GetCount() == 0 )
        m_reasoningChoice->Append( _( "Default reasoning" ) );

    m_reasoningChoice->SetSelection( defaultSelection );
    m_reasoningChoice->Enable( m_reasoningChoice->GetCount() > 1 );
}


void CODEX_PANEL::savePreferences()
{
    if( m_preferenceSaver )
        m_preferenceSaver( m_preferredModel, m_preferredReasoningEffort );
}


void CODEX_PANEL::loadSavedConversation()
{
    selectProjectThread();

    if( m_conversationLoaded || m_threadPreparing || !m_initialized || !m_authenticated )
        return;

    if( !m_conversationHistory.empty() )
    {
        renderConversation();
        m_conversationLoaded = true;
        setStatus( _( "Saved Codex conversation loaded." ) );
        setBusy( false );
        return;
    }

    if( m_savedThreadId.empty() )
    {
        m_conversationLoaded = true;
        setBusy( false );
        return;
    }

    const wxString project = m_threadProjectPath;
    const std::string savedThreadId = m_savedThreadId;
    m_threadPreparing = true;
    setStatus( _( "Loading saved Codex conversation..." ) );
    setBusy( false );

    m_client.SendRequest(
            "thread/read", { { "threadId", savedThreadId }, { "includeTurns", true } },
            [this, project, savedThreadId]( const JSON& aResponse )
            {
                if( project != m_threadProjectPath || savedThreadId != m_savedThreadId )
                    return;

                m_threadPreparing = false;

                if( !aResponse.contains( "result" ) )
                {
                    appendTranscript( wxS( "\n[" )
                                      + responseErrorMessage(
                                                aResponse,
                                                _( "Could not load the saved Codex "
                                                   "conversation." ) )
                                      + _( " The saved conversation was preserved. Press Send "
                                           "to retry, or use the new conversation button only "
                                           "if you want to replace it.]\n" ) );
                    setStatus( _( "Saved Codex conversation could not be loaded." ) );
                    setBusy( false );
                    return;
                }

                const JSON& thread = aResponse["result"].value( "thread", JSON::object() );
                m_conversationHistory =
                        conversationMessages( thread.value( "turns", JSON::array() ) );
                m_conversationLoaded = true;
                renderConversation();
                persistConversation();
                setStatus( _( "Saved Codex conversation loaded." ) );
                setBusy( false );
            } );
}


void CODEX_PANEL::renderConversation()
{
    m_transcript->Clear();

    for( const CODEX_THREAD_STORE::MESSAGE& message : m_conversationHistory )
    {
        const wxString text = wxString::FromUTF8( message.text );

        if( message.role == "user" )
            appendTranscript( wxString::Format( _( "\nYou: %s\n" ), text ) );
        else if( message.role == "assistant" )
            appendTranscript( wxString::Format( _( "\nCodex: %s\n" ), text ) );
    }
}


void CODEX_PANEL::persistConversation()
{
    const std::string& persistedThreadId =
            m_threadId.empty() ? m_savedThreadId : m_threadId;

    if( persistedThreadId.empty() )
        return;

    wxString error;
    CODEX_THREAD_STORE::BINDING binding = {
        persistedThreadId, CODEX_TOOL_REGISTRY::SCHEMA_VERSION, m_conversationHistory
    };

    if( !m_threadStore.Save( m_threadProjectPath, binding, &error ) )
    {
        appendTranscript( wxString::Format( _( "\n[Conversation persistence: %s]\n" ),
                                            error ) );
    }
}


CODEX_PANEL::JSON CODEX_PANEL::conversationHistoryItems() const
{
    JSON items = JSON::array();

    for( const CODEX_THREAD_STORE::MESSAGE& message : m_conversationHistory )
    {
        if( message.role == "user" )
        {
            items.push_back( {
                { "type", "message" },
                { "role", "user" },
                { "content", JSON::array( { { { "type", "input_text" },
                                               { "text", message.text } } } ) }
            } );
        }
        else if( message.role == "assistant" )
        {
            items.push_back( {
                { "type", "message" },
                { "role", "assistant" },
                { "content", JSON::array( { { { "type", "output_text" },
                                               { "text", message.text } } } ) }
            } );
        }
    }

    return items;
}


void CODEX_PANEL::ensureThreadLoaded( const wxString& aDisplayedMessage,
                                     std::function<void()> aReadyHandler )
{
    wxUnusedVar( aDisplayedMessage );

    if( m_threadId.empty() )
        startThread( std::move( aReadyHandler ) );
    else
        aReadyHandler();
}


void CODEX_PANEL::startThread( std::function<void()> aReadyHandler )
{
    m_threadPreparing = true;
    setBusy( true );

    wxString cwd = projectPath();

    JSON params = {
        { "cwd", std::string( cwd.ToUTF8() ) },
        { "runtimeWorkspaceRoots", JSON::array( { std::string( cwd.ToUTF8() ) } ) },
        { "approvalPolicy", "never" },
        { "allowProviderModelFallback", false },
        { "sandbox", "read-only" },
        { "ephemeral", false },
        // Legacy history lets thread/read return complete turns for the one-time import into
        // KiChad's project-scoped conversation store.  A fresh tool-enabled app-server thread is
        // seeded from that store after each cold launch.
        { "historyMode", "legacy" },
        { "serviceName", "KiChad" },
        { "baseInstructions", m_externalLayoutMode
                                      ? std::string( KICHAD::CODEX_AGENT_POLICY::BaseInstructions() )
                                                + "\n\n"
                                                + KICHAD::CODEX_AGENT_POLICY::ExternalLayoutPolicy()
                                      : std::string( KICHAD::CODEX_AGENT_POLICY::BaseInstructions() ) },
        { "developerInstructions", KICHAD::CODEX_AGENT_POLICY::DeveloperInstructions() },
        { "config", KICHAD::CODEX_AGENT_POLICY::ThreadConfig() }
    };

    params["dynamicTools"] = m_toolRegistry.Specs();

    int modelSelection = m_modelChoice->GetSelection();

    if( modelSelection >= 0 && static_cast<size_t>( modelSelection ) < m_models.size() )
        params["model"] = m_models[modelSelection].value( "model", "" );

    m_client.SendRequest(
            "thread/start", params,
            [this, aReadyHandler = std::move( aReadyHandler )]( const JSON& aResponse )
            {
                if( !aResponse.contains( "result" ) )
                {
                    appendTranscript( wxS( "\n[" )
                                      + responseErrorMessage(
                                                aResponse,
                                                _( "Could not start a persistent Codex "
                                                   "conversation." ) )
                                      + wxS( "]\n" ) );
                    m_threadPreparing = false;
                    setBusy( false );
                    return;
                }

                m_threadId = aResponse["result"]["thread"].value( "id", "" );

                if( m_threadId.empty() )
                {
                    appendTranscript( _( "\n[Codex returned no conversation identifier.]\n" ) );
                    m_threadPreparing = false;
                    setBusy( false );
                    return;
                }

                JSON history = conversationHistoryItems();

                if( history.empty() )
                {
                    m_savedThreadId = m_threadId;
                    persistConversation();
                    m_threadPreparing = false;
                    aReadyHandler();
                    return;
                }

                const std::string startedThreadId = m_threadId;
                m_client.SendRequest(
                        "thread/inject_items",
                        { { "threadId", startedThreadId }, { "items", std::move( history ) } },
                        [this, startedThreadId,
                         aReadyHandler = std::move( aReadyHandler )]( const JSON& aInjectResponse )
                        {
                            if( startedThreadId != m_threadId )
                                return;

                            if( !aInjectResponse.contains( "result" ) )
                            {
                                appendTranscript( wxS( "\n[" )
                                                  + responseErrorMessage(
                                                            aInjectResponse,
                                                            _( "Could not restore the saved "
                                                               "conversation context." ) )
                                                  + _( " The original conversation remains "
                                                       "preserved; retry Send to try again.]\n" ) );
                                m_threadId.clear();
                                m_threadPreparing = false;
                                setBusy( false );
                                return;
                            }

                            m_savedThreadId = m_threadId;
                            persistConversation();
                            m_threadPreparing = false;
                            aReadyHandler();
                        } );
            } );
}


void CODEX_PANEL::startTurn( const std::string& aMessage )
{
    JSON params = {
        { "threadId", m_threadId },
        { "input", JSON::array( { { { "type", "text" }, { "text", aMessage },
                                     { "text_elements", JSON::array() } } } ) }
    };

    int modelSelection = m_modelChoice->GetSelection();

    if( modelSelection >= 0 && static_cast<size_t>( modelSelection ) < m_models.size() )
        params["model"] = m_models[modelSelection].value( "model", "" );

    int reasoningSelection = m_reasoningChoice->GetSelection();

    if( reasoningSelection >= 0 && m_reasoningChoice->IsEnabled() )
        params["effort"] = std::string( m_reasoningChoice->GetString( reasoningSelection ).ToUTF8() );

    m_conversationHistory.push_back( { "user", aMessage } );
    beginTurnDisplay();
    const int64_t requestId = m_client.SendRequest(
            "turn/start", params,
            [this, aMessage]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                {
                    m_turnId = aResponse["result"]["turn"].value( "id", "" );
                    persistConversation();
                    appendDialogLog( wxS( "USER" ), aMessage );
                }
                else
                {
                    if( !m_conversationHistory.empty()
                        && m_conversationHistory.back().role == "user"
                        && m_conversationHistory.back().text == aMessage )
                    {
                        m_conversationHistory.pop_back();
                    }

                    appendTranscript( wxS( "[" )
                                      + responseErrorMessage(
                                                aResponse, _( "Codex turn failed to start." ) )
                                      + _( " The message was restored.]\n" ) );
                    restoreInputMessage( wxString::FromUTF8( aMessage ) );
                    setBusy( false );
                }
            } );

    if( requestId == 0 )
    {
        if( !m_conversationHistory.empty() && m_conversationHistory.back().role == "user"
            && m_conversationHistory.back().text == aMessage )
        {
            m_conversationHistory.pop_back();
        }

        restoreInputMessage( wxString::FromUTF8( aMessage ) );
        appendTranscript( _( "[Codex turn could not be sent. The message was restored.]\n" ) );
        setBusy( false );
    }
}


void CODEX_PANEL::steerTurn( const wxString& aMessage )
{
    if( m_threadId.empty() || m_turnId.empty() )
    {
        restoreInputMessage( aMessage );
        setStatus( _( "The active Codex turn ended before steering could be sent." ) );
        return;
    }

    const uint64_t messageId = m_nextSteerMessageId++;
    const std::string expectedTurnId = m_turnId;
    const std::string clientMessageId = "kichad-steer-" + std::to_string( messageId );
    const std::string utf8Message( aMessage.ToUTF8() );
    JSON params = {
        { "threadId", m_threadId },
        { "expectedTurnId", expectedTurnId },
        { "clientUserMessageId", clientMessageId },
        { "input", JSON::array( { { { "type", "text" }, { "text", utf8Message },
                                     { "text_elements", JSON::array() } } } ) }
    };

    m_pendingSteers.emplace( messageId, PENDING_STEER{ expectedTurnId, aMessage } );
    const int64_t requestId = m_client.SendRequest(
            "turn/steer", params,
            [this, messageId]( const JSON& aResponse )
            {
                auto pending = m_pendingSteers.find( messageId );

                if( pending == m_pendingSteers.end() )
                    return;

                const PENDING_STEER steer = pending->second;
                m_pendingSteers.erase( pending );
                const bool accepted = aResponse.contains( "result" )
                                      && aResponse["result"].value( "turnId", "" )
                                                 == steer.expectedTurnId;

                if( accepted )
                {
                    const std::string text( steer.message.ToUTF8() );
                    appendTranscript( wxString::Format( _( "\nYou (steering): %s\n" ),
                                                        steer.message ) );
                    m_conversationHistory.push_back( { "user", text } );
                    persistConversation();
                    setStatus( m_turnId.empty() ? _( "Steering was delivered." )
                                                : _( "Steering delivered; Codex is continuing..." ) );
                }
                else
                {
                    const wxString error = responseErrorMessage(
                            aResponse, _( "The active turn could not accept steering." ) );
                    restoreInputMessage( steer.message );
                    appendTranscript( wxString::Format(
                            _( "\n[Steering was not delivered: %s The message was restored.]\n" ),
                            error ) );
                    setStatus( error );
                }

                setBusy( !m_turnId.empty() );
            } );

    if( requestId == 0 )
    {
        m_pendingSteers.erase( messageId );
        restoreInputMessage( aMessage );
        appendTranscript( _( "\n[Steering could not be sent. The message was restored.]\n" ) );
        setStatus( _( "Steering could not be sent." ) );
        setBusy( !m_turnId.empty() );
        return;
    }

    setStatus( _( "Steering the active Codex turn..." ) );
    setBusy( true );
}


void CODEX_PANEL::restoreInputMessage( const wxString& aMessage )
{
    if( aMessage.IsEmpty() )
        return;

    const wxString current = m_input->GetValue();
    m_input->ChangeValue( current.IsEmpty() ? aMessage : aMessage + wxS( "\n" ) + current );
    m_input->SetInsertionPointEnd();
}


void CODEX_PANEL::failPendingSteers( const wxString& aReason )
{
    if( m_pendingSteers.empty() )
        return;

    for( auto pending = m_pendingSteers.rbegin(); pending != m_pendingSteers.rend(); ++pending )
        restoreInputMessage( pending->second.message );

    appendTranscript( wxString::Format(
            _( "\n[%zu steering message(s) were not delivered: %s The messages were restored.]\n" ),
            m_pendingSteers.size(), aReason ) );
    m_pendingSteers.clear();
}


void CODEX_PANEL::handleUserInputRequest( const JSON& aMessage )
{
    if( !aMessage.contains( "id" ) )
        return;

    const JSON& params = aMessage.value( "params", JSON::object() );
    wxString error;

    if( !CODEX_USER_INPUT_DIALOG::ValidateRequest( params, error ) )
    {
        m_client.SendError( aMessage["id"], -32602, std::string( error.ToUTF8() ) );
        appendTranscript( wxString::Format( _( "\n[Codex question rejected: %s]\n" ), error ) );
        return;
    }

    const std::string requestThreadId = params.value( "threadId", "" );
    const std::string requestTurnId = params.value( "turnId", "" );

    if( requestThreadId != m_threadId
        || ( !m_turnId.empty() && requestTurnId != m_turnId ) )
    {
        m_client.SendError( aMessage["id"], -32602,
                            "Codex user-input request does not belong to the active turn" );
        appendTranscript( _( "\n[Ignored a Codex question for a different turn.]\n" ) );
        return;
    }

    // A request can race the turn/started notification on a fast local app-server.  The request
    // carries the same authoritative turn id, so adopting it here prevents a false idle state.
    if( m_turnId.empty() )
    {
        m_turnId = requestTurnId;
        setBusy( true );
    }

    m_pendingUserInputRequests.push_back( { aMessage["id"], params } );
    appendTranscript( wxString::Format(
            _( "\n[Codex is waiting for your answer%s.]\n" ),
            m_pendingUserInputRequests.size() > 1 ? _( "; another question is queued" )
                                                   : wxString() ) );
    showNextUserInputRequest();
}


void CODEX_PANEL::showNextUserInputRequest()
{
    if( m_userInputDialog || m_pendingUserInputRequests.empty() || m_shuttingDown.load() )
        return;

    const PENDING_USER_INPUT_REQUEST& request = m_pendingUserInputRequests.front();
    const JSON requestId = request.requestId;
    m_userInputDialog = new CODEX_USER_INPUT_DIALOG(
            this, request.params,
            [this, requestId]( const JSON& aResponse, const std::string& aPersistedText,
                               const wxString& aTranscript, bool aSensitive )
            {
                return submitUserInputResponse( requestId, aResponse, aPersistedText,
                                                aTranscript, aSensitive );
            },
            [this, requestId]() { return stopForUserInput( requestId ); } );
    m_userInputDialog->Show();
    m_userInputDialog->Raise();
    setStatus( _( "Codex is waiting for your input." ) );
    setBusy( true );
}


bool CODEX_PANEL::submitUserInputResponse( const JSON& aRequestId, const JSON& aResponse,
                                           const std::string& aPersistedText,
                                           const wxString& aTranscript, bool aSensitive )
{
    if( m_pendingUserInputRequests.empty()
        || m_pendingUserInputRequests.front().requestId != aRequestId )
    {
        return false;
    }

    if( !m_client.SendResponse( aRequestId, aResponse, aSensitive ) )
        return false;

    appendTranscript( aTranscript );

    if( !aPersistedText.empty() )
    {
        m_conversationHistory.push_back( { "user", aPersistedText } );
        persistConversation();
    }

    setStatus( _( "Answer delivered; Codex is continuing..." ) );
    // Dismiss the modeless dialog only after its submit callback unwinds.  Destroying the
    // dialog would otherwise clear the std::function that is currently executing.
    CallAfter( [this, aRequestId]() { resolveUserInputRequest( aRequestId ); } );
    return true;
}


bool CODEX_PANEL::stopForUserInput( const JSON& aRequestId )
{
    if( m_pendingUserInputRequests.empty()
        || m_pendingUserInputRequests.front().requestId != aRequestId )
    {
        return false;
    }

    const JSON& params = m_pendingUserInputRequests.front().params;
    const std::string threadId = params.value( "threadId", "" );
    const std::string turnId = params.value( "turnId", "" );

    if( threadId.empty() || turnId.empty() )
        return false;

    const int64_t requestId = m_client.SendRequest(
            "turn/interrupt", { { "threadId", threadId }, { "turnId", turnId } },
            [this, aRequestId]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                    return;

                if( m_userInputDialog && !m_pendingUserInputRequests.empty()
                    && m_pendingUserInputRequests.front().requestId == aRequestId )
                {
                    const wxString error = responseErrorMessage(
                            aResponse, _( "The Codex turn could not be stopped." ) );
                    m_userInputDialog->SetStopping( false, error );
                    setStatus( error );
                }
            } );

    if( requestId == 0 )
        return false;

    setStatus( _( "Stopping Codex turn..." ) );
    return true;
}


void CODEX_PANEL::resolveUserInputRequest( const JSON& aRequestId )
{
    auto request = std::find_if(
            m_pendingUserInputRequests.begin(), m_pendingUserInputRequests.end(),
            [&aRequestId]( const PENDING_USER_INPUT_REQUEST& aPending )
            {
                return aPending.requestId == aRequestId;
            } );

    if( request == m_pendingUserInputRequests.end() )
        return;

    const bool current = request == m_pendingUserInputRequests.begin();

    if( current && m_userInputDialog )
    {
        CODEX_USER_INPUT_DIALOG* dialog = m_userInputDialog;
        m_userInputDialog = nullptr;
        dialog->Dismiss();
    }

    m_pendingUserInputRequests.erase( request );

    if( current )
        showNextUserInputRequest();
}


void CODEX_PANEL::clearUserInputRequests()
{
    if( m_userInputDialog )
    {
        CODEX_USER_INPUT_DIALOG* dialog = m_userInputDialog;
        m_userInputDialog = nullptr;
        dialog->Dismiss();
    }

    m_pendingUserInputRequests.clear();
}


void CODEX_PANEL::beginTurnDisplay()
{
    m_reasoningSummaryOpen = false;
    m_agentResponseOpen = false;
    m_currentAgentMessage.clear();
    appendActivity( _( "\n[Starting Codex turn...]\n" ) );
    setStatus( _( "Starting Codex turn..." ) );
}


void CODEX_PANEL::finishReasoningDisplay()
{
    if( !m_reasoningSummaryOpen )
        return;

    appendActivity( wxS( "\n" ) );
    m_reasoningSummaryOpen = false;
}


void CODEX_PANEL::appendGoal( const JSON& aGoal )
{
    if( !aGoal.is_object() )
    {
        appendTranscript( _( "[No active goal.]\n" ) );
        return;
    }

    const wxString objective = wxString::FromUTF8( aGoal.value( "objective", "" ) );
    const wxString status = wxString::FromUTF8( aGoal.value( "status", "" ) );
    const long long tokensUsed = aGoal.value( "tokensUsed", 0LL );
    const long long timeUsedSeconds = aGoal.value( "timeUsedSeconds", 0LL );
    wxString usage = wxString::Format( _( "%lld tokens, %lld seconds" ), tokensUsed,
                                       timeUsedSeconds );

    if( aGoal.contains( "tokenBudget" ) && aGoal["tokenBudget"].is_number_integer() )
    {
        usage = wxString::Format( _( "%lld / %lld tokens, %lld seconds" ), tokensUsed,
                                  aGoal["tokenBudget"].get<long long>(), timeUsedSeconds );
    }

    appendTranscript( wxString::Format( _( "[Goal — %s: %s (%s)]\n" ), status, objective,
                                        usage ) );
}


void CODEX_PANEL::showGoal()
{
    m_client.SendRequest(
            "thread/goal/get", { { "threadId", m_threadId } },
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                    appendGoal( aResponse["result"].value( "goal", JSON() ) );
                else
                    appendTranscript( wxS( "[" )
                                      + responseErrorMessage( aResponse,
                                                              _( "Could not read the goal." ) )
                                      + wxS( "]\n" ) );

                setBusy( !m_turnId.empty() );
            } );
}


void CODEX_PANEL::setGoal( const wxString& aObjective, bool aActivate )
{
    JSON params = { { "threadId", m_threadId },
                    { "objective", std::string( aObjective.ToUTF8() ) } };

    if( aActivate )
        params["status"] = "active";

    m_client.SendRequest(
            "thread/goal/set", params,
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                {
                    const JSON& goal = aResponse["result"].value( "goal", JSON() );
                    appendGoal( goal );

                    if( goal.value( "status", "" ) == "active" && m_turnId.empty() )
                        setStatus( _( "Goal active; starting Codex..." ) );
                    else
                        setBusy( !m_turnId.empty() );
                }
                else
                {
                    appendTranscript( wxS( "[" )
                                      + responseErrorMessage( aResponse,
                                                              _( "Could not set the goal." ) )
                                      + wxS( "]\n" ) );
                    setBusy( !m_turnId.empty() );
                }
            } );
}


void CODEX_PANEL::setGoalStatus( const std::string& aStatus )
{
    m_client.SendRequest(
            "thread/goal/set", { { "threadId", m_threadId }, { "status", aStatus } },
            [this, aStatus]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                {
                    appendGoal( aResponse["result"].value( "goal", JSON() ) );

                    if( aStatus == "active" && m_turnId.empty() )
                        setStatus( _( "Goal active; starting Codex..." ) );
                    else
                        setBusy( !m_turnId.empty() );
                }
                else
                {
                    appendTranscript( wxS( "[" )
                                      + responseErrorMessage(
                                                aResponse, _( "Could not update the goal." ) )
                                      + wxS( "]\n" ) );
                    setBusy( !m_turnId.empty() );
                }
            } );
}


void CODEX_PANEL::clearGoal()
{
    m_client.SendRequest(
            "thread/goal/clear", { { "threadId", m_threadId } },
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                    appendTranscript( aResponse["result"].value( "cleared", false )
                                              ? _( "[Goal cleared.]\n" )
                                              : _( "[No active goal.]\n" ) );
                else
                    appendTranscript( wxS( "[" )
                                      + responseErrorMessage( aResponse,
                                                              _( "Could not clear the goal." ) )
                                      + wxS( "]\n" ) );

                setBusy( !m_turnId.empty() );
            } );
}


bool CODEX_PANEL::handleGoalCommand( const wxString& aMessage )
{
    if( aMessage != wxS( "/goal" ) && !aMessage.StartsWith( wxS( "/goal " ) ) )
        return false;

    wxString arguments = aMessage.Mid( 5 );
    arguments.Trim( true ).Trim( false );
    wxString command = arguments.BeforeFirst( ' ' ).Lower();
    wxString objective = arguments.AfterFirst( ' ' );
    objective.Trim( true ).Trim( false );

    enum class ACTION
    {
        SHOW,
        SET,
        EDIT,
        PAUSE,
        RESUME,
        CLEAR
    };

    ACTION action = ACTION::SHOW;

    if( arguments.IsEmpty() )
        action = ACTION::SHOW;
    else if( command == wxS( "clear" ) )
        action = ACTION::CLEAR;
    else if( command == wxS( "pause" ) )
        action = ACTION::PAUSE;
    else if( command == wxS( "resume" ) )
        action = ACTION::RESUME;
    else if( command == wxS( "edit" ) )
    {
        if( objective.IsEmpty() )
        {
            appendTranscript( _( "[Usage: /goal edit <objective>]\n" ) );
            return true;
        }

        action = ACTION::EDIT;
    }
    else
    {
        objective = arguments;
        action = ACTION::SET;
    }

    if( m_threadId.empty() && action != ACTION::SET )
    {
        appendTranscript( _( "[No active goal.]\n" ) );
        return true;
    }

    setBusy( true );
    ensureThreadLoaded(
            aMessage,
            [this, action, objective]()
            {
                switch( action )
                {
                case ACTION::SHOW:
                    showGoal();
                    break;

                case ACTION::SET:
                    setGoal( objective, true );
                    break;

                case ACTION::EDIT:
                    setGoal( objective, false );
                    break;

                case ACTION::PAUSE:
                    setGoalStatus( "paused" );
                    break;

                case ACTION::RESUME:
                    setGoalStatus( "active" );
                    break;

                case ACTION::CLEAR:
                    clearGoal();
                    break;
                }
            } );
    return true;
}


void CODEX_PANEL::RefreshExternalLayoutSettings()
{
    bool     mode = false;
    wxString tool;
    int      layers = 2;

    if( KICAD_SETTINGS* settings = GetAppSettings<KICAD_SETTINGS>( "kicad" ) )
    {
        mode = settings->m_CodexExternalLayoutMode;
        tool = settings->m_CodexExternalLayoutTool;
        layers = settings->m_CodexExternalLayoutLayers;
    }

    m_externalLayoutMode = mode;
    m_toolRegistry.SetExternalLayoutTool( tool );
    m_toolRegistry.SetExternalLayoutLayers( layers );
}


void CODEX_PANEL::appendTranscript( const wxString& aText )
{
    m_transcript->AppendText( aText );
    m_transcript->ShowPosition( m_transcript->GetLastPosition() );
}


void CODEX_PANEL::appendActivity( const wxString& aText )
{
    m_activity->AppendText( aText );
    m_activity->ShowPosition( m_activity->GetLastPosition() );
}


void CODEX_PANEL::appendDialogLog( const wxString& aRole, const std::string& aText )
{
    // Only log into a real project directory; skip the cwd fallback used elsewhere so no
    // stray codex_dialog.txt appears outside a project.
    wxString dir = m_projectPathProvider ? m_projectPathProvider() : wxString();

    if( dir.IsEmpty() || !wxFileName::DirExists( dir ) )
        return;

    wxFFile file( wxFileName( dir, wxS( "codex_dialog.txt" ) ).GetFullPath(), wxS( "ab" ) );

    if( !file.IsOpened() )
        return;

    const wxString entry =
            wxString::Format( wxS( "[%s] %s:\n%s\n\n" ),
                              wxDateTime::Now().Format( wxS( "%Y-%m-%d %H:%M:%S" ) ), aRole,
                              wxString::FromUTF8( aText ) );
    file.Write( entry, wxConvUTF8 );
}


void CODEX_PANEL::setBusy( bool aBusy )
{
    // Goal control remains available while Codex is working so users can issue /goal pause or
    // /goal clear without first interrupting the current goal turn.
    m_sendButton->Enable( m_initialized && m_authenticated
                          && ( m_conversationLoaded || !m_savedThreadId.empty() )
                          && !m_threadPreparing && m_pendingSteers.empty() );
    m_newConversationButton->Enable( !aBusy && m_initialized && !m_threadPreparing );
    m_stopButton->Enable( aBusy && !m_threadPreparing );
    m_revertButton->Enable( !aBusy && !m_threadPreparing
                            && !m_turnSnapshotHash.IsEmpty() );
    m_input->Enable( !m_threadPreparing );
    m_sendButton->SetLabel( !m_turnId.empty() && m_pendingUserInputRequests.empty()
                                    ? _( "Steer" )
                                    : _( "Send" ) );

    if( !m_turnId.empty() && m_pendingUserInputRequests.empty() )
        m_input->SetHint( _( "Steer the active Codex turn..." ) );
    else
        m_input->SetHint( _( "Describe the board you want Codex to design..." ) );
}


void CODEX_PANEL::setLoginPending( bool aPending )
{
    const bool ready = m_initialized && m_client.IsRunning();
    m_loginButton->Enable( ready && !aPending );
    m_deviceLoginButton->Enable( ready && !aPending && !m_authenticated );
    m_cancelLoginButton->Enable( ready && aPending && !m_loginId.empty() );
}


void CODEX_PANEL::setStatus( const wxString& aStatus )
{
    m_status->SetLabel( aStatus );
    Layout();
}


bool CODEX_PANEL::ensureRuntimeDependency(
        const CODEX_TOOL_REGISTRY::RUNTIME_DEPENDENCY& aDependency, std::string& aError )
{
    if( m_shuttingDown.load() )
    {
        aError = "KiChad is shutting down";
        return false;
    }

    if( !m_applicationOpener )
    {
        aError = "the KiCad application broker is unavailable";
        return false;
    }

    auto request = std::make_shared<RUNTIME_DEPENDENCY_REQUEST>( aDependency );

    {
        std::lock_guard<std::mutex> lock( m_dependencyRequestMutex );

        if( m_shuttingDown.load() )
        {
            aError = "KiChad is shutting down";
            return false;
        }

        if( m_pendingDependencyRequest )
        {
            aError = "another KiCad application dependency is still being resolved";
            return false;
        }

        m_pendingDependencyRequest = request;
    }

    wxThreadEvent* event = new wxThreadEvent( KICHAD_CODEX_DEPENDENCY_REQUESTED );
    event->SetPayload( request );
    wxQueueEvent( this, event );

    std::unique_lock<std::mutex> requestLock( request->mutex );
    request->condition.wait( requestLock,
                             [&]() { return request->completed || m_shuttingDown.load(); } );
    const bool success = request->completed && request->success;
    const wxString detail = request->detail;
    requestLock.unlock();

    {
        std::lock_guard<std::mutex> lock( m_dependencyRequestMutex );

        if( m_pendingDependencyRequest == request )
            m_pendingDependencyRequest.reset();
    }

    if( !success )
    {
        aError = detail.IsEmpty() ? "the required KiCad application could not be opened"
                                  : std::string( detail.ToUTF8() );
    }

    return success;
}


wxString CODEX_PANEL::projectPath() const
{
    wxString path = m_projectPathProvider ? m_projectPathProvider() : wxString();

    if( path.IsEmpty() || !wxFileName::DirExists( path ) )
        path = wxGetCwd();

    return path;
}


void CODEX_PANEL::selectProjectThread()
{
    wxString activePath = projectPath();

    if( activePath == m_threadProjectPath )
        return;

    if( !m_threadId.empty() && !m_turnId.empty() )
    {
        m_client.SendRequest( "turn/interrupt",
                              { { "threadId", m_threadId }, { "turnId", m_turnId } } );
    }

    failPendingSteers( _( "The active project changed." ) );
    clearUserInputRequests();
    m_threadProjectPath = activePath;
    CODEX_THREAD_STORE::BINDING binding = m_threadStore.Load( activePath );
    m_savedThreadId = std::move( binding.threadId );
    m_conversationHistory = std::move( binding.messages );
    m_threadId.clear();
    m_turnId.clear();
    m_turnSnapshotHash.clear();
    m_revertButton->Disable();
    m_conversationLoaded = false;
    m_threadPreparing = false;
    m_currentAgentMessage.clear();
    m_transcript->Clear();
    m_activity->Clear();

    if( !m_savedThreadId.empty() )
        setStatus( _( "Saved Codex conversation found." ) );
}


void CODEX_PANEL::onAppServerMessage( const JSON& aMessage )
{
    const std::string method = aMessage.value( "method", "" );

    if( method == "account/updated" )
    {
        // Current app-server versions publish the authoritative authentication transition on
        // account/updated.  Always re-read the owned credential store instead of trying to build
        // an incomplete account from the notification's authMode and planType summary.
        readAccount();
        readModels();
    }
    else if( method == "account/login/completed" )
    {
        const JSON& params = aMessage.value( "params", JSON::object() );
        const std::string eventLoginId =
                params.contains( "loginId" ) && params["loginId"].is_string()
                        ? params["loginId"].get<std::string>()
                        : std::string();

        if( !m_loginId.empty() && !eventLoginId.empty() && eventLoginId != m_loginId )
            return;

        if( !params.value( "success", false ) )
        {
            const std::string error = params.contains( "error" ) && params["error"].is_string()
                                              ? params["error"].get<std::string>()
                                              : std::string();
            setStatus( error.empty() ? _( "ChatGPT sign-in failed." )
                                     : wxString::FromUTF8( error ) );
            m_loginId.clear();
            setLoginPending( false );
            return;
        }

        // Match the working VibeCAD flow: force one refresh after successful OAuth before
        // enabling inference, then reload the account-scoped model catalog.
        m_cancelLoginButton->Disable();
        readAccount( true );
        readModels();
    }
    else if( method == "item/agentMessage/delta" )
    {
        finishReasoningDisplay();

        const std::string delta = aMessage["params"].value( "delta", "" );

        if( !m_agentResponseOpen )
        {
            appendTranscript( _( "\nCodex: " ) );
            m_agentResponseOpen = true;
        }

        m_currentAgentMessage += delta;
        appendTranscript( wxString::FromUTF8( delta ) );
        setStatus( _( "Codex is responding..." ) );
    }
    else if( method == "item/reasoning/summaryPartAdded" )
    {
        finishReasoningDisplay();
        setStatus( _( "Codex is thinking..." ) );
    }
    else if( method == "item/reasoning/summaryTextDelta" )
    {
        if( !m_reasoningSummaryOpen )
        {
            appendActivity( _( "\nThinking: " ) );
            m_reasoningSummaryOpen = true;
        }

        appendActivity( wxString::FromUTF8( aMessage["params"].value( "delta", "" ) ) );
        setStatus( _( "Codex is thinking..." ) );
    }
    else if( method == "item/reasoning/textDelta" )
    {
        // Raw hidden reasoning is intentionally not rendered.  The app-server's supported
        // reasoning summaries arrive through item/reasoning/summaryTextDelta above.
        setStatus( _( "Codex is thinking..." ) );
    }
    else if( method == "item/started" )
    {
        const JSON& item = aMessage["params"]["item"];
        const std::string type = item.value( "type", "" );

        if( type == "reasoning" )
        {
            setStatus( _( "Codex is thinking..." ) );
        }
        else if( type == "dynamicToolCall" )
        {
            finishReasoningDisplay();
            const wxString tool = wxString::FromUTF8( item.value( "tool", "" ) );
            appendActivity( wxString::Format( _( "\n[tool: %s — started]\n" ),
                                              tool ) );
            setStatus( wxString::Format( _( "Running KiChad tool: %s..." ), tool ) );
        }
        else if( type == "webSearch" )
        {
            finishReasoningDisplay();
            const wxString query = wxString::FromUTF8( item.value( "query", "" ) );
            appendActivity( query.IsEmpty()
                                    ? _( "\n[Web research started.]\n" )
                                    : wxString::Format( _( "\n[Web research: %s]\n" ), query ) );
            setStatus( _( "Codex is researching the web..." ) );
        }
        else if( type == "imageView" )
        {
            finishReasoningDisplay();
            appendActivity( wxString::Format( _( "\n[Viewing image: %s]\n" ),
                                              wxString::FromUTF8( item.value( "path", "" ) ) ) );
            setStatus( _( "Codex is inspecting an image..." ) );
        }
        else if( type == "contextCompaction" )
        {
            finishReasoningDisplay();
            appendActivity( _( "\n[Codex is compacting conversation context...]\n" ) );
            setStatus( _( "Codex is compacting context..." ) );
        }
    }
    else if( method == "item/completed" )
    {
        const JSON& item = aMessage["params"]["item"];
        const std::string type = item.value( "type", "" );

        if( type == "reasoning" )
        {
            finishReasoningDisplay();
            setStatus( _( "Codex finished thinking; continuing..." ) );
        }
        else if( type == "dynamicToolCall" )
        {
            const wxString tool = wxString::FromUTF8( item.value( "tool", "" ) );
            const wxString status = wxString::FromUTF8( item.value( "status", "" ) );
            const long long durationMs = item.contains( "durationMs" )
                                                         && item["durationMs"].is_number_integer()
                                                 ? item["durationMs"].get<long long>()
                                                 : 0LL;
            const wxString duration = durationMs > 0
                                              ? wxString::Format( _( ", %lld ms" ), durationMs )
                                              : wxString();
            appendActivity( wxString::Format( _( "[tool: %s — %s%s]\n" ), tool, status,
                                              duration ) );
            setStatus( status == wxS( "failed" )
                               ? wxString::Format( _( "KiChad tool failed: %s" ), tool )
                               : _( "Tool finished; Codex is continuing..." ) );
        }
        else if( type == "webSearch" )
        {
            appendActivity( _( "[Web research completed.]\n" ) );
            setStatus( _( "Research finished; Codex is continuing..." ) );
        }
        else if( type == "imageView" )
        {
            appendActivity( _( "[Image inspection completed.]\n" ) );
            setStatus( _( "Image inspected; Codex is continuing..." ) );
        }
        else if( type == "agentMessage" )
        {
            const std::string completedText = item.value( "text", "" );

            if( !completedText.empty() )
                m_currentAgentMessage = completedText;

            if( m_agentResponseOpen )
                appendTranscript( wxS( "\n" ) );

            m_agentResponseOpen = false;
        }
    }
    else if( method == "serverRequest/resolved" )
    {
        const JSON& params = aMessage.value( "params", JSON::object() );

        if( params.value( "threadId", "" ) == m_threadId && params.contains( "requestId" ) )
            resolveUserInputRequest( params["requestId"] );
    }
    else if( method == "thread/status/changed" )
    {
        const JSON& status = aMessage["params"].value( "status", JSON::object() );
        const std::string type = status.value( "type", "" );

        if( type == "systemError" )
        {
            appendTranscript( _( "\n[Codex thread entered a system-error state.]\n" ) );
            setStatus( _( "Codex thread error" ) );
        }
        else if( type == "active" )
        {
            const JSON& flags = status.value( "activeFlags", JSON::array() );

            if( flags.is_array()
                && std::find( flags.begin(), flags.end(), "waitingOnUserInput" ) != flags.end() )
            {
                setStatus( _( "Codex is waiting for your input." ) );
            }
            else if( flags.is_array()
                     && std::find( flags.begin(), flags.end(), "waitingOnApproval" )
                                != flags.end() )
            {
                setStatus( _( "Codex is waiting for approval." ) );
            }
        }
    }
    else if( method == "turn/started" )
    {
        m_turnId = aMessage["params"]["turn"].value( "id", "" );
        m_reasoningSummaryOpen = false;
        m_agentResponseOpen = false;
        appendActivity( _( "[Codex turn started.]\n" ) );
        setStatus( _( "Codex is working..." ) );
        setBusy( true );
    }
    else if( method == "turn/completed" )
    {
        finishReasoningDisplay();
        failPendingSteers( _( "The Codex turn ended before app-server accepted them." ) );
        clearUserInputRequests();

        if( m_agentResponseOpen )
            appendTranscript( wxS( "\n" ) );

        const JSON& turn = aMessage["params"].value( "turn", JSON::object() );
        const std::string status = turn.value( "status", "" );
        const long long durationMs = turn.contains( "durationMs" )
                                             && turn["durationMs"].is_number_integer()
                                     ? turn["durationMs"].get<long long>()
                                     : 0LL;
        const wxString duration = durationMs > 0
                                          ? wxString::Format( _( " in %.1f seconds" ),
                                                              durationMs / 1000.0 )
                                          : wxString();

        if( status == "completed" )
        {
            appendTranscript( wxString::Format( _( "[Turn completed%s.]\n" ), duration ) );
            setStatus( _( "Codex is ready." ) );
        }
        else if( status == "interrupted" )
        {
            appendTranscript( wxString::Format( _( "[Turn interrupted%s.]\n" ), duration ) );
            setStatus( _( "Codex turn interrupted." ) );
        }
        else
        {
            const wxString error = responseErrorMessage( turn, _( "Unknown Codex turn error." ) );
            appendTranscript( wxString::Format( _( "[Turn failed%s: %s]\n" ), duration,
                                                error ) );
            setStatus( wxString::Format( _( "Codex turn failed: %s" ), error ) );
        }

        if( !m_currentAgentMessage.empty() )
        {
            m_conversationHistory.push_back( { "assistant", m_currentAgentMessage } );
            appendDialogLog( wxS( "CODEX" ), m_currentAgentMessage );
            m_currentAgentMessage.clear();
        }

        persistConversation();

        m_agentResponseOpen = false;
        m_turnId.clear();
        setBusy( false );
    }
    else if( method == "error" )
    {
        finishReasoningDisplay();
        const JSON& params = aMessage.value( "params", JSON::object() );
        const wxString error = responseErrorMessage( params, _( "Unknown Codex error." ) );
        const bool willRetry = params.value( "willRetry", false );
        if( willRetry )
            appendActivity( wxString::Format( _( "\n[Codex error; retrying: %s]\n" ), error ) );
        else
            appendTranscript( wxString::Format( _( "\n[Codex error: %s]\n" ), error ) );
        setStatus( willRetry ? wxString::Format( _( "Codex error; retrying: %s" ), error )
                             : wxString::Format( _( "Codex error: %s" ), error ) );
    }
    else if( method == "item/tool/requestUserInput" && aMessage.contains( "id" ) )
    {
        handleUserInputRequest( aMessage );
    }
    else if( method == "item/tool/call" && aMessage.contains( "id" ) )
    {
        if( !aMessage.contains( "params" ) || !aMessage["params"].is_object()
            || !aMessage["params"].contains( "tool" )
            || !aMessage["params"]["tool"].is_string() )
        {
            m_client.SendError( aMessage["id"], -32602,
                                "Native KiChad tool parameters are invalid" );
            appendActivity( _( "[tool result: invalid request]\n" ) );
            return;
        }

        const JSON& params = aMessage["params"];
        std::string tool = params.value( "tool", "" );
        JSON arguments = params.value( "arguments", JSON::object() );
        appendActivity( wxString::Format( _( "\n[tool request: %s %s]\n" ),
                                          wxString::FromUTF8( tool ),
                                          wxString::FromUTF8( arguments.dump() ) ) );

        // Serialize design operations so concurrent model calls cannot observe or mutate
        // partially overlapping project state.
        if( !m_toolWorkers.empty() )
        {
            m_client.SendError( aMessage["id"], -32001,
                                "Another native KiChad tool call is still running" );
            appendActivity( _( "[tool result: executor busy]\n" ) );
            return;
        }

        const int taskId = m_nextToolTaskId++;
        JSON      requestId = aMessage["id"];
        wxString  activeProject =
                m_projectPathProvider ? m_projectPathProvider() : wxString();
        bool mutationAvailable = !m_turnSnapshotHash.IsEmpty();
        bool finalActionApproved = false;

        if( CODEX_TOOL_REGISTRY::RequiresFinalConfirmation( tool, arguments ) )
        {
            wxString confirmationMessage =
                    _( "Codex is requesting a final fabrication export. KiChad will rerun ERC, "
                       "DRC, and sourcing gates, then atomically replace the project's "
                       "fabrication output directory only if every requested artifact validates. "
                       "Any KDS-declared firmware and programming/bring-up plan will be hash-checked "
                       "and included in that release.\n\n" );

            if( arguments.is_object() && arguments.contains( "allowWaivers" )
                && arguments["allowWaivers"].is_boolean()
                && arguments["allowWaivers"].get<bool>() )
            {
                confirmationMessage +=
                        _( "This request also authorizes release when ERC or DRC contains "
                           "ignored checks or exclusions; the manifest will mark the package "
                           "as waived.\n\n" );
            }

            confirmationMessage += _( "Allow this export?" );
            wxMessageDialog confirmation(
                    this, confirmationMessage, _( "Confirm fabrication export" ),
                    wxYES_NO | wxNO_DEFAULT | wxICON_WARNING );
            finalActionApproved = confirmation.ShowModal() == wxID_YES;
        }

        try
        {
            auto worker = m_toolWorkers.try_emplace( taskId ).first;
            m_toolRequestIds.emplace( taskId, requestId );

            worker->second = std::thread(
                            [this, taskId, tool = std::move( tool ),
                             arguments = std::move( arguments ),
                             activeProject = std::move( activeProject ), mutationAvailable,
                             finalActionApproved]()
                            {
                                std::string serialized;

                                try
                                {
                                    JSON result = m_toolRegistry.HandleWithContext(
                                            tool, arguments, activeProject, mutationAvailable,
                                            wxString(), finalActionApproved,
                                            std::chrono::milliseconds( 15000 ),
                                            [this]( const CODEX_TOOL_REGISTRY::RUNTIME_DEPENDENCY&
                                                            aDependency,
                                                    std::string& aError )
                                            {
                                                return ensureRuntimeDependency( aDependency,
                                                                                aError );
                                            } );
                                    serialized = result.dump();
                                }
                                catch( const std::exception& )
                                {
                                    JSON error = { { "ok", false },
                                                   { "error",
                                                     { { "contractVersion", 1 },
                                                       { "code", "tool_failed" },
                                                       { "message",
                                                         "Native tool result could not be serialized" },
                                                       { "stage", tool },
                                                       { "retryable", false },
                                                       { "stateChanged", "unknown" },
                                                       { "recovery",
                                                         { { "summary",
                                                             "Do not retry unchanged; inspect the KiChad protocol log." },
                                                           { "steps", JSON::array() } } } } } };
                                    JSON result = {
                                        { "contentItems",
                                          JSON::array( { { { "type", "inputText" },
                                                           { "text", error.dump() } } } ) },
                                        { "success", false }
                                    };
                                    serialized = result.dump();
                                }

                                std::lock_guard<std::mutex> lock( m_toolEventMutex );

                                if( !m_shuttingDown.load() )
                                {
                                    wxThreadEvent* event =
                                            new wxThreadEvent( KICHAD_CODEX_TOOL_COMPLETED );
                                    event->SetInt( taskId );
                                    event->SetString( wxString::FromUTF8( serialized.data(),
                                                                         serialized.size() ) );
                                    wxQueueEvent( this, event );
                                }
                            } );
        }
        catch( const std::exception& error )
        {
            m_toolWorkers.erase( taskId );
            m_toolRequestIds.erase( taskId );
            m_client.SendError( aMessage["id"], -32000,
                                std::string( "Could not start native tool worker: " ) + error.what() );
            appendActivity( _( "[tool result: failed to start worker]\n" ) );
        }
    }
}


void CODEX_PANEL::onRuntimeDependencyRequested( wxThreadEvent& aEvent )
{
    const std::shared_ptr<RUNTIME_DEPENDENCY_REQUEST> request =
            aEvent.GetPayload<std::shared_ptr<RUNTIME_DEPENDENCY_REQUEST>>();

    if( !request )
        return;

    {
        std::lock_guard<std::mutex> lock( request->mutex );

        if( request->completed )
            return;
    }

    const wxString application =
            request->dependency.application
                            == CODEX_TOOL_REGISTRY::RUNTIME_APPLICATION::PCB_EDITOR
                    ? _( "PCB Editor" )
                    : _( "Schematic Editor" );
    appendActivity( wxString::Format(
            _( "[dependency requested: %s — %s]\n" ), application,
            request->dependency.document.GetFullName() ) );
    setStatus( wxString::Format( _( "Opening %s for %s..." ), application,
                                 request->dependency.document.GetFullName() ) );

    wxString detail;
    const bool success = !m_shuttingDown.load()
                         && m_applicationOpener
                         && m_applicationOpener( request->dependency.application,
                                                 request->dependency.document, detail );

    if( success )
    {
        appendActivity( wxString::Format(
                _( "[dependency opened: %s — %s]\n" ), application,
                detail.IsEmpty() ? request->dependency.document.GetFullName() : detail ) );
        setStatus( _( "Application opened; waiting for its KiCad service..." ) );
    }
    else
    {
        if( detail.IsEmpty() )
            detail = _( "application could not open the requested document" );

        appendActivity( wxString::Format(
                _( "[dependency launch failed: %s — %s]\n" ), application, detail ) );
        setStatus( wxString::Format( _( "Could not open required %s." ), application ) );
    }

    {
        std::lock_guard<std::mutex> lock( request->mutex );
        request->success = success;
        request->detail = detail;
        request->completed = true;
    }

    request->condition.notify_all();
}


void CODEX_PANEL::onToolCompleted( wxThreadEvent& aEvent )
{
    JSON requestId;
    auto request = m_toolRequestIds.find( aEvent.GetInt() );

    if( request != m_toolRequestIds.end() )
    {
        requestId = std::move( request->second );
        m_toolRequestIds.erase( request );
    }

    auto worker = m_toolWorkers.find( aEvent.GetInt() );

    if( worker != m_toolWorkers.end() )
    {
        if( worker->second.joinable() )
            worker->second.join();

        m_toolWorkers.erase( worker );
    }

    try
    {
        JSON result = JSON::parse( std::string( aEvent.GetString().ToUTF8() ) );

        if( requestId.is_null() )
        {
            appendTranscript( _( "[tool result could not be delivered: request ID was lost]\n" ) );
            return;
        }

        if( !m_client.SendResponse( requestId, result ) )
        {
            appendTranscript(
                    _( "[tool result could not be delivered: Codex transport write failed]\n" ) );
            setStatus( _( "Tool completed, but Codex transport failed. Restart the conversation "
                          "service before continuing." ) );
            return;
        }

        if( result.value( "success", false ) )
        {
            appendActivity( _( "[tool result: success]\n" ) );
            setStatus( _( "Tool result delivered; Codex is continuing..." ) );
        }
        else
        {
            wxString code;
            wxString message;
            wxString details;
            wxString recovery;
            wxString stateChanged;

            if( nativeToolFailureSummary( result, code, message, details, recovery,
                                          stateChanged ) )
            {
                appendActivity( wxString::Format( _( "[tool result: failed — %s: %s]\n" ),
                                                  code, message ) );

                if( !details.IsEmpty() )
                    appendActivity( wxString::Format( _( "[details: %s]\n" ), details ) );

                if( !recovery.IsEmpty() )
                    appendActivity( wxString::Format( _( "[recovery: %s]\n" ), recovery ) );

                if( stateChanged != wxS( "none" ) )
                {
                    appendTranscript( wxString::Format(
                            _( "[project state after failure: %s]\n" ), stateChanged ) );
                }

                setStatus( wxString::Format( _( "KiChad tool failed: %s — %s" ),
                                             code, message ) );
            }
            else
            {
                appendTranscript( _( "[tool result: failed — invalid failure details]\n" ) );
                setStatus( _( "KiChad tool failed without valid diagnostics." ) );
            }
        }
    }
    catch( const JSON::exception& error )
    {
        if( !requestId.is_null() )
            m_client.SendError( requestId, -32000, "Native KiChad tool result could not be decoded" );

        appendTranscript( wxString::Format( _( "[tool result could not be decoded: %s]\n" ),
                                            wxString::FromUTF8( error.what() ) ) );
    }
}


void CODEX_PANEL::onAppServerState( bool aRunning, const wxString& aDetail )
{
    setStatus( aDetail );

    if( aRunning )
    {
        m_processStatus->SetLabel( _( "Codex service: connected" ) );
    }
    else
    {
        m_processStatus->SetLabel( _( "Codex service: unavailable" ) );
    }

    if( !aRunning )
    {
        finishReasoningDisplay();
        failPendingSteers( _( "The Codex service became unavailable." ) );
        clearUserInputRequests();

        if( m_agentResponseOpen )
            appendTranscript( wxS( "\n" ) );

        appendTranscript( wxString::Format( _( "\n[Codex service unavailable: %s]\n" ),
                                            aDetail ) );
        m_agentResponseOpen = false;
        m_turnId.clear();
        m_initialized = false;
        m_authenticated = false;
        m_loginButton->Disable();
        m_deviceLoginButton->Disable();
        m_cancelLoginButton->Disable();
        m_newConversationButton->Disable();
        m_sendButton->Disable();
        m_stopButton->Disable();
    }
}


void CODEX_PANEL::onLogin( wxCommandEvent& aEvent )
{
    if( m_authenticated )
    {
        setLoginPending( true );
        m_client.SendRequest( "account/logout", JSON::object(),
                              [this]( const JSON& ) { readAccount(); } );
        return;
    }

    m_loginId.clear();
    setLoginPending( true );
    setStatus( _( "Starting ChatGPT sign-in..." ) );
    m_client.SendRequest(
            "account/login/start", { { "type", "chatgpt" },
                                     { "useHostedLoginSuccessPage", true },
                                     { "appBrand", "chatgpt" } },
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                {
                    const JSON& result = aResponse["result"];
                    std::string authUrl = result.value( "authUrl", "" );
                    std::string loginId = result.value( "loginId", "" );

                    if( result.value( "type", "" ) == "chatgpt" && !authUrl.empty()
                        && !loginId.empty() )
                    {
                        m_loginId = std::move( loginId );
                        setLoginPending( true );
                        wxLaunchDefaultBrowser( wxString::FromUTF8( authUrl ) );
                        setStatus( _( "Complete ChatGPT sign-in in your browser..." ) );
                        return;
                    }
                }

                setStatus( responseErrorMessage(
                        aResponse, _( "Could not start ChatGPT sign-in." ) ) );
                m_loginId.clear();
                setLoginPending( false );
            } );
}


void CODEX_PANEL::onDeviceLogin( wxCommandEvent& aEvent )
{
    if( m_authenticated )
        return;

    m_loginId.clear();
    setLoginPending( true );
    setStatus( _( "Requesting a ChatGPT device code..." ) );
    m_client.SendRequest(
            "account/login/start", { { "type", "chatgptDeviceCode" } },
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                {
                    const JSON& result = aResponse["result"];
                    std::string loginId = result.value( "loginId", "" );
                    std::string verificationUrl = result.value( "verificationUrl", "" );
                    std::string userCode = result.value( "userCode", "" );

                    if( result.value( "type", "" ) == "chatgptDeviceCode"
                        && !loginId.empty() && !verificationUrl.empty() && !userCode.empty() )
                    {
                        m_loginId = std::move( loginId );
                        setLoginPending( true );

                        const wxString url = wxString::FromUTF8( verificationUrl );
                        const wxString code = wxString::FromUTF8( userCode );
                        wxLaunchDefaultBrowser( url );
                        setStatus( wxString::Format( _( "Enter device code %s" ), code ) );
                        appendTranscript( wxString::Format(
                                _( "\n[ChatGPT device sign-in: open %s and enter code %s.]\n" ),
                                url, code ) );
                        return;
                    }
                }

                setStatus( responseErrorMessage(
                        aResponse, _( "Could not request a ChatGPT device code." ) ) );
                m_loginId.clear();
                setLoginPending( false );
            } );
}


void CODEX_PANEL::onCancelLogin( wxCommandEvent& aEvent )
{
    if( m_loginId.empty() )
        return;

    const std::string loginId = m_loginId;
    m_cancelLoginButton->Disable();
    setStatus( _( "Cancelling ChatGPT sign-in..." ) );
    m_client.SendRequest(
            "account/login/cancel", { { "loginId", loginId } },
            [this, loginId]( const JSON& aResponse )
            {
                if( m_loginId == loginId )
                    m_loginId.clear();

                if( aResponse.contains( "result" ) )
                    setStatus( _( "ChatGPT sign-in cancelled." ) );
                else
                    setStatus( responseErrorMessage(
                            aResponse, _( "Could not cancel ChatGPT sign-in." ) ) );

                setLoginPending( false );
            } );
}


void CODEX_PANEL::onSend( wxCommandEvent& aEvent )
{
    selectProjectThread();

    if( !m_conversationLoaded )
    {
        loadSavedConversation();
        return;
    }

    wxString message = m_input->GetValue();
    message.Trim( true ).Trim( false );

    if( message.IsEmpty() )
        return;

    // An unanswered app-server question owns the input focus: route the user back to the
    // dialog instead of queuing a message the running turn cannot see.
    if( !m_pendingUserInputRequests.empty() )
    {
        setStatus( _( "Answer the Codex question before sending another message." ) );

        if( m_userInputDialog )
        {
            m_userInputDialog->Show();
            m_userInputDialog->Raise();
        }

        return;
    }

    m_input->Clear();
    submitUserMessage( message );
}


bool CODEX_PANEL::submitUserMessage( const wxString& aMessage )
{
    const wxString& message = aMessage;

    appendTranscript( wxString::Format( _( "\nYou: %s\n" ), message ) );

    if( handleGoalCommand( message ) )
        return false;

    // Steer the running turn rather than refusing the message outright.
    if( !m_turnId.empty() )
    {
        steerTurn( message );
        return false;
    }

    m_turnSnapshotHash.clear();

    if( m_snapshotProvider )
    {
        m_turnSnapshotHash = m_snapshotProvider( _( "Before Codex turn" ) );

        const wxString activeProject =
                m_projectPathProvider ? m_projectPathProvider() : wxString();

        if( m_turnSnapshotHash.IsEmpty() && !activeProject.IsEmpty() )
        {
            appendTranscript( _( "[A turn snapshot could not be created; mutating tools will "
                                 "remain unavailable.]\n" ) );
        }
    }

    setBusy( true );

    std::string utf8Message( message.ToUTF8() );
    ensureThreadLoaded( message, [this, utf8Message]() { startTurn( utf8Message ); } );
    return true;
}


void CODEX_PANEL::onStop( wxCommandEvent& aEvent )
{
    if( m_threadId.empty() || m_turnId.empty() )
        return;

    if( !m_pendingUserInputRequests.empty() )
    {
        const JSON requestId = m_pendingUserInputRequests.front().requestId;

        if( stopForUserInput( requestId ) && m_userInputDialog )
            m_userInputDialog->SetStopping( true );

        return;
    }

    m_client.SendRequest( "turn/interrupt", { { "threadId", m_threadId }, { "turnId", m_turnId } } );
    setStatus( _( "Stopping Codex turn..." ) );
}


void CODEX_PANEL::onRevertTurn( wxCommandEvent& aEvent )
{
    if( m_turnSnapshotHash.IsEmpty() || !m_restoreHandler )
        return;

    const wxString snapshot = m_turnSnapshotHash;
    m_revertButton->Disable();

    if( m_restoreHandler( snapshot ) )
    {
        appendTranscript( _( "\n[Reverted the project to its pre-turn snapshot.]\n" ) );
        m_turnSnapshotHash.clear();
    }
    else
    {
        appendTranscript( _( "\n[The pre-turn snapshot was not restored.]\n" ) );
        m_revertButton->Enable();
    }
}


void CODEX_PANEL::onNewConversation( wxCommandEvent& aEvent )
{
    selectProjectThread();

    if( !m_turnId.empty() )
    {
        appendTranscript( _( "\n[Stop the active turn before starting a new conversation.]\n" ) );
        return;
    }

    wxString clearError;

    if( !m_threadStore.Clear( m_threadProjectPath, &clearError ) )
    {
        appendTranscript( wxString::Format( _( "\n[Could not start a new conversation: %s]\n" ),
                                            clearError ) );
        return;
    }

    const std::string previousThreadId =
            m_threadId.empty() ? m_savedThreadId : m_threadId;
    m_threadId.clear();
    m_savedThreadId.clear();
    clearUserInputRequests();
    m_pendingSteers.clear();
    m_conversationHistory.clear();
    m_currentAgentMessage.clear();
    m_turnId.clear();
    m_turnSnapshotHash.clear();
    m_conversationLoaded = true;
    m_threadPreparing = false;
    m_reasoningSummaryOpen = false;
    m_agentResponseOpen = false;
    m_transcript->Clear();
    m_activity->Clear();
    appendTranscript( _( "[New conversation started. The previous context has been cleared.]\n" ) );
    setStatus( _( "New Codex conversation ready." ) );
    setBusy( false );

    if( !previousThreadId.empty() )
    {
        m_client.SendRequest(
                "thread/archive", { { "threadId", previousThreadId } },
                [this]( const JSON& aResponse )
                {
                    if( !aResponse.contains( "result" ) )
                    {
                        appendTranscript( wxS( "[" )
                                          + responseErrorMessage(
                                                    aResponse,
                                                    _( "The previous conversation could not be "
                                                       "archived." ) )
                                          + wxS( "]\n" ) );
                    }
                } );
    }
}


void CODEX_PANEL::onModelChanged( wxCommandEvent& aEvent )
{
    const int selection = m_modelChoice->GetSelection();

    if( selection >= 0 && static_cast<size_t>( selection ) < m_models.size() )
        m_preferredModel = wxString::FromUTF8( m_models[selection].value( "model", "" ) );

    updateReasoningChoices();

    const int reasoningSelection = m_reasoningChoice->GetSelection();

    if( reasoningSelection >= 0 )
        m_preferredReasoningEffort = m_reasoningChoice->GetString( reasoningSelection );

    savePreferences();
}


void CODEX_PANEL::onReasoningChanged( wxCommandEvent& aEvent )
{
    const int selection = m_reasoningChoice->GetSelection();

    if( selection < 0 )
        return;

    m_preferredReasoningEffort = m_reasoningChoice->GetString( selection );
    savePreferences();
}
