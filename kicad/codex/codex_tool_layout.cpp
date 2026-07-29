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

#include <chrono>
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
              { "enum", nlohmann::json::array( { "status", "run", "adopt", "revert" } ) } };
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
               "result, then run DRC and the remaining gates; revert if it is rejected." },
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
        && operation != "revert" )
    {
        return failure( "invalid_arguments",
                        "layout.operation must be status, run, adopt, or revert" );
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
