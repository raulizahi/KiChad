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
#include "lossless_sexpr_document.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <boost/process.hpp>

#include <wx/dir.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include <kiid.h>


namespace
{

constexpr int  DEFAULT_EXTERNAL_PNR_TIMEOUT_SECONDS = 600;
constexpr int  MAX_EXTERNAL_PNR_TIMEOUT_SECONDS = 3600;
constexpr long MAX_EXTERNAL_PNR_LOG_TAIL_BYTES = 4096;

const char* EXTERNAL_PNR_ENV = "KICHAD_EXTERNAL_PNR";


std::string readLogTail( const wxFileName& aLog )
{
    if( !aLog.FileExists() )
        return std::string();

    wxFile file( aLog.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
        return std::string();

    const wxFileOffset length = file.Length();

    if( length <= 0 )
        return std::string();

    const wxFileOffset start = length > MAX_EXTERNAL_PNR_LOG_TAIL_BYTES
                                       ? length - MAX_EXTERNAL_PNR_LOG_TAIL_BYTES
                                       : 0;
    file.Seek( start );
    std::string tail( static_cast<size_t>( length - start ), '\0' );

    if( file.Read( tail.data(), tail.size() ) != static_cast<ssize_t>( tail.size() ) )
        return std::string();

    return tail;
}


bool resolveExternalTool( const wxString& aConfiguredPath, wxFileName& aTool,
                          std::string& aError )
{
    wxString configured = aConfiguredPath;

    if( configured.IsEmpty()
        && ( !wxGetEnv( EXTERNAL_PNR_ENV, &configured ) || configured.IsEmpty() ) )
    {
        aError = std::string( "no external place-and-route tool is configured; set it in "
                              "Preferences > PCB Editor > External Layout, or set " )
                 + EXTERNAL_PNR_ENV + " to the absolute path of the executable";
        return false;
    }

    aTool.Assign( configured );

    if( !aTool.IsAbsolute() || !aTool.FileExists() )
    {
        aError = std::string( EXTERNAL_PNR_ENV )
                 + " must name an existing absolute executable path";
        return false;
    }

    if( !aTool.IsFileExecutable() )
    {
        aError = "the configured external place-and-route tool is not executable";
        return false;
    }

    return true;
}


std::string formatMm( double aMm )
{
    char buffer[64];
    std::snprintf( buffer, sizeof( buffer ), "%.6f", aMm );
    std::string text( buffer );

    while( !text.empty() && text.back() == '0' )
        text.pop_back();

    if( !text.empty() && text.back() == '.' )
        text.pop_back();

    return text + "mm";
}


std::string quoteKds( const std::string& aValue )
{
    std::string quoted = "\"";

    for( char c : aValue )
    {
        if( c == '"' || c == '\\' )
            quoted += '\\';

        quoted += c;
    }

    return quoted + "\"";
}


struct BOARD_EXTRACT
{
    struct PLACEMENT
    {
        std::string reference;
        double      x = 0.0;
        double      y = 0.0;
        double      rotation = 0.0;
        bool        back = false;
    };

    struct TRACK
    {
        std::string net;
        double      x1 = 0.0, y1 = 0.0;
        bool        hasMid = false;
        double      xm = 0.0, ym = 0.0;
        double      x2 = 0.0, y2 = 0.0;
        double      width = 0.0;
        std::string layer;
    };

    struct VIA
    {
        std::string net;
        double      x = 0.0, y = 0.0;
        double      drill = 0.0;
        double      diameter = 0.0;
        std::string startLayer;
        std::string endLayer;
    };

    std::vector<PLACEMENT>   placements;
    std::vector<TRACK>       tracks;
    std::vector<VIA>         vias;
    std::vector<std::string> copperLayers;
    double                   edgeMinX = std::numeric_limits<double>::max();
    double                   edgeMinY = std::numeric_limits<double>::max();
    double                   edgeMaxX = std::numeric_limits<double>::lowest();
    double                   edgeMaxY = std::numeric_limits<double>::lowest();
    bool                     hasEdge = false;
    size_t                   skippedNoNet = 0;
    size_t                   skippedZones = 0;
};


bool extractRoutedBoard( const KICHAD::LOSSLESS_SEXPR_DOCUMENT& aDoc, BOARD_EXTRACT& aOut,
                         std::string& aError )
{
    using DOC = KICHAD::LOSSLESS_SEXPR_DOCUMENT;

    const auto& nodes = aDoc.Nodes();

    if( aDoc.Roots().size() != 1 || aDoc.ListHead( aDoc.Roots()[0] ) != "kicad_pcb" )
    {
        aError = "the adopted board is not a kicad_pcb document";
        return false;
    }

    const size_t root = aDoc.Roots()[0];

    const auto childValues = [&]( size_t aList ) -> std::vector<std::string>
    {
        std::vector<std::string> values;

        for( size_t child : nodes[aList].children )
        {
            if( nodes[child].kind != DOC::NODE_KIND::LIST )
                values.push_back( aDoc.AtomText( child ) );
        }

        return values;
    };

    const auto findChildList = [&]( size_t aList, const std::string& aHead ) -> size_t
    {
        for( size_t child : nodes[aList].children )
        {
            if( nodes[child].kind == DOC::NODE_KIND::LIST && aDoc.ListHead( child ) == aHead )
                return child;
        }

        return DOC::NO_NODE;
    };

    const auto numberAt = [&]( size_t aList, size_t aIndex, double& aValue ) -> bool
    {
        const std::vector<std::string> values = childValues( aList );

        if( aIndex + 1 >= values.size() )
            return false;

        try
        {
            aValue = std::stod( values[aIndex + 1] );
        }
        catch( ... )
        {
            return false;
        }

        return true;
    };

    std::map<std::string, std::string> netNames;

    for( size_t child : nodes[root].children )
    {
        if( nodes[child].kind != DOC::NODE_KIND::LIST )
            continue;

        const std::string head = aDoc.ListHead( child );

        if( head == "net" )
        {
            const std::vector<std::string> values = childValues( child );

            if( values.size() >= 3 )
                netNames[values[1]] = values[2];
        }
        else if( head == "layers" )
        {
            for( size_t entry : nodes[child].children )
            {
                if( nodes[entry].kind != DOC::NODE_KIND::LIST )
                    continue;

                const std::vector<std::string> values = childValues( entry );

                if( values.size() >= 3 && values[1].size() > 3
                    && values[1].compare( values[1].size() - 3, 3, ".Cu" ) == 0
                    && values[2] == "signal" )
                {
                    aOut.copperLayers.push_back( values[1] );
                }
            }
        }
    }

    const auto netName = [&]( size_t aList ) -> std::string
    {
        const size_t netNode = findChildList( aList, "net" );

        if( netNode == DOC::NO_NODE )
            return std::string();

        const std::vector<std::string> values = childValues( netNode );

        if( values.size() < 2 )
            return std::string();

        // KiCad writes net references by table number; some external tools write the
        // net name directly. Accept both.
        const auto mapped = netNames.find( values[1] );
        return mapped != netNames.end() ? mapped->second : values[1];
    };

    for( size_t child : nodes[root].children )
    {
        if( nodes[child].kind != DOC::NODE_KIND::LIST )
            continue;

        const std::string head = aDoc.ListHead( child );

        if( head == "footprint" )
        {
            BOARD_EXTRACT::PLACEMENT placement;

            for( size_t sub : nodes[child].children )
            {
                if( nodes[sub].kind != DOC::NODE_KIND::LIST )
                    continue;

                const std::string subHead = aDoc.ListHead( sub );
                const std::vector<std::string> values = childValues( sub );

                if( subHead == "property" && values.size() >= 3 && values[1] == "Reference" )
                    placement.reference = values[2];
                else if( subHead == "layer" && values.size() >= 2 )
                    placement.back = values[1] == "B.Cu";
                else if( subHead == "at" && nodes[sub].parent == child )
                {
                    numberAt( sub, 0, placement.x );
                    numberAt( sub, 1, placement.y );

                    if( values.size() >= 4 )
                        numberAt( sub, 2, placement.rotation );
                }
            }

            if( !placement.reference.empty() )
                aOut.placements.push_back( placement );
        }
        else if( head == "segment" || head == "arc" )
        {
            BOARD_EXTRACT::TRACK track;
            track.net = netName( child );

            if( track.net.empty() )
            {
                ++aOut.skippedNoNet;
                continue;
            }

            const size_t start = findChildList( child, "start" );
            const size_t mid = findChildList( child, "mid" );
            const size_t end = findChildList( child, "end" );
            const size_t width = findChildList( child, "width" );
            const size_t layer = findChildList( child, "layer" );

            if( start == DOC::NO_NODE || end == DOC::NO_NODE || width == DOC::NO_NODE
                || layer == DOC::NO_NODE )
            {
                continue;
            }

            numberAt( start, 0, track.x1 );
            numberAt( start, 1, track.y1 );
            numberAt( end, 0, track.x2 );
            numberAt( end, 1, track.y2 );
            numberAt( width, 0, track.width );
            track.layer = childValues( layer ).size() >= 2 ? childValues( layer )[1]
                                                           : std::string();

            if( mid != DOC::NO_NODE )
            {
                track.hasMid = true;
                numberAt( mid, 0, track.xm );
                numberAt( mid, 1, track.ym );
            }

            aOut.tracks.push_back( track );
        }
        else if( head == "via" )
        {
            BOARD_EXTRACT::VIA via;
            via.net = netName( child );

            if( via.net.empty() )
            {
                ++aOut.skippedNoNet;
                continue;
            }

            const size_t at = findChildList( child, "at" );
            const size_t size = findChildList( child, "size" );
            const size_t drill = findChildList( child, "drill" );
            const size_t layers = findChildList( child, "layers" );

            if( at == DOC::NO_NODE || size == DOC::NO_NODE || drill == DOC::NO_NODE )
                continue;

            numberAt( at, 0, via.x );
            numberAt( at, 1, via.y );
            numberAt( size, 0, via.diameter );
            numberAt( drill, 0, via.drill );

            if( layers != DOC::NO_NODE )
            {
                const std::vector<std::string> values = childValues( layers );

                if( values.size() >= 3 )
                {
                    via.startLayer = values[1];
                    via.endLayer = values[2];
                }
            }

            aOut.vias.push_back( via );
        }
        else if( head == "zone" )
        {
            ++aOut.skippedZones;
        }
        else if( head.rfind( "gr_", 0 ) == 0 )
        {
            const size_t layer = findChildList( child, "layer" );

            if( layer == DOC::NO_NODE || childValues( layer ).size() < 2
                || childValues( layer )[1] != "Edge.Cuts" )
            {
                continue;
            }

            for( const char* pointHead : { "start", "end", "mid", "center" } )
            {
                const size_t point = findChildList( child, pointHead );

                if( point == DOC::NO_NODE )
                    continue;

                double x = 0.0, y = 0.0;

                if( numberAt( point, 0, x ) && numberAt( point, 1, y ) )
                {
                    aOut.hasEdge = true;
                    aOut.edgeMinX = std::min( aOut.edgeMinX, x );
                    aOut.edgeMinY = std::min( aOut.edgeMinY, y );
                    aOut.edgeMaxX = std::max( aOut.edgeMaxX, x );
                    aOut.edgeMaxY = std::max( aOut.edgeMaxY, y );
                }
            }
        }
    }

    if( aOut.placements.empty() )
    {
        aError = "the adopted board contains no referenced footprints";
        return false;
    }

    return true;
}

wxString defaultOutputDirectoryName( const wxFileName& aProjectDirectory )
{
    // Mirror the staging convention: Foo-no-layout hands off to Foo; anything else
    // routes into a "-routed" sibling.
    wxString name = aProjectDirectory.GetDirs().Last();
    const wxString suffix = wxS( "-no-layout" );

    if( name.EndsWith( suffix ) )
        return name.Left( name.length() - suffix.length() );

    return name + wxS( "-routed" );
}

} // namespace


namespace KICHAD::CODEX_TOOLS
{

nlohmann::json LayoutSpec()
{
    nlohmann::json schema = { { "type", "object" },
                              { "additionalProperties", false },
                              { "required", nlohmann::json::array( { "operation" } ) } };
    schema["properties"]["operation"] =
            { { "type", "string" },
              { "enum", nlohmann::json::array(
                                { "status", "run", "adopt", "revert", "reconcile" } ) } };
    schema["properties"]["outputDirName"] =
            { { "type", "string" }, { "maxLength", 255 },
              { "description",
                "Sibling output directory name; defaults to the project name without its "
                "-no-layout suffix, or with -routed appended." } };
    schema["properties"]["timeoutSeconds"] =
            { { "type", "integer" }, { "minimum", 10 },
              { "maximum", MAX_EXTERNAL_PNR_TIMEOUT_SECONDS },
              { "description", "Maximum run time; defaults to 600." } };
    schema["properties"]["layers"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 64 },
              { "description",
                "Copper layer count the external tool may route on. Defaults to the desired "
                "layer count configured in Preferences > PCB Editor > External Layout; pass "
                "only when the board stackup genuinely differs from that setting." } };

