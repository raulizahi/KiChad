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

#ifndef PANEL_EXTERNAL_LAYOUT_SETTINGS_H
#define PANEL_EXTERNAL_LAYOUT_SETTINGS_H

#include <widgets/resettable_panel.h>

class wxCheckBox;
class wxFilePickerCtrl;
class wxSpinCtrl;

/**
 * Preferences page (PCB Editor > External Layout) selecting whether Codex delegates
 * placement and routing to a third-party tool, and which executable that is.
 */
class PANEL_EXTERNAL_LAYOUT_SETTINGS : public RESETTABLE_PANEL
{
public:
    PANEL_EXTERNAL_LAYOUT_SETTINGS( wxWindow* aParent );

    bool TransferDataToWindow() override;
    bool TransferDataFromWindow() override;
    void ResetPanel() override;

private:
    wxCheckBox*       m_enable;
    wxFilePickerCtrl* m_toolPicker;
    wxSpinCtrl*       m_layers;
};

#endif // PANEL_EXTERNAL_LAYOUT_SETTINGS_H
