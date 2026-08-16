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

#include "codex_user_input_dialog.h"

#include <algorithm>
#include <limits>
#include <set>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/intl.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>


namespace
{

using JSON = nlohmann::json;

constexpr int MIN_DIALOG_WIDTH = 520;
constexpr int MAX_DIALOG_WIDTH = 760;
constexpr int MIN_DIALOG_HEIGHT = 480;
constexpr int MAX_DIALOG_HEIGHT = 720;


bool nonEmptyString( const JSON& aObject, const char* aKey )
{
    return aObject.contains( aKey ) && aObject[aKey].is_string()
           && !aObject[aKey].get<std::string>().empty();
}


bool nonNegativeInt64( const JSON& aValue )
{
    if( aValue.is_number_unsigned() )
    {
        return aValue.get<uint64_t>()
               <= static_cast<uint64_t>( std::numeric_limits<int64_t>::max() );
    }

    return aValue.is_number_integer() && aValue.get<int64_t>() >= 0;
}


wxString answerDisplayText( const std::vector<wxString>& aAnswers )
{
    wxString display;

    for( const wxString& answer : aAnswers )
    {
        wxString value = answer;

        if( value.StartsWith( wxS( "user_note: " ) ) )
            value = value.Mid( 11 );

        if( !display.IsEmpty() )
            display += wxS( "; " );

        display += value;
    }

    return display;
}

} // namespace


