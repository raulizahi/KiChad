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

#include "panel_pcb_external_layout.h"

#include <settings/kicad_settings.h>
#include <settings/settings_manager.h>

#include <wx/checkbox.h>
#include <wx/filepicker.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>


PANEL_EXTERNAL_LAYOUT_SETTINGS::PANEL_EXTERNAL_LAYOUT_SETTINGS( wxWindow* aParent ) :
        RESETTABLE_PANEL( aParent ),
        m_enable( nullptr ),
        m_toolPicker( nullptr ),
        m_layers( nullptr )
{
    wxBoxSizer* main = new wxBoxSizer( wxVERTICAL );

    m_enable = new wxCheckBox( this, wxID_ANY,
                               _( "Delegate placement and routing to an external tool" ) );
    main->Add( m_enable, 0, wxALL, FromDIP( 8 ) );

    wxStaticText* explanation = new wxStaticText(
            this, wxID_ANY,
            _( "When enabled, new Codex conversations size the board outline to hold all "
               "components without abutting, place only connectors and mechanically "
               "constrained parts, stage every other footprint outside the outline connected "
               "only by the schematic ratsnest, and hand off to the tool below." ) );
    main->Add( explanation, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP( 8 ) );

    wxBoxSizer* toolRow = new wxBoxSizer( wxHORIZONTAL );
    toolRow->Add( new wxStaticText( this, wxID_ANY, _( "Place and route executable:" ) ), 0,
                  wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 6 ) );
    m_toolPicker = new wxFilePickerCtrl( this, wxID_ANY, wxEmptyString,
                                         _( "Select the external place and route executable" ) );
    toolRow->Add( m_toolPicker, 1, wxALIGN_CENTER_VERTICAL );
    main->Add( toolRow, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP( 8 ) );

    wxBoxSizer* layersRow = new wxBoxSizer( wxHORIZONTAL );
    layersRow->Add( new wxStaticText( this, wxID_ANY, _( "Desired copper layer count:" ) ), 0,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 6 ) );
    m_layers = new wxSpinCtrl( this, wxID_ANY );
    m_layers->SetRange( 1, 64 );
    m_layers->SetValue( 2 );
    layersRow->Add( m_layers, 0, wxALIGN_CENTER_VERTICAL );
    main->Add( layersRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    wxStaticText* contract = new wxStaticText(
            this, wxID_ANY,
            _( "The executable is invoked as: <tool> --input-dir <project> --output-dir "
               "<sibling> --layers <copper layer count>. The KICHAD_EXTERNAL_PNR environment "
               "variable is used as a fallback when no path is set here." ) );
    main->Add( contract, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP( 8 ) );

    if( !GetAppSettings<KICAD_SETTINGS>( "kicad" ) )
    {
        m_enable->Disable();
        m_toolPicker->Disable();
        m_layers->Disable();
        main->Add( new wxStaticText( this, wxID_ANY,
                                     _( "These settings are stored by the KiChad project "
                                        "manager and can only be edited when preferences are "
                                        "opened from it." ) ),
                   0, wxALL | wxEXPAND, FromDIP( 8 ) );
    }

    SetSizer( main );

    explanation->Wrap( FromDIP( 560 ) );
    contract->Wrap( FromDIP( 560 ) );
}


bool PANEL_EXTERNAL_LAYOUT_SETTINGS::TransferDataToWindow()
{
    if( KICAD_SETTINGS* settings = GetAppSettings<KICAD_SETTINGS>( "kicad" ) )
    {
        m_enable->SetValue( settings->m_CodexExternalLayoutMode );
        m_toolPicker->SetPath( settings->m_CodexExternalLayoutTool );
        m_layers->SetValue( settings->m_CodexExternalLayoutLayers );
    }

    return true;
}


bool PANEL_EXTERNAL_LAYOUT_SETTINGS::TransferDataFromWindow()
{
    if( KICAD_SETTINGS* settings = GetAppSettings<KICAD_SETTINGS>( "kicad" ) )
    {
        settings->m_CodexExternalLayoutMode = m_enable->GetValue();
        settings->m_CodexExternalLayoutTool = m_toolPicker->GetPath();
        settings->m_CodexExternalLayoutLayers = m_layers->GetValue();
    }

    return true;
}


void PANEL_EXTERNAL_LAYOUT_SETTINGS::ResetPanel()
{
    m_enable->SetValue( false );
    m_toolPicker->SetPath( wxEmptyString );
    m_layers->SetValue( 2 );
}
