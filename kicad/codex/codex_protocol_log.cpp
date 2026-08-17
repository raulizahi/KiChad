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

#include "codex_protocol_log.h"
#include "kichad_remove_file.h"

#include "codex_paths.h"

#include <algorithm>
#include <cctype>
#include <iterator>

#include <wx/datetime.h>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>


namespace
{

using JSON = nlohmann::json;

constexpr wxFileOffset MAX_LOG_BYTES = 5 * 1024 * 1024;
constexpr size_t       MAX_MESSAGE_BYTES = 256 * 1024;
constexpr size_t       MAX_TEXT_BYTES = 16 * 1024;


bool sensitiveKey( std::string aKey )
{
    std::transform( aKey.begin(), aKey.end(), aKey.begin(),
                    []( unsigned char aCharacter ) { return std::tolower( aCharacter ); } );

    static const char* sensitive[] = {
        "apikey", "authurl", "authorization", "cookie", "credential", "email",
        "encrypted_content", "password", "secret", "token", "usercode"
    };

    return std::any_of( std::begin( sensitive ), std::end( sensitive ),
                        [&aKey]( const char* aNeedle )
                        {
                            return aKey.find( aNeedle ) != std::string::npos;
                        } );
}


void redact( JSON& aValue )
{
    if( aValue.is_object() )
    {
        for( auto& [key, value] : aValue.items() )
        {
            if( sensitiveKey( key ) )
                value = "[REDACTED]";
            else
                redact( value );
        }
    }
    else if( aValue.is_array() )
    {
        for( JSON& value : aValue )
            redact( value );
    }
}


std::string timestamp()
{
    const wxDateTime now = wxDateTime::Now().ToUTC();
    wxString value = now.Format( wxS( "%Y-%m-%dT%H:%M:%S" ) );
    value += wxString::Format( wxS( ".%03dZ" ), now.GetMillisecond() );
    return std::string( value.ToUTF8() );
}


void rotateIfNeeded( const wxString& aPath )
{
    if( !wxFileName::FileExists( aPath ) || wxFileName::GetSize( aPath ) < MAX_LOG_BYTES )
        return;

    const wxString previous = aPath + wxS( ".1" );

    if( wxFileName::FileExists( previous ) )
        KICHAD::RemoveFileWithRetry( previous );

    wxRenameFile( aPath, previous, true );
}


void append( const char* aEvent, JSON aDetail )
{
    const wxString path = KICHAD::CODEX_PATHS::DiagnosticLog();
    wxFileName target( path );

    if( !wxFileName::Mkdir( target.GetPath(), 0700, wxPATH_MKDIR_FULL )
        && !wxFileName::DirExists( target.GetPath() ) )
    {
        return;
    }

    redact( aDetail );
    std::string serializedDetail = aDetail.dump();

    if( serializedDetail.size() > MAX_MESSAGE_BYTES )
    {
        aDetail = {
            { "truncated", true },
            { "originalBytes", serializedDetail.size() },
            { "preview", serializedDetail.substr( 0, MAX_TEXT_BYTES ) }
        };
    }

    const JSON record = {
        { "timestamp", timestamp() },
        { "event", aEvent ? aEvent : "unknown" },
        { "detail", std::move( aDetail ) }
    };
    const std::string line = record.dump() + "\n";

    rotateIfNeeded( path );
    wxFFile file( path, wxS( "ab" ) );

    if( !file.IsOpened() )
        return;

    wxChmod( path, wxS_IRUSR | wxS_IWUSR );
    file.Write( line.data(), line.size() );
    file.Flush();
}

} // namespace


namespace KICHAD::CODEX_PROTOCOL_LOG
{

void Protocol( const char* aDirection, const JSON& aMessage )
{
    append( "protocol", { { "direction", aDirection ? aDirection : "unknown" },
                           { "message", aMessage } } );
}


void Event( const char* aEvent, const JSON& aDetail )
{
    append( aEvent, aDetail );
}


void Text( const char* aEvent, const std::string& aText )
{
    const std::string text = aText.size() > MAX_TEXT_BYTES
                                     ? aText.substr( 0, MAX_TEXT_BYTES ) + "[TRUNCATED]"
                                     : aText;
    append( aEvent, { { "text", text } } );
}

} // namespace KICHAD::CODEX_PROTOCOL_LOG
