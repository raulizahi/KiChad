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

#ifndef KICHAD_CODEX_USER_INPUT_DIALOG_H
#define KICHAD_CODEX_USER_INPUT_DIALOG_H

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <wx/dialog.h>
#include <wx/timer.h>


class wxButton;
class wxChoice;
class wxStaticText;
class wxTextCtrl;


/** Displays and answers one app-server item/tool/requestUserInput request. */
class CODEX_USER_INPUT_DIALOG : public wxDialog
{
public:
    using JSON = nlohmann::json;
    using SUBMIT_HANDLER =
            std::function<bool( const JSON&, const std::string&, const wxString&, bool )>;
    using STOP_HANDLER = std::function<bool()>;

    CODEX_USER_INPUT_DIALOG( wxWindow* aParent, const JSON& aRequest,
                             SUBMIT_HANDLER aSubmitHandler, STOP_HANDLER aStopHandler );
    ~CODEX_USER_INPUT_DIALOG() override;

    static bool ValidateRequest( const JSON& aRequest, wxString& aError );

    void Dismiss();
    void SetStopping( bool aStopping, const wxString& aError = wxString() );

private:
    struct QUESTION_VIEW
    {
        std::string           id;
        wxString              header;
        wxString              question;
        bool                  secret = false;
        wxChoice*             choice = nullptr;
        wxStaticText*         description = nullptr;
        wxTextCtrl*           notes = nullptr;
        std::vector<wxString> optionLabels;
        std::vector<wxString> optionDescriptions;
    };

    void updateChoiceDescription( size_t aQuestionIndex );
    void snoozeAutoResolution();
    void updateAutoResolutionStatus();
    bool submitAnswers( bool aAutomatic );
    void onSubmit( wxCommandEvent& aEvent );
    void onStop( wxCommandEvent& aEvent );
    void onChoiceChanged( wxCommandEvent& aEvent );
    void onTextChanged( wxCommandEvent& aEvent );
    void onTimer( wxTimerEvent& aEvent );
    void onClose( wxCloseEvent& aEvent );

    JSON                                  m_request;
    SUBMIT_HANDLER                        m_submitHandler;
    STOP_HANDLER                          m_stopHandler;
    std::vector<QUESTION_VIEW>            m_questions;
    wxStaticText*                         m_autoResolutionStatus;
    wxButton*                             m_stopButton;
    wxButton*                             m_submitButton;
    wxTimer                               m_autoResolutionTimer;
    std::chrono::steady_clock::time_point m_autoResolutionDeadline;
    bool                                  m_autoResolutionActive;
    bool                                  m_submitting;
    bool                                  m_dismissing;
};

#endif // KICHAD_CODEX_USER_INPUT_DIALOG_H