CODEX_USER_INPUT_DIALOG::CODEX_USER_INPUT_DIALOG( wxWindow* aParent, const JSON& aRequest,
                                                  SUBMIT_HANDLER aSubmitHandler,
                                                  STOP_HANDLER aStopHandler ) :
        wxDialog( aParent, wxID_ANY, _( "Codex needs your input" ), wxDefaultPosition,
                  wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER ),
        m_request( aRequest ),
        m_submitHandler( std::move( aSubmitHandler ) ),
        m_stopHandler( std::move( aStopHandler ) ),
        m_autoResolutionStatus( nullptr ),
        m_stopButton( nullptr ),
        m_submitButton( nullptr ),
        m_autoResolutionTimer( this ),
        m_autoResolutionActive( false ),
        m_submitting( false ),
        m_dismissing( false )
{
    wxBoxSizer* root = new wxBoxSizer( wxVERTICAL );
    wxStaticText* introduction =
            new wxStaticText( this, wxID_ANY,
                              _( "Codex paused the current turn until you answer." ) );
    root->Add( introduction, 0, wxEXPAND | wxALL, FromDIP( 12 ) );

    wxScrolledWindow* scroller =
            new wxScrolledWindow( this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxVSCROLL | wxTAB_TRAVERSAL );
    scroller->SetScrollRate( 0, FromDIP( 12 ) );
    wxBoxSizer* questionsSizer = new wxBoxSizer( wxVERTICAL );

    const JSON& questions = m_request["questions"];
    m_questions.reserve( questions.size() );

    for( size_t index = 0; index < questions.size(); ++index )
    {
        const JSON& question = questions[index];
        QUESTION_VIEW view;
        view.id = question["id"].get<std::string>();
        view.header = wxString::FromUTF8( question["header"].get<std::string>() );
        view.question = wxString::FromUTF8( question["question"].get<std::string>() );
        view.secret = question.value( "isSecret", false );
        const bool hasOther = question.value( "isOther", false );

        wxStaticBoxSizer* questionSizer =
                new wxStaticBoxSizer( wxVERTICAL, scroller, view.header );
        wxStaticText* prompt = new wxStaticText( scroller, wxID_ANY, view.question );
        prompt->Wrap( FromDIP( MAX_DIALOG_WIDTH - 80 ) );
        questionSizer->Add( prompt, 0, wxEXPAND | wxALL, FromDIP( 8 ) );

        const bool hasOptions = question.contains( "options" )
                                && question["options"].is_array()
                                && !question["options"].empty();

        if( hasOptions )
        {
            view.choice = new wxChoice( scroller, wxID_ANY );

            for( const JSON& option : question["options"] )
            {
                const wxString label = wxString::FromUTF8( option["label"].get<std::string>() );
                view.optionLabels.push_back( label );
                view.optionDescriptions.push_back(
                        wxString::FromUTF8( option["description"].get<std::string>() ) );
                view.choice->Append( label );
            }

            if( hasOther )
            {
                view.optionLabels.push_back( _( "None of the above" ) );
                view.optionDescriptions.push_back(
                        _( "Use the notes field to provide a different answer." ) );
                view.choice->Append( view.optionLabels.back() );
            }

            view.choice->SetSelection( 0 );
            questionSizer->Add( view.choice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
                                FromDIP( 8 ) );
            view.description = new wxStaticText( scroller, wxID_ANY, wxEmptyString );
            view.description->Wrap( FromDIP( MAX_DIALOG_WIDTH - 100 ) );
            questionSizer->Add( view.description, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
                                FromDIP( 8 ) );

            questionSizer->Add( new wxStaticText( scroller, wxID_ANY,
                                                   view.secret ? _( "Private notes" )
                                                               : _( "Notes (optional)" ) ),
                                0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP( 8 ) );
        }
        else
        {
            questionSizer->Add( new wxStaticText( scroller, wxID_ANY,
                                                   view.secret ? _( "Private answer" )
                                                               : _( "Answer" ) ),
                                0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP( 8 ) );
        }

        const long textStyle = view.secret ? wxTE_PASSWORD : wxTE_MULTILINE;
        const wxSize textSize = view.secret ? FromDIP( wxSize( -1, 30 ) )
                                            : FromDIP( wxSize( -1, 64 ) );
        view.notes = new wxTextCtrl( scroller, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                     textSize, textStyle );
        view.notes->SetHint( hasOptions ? _( "Add useful context for this choice" )
                                        : _( "Type your answer" ) );
        questionSizer->Add( view.notes, 0, wxEXPAND | wxALL, FromDIP( 8 ) );
        questionsSizer->Add( questionSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,
                             FromDIP( 8 ) );
        m_questions.push_back( std::move( view ) );
    }

    scroller->SetSizer( questionsSizer );
    scroller->FitInside();
    root->Add( scroller, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP( 4 ) );

    m_autoResolutionStatus = new wxStaticText( this, wxID_ANY, wxEmptyString );
    root->Add( m_autoResolutionStatus, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
               FromDIP( 12 ) );

    wxBoxSizer* buttons = new wxBoxSizer( wxHORIZONTAL );
    m_stopButton = new wxButton( this, wxID_CANCEL, _( "Stop turn" ) );
    m_submitButton = new wxButton( this, wxID_OK, _( "Answer" ) );
    buttons->AddStretchSpacer();
    buttons->Add( m_stopButton, 0, wxRIGHT, FromDIP( 8 ) );
    buttons->Add( m_submitButton );
    root->Add( buttons, 0, wxEXPAND | wxALL, FromDIP( 12 ) );

    SetSizer( root );
    SetEscapeId( wxID_CANCEL );

    for( size_t index = 0; index < m_questions.size(); ++index )
    {
        QUESTION_VIEW& view = m_questions[index];

        if( view.choice )
        {
            view.choice->Bind( wxEVT_CHOICE, &CODEX_USER_INPUT_DIALOG::onChoiceChanged, this );
            view.choice->SetClientData( reinterpret_cast<void*>( index + 1 ) );
            updateChoiceDescription( index );
        }

        view.notes->Bind( wxEVT_TEXT, &CODEX_USER_INPUT_DIALOG::onTextChanged, this );
    }

    m_submitButton->Bind( wxEVT_BUTTON, &CODEX_USER_INPUT_DIALOG::onSubmit, this );
    m_stopButton->Bind( wxEVT_BUTTON, &CODEX_USER_INPUT_DIALOG::onStop, this );
    Bind( wxEVT_TIMER, &CODEX_USER_INPUT_DIALOG::onTimer, this,
          m_autoResolutionTimer.GetId() );
    Bind( wxEVT_CLOSE_WINDOW, &CODEX_USER_INPUT_DIALOG::onClose, this );

    if( m_request.contains( "autoResolutionMs" )
        && nonNegativeInt64( m_request["autoResolutionMs"] )
        && m_request["autoResolutionMs"].get<int64_t>() > 0 )
    {
        const int64_t delayMs = m_request["autoResolutionMs"].get<int64_t>();
        m_autoResolutionDeadline = std::chrono::steady_clock::now()
                                   + std::chrono::milliseconds( delayMs );
        m_autoResolutionActive = true;
        m_autoResolutionTimer.Start( 1000 );
        updateAutoResolutionStatus();
    }
    else
    {
        m_autoResolutionStatus->SetLabel(
                _( "This answer is required before Codex can continue." ) );
    }

    Fit();
    const wxSize best = GetBestSize();
    const wxSize minimum = FromDIP( wxSize( MIN_DIALOG_WIDTH, MIN_DIALOG_HEIGHT ) );
    const wxSize maximum = FromDIP( wxSize( MAX_DIALOG_WIDTH, MAX_DIALOG_HEIGHT ) );
    SetSize( wxSize( std::clamp( best.GetWidth(), minimum.GetWidth(), maximum.GetWidth() ),
                     std::clamp( best.GetHeight(), minimum.GetHeight(), maximum.GetHeight() ) ) );
    SetMinSize( minimum );
    CentreOnParent();

    if( !m_questions.empty() )
    {
        if( m_questions.front().choice )
            m_questions.front().choice->SetFocus();
        else
            m_questions.front().notes->SetFocus();
    }
}


