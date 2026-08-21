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

#ifndef KICHAD_CODEX_PANEL_H
#define KICHAD_CODEX_PANEL_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <wx/panel.h>

#include "codex_app_server_client.h"
#include "codex_thread_store.h"
#include "codex_tool_registry.h"


class wxButton;
class wxCheckBox;
class wxChoice;
class wxStaticText;
class wxTextCtrl;
class wxThreadEvent;


/** Native docked Codex conversation surface owned by the KiChad project manager. */
class CODEX_PANEL : public wxPanel
{
public:
    using SNAPSHOT_PROVIDER = std::function<wxString( const wxString& )>;
    using RESTORE_HANDLER = std::function<bool( const wxString& )>;
    using PREFERENCE_SAVER = std::function<void( const wxString&, const wxString& )>;
    using APPLICATION_OPENER =
            std::function<bool( CODEX_TOOL_REGISTRY::RUNTIME_APPLICATION,
                                const wxFileName&, wxString& )>;

    explicit CODEX_PANEL( wxWindow* aParent, std::function<wxString()> aProjectPathProvider,
                          SNAPSHOT_PROVIDER aSnapshotProvider, RESTORE_HANDLER aRestoreHandler,
                          wxString aPreferredModel, wxString aPreferredReasoningEffort,
                          PREFERENCE_SAVER aPreferenceSaver,
                          APPLICATION_OPENER aApplicationOpener );
    ~CODEX_PANEL() override;

private:
    using JSON = nlohmann::json;

    struct RUNTIME_DEPENDENCY_REQUEST
    {
        explicit RUNTIME_DEPENDENCY_REQUEST(
                const CODEX_TOOL_REGISTRY::RUNTIME_DEPENDENCY& aDependency ) :
                dependency( aDependency )
        {}

        CODEX_TOOL_REGISTRY::RUNTIME_DEPENDENCY dependency;
        std::mutex                              mutex;
        std::condition_variable                 condition;
        bool                                    completed = false;
        bool                                    success = false;
        wxString                                detail;
    };

    void initializeAppServer();
    void readAccount( bool aRefreshToken = false );
    void readModels();
    void updateReasoningChoices();
    void savePreferences();
    void loadSavedConversation();
    void renderConversation();
    void persistConversation();
    JSON conversationHistoryItems() const;
    void ensureThreadLoaded( const wxString& aDisplayedMessage,
                             std::function<void()> aReadyHandler );
    void startThread( std::function<void()> aReadyHandler );
    void startTurn( const std::string& aMessage );
    bool handleGoalCommand( const wxString& aMessage );
    void showGoal();
    void setGoal( const wxString& aObjective, bool aActivate );
    void setGoalStatus( const std::string& aStatus );
    void clearGoal();
    void appendGoal( const JSON& aGoal );
    void beginTurnDisplay();
    void finishReasoningDisplay();
    bool ensureRuntimeDependency( const CODEX_TOOL_REGISTRY::RUNTIME_DEPENDENCY& aDependency,
                                  std::string& aError );
    void selectProjectThread();
public:
    /// Re-read the external-layout preferences (mode + tool path) from KICAD_SETTINGS.
    void RefreshExternalLayoutSettings();

    /// Rebind the panel to the newly active project, clearing both transcript panes.
    void OnProjectChanged() { selectProjectThread(); }

private:
    void appendTranscript( const wxString& aText );
    void appendActivity( const wxString& aText );
    void appendDialogLog( const wxString& aRole, const std::string& aText );
    bool submitUserMessage( const wxString& aMessage );
    void maybeAutoContinue( const wxString& aAgentText );
    void setBusy( bool aBusy );
    void setLoginPending( bool aPending );
    void setStatus( const wxString& aStatus );
    wxString projectPath() const;

    void onAppServerMessage( const JSON& aMessage );
    void onAppServerState( bool aRunning, const wxString& aDetail );
    void onRuntimeDependencyRequested( wxThreadEvent& aEvent );
    void onToolCompleted( wxThreadEvent& aEvent );
    void onLogin( wxCommandEvent& aEvent );
    void onDeviceLogin( wxCommandEvent& aEvent );
    void onCancelLogin( wxCommandEvent& aEvent );
    void onSend( wxCommandEvent& aEvent );
    void onStop( wxCommandEvent& aEvent );
    void onRevertTurn( wxCommandEvent& aEvent );
    void onNewConversation( wxCommandEvent& aEvent );
    void onModelChanged( wxCommandEvent& aEvent );
    void onReasoningChanged( wxCommandEvent& aEvent );

    std::function<wxString()> m_projectPathProvider;
    SNAPSHOT_PROVIDER         m_snapshotProvider;
    RESTORE_HANDLER           m_restoreHandler;
    PREFERENCE_SAVER          m_preferenceSaver;
    APPLICATION_OPENER        m_applicationOpener;
    CODEX_TOOL_REGISTRY       m_toolRegistry;
    CODEX_THREAD_STORE        m_threadStore;
    CODEX_APP_SERVER_CLIENT   m_client;
    wxStaticText*             m_status;
    wxStaticText*             m_processStatus;
    wxButton*                 m_loginButton;
    wxButton*                 m_deviceLoginButton;
    wxButton*                 m_cancelLoginButton;
    wxChoice*                 m_modelChoice;
    wxChoice*                 m_reasoningChoice;
    wxTextCtrl*               m_transcript;
    wxTextCtrl*               m_activity;
    wxTextCtrl*               m_input;
    wxButton*                 m_sendButton;
    wxButton*                 m_stopButton;
    wxCheckBox*               m_autoContinueCheckbox;
    int                       m_autoContinueRemaining;
    wxButton*                 m_revertButton;
    wxButton*                 m_newConversationButton;
    std::vector<JSON>         m_models;
    std::vector<CODEX_THREAD_STORE::MESSAGE> m_conversationHistory;
    bool                      m_externalLayoutMode;
    std::string               m_threadId;
    std::string               m_savedThreadId;
    std::string               m_loginId;
    wxString                  m_threadProjectPath;
    std::string               m_turnId;
    wxString                  m_turnSnapshotHash;
    wxString                  m_preferredModel;
    wxString                  m_preferredReasoningEffort;
    std::map<int, std::thread> m_toolWorkers;
    std::map<int, JSON>       m_toolRequestIds;
    std::mutex                m_toolEventMutex;
    std::mutex                m_dependencyRequestMutex;
    std::shared_ptr<RUNTIME_DEPENDENCY_REQUEST> m_pendingDependencyRequest;
    std::atomic<bool>         m_shuttingDown;
    int                       m_nextToolTaskId;
    bool                      m_initialized;
    bool                      m_authenticated;
    bool                      m_conversationLoaded;
    bool                      m_threadPreparing;
    bool                      m_reasoningSummaryOpen;
    bool                      m_agentResponseOpen;
    std::string               m_currentAgentMessage;
};

#endif // KICHAD_CODEX_PANEL_H
