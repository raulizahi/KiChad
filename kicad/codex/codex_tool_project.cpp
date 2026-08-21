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

#include "codex_tool_registry.h"
#include "codex_tool_internal.h"

#include <build_version.h>

#include <functional>

#include <wx/dir.h>
#include <wx/filename.h>


namespace KICHAD::CODEX_TOOLS
{

nlohmann::json ProjectSpec()
{
    nlohmann::json schema = { { "type", "object" },
                              { "additionalProperties", false },
                              { "required", nlohmann::json::array( { "operation" } ) } };
    schema["properties"]["operation"] =
            { { "type", "string" }, { "enum", nlohmann::json::array( { "context" } ) } };

    return { { "type", "function" },
             { "name", "project" },
             { "description",
               "Read the active KiChad project context. Use operation 'context' to discover "
               "the stable KiCad version, design files, and whether a turn snapshot allows "
               "mutation. The response's libraryAssets array lists every .kicad_sym symbol "
               "library, .pretty footprint library, and loose .kicad_mod found anywhere in "
               "the project directory (including extracted vendor archives in "
               "subdirectories) as project-relative paths; reference them in KDS library "
               "statements via ${KIPRJMOD}/<path> instead of reporting them missing." },
             { "inputSchema", std::move( schema ) } };
}

} // namespace KICHAD::CODEX_TOOLS


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleProject(
        const JSON& aArguments, const wxString& aProjectPath, bool aMutationAvailable ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string()
        || aArguments["operation"].get<std::string>() != "context" )
    {
        return failure( "invalid_arguments", "project.operation must be 'context'" );
    }

    wxString path = aProjectPath;

    if( !wxFileName::DirExists( path ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    JSON files = JSON::array();
    wxDir directory( path );
    wxString name;
    bool found = directory.GetFirst( &name, wxS( "*.kicad_*" ), wxDIR_FILES );

    while( found )
    {
        wxFileName file( path, name );
        wxULongLong size = file.GetSize();
        JSON item = { { "name", std::string( name.ToUTF8() ) } };

        if( size != wxInvalidSize )
            item["bytes"] = size.GetValue();

        files.emplace_back( std::move( item ) );
        found = directory.GetNext( &name );
    }

    // Library assets are often supplied as extracted vendor archives in subdirectories
    // (Ultra Librarian, SnapEDA, manufacturer downloads).  Surface every .kicad_sym,
    // .pretty directory, and loose .kicad_mod under the project so the agent can
    // reference them by relative path instead of reporting them missing.
    JSON libraryAssets = JSON::array();

    std::function<void( const wxString&, const wxString&, int )> scanAssets =
            [&]( const wxString& aDir, const wxString& aRelative, int aDepth )
    {
        if( aDepth > 4 || libraryAssets.size() >= 256 )
            return;

        wxDir scan( aDir );
        wxString entry;

        if( !scan.IsOpened() )
            return;

        bool more = scan.GetFirst( &entry, wxEmptyString, wxDIR_FILES | wxDIR_DIRS );

        while( more )
        {
            const wxString relative = aRelative.IsEmpty() ? entry : aRelative + wxS( "/" ) + entry;
            const wxFileName full( aDir, entry );
            const wxString fullDir = full.GetFullPath();

            if( wxFileName::DirExists( fullDir ) )
            {
                if( !entry.StartsWith( wxS( "." ) ) && !entry.EndsWith( wxS( "-backups" ) ) )
                {
                    if( entry.EndsWith( wxS( ".pretty" ) ) )
                    {
                        wxDir pretty( fullDir );
                        wxString mod;
                        size_t footprints = 0;

                        for( bool m = pretty.IsOpened()
                                      && pretty.GetFirst( &mod, wxS( "*.kicad_mod" ), wxDIR_FILES );
                             m; m = pretty.GetNext( &mod ) )
                        {
                            ++footprints;
                        }

                        libraryAssets.emplace_back(
                                JSON{ { "kind", "footprintLibrary" },
                                      { "path", std::string( relative.ToUTF8() ) },
                                      { "footprints", footprints } } );
                    }
                    else
                    {
                        scanAssets( fullDir, relative, aDepth + 1 );
                    }
                }
            }
            else if( entry.EndsWith( wxS( ".kicad_sym" ) ) && !aRelative.IsEmpty() )
            {
                libraryAssets.emplace_back(
                        JSON{ { "kind", "symbolLibrary" },
                              { "path", std::string( relative.ToUTF8() ) } } );
            }
            else if( entry.EndsWith( wxS( ".kicad_mod" ) ) && !aRelative.EndsWith( wxS( ".pretty" ) ) )
            {
                libraryAssets.emplace_back(
                        JSON{ { "kind", "looseFootprint" },
                              { "path", std::string( relative.ToUTF8() ) } } );
            }

            more = scan.GetNext( &entry );
        }
    };

    scanAssets( path, wxString(), 0 );

    // Top-level .kicad_sym files are already visible in "files", but include them here
    // too so the asset list is complete on its own.
    {
        wxDir topLevel( path );
        wxString symName;

        for( bool m = topLevel.IsOpened()
                      && topLevel.GetFirst( &symName, wxS( "*.kicad_sym" ), wxDIR_FILES );
             m; m = topLevel.GetNext( &symName ) )
        {
            libraryAssets.emplace_back( JSON{ { "kind", "symbolLibrary" },
                                              { "path", std::string( symName.ToUTF8() ) } } );
        }
    }

    JSON payload = {
        { "operation", "context" },
        { "projectPath", std::string( path.ToUTF8() ) },
        { "kicadVersion", std::string( GetBuildVersion().ToUTF8() ) },
        { "files", std::move( files ) },
        { "libraryAssets", std::move( libraryAssets ) },
        { "mutationAvailable", aMutationAvailable }
    };

    return success( payload );
}