CODEX_USER_INPUT_DIALOG::~CODEX_USER_INPUT_DIALOG()
{
    m_autoResolutionTimer.Stop();
}


bool CODEX_USER_INPUT_DIALOG::ValidateRequest( const JSON& aRequest, wxString& aError )
{
    if( !aRequest.is_object() || !nonEmptyString( aRequest, "threadId" )
        || !nonEmptyString( aRequest, "turnId" ) || !nonEmptyString( aRequest, "itemId" )
        || !aRequest.contains( "questions" ) || !aRequest["questions"].is_array()
        || aRequest["questions"].empty() || aRequest["questions"].size() > 3 )
    {
        aError = _( "Codex supplied an invalid user-input request." );
        return false;
    }

    if( aRequest.contains( "autoResolutionMs" ) && !aRequest["autoResolutionMs"].is_null()
        && !nonNegativeInt64( aRequest["autoResolutionMs"] ) )
    {
        aError = _( "Codex supplied an invalid automatic-resolution timeout." );
        return false;
    }

    std::set<std::string> questionIds;

    for( const JSON& question : aRequest["questions"] )
    {
        if( !question.is_object() || !nonEmptyString( question, "id" )
            || !nonEmptyString( question, "header" ) || !nonEmptyString( question, "question" ) )
        {
            aError = _( "Codex supplied an incomplete question." );
            return false;
        }

        if( !questionIds.insert( question["id"].get<std::string>() ).second )
        {
            aError = _( "Codex supplied duplicate question identifiers." );
            return false;
        }

        for( const char* booleanKey : { "isOther", "isSecret" } )
        {
            if( question.contains( booleanKey ) && !question[booleanKey].is_boolean() )
            {
                aError = _( "Codex supplied invalid question options." );
                return false;
            }
        }

        if( question.contains( "options" ) && !question["options"].is_null() )
        {
            if( !question["options"].is_array() || question["options"].empty()
                || question["options"].size() > 10 )
            {
                aError = _( "Codex supplied an invalid option list." );
                return false;
            }

            for( const JSON& option : question["options"] )
            {
                if( !option.is_object() || !nonEmptyString( option, "label" )
                    || !option.contains( "description" ) || !option["description"].is_string() )
                {
                    aError = _( "Codex supplied an incomplete answer option." );
                    return false;
                }
            }
        }
    }

    return true;
}


void CODEX_USER_INPUT_DIALOG::Dismiss()
{
    if( m_dismissing )
        return;

    m_dismissing = true;
    m_autoResolutionTimer.Stop();
    m_submitHandler = {};
    m_stopHandler = {};
    Hide();
    Destroy();
}


void CODEX_USER_INPUT_DIALOG::SetStopping( bool aStopping, const wxString& aError )
{
    m_submitting = aStopping;
    m_submitButton->Enable( !aStopping );
    m_stopButton->Enable( !aStopping );

    if( aStopping )
    {
        m_autoResolutionTimer.Stop();
        m_autoResolutionActive = false;
        m_autoResolutionStatus->SetLabel( _( "Stopping the active Codex turn..." ) );
    }
    else if( !aError.IsEmpty() )
    {
        m_autoResolutionStatus->SetLabel( aError );
    }
}


void CODEX_USER_INPUT_DIALOG::updateChoiceDescription( size_t aQuestionIndex )
{
    if( aQuestionIndex >= m_questions.size() )
        return;

    QUESTION_VIEW& view = m_questions[aQuestionIndex];

    if( !view.choice || !view.description )
        return;

    const int selection = view.choice->GetSelection();

    if( selection >= 0 && static_cast<size_t>( selection ) < view.optionDescriptions.size() )
        view.description->SetLabel( view.optionDescriptions[selection] );
    else
        view.description->SetLabel( wxEmptyString );

    view.description->Wrap( FromDIP( MAX_DIALOG_WIDTH - 100 ) );
    Layout();
}


void CODEX_USER_INPUT_DIALOG::snoozeAutoResolution()
{
    if( !m_autoResolutionActive )
        return;

    m_autoResolutionActive = false;
    m_autoResolutionTimer.Stop();
    m_autoResolutionStatus->SetLabel(
            _( "Automatic continuation paused while you answer." ) );
}