    return { { "type", "function" },
             { "name", "layout" },
             { "description",
               "Invoke the user-configured external third-party place-and-route executable. "
               "It reads the current project directory (expected staged for handoff: outline "
               "sized to hold all components, connectors fixed, all other footprints outside "
               "the outline, no tracks) and writes a fully placed and routed copy of the "
               "project into a sibling output directory. Operation status reports whether the "
               "tool is configured; run executes it and reports the output location. The "
               "input project is never modified by run. Operation adopt first saves the "
               "current staged board to the project's .kichad/pre-layout/ directory, then "
               "copies the routed board from the output directory into the active project; "
               "operation revert restores that saved pre-layout board when the routed result "
               "is rejected. Both require a pre-turn snapshot, and the PCB editor must not "
               "hold the board open (or must reload it afterward). Adopting twice without a "
               "revert replaces the saved pre-layout board with the previously adopted one, "
               "so re-stage before adopting again. After adopt, render and inspect the "
               "result, then run DRC and the remaining gates; revert if it is rejected. "
               "Operation reconcile back-annotates the adopted routed board into the KDS: it "
               "replaces the KDS outline, placements, routes, and vias with the board's "
               "actual geometry as authored statements, validates by compiling, and restores "
               "the previous KDS if compilation fails. After a successful reconcile the KDS "
               "is the single source of truth again and design.apply no longer erases the "
               "routing. Copper zones are not imported and are reported as skipped." },
             { "inputSchema", std::move( schema ) } };
}

} // namespace KICHAD::CODEX_TOOLS


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleLayout( const JSON& aArguments,
                                                             const wxString& aProjectPath,
                                                             bool aMutationAvailable ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string() )
    {
        return failure( "invalid_arguments", "layout requires a string operation" );
    }

    const std::string operation = aArguments["operation"].get<std::string>();

    if( operation != "status" && operation != "run" && operation != "adopt"
        && operation != "revert" && operation != "reconcile" )
    {
        return failure( "invalid_arguments",
                        "layout.operation must be status, run, adopt, revert, or reconcile" );
    }

    if( aProjectPath.IsEmpty() || !wxFileName::DirExists( aProjectPath ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    wxFileName projectDirectory = wxFileName::DirName( aProjectPath );
    projectDirectory.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( projectDirectory.GetDirCount() < 1 )
        return failure( "project_unavailable", "The project directory has no parent" );

    wxString outputName = defaultOutputDirectoryName( projectDirectory );

    if( aArguments.contains( "outputDirName" ) )
    {
        if( !aArguments["outputDirName"].is_string() )
            return failure( "invalid_arguments", "layout.outputDirName must be a string" );

        outputName = wxString::FromUTF8( aArguments["outputDirName"].get<std::string>() );

        if( outputName.IsEmpty() || outputName.Contains( wxS( "/" ) )
            || outputName.Contains( wxS( "\\" ) ) || outputName == wxS( "." )
            || outputName == wxS( ".." ) )
        {
            return failure( "invalid_arguments",
                            "layout.outputDirName must be a single directory name" );
        }
    }

    if( outputName == projectDirectory.GetDirs().Last() )
        return failure( "invalid_arguments",
                        "layout output directory must differ from the project directory" );

    wxFileName outputDirectory = projectDirectory;
    outputDirectory.RemoveLastDir();
    outputDirectory.AppendDir( outputName );

    wxFileName tool;
    std::string toolError;
    const bool configured = resolveExternalTool( ExternalLayoutTool(), tool, toolError );

    if( operation == "status" )
    {
        JSON payload = { { "configured", configured },
                         { "inputDirectory", projectDirectory.GetFullPath().ToUTF8().data() },
                         { "outputDirectory", outputDirectory.GetFullPath().ToUTF8().data() } };

        if( configured )
            payload["tool"] = tool.GetFullPath().ToUTF8().data();
        else
            payload["reason"] = toolError;

        return success( payload );
    }

    if( operation == "adopt" )
    {
        if( !aMutationAvailable )
        {
            return failure( "snapshot_required",
                            "A complete pre-turn project snapshot is required to adopt the "
                            "routed board" );
        }

        if( !outputDirectory.DirExists() )
        {
            return failure( "output_missing",
                            "the layout output directory does not exist; run the external "
                            "tool first" );
        }

        wxDir routedDir( outputDirectory.GetFullPath() );
        wxString routedName;

        if( !routedDir.IsOpened()
            || !routedDir.GetFirst( &routedName, wxS( "*.kicad_pcb" ), wxDIR_FILES ) )
        {
            return failure( "invalid_output",
                            "the layout output directory contains no .kicad_pcb board" );
        }

        wxString extraRouted;

        if( routedDir.GetNext( &extraRouted ) )
        {
            return failure( "invalid_output",
                            "the layout output directory contains more than one .kicad_pcb; "
                            "adopt requires exactly one" );
        }

        // Overwrite the project's own board file, keeping the project's board filename.
        wxDir projectDir( projectDirectory.GetFullPath() );
        wxString targetName;

        if( !projectDir.IsOpened()
            || !projectDir.GetFirst( &targetName, wxS( "*.kicad_pcb" ), wxDIR_FILES ) )
        {
            targetName = routedName;
        }
        else
        {
            wxString extraTarget;

            if( projectDir.GetNext( &extraTarget ) )
            {
                return failure( "invalid_arguments",
                                "the active project contains more than one .kicad_pcb; adopt "
                                "cannot determine which board to replace" );
            }
        }

        const wxFileName source( outputDirectory.GetFullPath(), routedName );
        const wxFileName target( projectDirectory.GetFullPath(), targetName );

        // Preserve the staged (unrouted) board so layout.revert can restore it if the
        // routed result is rejected.
        wxFileName backupDir = wxFileName::DirName( projectDirectory.GetFullPath() );
        backupDir.AppendDir( wxS( ".kichad" ) );
        backupDir.AppendDir( wxS( "pre-layout" ) );

        if( !backupDir.DirExists()
            && !wxFileName::Mkdir( backupDir.GetFullPath(), 0755, wxPATH_MKDIR_FULL ) )
        {
            return failure( "write_failed",
                            "could not create the project's pre-layout backup directory" );
        }

        const wxFileName backup( backupDir.GetFullPath(), targetName );

        if( target.FileExists()
            && !wxCopyFile( target.GetFullPath(), backup.GetFullPath(), true ) )
        {
            return failure( "write_failed",
                            "could not back up the staged board before adoption" );
        }

        if( !wxCopyFile( source.GetFullPath(), target.GetFullPath(), true ) )
        {
            return failure( "write_failed",
                            "could not copy the routed board into the active project" );
        }

        return success( { { "adoptedBoard", targetName.ToUTF8().data() },
                          { "sourceBoard", source.GetFullPath().ToUTF8().data() },
                          { "preLayoutBackup", backup.GetFullPath().ToUTF8().data() },
                          { "outputDirectory",
                            outputDirectory.GetFullPath().ToUTF8().data() } } );
    }

    if( operation == "revert" )
    {
        if( !aMutationAvailable )
        {
            return failure( "snapshot_required",
                            "A complete pre-turn project snapshot is required to revert to the "
                            "pre-layout board" );
        }

        wxFileName backupDir = wxFileName::DirName( projectDirectory.GetFullPath() );
        backupDir.AppendDir( wxS( ".kichad" ) );
        backupDir.AppendDir( wxS( "pre-layout" ) );

        if( !backupDir.DirExists() )
        {
            return failure( "backup_missing",
                            "no pre-layout backup exists; layout.adopt has not been run in "
                            "this project" );
        }

        wxDir savedDir( backupDir.GetFullPath() );
        wxString savedName;

        if( !savedDir.IsOpened()
            || !savedDir.GetFirst( &savedName, wxS( "*.kicad_pcb" ), wxDIR_FILES ) )
        {
            return failure( "backup_missing",
                            "the pre-layout backup directory contains no .kicad_pcb board" );
        }

        wxString extraSaved;

        if( savedDir.GetNext( &extraSaved ) )
        {
            return failure( "backup_missing",
                            "the pre-layout backup directory contains more than one "
                            ".kicad_pcb; revert requires exactly one" );
        }

        const wxFileName backup( backupDir.GetFullPath(), savedName );
        const wxFileName target( projectDirectory.GetFullPath(), savedName );

        if( !wxCopyFile( backup.GetFullPath(), target.GetFullPath(), true ) )
        {
            return failure( "write_failed",
                            "could not restore the pre-layout board into the active project" );
        }

        return success( { { "revertedBoard", savedName.ToUTF8().data() },
                          { "preLayoutBackup", backup.GetFullPath().ToUTF8().data() } } );
    }

    if( operation == "reconcile" )
    {
        if( !aMutationAvailable )
        {
            return failure( "snapshot_required",
                            "A complete pre-turn project snapshot is required to reconcile the "
                            "routed board into the KDS" );
        }

        const auto singleFile = [&]( const wxString& aPattern, wxString& aName,
                                     const char* aWhat ) -> JSON
        {
            wxDir dir( projectDirectory.GetFullPath() );
            wxString extra;

            if( !dir.IsOpened() || !dir.GetFirst( &aName, aPattern, wxDIR_FILES ) )
                return failure( "invalid_arguments",
                                std::string( "the active project contains no " ) + aWhat );

            if( dir.GetNext( &extra ) )
                return failure( "invalid_arguments",
                                std::string( "the active project contains more than one " )
                                        + aWhat );

            return JSON();
        };

        wxString boardName, kdsName;

        if( JSON error = singleFile( wxS( "*.kicad_pcb" ), boardName, ".kicad_pcb board" );
            !error.is_null() )
            return error;

        if( JSON error = singleFile( wxS( "*.kicad_kds" ), kdsName, ".kicad_kds design" );
            !error.is_null() )
            return error;

        const auto readAll = [&]( const wxFileName& aPath, std::string& aText ) -> bool
        {
            wxFile file( aPath.GetFullPath(), wxFile::read );
            const wxFileOffset length = file.IsOpened() ? file.Length() : -1;

            if( length < 0 || length > 64 * 1024 * 1024 )
                return false;

            aText.resize( static_cast<size_t>( length ) );
            return file.Read( aText.data(), aText.size() )
                   == static_cast<ssize_t>( aText.size() );
        };

        const wxFileName boardPath( projectDirectory.GetFullPath(), boardName );
        const wxFileName kdsPath( projectDirectory.GetFullPath(), kdsName );
        std::string boardText, kdsText, parseError;

        if( !readAll( boardPath, boardText ) )
            return failure( "read_failed", "could not read the adopted board file" );

        if( !readAll( kdsPath, kdsText ) )
            return failure( "read_failed", "could not read the KDS design file" );

        auto boardDoc = KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( std::move( boardText ),
                                                                &parseError );

        if( !boardDoc )
            return failure( "parse_failed", "could not parse the adopted board: " + parseError );

        BOARD_EXTRACT extract;
        std::string extractError;

        if( !extractRoutedBoard( *boardDoc, extract, extractError ) )
            return failure( "invalid_output", extractError );

        if( !extract.hasEdge )
            return failure( "invalid_output", "the adopted board has no Edge.Cuts outline" );

        auto kdsDoc = KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( std::move( kdsText ),
                                                              &parseError );

        if( !kdsDoc )
            return failure( "parse_failed", "could not parse the KDS design: " + parseError );

        const auto& kdsNodes = kdsDoc->Nodes();
        size_t boardList = KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE;

        for( size_t rootNode : kdsDoc->Roots() )
        {
            if( kdsNodes[rootNode].kind == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
                && kdsDoc->ListHead( rootNode ) == "board" )
            {
                boardList = rootNode;
                break;
            }
        }

        if( boardList == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE )
            return failure( "invalid_source", "the KDS design has no top-level (board ...)" );

        std::string editError;
        size_t existingOutline = KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE;
        size_t existingStackup = KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE;

        for( size_t child : kdsNodes[boardList].children )
        {
            if( kdsNodes[child].kind != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST )
                continue;

            const std::string head = kdsDoc->ListHead( child );

            if( head == "place" || head == "route" || head == "via" )
            {
                if( !kdsDoc->RemoveNode( child, &editError ) )
                    return failure( "write_failed", "KDS edit failed: " + editError );
            }
            else if( head == "outline" )
            {
                existingOutline = child;
            }
            else if( head == "stackup" )
            {
                existingStackup = child;
            }
        }

        const std::string outlineText =
                "(outline (rectangle pcb_outline (start " + formatMm( extract.edgeMinX ) + " "
                + formatMm( extract.edgeMinY ) + ") (end " + formatMm( extract.edgeMaxX ) + " "
                + formatMm( extract.edgeMaxY )
                + ") (stroke 0.25mm solid) (layers Edge.Cuts) (fill none)))";

        if( existingOutline != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE )
        {
            if( !kdsDoc->ReplaceNode( existingOutline, outlineText, &editError ) )
                return failure( "write_failed", "KDS edit failed: " + editError );
        }

        std::string statements;

        if( existingOutline == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE )
            statements += "\n  " + outlineText;

        if( extract.copperLayers.size() > 2
            && existingStackup == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE )
        {
            statements += "\n  (stackup custom (layers";

            for( const std::string& layer : extract.copperLayers )
                statements += " " + layer;

            statements += "))";
        }

        for( const BOARD_EXTRACT::PLACEMENT& placement : extract.placements )
        {
            statements += "\n  (place " + placement.reference + " (at "
                          + formatMm( placement.x ) + " " + formatMm( placement.y ) + ")";

            if( placement.rotation != 0.0 )
            {
                std::string angle = formatMm( placement.rotation );
                angle.replace( angle.size() - 2, 2, "deg" );
                statements += " (rotation " + angle + ")";
            }

            if( placement.back )
                statements += " (side back)";

            statements += ")";
        }

        size_t identifier = 0;

        for( const BOARD_EXTRACT::TRACK& track : extract.tracks )
        {
            statements += "\n  (route " + quoteKds( track.net ) + " (id klrt"
                          + std::to_string( ++identifier ) + ") (from " + formatMm( track.x1 )
                          + " " + formatMm( track.y1 ) + ")";

            if( track.hasMid )
                statements += " (mid " + formatMm( track.xm ) + " " + formatMm( track.ym ) + ")";

            statements += " (to " + formatMm( track.x2 ) + " " + formatMm( track.y2 )
                          + ") (width " + formatMm( track.width ) + ") (layer " + track.layer
                          + "))";
        }

        for( const BOARD_EXTRACT::VIA& via : extract.vias )
        {
            statements += "\n  (via " + quoteKds( via.net ) + " (id klvi"
                          + std::to_string( ++identifier ) + ") (at " + formatMm( via.x ) + " "
                          + formatMm( via.y ) + ") (drill " + formatMm( via.drill )
                          + ") (diameter " + formatMm( via.diameter ) + ")";

            if( !via.startLayer.empty() && !via.endLayer.empty() )
                statements += " (layers " + via.startLayer + " " + via.endLayer + ")";

            statements += ")";
        }

        statements += "\n";

        if( !kdsDoc->InsertBeforeClosingList( boardList, statements, &editError ) )
            return failure( "write_failed", "KDS edit failed: " + editError );

        std::string rendered;

        if( !kdsDoc->Render( rendered, &editError ) )
            return failure( "write_failed", "KDS render failed: " + editError );

        // Back up the current KDS, write the reconciled one, and validate by compiling;
        // restore the backup if the compiler rejects the generated statements.
        wxFileName backupDir = wxFileName::DirName( projectDirectory.GetFullPath() );
        backupDir.AppendDir( wxS( ".kichad" ) );
        backupDir.AppendDir( wxS( "pre-layout" ) );

        if( !backupDir.DirExists()
            && !wxFileName::Mkdir( backupDir.GetFullPath(), 0755, wxPATH_MKDIR_FULL ) )
        {
            return failure( "write_failed", "could not create the pre-layout backup directory" );
        }

        const wxFileName kdsBackup( backupDir.GetFullPath(), kdsName );

        if( !wxCopyFile( kdsPath.GetFullPath(), kdsBackup.GetFullPath(), true ) )
            return failure( "write_failed", "could not back up the KDS before reconciling" );

        {
            wxFile out( kdsPath.GetFullPath(), wxFile::write );

            if( !out.IsOpened() || out.Write( rendered.data(), rendered.size() )
                                           != rendered.size() )
            {
                wxCopyFile( kdsBackup.GetFullPath(), kdsPath.GetFullPath(), true );
                return failure( "write_failed", "could not write the reconciled KDS" );
            }
        }

        const JSON compileResult = handleDesign(
                { { "operation", "compile" }, { "path", std::string( kdsName.ToUTF8() ) } },
                aProjectPath, false, wxString(), std::chrono::milliseconds( 2000 ), {} );

        if( !compileResult.value( "success", false ) )
        {
            wxCopyFile( kdsBackup.GetFullPath(), kdsPath.GetFullPath(), true );
            JSON details = { { "compile", compileResult } };
            return failure( "compile_failed",
                            "the reconciled KDS did not compile; the previous KDS was restored",
                            details );
        }

        return success( { { "kds", kdsName.ToUTF8().data() },
                          { "placements", extract.placements.size() },
                          { "routes", extract.tracks.size() },
                          { "vias", extract.vias.size() },
                          { "copperLayers", extract.copperLayers.size() },
                          { "skippedZones", extract.skippedZones },
                          { "skippedNoNetItems", extract.skippedNoNet },
                          { "kdsBackup", kdsBackup.GetFullPath().ToUTF8().data() } } );
    }

    if( !configured )
        return failure( "tool_unconfigured", toolError );

    if( outputDirectory.DirExists() )
    {
        wxDir existing( outputDirectory.GetFullPath() );
        wxString ignored;

        if( existing.IsOpened() && existing.GetFirst( &ignored ) )
        {
            return failure( "output_exists",
                            "the layout output directory already exists and is not empty; "
                            "remove it or pass a different outputDirName" );
        }
    }

    int64_t layers = ExternalLayoutLayers();

    if( aArguments.contains( "layers" ) )
    {
        if( !aArguments["layers"].is_number_integer() )
            return failure( "invalid_arguments", "layout.layers must be an integer" );

        layers = aArguments["layers"].get<int64_t>();
    }

    if( layers < 1 || layers > 64 )
        return failure( "invalid_arguments", "layout.layers must be between 1 and 64" );

    int timeoutSeconds = DEFAULT_EXTERNAL_PNR_TIMEOUT_SECONDS;

    if( aArguments.contains( "timeoutSeconds" ) )
    {
        if( !aArguments["timeoutSeconds"].is_number_integer() )
            return failure( "invalid_arguments", "layout.timeoutSeconds must be an integer" );

        const int64_t requested = aArguments["timeoutSeconds"].get<int64_t>();

        if( requested < 10 || requested > MAX_EXTERNAL_PNR_TIMEOUT_SECONDS )
        {
            return failure( "invalid_arguments", "layout.timeoutSeconds must be between 10 "
                                                 "and 3600" );
        }

        timeoutSeconds = static_cast<int>( requested );
    }

    namespace bp = boost::process;

    wxFileName temporaryRoot = wxFileName::DirName( wxFileName::GetTempDir() );
    temporaryRoot.AppendDir( wxS( "kichad-layout-" ) + KIID().AsString() );

    if( !wxFileName::Mkdir( temporaryRoot.GetFullPath(), 0700 ) )
        return failure( "tool_failed", "could not create a private layout log directory" );

    wxFileName stdoutLog( temporaryRoot.GetFullPath(), wxS( "stdout.log" ) );
    wxFileName stderrLog( temporaryRoot.GetFullPath(), wxS( "stderr.log" ) );

    bool        finished = false;
    int         exitCode = -1;
    std::string runError;
    const auto  started = std::chrono::steady_clock::now();

    try
    {
        bp::child process( tool.GetFullPath().ToStdString(),
                           bp::args( { std::string( "--input-dir" ),
                                       projectDirectory.GetFullPath().ToStdString(),
                                       std::string( "--output-dir" ),
                                       outputDirectory.GetFullPath().ToStdString(),
                                       std::string( "--layers" ),
                                       std::to_string( layers ) } ),
                           bp::std_out > stdoutLog.GetFullPath().ToStdString(),
                           bp::std_err > stderrLog.GetFullPath().ToStdString() );
        const auto deadline = started + std::chrono::seconds( timeoutSeconds );
        std::error_code processError;

        while( process.running( processError ) && !processError
               && std::chrono::steady_clock::now() < deadline )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        }

        finished = !process.running( processError ) && !processError;

        if( !finished )
        {
            process.terminate();
            process.wait();

            if( processError )
                runError = "external place-and-route process failed: " + processError.message();
        }
        else
        {
            exitCode = process.exit_code();
        }
    }
    catch( const std::exception& error )
    {
        runError = std::string( "could not run the external place-and-route tool: " )
                   + error.what();
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started );
    const std::string stderrTail = readLogTail( stderrLog );

    if( !runError.empty() || !finished || exitCode != 0 )
    {
        if( runError.empty() )
        {
            runError = !finished ? "the external place-and-route tool exceeded its timeout"
                                 : "the external place-and-route tool reported failure";
        }

        JSON details = { { "exitCode", exitCode },
                         { "elapsedMs", elapsed.count() } };

        if( !stderrTail.empty() )
            details["stderrTail"] = stderrTail;

        return failure( "external_pnr_failed", runError, details );
    }

    if( !outputDirectory.DirExists() )
    {
        return failure( "invalid_output",
                        "the external place-and-route tool exited successfully but did not "
                        "create the output directory" );
    }

    wxDir produced( outputDirectory.GetFullPath() );
    wxString boardFile;
    bool hasBoard = produced.IsOpened()
                    && produced.GetFirst( &boardFile, wxS( "*.kicad_pcb" ), wxDIR_FILES );

    if( !hasBoard )
    {
        return failure( "invalid_output",
                        "the external place-and-route output directory contains no "
                        ".kicad_pcb board" );
    }

    JSON payload = { { "outputDirectory", outputDirectory.GetFullPath().ToUTF8().data() },
                     { "board", boardFile.ToUTF8().data() },
                     { "exitCode", exitCode },
                     { "elapsedMs", elapsed.count() } };

    if( !stderrTail.empty() )
        payload["stderrTail"] = stderrTail;

    return success( payload );
}
