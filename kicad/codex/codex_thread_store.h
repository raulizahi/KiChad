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

#ifndef KICHAD_CODEX_THREAD_STORE_H
#define KICHAD_CODEX_THREAD_STORE_H

#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>


/** Persists the active Codex conversation keyed by project. */
class CODEX_THREAD_STORE
{
public:
    struct MESSAGE
    {
        std::string role;
        std::string text;
    };

    struct BINDING
    {
        std::string          threadId;
        int                  toolSchemaVersion = 0;
        std::vector<MESSAGE> messages;
    };

    explicit CODEX_THREAD_STORE( wxString aStoragePath = wxString() ) :
            m_storagePath( std::move( aStoragePath ) )
    {}

    BINDING Load( const wxString& aProjectPath ) const;

    /**
     * Recover a conversation from a project's codex_dialog.txt transcript (entries of the form
     * "[YYYY-MM-DD HH:MM:SS] USER:" / "CODEX:" followed by the text and a blank line).  Only
     * the most recent aMaxBytes of message text is kept so a long project history still fits
     * a fresh thread.
     */
    static std::vector<MESSAGE> ParseDialogLog( const std::string& aLog,
                                                size_t aMaxBytes = 256 * 1024 );
    bool Save( const wxString& aProjectPath, const BINDING& aBinding,
               wxString* aError = nullptr ) const;
    bool Clear( const wxString& aProjectPath, wxString* aError = nullptr ) const;

private:
    wxString storagePath() const;
    std::string projectKey( const wxString& aProjectPath ) const;

    wxString m_storagePath;
};

#endif // KICHAD_CODEX_THREAD_STORE_H