void CODEX_USER_INPUT_DIALOG::updateAutoResolutionStatus()
{
    if( !m_autoResolutionActive )
        return;

    const auto now = std::chrono::steady_clock::now();

    if( now >= m_autoResolutionDeadline )
    {
        submitAnswers( true );
        return;
    }

    const auto remaining = m_autoResolutionDeadline - now;
    const auto remainingMs =
            std::chrono::duration_cast<std::chrono::milliseconds>( remaining ).count();
    const long long remainingSeconds = std::max<long long>( 1, ( remainingMs + 999 ) / 1000 );
    m_autoResolutionStatus->SetLabel(
            wxString::Format( _( "Codex can continue with its best judgment in %lld seconds." ),
                              remainingSeconds ) );
}


bool CODEX_USER_INPUT_DIALOG::submitAnswers( bool aAutomatic )
{
    if( m_submitting || !m_submitHandler )
        return false;

    JSON response = { { "answers", JSON::object() } };
    std::string persistedText;
    wxString transcript;
    bool sensitive = false;

    if( aAutomatic )
    {
        transcript = _( "\n[No answer supplied; Codex continued with best judgment.]\n" );
    }
    else
    {
        for( QUESTION_VIEW& view : m_questions )
        {
            std::vector<wxString> displayAnswers;
            JSON answerValues = JSON::array();
            const wxString notes = view.notes->GetValue().Strip( wxString::both );

            if( view.choice )
            {
                const int selection = view.choice->GetSelection();

                if( selection < 0 || static_cast<size_t>( selection ) >= view.optionLabels.size() )
                {
                    wxMessageBox( _( "Choose an answer for every Codex question." ),
                                  _( "Codex needs your input" ), wxOK | wxICON_INFORMATION,
                                  this );
                    view.choice->SetFocus();
                    return false;
                }

                const wxString selected = view.optionLabels[selection];
                answerValues.push_back( std::string( selected.ToUTF8() ) );
                displayAnswers.push_back( selected );
            }
            else if( notes.IsEmpty() )
            {
                wxMessageBox( _( "Answer every Codex question before continuing." ),
                              _( "Codex needs your input" ), wxOK | wxICON_INFORMATION, this );
                view.notes->SetFocus();
                return false;
            }

            if( !notes.IsEmpty() )
            {
                answerValues.push_back( "user_note: " + std::string( notes.ToUTF8() ) );
                displayAnswers.push_back( notes );
            }

            response["answers"][view.id] = { { "answers", std::move( answerValues ) } };
            sensitive = sensitive || view.secret;

            const wxString display = view.secret ? _( "[private answer submitted]" )
                                                 : answerDisplayText( displayAnswers );
            transcript += wxString::Format( _( "\nYou (answer): %s — %s\n" ), view.header,
                                            display );

            if( !view.secret )
            {
                if( !persistedText.empty() )
                    persistedText += "\n\n";

                persistedText += "Codex asked: " + std::string( view.question.ToUTF8() )
                                 + "\nAnswer: " + std::string( display.ToUTF8() );
            }
        }
    }

    m_submitting = true;
    m_submitButton->Disable();
    m_stopButton->Disable();
    m_autoResolutionTimer.Stop();
    m_autoResolutionActive = false;

    if( m_submitHandler( response, persistedText, transcript, sensitive ) )
        return true;

    m_submitting = false;
    m_submitButton->Enable();
    m_stopButton->Enable();
    m_autoResolutionStatus->SetLabel(
            _( "The answer could not be delivered. It remains available to retry." ) );
    return false;
}


void CODEX_USER_INPUT_DIALOG::onSubmit( wxCommandEvent& aEvent )
{
    submitAnswers( false );
}


void CODEX_USER_INPUT_DIALOG::onStop( wxCommandEvent& aEvent )
{
    if( m_submitting || !m_stopHandler )
        return;

    if( m_stopHandler() )
        SetStopping( true );
    else
        SetStopping( false, _( "The Codex turn could not be stopped." ) );
}


void CODEX_USER_INPUT_DIALOG::onChoiceChanged( wxCommandEvent& aEvent )
{
    wxChoice* choice = dynamic_cast<wxChoice*>( aEvent.GetEventObject() );

    if( choice )
    {
        const size_t encodedIndex = reinterpret_cast<size_t>( choice->GetClientData() );

        if( encodedIndex > 0 )
            updateChoiceDescription( encodedIndex - 1 );
    }

    snoozeAutoResolution();
}


void CODEX_USER_INPUT_DIALOG::onTextChanged( wxCommandEvent& aEvent )
{
    snoozeAutoResolution();
    aEvent.Skip();
}


void CODEX_USER_INPUT_DIALOG::onTimer( wxTimerEvent& aEvent )
{
    updateAutoResolutionStatus();
}


void CODEX_USER_INPUT_DIALOG::onClose( wxCloseEvent& aEvent )
{
    if( m_dismissing )
    {
        aEvent.Skip();
        return;
    }

    aEvent.Veto();

    if( !m_submitting )
    {
        wxCommandEvent event;
        onStop( event );
    }
}
