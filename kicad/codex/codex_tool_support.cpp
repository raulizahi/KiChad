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

#include "kichad_protobuf_compat.h"
#include "codex_tool_internal.h"
#include "board_render_artifact_validator.h"
#include "board_ps_artifact_validator.h"
#include "design_script_compiler.h"
#include "design_script_layout_analyzer.h"
#include "design_script_pcb_planner.h"
#include "design_script_pcb_reconciler.h"
#include "design_script_schematic_planner.h"
#include "design_script_schematic_reconciler.h"
#include "design_script_symbol_resolver.h"
#include "fabrication_artifact_validator.h"
#include "legacy_bom_artifact_validator.h"
#include "mechanical_artifact_validator.h"
#include "production_package_writer.h"
#include "schematic_bom_artifact_validator.h"
#include "schematic_dxf_artifact_validator.h"
#include "schematic_pdf_artifact_validator.h"
#include "schematic_ps_artifact_validator.h"
#include "schematic_svg_artifact_validator.h"
#include "stepz_artifact_validator.h"
#include "three_d_pdf_artifact_validator.h"
#include "u3d_artifact_validator.h"
#include "kicad_ipc_client.h"
#include "kicad_ipc_transaction.h"
#include "lossless_sexpr_document.h"

#include <build_version.h>
#include <env_vars.h>
#include <kiid.h>
#include <libraries/library_table_parser.h>
#include <paths.h>

#include <algorithm>
#include <array>
#include "kichad_boost_process.h"
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <api/board/board_commands.pb.h>
#include <api/board/board_types.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/commands/project_commands.pb.h>
#include <api/common/types/project_settings.pb.h>
#include <google/protobuf/util/field_mask_util.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/message_differencer.h>
#include <wx/base64.h>
#include <wx/datetime.h>
#include <wx/dir.h>
#include <wx/file.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>
#include <wx/wfstream.h>
#include <wx/xml/xml.h>
#include <wx/zipstrm.h>

#include <picosha2.h>


namespace
{

using JSON = nlohmann::json;
using KICHAD::CODEX_TOOLS::PRIVATE_TEMPORARY_DIRECTORY;

constexpr wxFileOffset MAX_INSPECTION_BYTES = 16 * 1024 * 1024;
constexpr size_t       MAX_EXPRESSION_BYTES = 32 * 1024;
constexpr size_t       MAX_RESULT_BYTES = 256 * 1024;
constexpr size_t       MAX_DISTINCT_HEADS = 512;
constexpr size_t       MAX_PCB_ARGUMENT_BYTES = 1024 * 1024;
constexpr size_t       MAX_PCB_RESULT_BYTES = 256 * 1024;
constexpr size_t       MAX_DESIGN_SCRIPT_BYTES = 16 * 1024 * 1024;
constexpr size_t       MAX_DESIGN_STATE_BYTES = 64 * 1024 * 1024;
constexpr size_t       MAX_PROJECT_LIBRARY_TABLE_BYTES = 1024 * 1024;
constexpr size_t       MAX_PROJECT_LIBRARY_ROWS = 256;
constexpr size_t       MAX_SCHEMATIC_FILE_BYTES = 16 * 1024 * 1024;
constexpr size_t       MAX_SCHEMATIC_INVENTORY_BYTES = 32 * 1024 * 1024;
constexpr wxFileOffset MAX_VERIFICATION_REPORT_BYTES = 64 * 1024 * 1024;
constexpr size_t       MAX_VERIFICATION_RESULT_BYTES = 240 * 1024;
constexpr size_t       MAX_VERIFICATION_IGNORED_CHECKS = 200;
constexpr size_t       MAX_VERIFICATION_IGNORED_BYTES = 64 * 1024;
constexpr uintmax_t    MAX_VERIFICATION_SNAPSHOT_BYTES = 512ULL * 1024ULL * 1024ULL;
constexpr size_t       MAX_VERIFICATION_SNAPSHOT_FILES = 10000;
constexpr size_t       MAX_PCB_FOOTPRINTS = 10000;
constexpr size_t       MAX_PCB_ZONES = 10000;
constexpr auto         MAX_ZONE_REFILL_WAIT = std::chrono::seconds( 30 );
constexpr auto         MAX_SCHEMATIC_VALIDATION_WAIT = std::chrono::seconds( 30 );
constexpr auto         MAX_NATIVE_CHECK_WAIT = std::chrono::seconds( 60 );
constexpr auto         MAX_NATIVE_FABRICATION_WAIT = std::chrono::seconds( 120 );
constexpr uintmax_t    MAX_FABRICATION_FILE_BYTES = 512ULL * 1024ULL * 1024ULL;
constexpr uintmax_t    MAX_FABRICATION_TOTAL_BYTES = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr size_t       MAX_FABRICATION_FILES = 512;


bool createFabricationVerificationSnapshot(
        const wxFileName& aProjectRoot, const wxFileName& aBoard,
        const wxFileName& aSchematic, const wxFileName& aSidecar,
        const std::array<std::string, 3>& aRelativePaths,
        const std::vector<std::string>& aAdditionalRelativePaths,
        PRIVATE_TEMPORARY_DIRECTORY& aSnapshot, std::string& aError )
{
    if( !aSnapshot.Create( "kichad-fabrication-verify-", aError ) )
        return false;

    const std::filesystem::path sourceRoot( aProjectRoot.GetFullPath().ToStdString() );
    uintmax_t totalBytes = 0;
    size_t fileCount = 0;
    std::set<std::filesystem::path> copied;

    const auto copyFile = [&]( const std::filesystem::path& aSource,
                               const std::filesystem::path& aRelative )
    {
        const std::filesystem::path normalized = aRelative.lexically_normal();

        if( normalized.empty() || normalized.is_absolute()
            || normalized.begin() == normalized.end() || *normalized.begin() == ".." )
        {
            aError = "verification snapshot contains an unsafe project-relative path";
            return false;
        }

        std::error_code filesystemError;
        const std::filesystem::file_status status =
                std::filesystem::symlink_status( aSource, filesystemError );

        if( filesystemError || !std::filesystem::is_regular_file( status ) )
        {
            aError = "verification snapshot input is not a regular file";
            return false;
        }

        const uintmax_t bytes = std::filesystem::file_size( aSource, filesystemError );

        if( filesystemError || bytes > MAX_FABRICATION_FILE_BYTES )
        {
            aError = "verification snapshot input is unreadable or exceeds 512 MiB";
            return false;
        }

        const bool alreadyCopied = copied.contains( normalized );

        if( !alreadyCopied
            && ( fileCount >= MAX_VERIFICATION_SNAPSHOT_FILES
                 || totalBytes > MAX_VERIFICATION_SNAPSHOT_BYTES - bytes ) )
        {
            aError = "verification snapshot exceeds 10,000 files or 512 MiB";
            return false;
        }

        const std::filesystem::path destination = aSnapshot.Path() / normalized;
        std::filesystem::create_directories( destination.parent_path(), filesystemError );

        if( filesystemError )
        {
            aError = "could not create the verification snapshot directory structure";
            return false;
        }

        std::filesystem::copy_file( aSource, destination,
                                    std::filesystem::copy_options::overwrite_existing,
                                    filesystemError );

        if( filesystemError )
        {
            aError = "could not copy a complete verification snapshot input";
            return false;
        }

        if( !alreadyCopied )
        {
            copied.emplace( normalized );
            totalBytes += bytes;
            ++fileCount;
        }

        return true;
    };

    std::error_code filesystemError;
    std::filesystem::recursive_directory_iterator iterator(
            sourceRoot, std::filesystem::directory_options::skip_permission_denied,
            filesystemError );
    const std::filesystem::recursive_directory_iterator end;

    while( iterator != end && !filesystemError )
    {
        const std::filesystem::directory_entry& entry = *iterator;
        const std::filesystem::path relative = entry.path().lexically_relative( sourceRoot );
        const std::string name = entry.path().filename().string();

        if( entry.is_directory( filesystemError ) )
        {
            if( name == ".git" || name == "fabrication"
                || name.starts_with( ".kichad-fabrication-" ) )
            {
                iterator.disable_recursion_pending();
            }
        }
        else if( !filesystemError )
        {
            std::string extension = entry.path().extension().string();
            std::transform( extension.begin(), extension.end(), extension.begin(),
                            []( unsigned char aCharacter )
                            {
                                return static_cast<char>( std::tolower( aCharacter ) );
                            } );
            const bool selected = extension == ".kicad_sch" || extension == ".kicad_sym"
                                  || extension == ".kicad_mod"
                                  || extension == ".kicad_pro"
                                  || extension == ".kicad_prl"
                                  || extension == ".kicad_dru"
                                  || extension == ".kicad_wks"
                                  || extension == ".step" || extension == ".stp"
                                  || extension == ".wrl"
                                  || name == "sym-lib-table" || name == "fp-lib-table";

            if( selected && !copyFile( entry.path(), relative ) )
                return false;
        }

        iterator.increment( filesystemError );
    }

    if( filesystemError )
    {
        aError = "could not enumerate project inputs for the verification snapshot";
        return false;
    }

    const std::array<const wxFileName*, 3> required = {
        &aSidecar, &aBoard, &aSchematic
    };

    for( size_t index = 0; index < required.size(); ++index )
    {
        if( !copyFile( required[index]->GetFullPath().ToStdString(),
                       std::filesystem::path( aRelativePaths[index] ) ) )
        {
            return false;
        }
    }

    for( const std::string& relativePath : aAdditionalRelativePaths )
    {
        const std::filesystem::path relative( relativePath );
        const std::filesystem::path candidate = ( sourceRoot / relative ).lexically_normal();
        std::error_code canonicalError;
        const std::filesystem::path canonicalRoot =
                std::filesystem::canonical( sourceRoot, canonicalError );
        const std::filesystem::path canonical =
                canonicalError ? std::filesystem::path()
                               : std::filesystem::canonical( candidate, canonicalError );
        const std::filesystem::path confined =
                canonicalError ? std::filesystem::path()
                               : canonical.lexically_relative( canonicalRoot );

        if( canonicalError || relative.empty() || relative.is_absolute()
            || relative.lexically_normal() != relative || confined.empty()
            || confined.is_absolute() || confined.begin() == confined.end()
            || *confined.begin() == ".." || canonical != candidate )
        {
            aError = "production input must be a confined project-relative regular file";
            return false;
        }

        if( !copyFile( canonical, relative ) )
            return false;
    }

    return true;
}


struct PROJECT_LIBRARY_TABLE_UPDATE
{
    std::string kind;
    wxFileName  path;
    std::string source;
    size_t      rows = 0;
    bool        previousPresent = false;
    std::string previousSource;
    bool        applied = false;
};


struct SCHEMATIC_FILE_UPDATE
{
    std::string relativePath;
    wxFileName  path;
    std::string source;
    bool        previousPresent = false;
    std::string previousSource;
    bool        applied = false;
};


bool isInspectableExtension( const wxString& aExtension )
{
    return aExtension == wxS( "kicad_sch" ) || aExtension == wxS( "kicad_pcb" )
           || aExtension == wxS( "kicad_sym" ) || aExtension == wxS( "kicad_mod" );
}


bool canonicalizeExisting( wxFileName& aPath, bool aDirectory = false )
{
    std::error_code         error;
    const std::string       utf8Path( aPath.GetFullPath().ToUTF8() );
    const std::u8string     filesystemPath(
            reinterpret_cast<const char8_t*>( utf8Path.data() ), utf8Path.size() );
    std::filesystem::path   canonical =
            std::filesystem::canonical( std::filesystem::path( filesystemPath ), error );

    if( error )
        return false;

    const std::u8string utf8Canonical = canonical.generic_u8string();
    wxString canonicalPath = wxString::FromUTF8(
            reinterpret_cast<const char*>( utf8Canonical.data() ), utf8Canonical.size() );

    if( aDirectory )
        aPath.AssignDir( canonicalPath );
    else
        aPath.Assign( canonicalPath );

    return true;
}


bool resolveProjectLibraryTable( const wxString& aProjectPath, const std::string& aName,
                                 wxFileName& aResolved, std::string& aError )
{
    if( aName != "sym-lib-table" && aName != "fp-lib-table" )
    {
        aError = "project library table name is not supported";
        return false;
    }

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( root, true ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    wxFileName candidate( root.GetFullPath(), wxString::FromUTF8( aName ) );

    if( wxDirExists( candidate.GetFullPath() ) )
    {
        aError = "project library table path is a directory";
        return false;
    }

    if( candidate.FileExists() )
    {
        wxFileName canonical = candidate;

        if( !canonicalizeExisting( canonical ) )
        {
            aError = "existing project library table could not be resolved";
            return false;
        }

        wxString canonicalPath = canonical.GetFullPath();
        wxString candidatePath = candidate.GetFullPath();

#ifdef __WXMSW__
        canonicalPath.MakeLower();
        candidatePath.MakeLower();
#endif

        if( canonicalPath != candidatePath )
        {
            aError = "project library table cannot be a symbolic link";
            return false;
        }
    }

    aResolved = candidate;
    return true;
}


bool readOptionalTextFile( const wxFileName& aPath, bool& aPresent, std::string& aSource,
                           std::string& aError )
{
    aPresent = aPath.FileExists();
    aSource.clear();

    if( !aPresent )
        return true;

    wxFile file( aPath.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
    {
        aError = "could not open the project library table";
        return false;
    }

    const wxFileOffset length = file.Length();

    if( length < 0 || length > static_cast<wxFileOffset>( MAX_PROJECT_LIBRARY_TABLE_BYTES ) )
    {
        aError = "project library table exceeds 1 MiB";
        return false;
    }

    aSource.assign( static_cast<size_t>( length ), '\0' );

    if( length > 0 && file.Read( aSource.data(), aSource.size() ) != length )
    {
        aSource.clear();
        aError = "could not read the complete project library table";
        return false;
    }

    return true;
}


bool installTextFileAtomically( const wxFileName& aPath, bool aPresent,
                                const std::string& aSource, std::string& aError )
{
    if( !aPresent )
    {
        if( aPath.FileExists() && !wxRemoveFile( aPath.GetFullPath() ) )
        {
            aError = "could not remove the project library table during rollback";
            return false;
        }

        if( aPath.FileExists() )
        {
            aError = "project library table remained after rollback";
            return false;
        }

        return true;
    }

    if( aSource.size() > MAX_PROJECT_LIBRARY_TABLE_BYTES )
    {
        aError = "project library table exceeds 1 MiB";
        return false;
    }

    const wxString temporaryPath =
            aPath.GetFullPath() + wxS( ".tmp-" ) + KIID().AsString();
    wxFile temporary;

    if( !temporary.Create( temporaryPath, true )
        || temporary.Write( aSource.data(), aSource.size() ) != aSource.size()
        || !temporary.Flush() )
    {
        temporary.Close();
        wxRemoveFile( temporaryPath );
        aError = "could not durably write the project library table";
        return false;
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, aPath.GetFullPath(), true ) )
    {
        wxRemoveFile( temporaryPath );
        aError = "could not atomically install the project library table";
        return false;
    }

    bool        installedPresent = false;
    std::string installedSource;

    if( !readOptionalTextFile( aPath, installedPresent, installedSource, aError )
        || !installedPresent || installedSource != aSource )
    {
        aError = "project library table verification failed after installation";
        return false;
    }

    return true;
}


bool resolveProjectSchematic( const wxString& aProjectPath, const std::string& aRelativePath,
                              wxFileName& aResolved, std::string& aError )
{
    wxString relative = wxString::FromUTF8( aRelativePath );
    wxFileName candidate( relative );

    if( aRelativePath.empty() || aRelativePath.size() > 4096
        || aRelativePath.find( '\0' ) != std::string::npos || candidate.IsAbsolute()
        || candidate.GetExt() != wxS( "kicad_sch" ) )
    {
        aError = "schematic path must be a project-relative .kicad_sch file";
        return false;
    }

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( root, true ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    candidate.MakeAbsolute( root.GetFullPath() );
    candidate.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    wxFileName parent = wxFileName::DirName( candidate.GetPath() );

    if( !canonicalizeExisting( parent, true ) )
    {
        aError = "schematic parent directory does not exist";
        return false;
    }

    wxString parentPath = parent.GetPathWithSep();
    wxString rootPath = root.GetPathWithSep();

#ifdef __WXMSW__
    parentPath.MakeLower();
    rootPath.MakeLower();
#endif

    if( !parentPath.StartsWith( rootPath ) && parent.GetFullPath() != root.GetFullPath() )
    {
        aError = "schematic path resolves outside the active project";
        return false;
    }

    aResolved.Assign( parent.GetFullPath(), candidate.GetFullName() );

    if( aResolved.FileExists() )
    {
        wxFileName canonical = aResolved;

        if( !canonicalizeExisting( canonical ) )
        {
            aError = "existing schematic path could not be resolved";
            return false;
        }

        wxString resolvedPath = aResolved.GetFullPath();
        wxString canonicalPath = canonical.GetFullPath();

#ifdef __WXMSW__
        resolvedPath.MakeLower();
        canonicalPath.MakeLower();
#endif

        if( resolvedPath != canonicalPath )
        {
            aError = "managed schematic path cannot be a symbolic link";
            return false;
        }
    }

    return true;
}


bool inventoryProjectSymbolLibraries( const wxString& aProjectPath,
                                      const nlohmann::json& aCompilerIr,
                                      nlohmann::json& aSources, std::string& aError )
{
    constexpr size_t MAX_SYMBOL_LIBRARY_BYTES = 32 * 1024 * 1024;
    constexpr size_t MAX_SYMBOL_INVENTORY_BYTES = 128 * 1024 * 1024;
    aSources = nlohmann::json::object();
    size_t totalBytes = 0;

    if( !aCompilerIr.is_object() || !aCompilerIr.contains( "libraries" )
        || !aCompilerIr["libraries"].is_array() || !aCompilerIr.contains( "schematic" )
        || !aCompilerIr["schematic"].is_object()
        || !aCompilerIr["schematic"].contains( "components" )
        || !aCompilerIr["schematic"]["components"].is_array() )
    {
        aError = "compiled KDS library inventory is malformed";
        return false;
    }

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( root, true ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    std::set<std::string> usedNicknames;

    for( const nlohmann::json& component : aCompilerIr["schematic"]["components"] )
    {
        if( !component.is_object() || !component.contains( "symbol" )
            || !component["symbol"].is_string() || !component.contains( "units" )
            || !component["units"].is_array() || component["units"].empty() )
        {
            continue;
        }

        const std::string libraryId = component["symbol"].get<std::string>();
        const size_t separator = libraryId.find( ':' );

        if( separator != std::string::npos )
            usedNicknames.emplace( libraryId.substr( 0, separator ) );
    }

    std::set<std::string> inventoriedNicknames;

    for( const nlohmann::json& library : aCompilerIr["libraries"] )
    {
        if( !library.is_object() || library.value( "kind", "" ) != "symbol"
            || library.value( "managed", false )
            || !usedNicknames.contains( library.value( "id", "" ) ) )
        {
            continue;
        }

        const std::string nickname = library.value( "id", "" );
        const std::string table = library.value( "table", "" );
        const std::string uri = library.contains( "uri" ) && library["uri"].is_string()
                                        ? library["uri"].get<std::string>()
                                        : "";
        constexpr std::string_view prefix = "${KIPRJMOD}/";
        wxFileName candidate;

        if( !inventoriedNicknames.emplace( nickname ).second )
        {
            aError = "symbol library nickname occurs more than once: " + nickname;
            return false;
        }

        if( table == "global" )
        {
            candidate.Assign( PATHS::GetStockSymbolsPath(),
                              wxString::FromUTF8( nickname + ".kicad_sym" ) );

            if( !candidate.FileExists() )
            {
                aError = "installed global symbol library is unavailable: " + nickname;
                return false;
            }
        }
        else if( table == "project" && !nickname.empty() && uri.starts_with( prefix )
                 && uri.size() > prefix.size() && uri.ends_with( ".kicad_sym" ) )
        {
            const std::string relativePath = uri.substr( prefix.size() );
            candidate.Assign( wxString::FromUTF8( relativePath ) );

            if( candidate.IsAbsolute() )
            {
                aError = "project symbol library path must be relative";
                return false;
            }

            candidate.MakeAbsolute( root.GetFullPath() );
            candidate.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
        }
        else
        {
            aError = "symbol library declaration is malformed";
            return false;
        }

        if( !candidate.FileExists() )
        {
            aError = "symbol library does not exist: "
                     + candidate.GetFullPath().ToStdString();
            return false;
        }

        wxFileName canonical = candidate;

        if( !canonicalizeExisting( canonical ) )
        {
            aError = "symbol library could not be resolved: " + nickname;
            return false;
        }

        wxString candidatePath = candidate.GetFullPath();
        wxString canonicalPath = canonical.GetFullPath();
        wxString rootPath = root.GetPathWithSep();

#ifdef __WXMSW__
        candidatePath.MakeLower();
        canonicalPath.MakeLower();
        rootPath.MakeLower();
#endif

        bool invalidPath = false;

        if( table == "project" )
        {
            invalidPath = candidatePath != canonicalPath || !canonicalPath.StartsWith( rootPath );
        }
        else
        {
            wxFileName stockRoot = wxFileName::DirName( PATHS::GetStockSymbolsPath() );

            if( !canonicalizeExisting( stockRoot, true ) )
            {
                aError = "installed stock symbol library root could not be resolved";
                return false;
            }

            wxString stockPath = stockRoot.GetPathWithSep();
#ifdef __WXMSW__
            stockPath.MakeLower();
#endif
            invalidPath = !canonicalPath.StartsWith( stockPath );
        }

        if( invalidPath )
        {
            aError = "symbol library must resolve to a regular file inside its declared library root";
            return false;
        }

        wxFile input;

        if( !input.Open( candidate.GetFullPath() ) )
        {
            aError = "could not open symbol library: " + nickname;
            return false;
        }

        const wxFileOffset length = input.Length();

        if( length <= 0 || length > static_cast<wxFileOffset>( MAX_SYMBOL_LIBRARY_BYTES )
            || static_cast<size_t>( length ) > MAX_SYMBOL_INVENTORY_BYTES - totalBytes )
        {
            aError = "symbol library inventory exceeds bounded size limits";
            return false;
        }

        std::string source( static_cast<size_t>( length ), '\0' );

        const auto bytesRead = input.Read( source.data(), source.size() );

        if( bytesRead < 0 || static_cast<size_t>( bytesRead ) != source.size() )
        {
            aError = "could not read complete symbol library: " + nickname;
            return false;
        }

        totalBytes += source.size();

        if( aSources.contains( nickname ) )
        {
            aError = "symbol library nickname occurs more than once: " + nickname;
            return false;
        }

        aSources[nickname] = std::move( source );
    }

    return true;
}


bool inventoryProjectFootprints( const wxString& aProjectPath,
                                 const nlohmann::json& aCompilerIr,
                                 nlohmann::json& aSources, std::string& aError )
{
    constexpr size_t MAX_FOOTPRINT_BYTES = 8 * 1024 * 1024;
    constexpr size_t MAX_FOOTPRINT_INVENTORY_BYTES = 64 * 1024 * 1024;
    aSources = nlohmann::json::object();

    if( !aCompilerIr.is_object() || !aCompilerIr.contains( "libraries" )
        || !aCompilerIr["libraries"].is_array() || !aCompilerIr.contains( "schematic" )
        || !aCompilerIr["schematic"].is_object()
        || !aCompilerIr["schematic"].contains( "components" )
        || !aCompilerIr["schematic"]["components"].is_array()
        || !aCompilerIr.contains( "pcb" ) || !aCompilerIr["pcb"].is_array() )
    {
        aError = "compiled KDS footprint inventory is malformed";
        return false;
    }

    std::set<std::string> placedReferences;

    for( const nlohmann::json& statement : aCompilerIr["pcb"] )
    {
        if( statement.is_object() && statement.value( "kind", "" ) == "place"
            && statement.contains( "component" ) && statement["component"].is_string() )
        {
            placedReferences.emplace( statement["component"].get<std::string>() );
        }
    }

    if( aCompilerIr.contains( "synthesis" ) && aCompilerIr["synthesis"].is_object() )
    {
        for( const nlohmann::json& component : aCompilerIr["schematic"]["components"] )
        {
            if( component.is_object() && component.contains( "reference" )
                && component["reference"].is_string() && component.contains( "footprint" )
                && component["footprint"].is_string() )
            {
                placedReferences.emplace( component["reference"].get<std::string>() );
            }
        }
    }

    std::map<std::string, std::set<std::string>> usedEntries;

    for( const nlohmann::json& component : aCompilerIr["schematic"]["components"] )
    {
        if( !component.is_object() || !component.contains( "reference" )
            || !component["reference"].is_string()
            || !placedReferences.contains( component["reference"].get<std::string>() )
            || !component.contains( "footprint" ) || !component["footprint"].is_string() )
        {
            continue;
        }

        const std::string libraryId = component["footprint"].get<std::string>();
        const size_t separator = libraryId.find( ':' );

        if( separator == std::string::npos || separator == 0
            || separator + 1 == libraryId.size()
            || libraryId.find( ':', separator + 1 ) != std::string::npos )
        {
            aError = "placed component contains an invalid footprint library ID";
            return false;
        }

        const std::string nickname = libraryId.substr( 0, separator );
        const std::string entry = libraryId.substr( separator + 1 );

        if( entry == "." || entry == ".." || entry.find( '/' ) != std::string::npos
            || entry.find( '\\' ) != std::string::npos
            || entry.find( '\0' ) != std::string::npos )
        {
            aError = "placed footprint entry name cannot contain a path";
            return false;
        }

        usedEntries[nickname].emplace( entry );
    }

    if( usedEntries.empty() )
        return true;

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( root, true ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    std::set<std::string> inventoriedNicknames;
    size_t totalBytes = 0;

    for( const nlohmann::json& library : aCompilerIr["libraries"] )
    {
        if( !library.is_object() || library.value( "kind", "" ) != "footprint"
            || !usedEntries.contains( library.value( "id", "" ) ) )
        {
            continue;
        }

        const std::string nickname = library.value( "id", "" );
        const std::string table = library.value( "table", "" );
        const std::string uri = library.contains( "uri" ) && library["uri"].is_string()
                                        ? library["uri"].get<std::string>()
                                        : "";
        constexpr std::string_view prefix = "${KIPRJMOD}/";

        if( !inventoriedNicknames.emplace( nickname ).second )
        {
            aError = "project footprint library nickname occurs more than once: " + nickname;
            return false;
        }

        // Whole-library KDS ownership is generated from the compiled IR and injected by the
        // design backend.  It must not depend on stale or absent on-disk artifacts.
        if( library.value( "managed", false ) )
            continue;

        wxFileName directory;

        if( table == "global" )
        {
            directory.AssignDir( PATHS::GetStockFootprintsPath()
                                 + wxString::FromUTF8( nickname + ".pretty" ) );
        }
        else if( table == "project" && !nickname.empty() && uri.starts_with( prefix )
                 && uri.size() > prefix.size() && uri.ends_with( ".pretty" ) )
        {
            const std::string relativePath = uri.substr( prefix.size() );
            directory.AssignDir( wxString::FromUTF8( relativePath ) );

            if( directory.IsAbsolute() )
            {
                aError = "project footprint library path must be relative";
                return false;
            }

            directory.MakeAbsolute( root.GetFullPath() );
            directory.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
        }
        else
        {
            aError = "footprint library declaration is malformed";
            return false;
        }

        if( !directory.DirExists() )
        {
            aError = "footprint library does not exist: "
                     + directory.GetFullPath().ToStdString();
            return false;
        }

        wxFileName canonicalDirectory = directory;

        if( !canonicalizeExisting( canonicalDirectory, true ) )
        {
            aError = "footprint library could not be resolved: " + nickname;
            return false;
        }

        wxString directoryPath = directory.GetFullPath();
        wxString canonicalDirectoryPath = canonicalDirectory.GetFullPath();
        wxString rootPath = root.GetPathWithSep();

#ifdef __WXMSW__
        directoryPath.MakeLower();
        canonicalDirectoryPath.MakeLower();
        rootPath.MakeLower();
#endif

        bool invalidDirectory = false;

        if( table == "project" )
        {
            invalidDirectory = directoryPath != canonicalDirectoryPath
                               || !( canonicalDirectoryPath
                                     + wxFileName::GetPathSeparator() )
                                           .StartsWith( rootPath );
        }
        else
        {
            wxFileName stockRoot = wxFileName::DirName( PATHS::GetStockFootprintsPath() );

            if( !canonicalizeExisting( stockRoot, true ) )
            {
                aError = "installed stock footprint library root could not be resolved";
                return false;
            }

            wxString stockPath = stockRoot.GetPathWithSep();
#ifdef __WXMSW__
            stockPath.MakeLower();
#endif
            invalidDirectory = !( canonicalDirectoryPath
                                  + wxFileName::GetPathSeparator() )
                                       .StartsWith( stockPath );
        }

        if( invalidDirectory )
        {
            aError = "footprint library must resolve to a directory inside its declared library root";
            return false;
        }

        for( const std::string& entry : usedEntries.at( nickname ) )
        {
            wxFileName candidate( directory.GetFullPath(),
                                  wxString::FromUTF8( entry + ".kicad_mod" ) );

            if( !candidate.FileExists() )
            {
                aError = "declared footprint does not exist: " + nickname + ":" + entry;
                return false;
            }

            wxFileName canonical = candidate;

            if( !canonicalizeExisting( canonical ) )
            {
                aError = "declared footprint could not be resolved: " + nickname + ":" + entry;
                return false;
            }

            wxString candidatePath = candidate.GetFullPath();
            wxString canonicalPath = canonical.GetFullPath();
            wxString libraryPath = canonicalDirectory.GetPathWithSep();

#ifdef __WXMSW__
            candidatePath.MakeLower();
            canonicalPath.MakeLower();
            libraryPath.MakeLower();
#endif

            if( ( table == "project" && candidatePath != canonicalPath )
                || !canonicalPath.StartsWith( libraryPath ) )
            {
                aError = "footprint must resolve to a regular file inside its declared library";
                return false;
            }

            wxFile input;

            if( !input.Open( candidate.GetFullPath() ) )
            {
                aError = "could not open declared footprint: " + nickname + ":" + entry;
                return false;
            }

            const wxFileOffset length = input.Length();

            if( length <= 0 || length > static_cast<wxFileOffset>( MAX_FOOTPRINT_BYTES )
                || static_cast<size_t>( length )
                           > MAX_FOOTPRINT_INVENTORY_BYTES - totalBytes )
            {
                aError = "footprint inventory exceeds bounded size limits";
                return false;
            }

            std::string source( static_cast<size_t>( length ), '\0' );

            if( input.Read( source.data(), source.size() ) != length )
            {
                aError = "could not read complete declared footprint: " + nickname + ":" + entry;
                return false;
            }

            totalBytes += source.size();
            aSources[nickname + ":" + entry] = std::move( source );
        }
    }

    return true;
}


bool validateFootprintModelAssets( const wxString& aProjectPath,
                                   const nlohmann::json& aCompilerIr,
                                   nlohmann::json& aSummary, std::string& aError )
{
    constexpr std::string_view projectPrefix = "${KIPRJMOD}/";
    constexpr std::string_view stockPrefix = "${KICAD10_3DMODEL_DIR}/";
    constexpr size_t maxModels = 4096;
    constexpr uint64_t maxModelBytes = 512ULL * 1024ULL * 1024ULL;
    aSummary = { { "checked", static_cast<uint64_t>( 0 ) },
                 { "project", static_cast<uint64_t>( 0 ) },
                 { "stock", static_cast<uint64_t>( 0 ) } };

    if( !aCompilerIr.is_object() || !aCompilerIr.contains( "authoredFootprints" )
        || !aCompilerIr["authoredFootprints"].is_array() )
    {
        aError = "compiled KDS does not contain a valid authored-footprint inventory";
        return false;
    }

    wxFileName projectRoot = wxFileName::DirName( aProjectPath );
    projectRoot.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( projectRoot, true ) )
    {
        aError = "active project path could not be resolved for 3D model validation";
        return false;
    }

    wxString stockModelPath;

    if( !wxGetEnv( ENV_VAR::GetVersionedEnvVarName( wxS( "3DMODEL_DIR" ) ),
                   &stockModelPath )
        || stockModelPath.IsEmpty() )
    {
        stockModelPath = PATHS::GetStock3dmodelsPath();
    }

    wxFileName stockRoot = wxFileName::DirName( stockModelPath );
    const bool hasStockRoot = canonicalizeExisting( stockRoot, true );
    std::set<wxString> validated;

    for( const nlohmann::json& footprint : aCompilerIr["authoredFootprints"] )
    {
        if( !footprint.is_object() || !footprint.contains( "id" )
            || !footprint["id"].is_string() || !footprint.contains( "models" )
            || !footprint["models"].is_array() )
        {
            aError = "compiled KDS contains malformed authored-footprint model metadata";
            return false;
        }

        const std::string footprintId = footprint["id"].get<std::string>();

        for( const nlohmann::json& model : footprint["models"] )
        {
            if( aSummary["checked"].get<size_t>() >= maxModels || !model.is_object()
                || !model.contains( "path" ) || !model["path"].is_string() )
            {
                aError = "authored-footprint 3D model inventory is malformed or excessive";
                return false;
            }

            const std::string path = model["path"].get<std::string>();
            const bool projectModel = path.starts_with( projectPrefix );
            const bool stockModel = path.starts_with( stockPrefix );

            if( !projectModel && !stockModel )
            {
                aError = "footprint " + footprintId
                         + " contains an unsupported 3D model root: " + path;
                return false;
            }

            if( stockModel && !hasStockRoot )
            {
                aError = "installed KiCad 10 stock 3D model root is unavailable while resolving "
                         + path + " for footprint " + footprintId;
                return false;
            }

            const std::string_view prefix = projectModel ? projectPrefix : stockPrefix;
            const std::string relative = path.substr( prefix.size() );
            wxFileName root = projectModel ? projectRoot : stockRoot;
            wxFileName candidate( root.GetPathWithSep() + wxString::FromUTF8( relative ) );
            candidate.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
            const wxString declaredPath = candidate.GetFullPath();

            if( relative.empty() || !candidate.FileExists() )
            {
                aError = "3D model " + path + " for footprint " + footprintId
                         + ( projectModel ? " does not exist in the project"
                                          : " is not installed with KiCad 10" );
                return false;
            }

            if( !canonicalizeExisting( candidate ) )
            {
                aError = "3D model " + path + " for footprint " + footprintId
                         + " could not be resolved";
                return false;
            }

            wxString canonicalPath = candidate.GetFullPath();
            wxString rootPath = root.GetPathWithSep();
            wxString comparableDeclared = declaredPath;

#ifdef __WXMSW__
            canonicalPath.MakeLower();
            rootPath.MakeLower();
            comparableDeclared.MakeLower();
#endif

            if( canonicalPath != comparableDeclared || !canonicalPath.StartsWith( rootPath ) )
            {
                aError = "3D model " + path + " for footprint " + footprintId
                         + " must be a regular non-symlink file inside its declared model root";
                return false;
            }

            const wxULongLong bytes = candidate.GetSize();

            if( bytes == wxInvalidSize || bytes.GetValue() == 0
                || bytes.GetValue() > maxModelBytes )
            {
                aError = "3D model " + path + " for footprint " + footprintId
                         + " is empty, unreadable, or exceeds 512 MiB";
                return false;
            }

            if( validated.emplace( canonicalPath ).second )
            {
                ++aSummary["checked"].get_ref<uint64_t&>();
                ++aSummary[projectModel ? "project" : "stock"].get_ref<uint64_t&>();
            }
        }
    }

    return true;
}


bool readOptionalSchematic( const wxFileName& aPath, bool& aPresent, std::string& aSource,
                            std::string& aError )
{
    aPresent = aPath.FileExists();
    aSource.clear();

    if( !aPresent )
        return true;

    wxFile file( aPath.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
    {
        aError = "could not open managed schematic";
        return false;
    }

    const wxFileOffset length = file.Length();

    if( length <= 0 || length > static_cast<wxFileOffset>( MAX_SCHEMATIC_FILE_BYTES ) )
    {
        aError = "managed schematic must contain 1 byte to 16 MiB";
        return false;
    }

    aSource.assign( static_cast<size_t>( length ), '\0' );

    if( file.Read( aSource.data(), aSource.size() ) != length )
    {
        aSource.clear();
        aError = "could not read the complete managed schematic";
        return false;
    }

    return true;
}


bool installSchematicAtomically( const wxFileName& aPath, bool aPresent,
                                 const std::string& aSource, std::string& aError )
{
    if( !aPresent )
    {
        if( aPath.FileExists() && !wxRemoveFile( aPath.GetFullPath() ) )
        {
            aError = "could not remove newly created schematic during rollback";
            return false;
        }

        if( aPath.FileExists() )
        {
            aError = "newly created schematic remained after rollback";
            return false;
        }

        return true;
    }

    if( aSource.empty() || aSource.size() > MAX_SCHEMATIC_FILE_BYTES )
    {
        aError = "managed schematic must contain 1 byte to 16 MiB";
        return false;
    }

    const wxString temporaryPath =
            aPath.GetFullPath() + wxS( ".tmp-" ) + KIID().AsString();
    wxFile temporary;

    if( !temporary.Create( temporaryPath, true )
        || temporary.Write( aSource.data(), aSource.size() ) != aSource.size()
        || !temporary.Flush() )
    {
        temporary.Close();
        wxRemoveFile( temporaryPath );
        aError = "could not durably write managed schematic temporary data";
        return false;
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, aPath.GetFullPath(), true ) )
    {
        wxRemoveFile( temporaryPath );
        aError = "could not atomically install managed schematic";
        return false;
    }

    bool installedPresent = false;
    std::string installedSource;

    if( !readOptionalSchematic( aPath, installedPresent, installedSource, aError )
        || !installedPresent || installedSource != aSource )
    {
        aError = "managed schematic verification failed after installation";
        return false;
    }

    return true;
}


std::string directSchematicScalar( const KICHAD::LOSSLESS_SEXPR_DOCUMENT& aDocument,
                                   size_t aParent, const std::string& aHead )
{
    size_t match = KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE;

    for( size_t child : aDocument.Nodes().at( aParent ).children )
    {
        if( aDocument.Nodes().at( child ).kind
                    != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
            || aDocument.ListHead( child ) != aHead )
        {
            continue;
        }

        if( match != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE )
            return {};

        match = child;
    }

    if( match == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE )
        return {};

    const auto& node = aDocument.Nodes().at( match );

    if( node.children.size() != 2
        || aDocument.Nodes().at( node.children[1] ).kind
                   == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST )
    {
        return {};
    }

    return aDocument.AtomText( node.children[1] );
}


bool schematicScreenUuid( const std::string& aSource, std::string& aUuid,
                          std::string& aError )
{
    std::unique_ptr<KICHAD::LOSSLESS_SEXPR_DOCUMENT> document =
            KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( aSource, &aError );

    if( !document || document->Roots().size() != 1
        || document->ListHead( document->Roots().front() ) != "kicad_sch" )
    {
        if( aError.empty() )
            aError = "managed schematic must contain one kicad_sch root";

        return false;
    }

    aUuid = directSchematicScalar( *document, document->Roots().front(), "uuid" );

    if( aUuid.empty() )
    {
        aError = "managed schematic must contain exactly one screen UUID";
        return false;
    }

    return true;
}


bool findNativeKiCadCli( wxFileName& aCli )
{
    wxFileName executable( wxStandardPaths::Get().GetExecutablePath() );
#ifdef __WXMSW__
    const wxString cliName = wxS( "kicad-cli.exe" );
#else
    const wxString cliName = wxS( "kicad-cli" );
#endif
    wxFileName sibling( executable.GetPath(), cliName );

    if( sibling.FileExists() && sibling.IsFileExecutable() )
    {
        aCli = sibling;
        return true;
    }

    wxString runFromBuild;

    if( !wxGetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ), &runFromBuild )
        || runFromBuild != wxS( "1" ) )
    {
        return false;
    }

    wxFileName root = wxFileName::DirName( executable.GetPath() );

    for( int depth = 0; depth < 6 && root.GetDirCount() > 0; ++depth )
    {
        wxFileName candidate = root;
        candidate.AppendDir( wxS( "kicad" ) );
        candidate.SetFullName( cliName );

        if( candidate.FileExists() && candidate.IsFileExecutable() )
        {
            aCli = candidate;
            return true;
        }

        root.RemoveLastDir();
    }

    return false;
}


bool validateNativeSchematicHierarchy( const wxFileName& aRootSchematic,
                                       const JSON* aCompilerIr,
                                       const JSON* aResolvedSymbols,
                                       std::string& aError )
{
    namespace bp = KICHAD_BP;

    wxFileName cli;

    if( !findNativeKiCadCli( cli ) )
    {
        aError = "the sibling kicad-cli native schematic validator is unavailable";
        return false;
    }

    const wxString token = KIID().AsString();
    wxFileName output( wxFileName::GetTempDir(), wxS( "kichad-kds-" ) + token + wxS( ".net" ) );
    wxFileName stdoutLog( wxFileName::GetTempDir(),
                           wxS( "kichad-kds-" ) + token + wxS( ".stdout" ) );
    wxFileName stderrLog( wxFileName::GetTempDir(),
                           wxS( "kichad-kds-" ) + token + wxS( ".stderr" ) );
    bool finished = false;
    int exitCode = -1;

    try
    {
        bp::child process(
                cli.GetFullPath().ToStdString(),
                bp::args( std::vector<std::string>{ "sch", "export", "netlist", "--output",
                                                    output.GetFullPath().ToStdString(),
                                                    aRootSchematic.GetFullPath().ToStdString() } ),
                bp::std_out > stdoutLog.GetFullPath().ToStdString(),
                bp::std_err > stderrLog.GetFullPath().ToStdString() );
        const auto deadline = std::chrono::steady_clock::now()
                              + MAX_SCHEMATIC_VALIDATION_WAIT;
        std::error_code processError;

        while( process.running( processError ) && !processError
               && std::chrono::steady_clock::now() < deadline )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }

        finished = !process.running( processError ) && !processError;

        if( !finished )
        {
            process.terminate();
            process.wait();

            if( processError )
                aError = "native schematic validation process failed: "
                         + processError.message();
        }
        else
        {
            exitCode = process.exit_code();
        }
    }
    catch( const std::exception& error )
    {
        aError = std::string( "could not run native schematic validation: " ) + error.what();
    }

    if( aError.empty() && ( !finished || exitCode != 0 || !output.FileExists() ) )
    {
        aError = !finished ? "native schematic validation exceeded 30 seconds"
                           : "native KiCad rejected the generated schematic hierarchy";

        if( stderrLog.FileExists() )
        {
            wxFile errorFile( stderrLog.GetFullPath(), wxFile::read );
            const wxFileOffset length = errorFile.IsOpened() ? errorFile.Length() : 0;

            if( length > 0 )
            {
                const size_t bounded = std::min<size_t>( static_cast<size_t>( length ), 4096 );
                std::string detail( bounded, '\0' );

                if( errorFile.Read( detail.data(), detail.size() )
                    == static_cast<wxFileOffset>( detail.size() ) )
                {
                    aError += ": " + detail;
                }
            }
        }
    }

    if( aError.empty() && aCompilerIr != nullptr )
    {
        JSON plan;
        std::string connectivityError;

        if( aResolvedSymbols != nullptr
            && !KICHAD::CODEX_TOOLS::BuildNativeNetlistValidationPlan(
                    *aCompilerIr, *aResolvedSymbols,
                    aRootSchematic.GetName().ToStdString(), plan,
                    connectivityError ) )
        {
            aError = "native schematic connectivity validation could not determine "
                     "KiCad netlist eligibility: " + connectivityError;
        }
        else
        {
            if( aResolvedSymbols == nullptr )
            {
                plan = KICHAD::CODEX_TOOLS::BuildFabricationPlan(
                        *aCompilerIr, aRootSchematic.GetName().ToStdString() );
            }

            if( !KICHAD::FABRICATION_ARTIFACT_VALIDATOR::ValidateKiCadNetlist(
                        std::filesystem::path( output.GetFullPath().ToStdString() ),
                        plan, connectivityError ) )
            {
                aError = "native schematic connectivity does not match compiled KDS: "
                         + connectivityError;
            }
        }
    }

    if( output.FileExists() )
        wxRemoveFile( output.GetFullPath() );

    if( stdoutLog.FileExists() )
        wxRemoveFile( stdoutLog.GetFullPath() );

    if( stderrLog.FileExists() )
        wxRemoveFile( stderrLog.GetFullPath() );

    return aError.empty();
}


bool runNativeKiCadCheck( const std::string& aCheck, const wxFileName& aInput,
                          std::string& aReport, std::string& aError )
{
    namespace bp = KICHAD_BP;

    wxFileName cli;

    if( !findNativeKiCadCli( cli ) )
    {
        aError = "the sibling kicad-cli verification backend is unavailable";
        return false;
    }

    wxFileName temporaryRoot = wxFileName::DirName( wxFileName::GetTempDir() );
    temporaryRoot.AppendDir( wxS( "kichad-verify-" ) + KIID().AsString() );

    if( !wxFileName::Mkdir( temporaryRoot.GetFullPath(), 0700 ) )
    {
        aError = "could not create a private native verification directory";
        return false;
    }

    wxFileName reportPath( temporaryRoot.GetFullPath(), wxS( "report.json" ) );
    wxFileName stdoutLog( temporaryRoot.GetFullPath(), wxS( "stdout.log" ) );
    wxFileName stderrLog( temporaryRoot.GetFullPath(), wxS( "stderr.log" ) );
    std::vector<std::string> arguments = aCheck == "erc"
                                                 ? std::vector<std::string>{ "sch", "erc" }
                                                 : std::vector<std::string>{ "pcb", "drc" };
    arguments.insert( arguments.end(), { "--format", "json", "--units", "mm",
                                         "--severity-all" } );

    if( aCheck == "drc" )
        arguments.push_back( "--schematic-parity" );

    arguments.insert( arguments.end(),
                      { "--output", reportPath.GetFullPath().ToStdString(),
                        aInput.GetFullPath().ToStdString() } );

    bool finished = false;
    int  exitCode = -1;

    try
    {
        bp::child process( cli.GetFullPath().ToStdString(), bp::args( arguments ),
                           bp::std_out > stdoutLog.GetFullPath().ToStdString(),
                           bp::std_err > stderrLog.GetFullPath().ToStdString() );
        const auto deadline = std::chrono::steady_clock::now() + MAX_NATIVE_CHECK_WAIT;
        std::error_code processError;

        while( process.running( processError ) && !processError
               && std::chrono::steady_clock::now() < deadline )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }

        finished = !process.running( processError ) && !processError;

        if( !finished )
        {
            process.terminate();
            process.wait();

            if( processError )
                aError = "native verification process failed: " + processError.message();
        }
        else
        {
            exitCode = process.exit_code();
        }
    }
    catch( const std::exception& error )
    {
        aError = std::string( "could not run native KiCad verification: " ) + error.what();
    }

    if( aError.empty() && ( !finished || exitCode != 0 || !reportPath.FileExists() ) )
    {
        aError = !finished ? "native KiCad verification exceeded 60 seconds"
                           : "native KiCad could not complete the requested check";

        if( stderrLog.FileExists() )
        {
            wxFile errorFile( stderrLog.GetFullPath(), wxFile::read );
            const wxFileOffset length = errorFile.IsOpened() ? errorFile.Length() : 0;

            if( length > 0 )
            {
                const size_t bounded = std::min<size_t>( static_cast<size_t>( length ), 4096 );
                std::string detail( bounded, '\0' );

                if( errorFile.Read( detail.data(), detail.size() )
                    == static_cast<wxFileOffset>( detail.size() ) )
                {
                    aError += ": " + detail;
                }
            }
        }
    }

    if( aError.empty() )
    {
        wxFile reportFile( reportPath.GetFullPath(), wxFile::read );
        const wxFileOffset length = reportFile.IsOpened() ? reportFile.Length() : wxInvalidOffset;

        if( length <= 0 || length > MAX_VERIFICATION_REPORT_BYTES )
        {
            aError = "native verification report must contain 1 byte to 64 MiB";
        }
        else
        {
            aReport.resize( static_cast<size_t>( length ) );

            if( reportFile.Read( aReport.data(), aReport.size() ) != length )
                aError = "could not read the complete native verification report";
        }
    }

    wxFileName::Rmdir( temporaryRoot.GetFullPath(), wxPATH_RMDIR_RECURSIVE );

    return aError.empty();
}


std::string joinCommaSeparated( const JSON& aValues )
{
    std::string joined;

    for( const JSON& value : aValues )
    {
        if( !joined.empty() )
            joined += ',';

        joined += value.get<std::string>();
    }

    return joined;
}


bool runNativeFabricationCommand( const wxFileName& aCli,
                                  const std::vector<std::string>& aArguments,
                                  const wxFileName& aLogDirectory, size_t aIndex,
                                  const std::string& aKind, std::string& aError )
{
    namespace bp = KICHAD_BP;

    wxFileName stdoutLog( aLogDirectory.GetFullPath(),
                          wxString::Format( wxS( "%zu.stdout" ), aIndex ) );
    wxFileName stderrLog( aLogDirectory.GetFullPath(),
                          wxString::Format( wxS( "%zu.stderr" ), aIndex ) );
    bool finished = false;
    int exitCode = -1;

    try
    {
        bp::child process( aCli.GetFullPath().ToStdString(), bp::args( aArguments ),
                           bp::std_out > stdoutLog.GetFullPath().ToStdString(),
                           bp::std_err > stderrLog.GetFullPath().ToStdString() );
        const auto deadline = std::chrono::steady_clock::now() + MAX_NATIVE_FABRICATION_WAIT;
        std::error_code processError;

        while( process.running( processError ) && !processError
               && std::chrono::steady_clock::now() < deadline )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }

        finished = !process.running( processError ) && !processError;

        if( !finished )
        {
            process.terminate();
            process.wait();

            if( processError )
                aError = "native " + aKind + " export process failed: "
                         + processError.message();
        }
        else
        {
            process.wait();
            exitCode = process.exit_code();
        }
    }
    catch( const std::exception& error )
    {
        aError = "could not run native " + aKind + " export: " + error.what();
    }

    if( aError.empty() && ( !finished || exitCode != 0 ) )
    {
        aError = !finished ? "native " + aKind + " export exceeded 120 seconds"
                           : "native KiCad could not complete the " + aKind + " export";

        if( stderrLog.FileExists() )
        {
            wxFile errorFile( stderrLog.GetFullPath(), wxFile::read );
            const wxFileOffset length = errorFile.IsOpened() ? errorFile.Length() : 0;

            if( length > 0 )
            {
                const size_t bounded = std::min<size_t>( static_cast<size_t>( length ), 4096 );
                std::string detail( bounded, '\0' );

                if( errorFile.Read( detail.data(), detail.size() )
                    == static_cast<wxFileOffset>( detail.size() ) )
                {
                    aError += ": " + detail;
                }
            }
        }
    }

    return aError.empty();
}


bool writeCroppedPreview( const wxFileName& aRasterized, const wxFileName& aOutput,
                          std::string& aError )
{
    // PDF plots retain their entire paper rectangle.  Crop uniform page margins so the model
    // receives the actual circuit at useful resolution instead of a mostly blank paper image.
    wxImage preview;
    bool wrotePreview = false;

    if( preview.LoadFile( aRasterized.GetFullPath(), wxBITMAP_TYPE_PNG ) && preview.IsOk()
        && preview.GetWidth() > 0 && preview.GetHeight() > 0 && preview.GetData() )
    {
        const int width = preview.GetWidth();
        const int height = preview.GetHeight();
        const unsigned char* pixels = preview.GetData();
        const auto channel = [&]( int aX, int aY, int aChannel ) -> int
        {
            return pixels[( static_cast<size_t>( aY ) * width + aX ) * 3 + aChannel];
        };
        const int background[3] = {
            ( channel( 0, 0, 0 ) + channel( width - 1, 0, 0 )
              + channel( 0, height - 1, 0 ) + channel( width - 1, height - 1, 0 ) ) / 4,
            ( channel( 0, 0, 1 ) + channel( width - 1, 0, 1 )
              + channel( 0, height - 1, 1 ) + channel( width - 1, height - 1, 1 ) ) / 4,
            ( channel( 0, 0, 2 ) + channel( width - 1, 0, 2 )
              + channel( 0, height - 1, 2 ) + channel( width - 1, height - 1, 2 ) ) / 4
        };
        int left = width;
        int top = height;
        int right = -1;
        int bottom = -1;

        for( int y = 0; y < height; ++y )
        {
            for( int x = 0; x < width; ++x )
            {
                const bool content =
                        std::abs( channel( x, y, 0 ) - background[0] ) > 12
                        || std::abs( channel( x, y, 1 ) - background[1] ) > 12
                        || std::abs( channel( x, y, 2 ) - background[2] ) > 12;

                if( content )
                {
                    left = std::min( left, x );
                    top = std::min( top, y );
                    right = std::max( right, x );
                    bottom = std::max( bottom, y );
                }
            }
        }

        if( right >= left && bottom >= top )
        {
            const int padding = std::max( 16, std::min( width, height ) / 40 );
            left = std::max( 0, left - padding );
            top = std::max( 0, top - padding );
            right = std::min( width - 1, right + padding );
            bottom = std::min( height - 1, bottom + padding );
            wxImage cropped = preview.GetSubImage(
                    wxRect( left, top, right - left + 1, bottom - top + 1 ) );
            wrotePreview = cropped.IsOk()
                           && cropped.SaveFile( aOutput.GetFullPath(), wxBITMAP_TYPE_PNG );
        }
    }

    if( !wrotePreview
        && !wxCopyFile( aRasterized.GetFullPath(), aOutput.GetFullPath(), true ) )
    {
        aError = "preview rasterization did not produce a readable PNG";
        return false;
    }

    return true;
}


bool runNativeKiCadPreview( const std::string& aView, const wxFileName& aInput,
                            const std::vector<int>& aPages,
                            const std::vector<wxFileName>& aOutputs,
                            std::string& aError )
{
    if( aPages.empty() || aPages.size() != aOutputs.size() )
    {
        aError = "native preview requires one output for every requested page";
        return false;
    }

    wxFileName cli;

    if( !findNativeKiCadCli( cli ) )
    {
        aError = "the sibling kicad-cli preview backend is unavailable";
        return false;
    }

    PRIVATE_TEMPORARY_DIRECTORY temporary;

    if( !temporary.Create( "kichad-preview", aError ) )
        return false;

    wxFileName logs = wxFileName::DirName(
            wxString::FromUTF8( temporary.Path().string() ) );

    for( size_t i = 0; i < aOutputs.size(); ++i )
    {
        if( aPages[i] < 1 || aPages[i] > 1000 )
        {
            aError = "native schematic preview page must be between 1 and 1000";
            return false;
        }

        if( aOutputs[i].FileExists() && !wxRemoveFile( aOutputs[i].GetFullPath() ) )
        {
            aError = "could not replace the prior derived preview";
            return false;
        }
    }

    if( aView == "pcb3d" )
    {
        if( aOutputs.size() != 1 )
        {
            aError = "PCB previews require exactly one output";
            return false;
        }

        const std::vector<std::string> arguments = {
            "pcb", "render", "--output", aOutputs.front().GetFullPath().ToStdString(),
            "--width", "1600", "--height", "1200", "--side", "top",
            "--background", "opaque", "--quality", "high", "--preset",
            "follow_plot_settings", aInput.GetFullPath().ToStdString()
        };

        if( !runNativeFabricationCommand( cli, arguments, logs, 0, "PCB preview", aError ) )
            return false;

        if( !aOutputs.front().FileExists() )
        {
            aError = "native KiCad did not produce the requested PCB preview";
            return false;
        }

        return true;
    }

    wxFileName pdf( wxString::FromUTF8( temporary.Path().string() ),
                    wxS( "preview.pdf" ) );
    std::vector<std::string> arguments;

    if( aView == "schematic" )
    {
        std::string pages;

        for( int page : aPages )
        {
            if( !pages.empty() )
                pages += ',';

            pages += std::to_string( page );
        }

        arguments = { "sch", "export", "pdf", "--output",
                      pdf.GetFullPath().ToStdString(), "--exclude-drawing-sheet",
                      "--pages", pages,
                      aInput.GetFullPath().ToStdString() };
    }
    else if( aView == "pcb2d" )
    {
        if( aOutputs.size() != 1 )
        {
            aError = "PCB previews require exactly one output";
            return false;
        }

        arguments = { "pcb", "export", "pdf", "--output",
                      pdf.GetFullPath().ToStdString(), "--layers",
                      "F.Cu,B.Cu,F.SilkS,B.SilkS,Edge.Cuts", "--mode-single",
                      "--scale", "0", "--exclude-value", "--no-property-popups",
                      aInput.GetFullPath().ToStdString() };
    }
    else if( aView == "pcblayout" )
    {
        if( aOutputs.size() != 1 )
        {
            aError = "PCB previews require exactly one output";
            return false;
        }

        arguments = { "pcb", "export", "pdf", "--output",
                      pdf.GetFullPath().ToStdString(), "--layers",
                      "F.Cu,B.Cu,F.SilkS,B.SilkS,F.Fab,B.Fab,F.CrtYd,B.CrtYd,Edge.Cuts",
                      "--mode-single", "--scale", "0", "--no-property-popups",
                      aInput.GetFullPath().ToStdString() };
    }
    else
    {
        aError = "unsupported native preview view";
        return false;
    }

    if( !runNativeFabricationCommand( cli, arguments, logs, 0, aView + " preview", aError )
        || !pdf.FileExists() )
    {
        if( aError.empty() )
            aError = "native KiCad did not produce the preview PDF";

        return false;
    }

    wxString executableSearchPath;
    wxString rasterizer;

    if( !wxGetEnv( wxS( "PATH" ), &executableSearchPath )
        || !wxFindFileInPath( &rasterizer, executableSearchPath, wxS( "pdftoppm" ) ) )
    {
        aError = "the PDF-to-PNG preview rasterizer is unavailable";
        return false;
    }

    wxFileName rasterizerPath( rasterizer );
    wxFileName prefix( wxString::FromUTF8( temporary.Path().string() ),
                       wxS( "rasterized" ) );
    prefix.ClearExt();
    std::vector<std::string> rasterArguments = { "-png", "-r", "160" };

    if( aOutputs.size() == 1 )
        rasterArguments.emplace_back( "-singlefile" );

    rasterArguments.emplace_back( pdf.GetFullPath().ToStdString() );
    rasterArguments.emplace_back( prefix.GetFullPath().ToStdString() );

    if( !runNativeFabricationCommand( rasterizerPath, rasterArguments, logs, 1,
                                      "preview rasterization", aError ) )
    {
        return false;
    }

    for( size_t i = 0; i < aOutputs.size(); ++i )
    {
        wxFileName rasterized = prefix;

        if( aOutputs.size() == 1 )
        {
            rasterized.SetExt( wxS( "png" ) );
        }
        else
        {
            rasterized.SetFullName( prefix.GetName() + wxString::Format( "-%zu.png", i + 1 ) );
        }

        if( !rasterized.FileExists()
            || !writeCroppedPreview( rasterized, aOutputs[i], aError ) )
        {
            if( aError.empty() )
                aError = "preview rasterization did not produce every requested page";

            for( const wxFileName& output : aOutputs )
            {
                if( output.FileExists() )
                    wxRemoveFile( output.GetFullPath() );
            }

            return false;
        }
    }

    return true;
}


bool runNativeKiCadFabrication( const wxFileName& aBoard,
                                const wxFileName& aSchematic,
                                const JSON& aPlan, const wxFileName& aStaging,
                                std::string& aError )
{
    wxFileName cli;

    if( !findNativeKiCadCli( cli ) )
    {
        aError = "the sibling kicad-cli fabrication backend is unavailable";
        return false;
    }

    const std::filesystem::path staging( aStaging.GetFullPath().ToStdString() );
    const std::filesystem::path logs = staging / ".native-logs";
    std::error_code filesystemError;
    std::filesystem::create_directories( logs, filesystemError );

    if( filesystemError )
    {
        aError = "could not create private native fabrication logs";
        return false;
    }

    wxFileName logDirectory = wxFileName::DirName( wxString::FromUTF8( logs.string() ) );
    size_t commandIndex = 0;

    for( const JSON& job : aPlan.at( "jobs" ) )
    {
        const std::string kind = job.at( "kind" ).get<std::string>();

        if( kind == "bom" )
            continue;

        const std::filesystem::path output =
                staging / job.at( "relativePath" ).get<std::string>();
        const std::filesystem::path parent =
                job.at( "directory" ).get<bool>() ? output : output.parent_path();
        std::filesystem::create_directories( parent, filesystemError );

        if( filesystemError )
        {
            aError = "could not create private " + kind + " staging directory";
            break;
        }

        std::vector<std::string> arguments;

        if( kind == "gerbers" )
        {
            arguments = { "pcb", "export", "gerbers", "--output", output.string(),
                          "--layers", joinCommaSeparated( job.at( "layers" ) ),
                          "--exclude-value", "--subtract-soldermask", "--precision", "6",
                          "--no-protel-ext", "--check-zones",
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "drill" )
        {
            arguments = { "pcb", "export", "drill", "--output", output.string(),
                          "--format", "excellon", "--drill-origin", "absolute",
                          "--excellon-zeros-format", "decimal", "--excellon-oval-format",
                          "alternate", "--excellon-units", "mm", "--excellon-separate-th",
                          "--generate-map", "--map-format", "pdf", "--generate-report",
                          "--report-path", ( output / "drill-report.rpt" ).string(),
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "ipcd356" )
        {
            arguments = { "pcb", "export", "ipcd356", "--output", output.string(),
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "ipc2581" )
        {
            arguments = { "pcb", "export", "ipc2581", "--output", output.string(),
                          "--precision", "6", "--version", "C", "--units", "mm",
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "odbpp" )
        {
            arguments = { "pcb", "export", "odb", "--output", output.string(),
                          "--precision", "4", "--compression", "zip", "--units", "mm",
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "pick_place" )
        {
            arguments = { "pcb", "export", "pos", "--output", output.string(),
                          "--side", "both", "--format", "csv", "--units", "mm",
                          "--exclude-dnp", aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "step" )
        {
            arguments = { "pcb", "export", "step", "--output", output.string(),
                          "--force", "--no-dnp", "--subst-models", "--include-tracks",
                          "--include-pads", "--include-zones", "--include-inner-copper",
                          "--include-silkscreen", "--include-soldermask",
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "brep" || kind == "glb" || kind == "stl"
                 || kind == "xao" || kind == "stepz" || kind == "u3d"
                 || kind == "3d_pdf" )
        {
            const std::string nativeKind = kind == "3d_pdf" ? "3dpdf"
                                           : kind == "stepz" ? "stpz"
                                                             : kind;
            arguments = { "pcb", "export", nativeKind, "--output", output.string(),
                          "--force", "--no-dnp", "--no-unspecified", "--subst-models",
                          "--include-tracks", "--include-pads", "--include-zones",
                          "--include-inner-copper", "--include-silkscreen",
                          "--include-soldermask", aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "pdf" )
        {
            arguments = { "pcb", "export", "pdf", "--output", output.string(),
                          "--layers", joinCommaSeparated( job.at( "layers" ) ),
                          "--mode-multipage", "--black-and-white", "--subtract-soldermask",
                          "--check-zones", "--no-property-popups",
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "board_ps" )
        {
            arguments = { "pcb", "export", "ps", "--output", output.string(),
                          "--layers", joinCommaSeparated( job.at( "layers" ) ),
                          "--mode-multi", "--black-and-white", "--subtract-soldermask",
                          "--force-a4", "--scale", "1", "--check-zones",
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "schematic_pdf" )
        {
            arguments = { "sch", "export", "pdf", "--output", output.string(),
                          "--black-and-white", "--no-background-color",
                          "--exclude-pdf-property-popups",
                          "--exclude-pdf-hierarchical-links",
                          aSchematic.GetFullPath().ToStdString() };
        }
        else if( kind == "schematic_svg" )
        {
            arguments = { "sch", "export", "svg", "--output", output.string(),
                          "--black-and-white", "--no-background-color",
                          aSchematic.GetFullPath().ToStdString() };
        }
        else if( kind == "schematic_dxf" )
        {
            arguments = { "sch", "export", "dxf", "--output", output.string(),
                          "--black-and-white", aSchematic.GetFullPath().ToStdString() };
        }
        else if( kind == "schematic_ps" )
        {
            arguments = { "sch", "export", "ps", "--output", output.string(),
                          "--black-and-white", "--no-background-color",
                          aSchematic.GetFullPath().ToStdString() };
        }
        else if( kind == "schematic_bom" )
        {
            arguments = { "sch", "export", "bom", "--output", output.string(),
                          "--fields", "Reference,Value,Footprint,QUANTITY,DNP",
                          "--labels", "Reference,Value,Footprint,Quantity,DNP",
                          "--sort-field", "Reference",
                          aSchematic.GetFullPath().ToStdString() };
        }
        else if( kind == "legacy_bom_xml" )
        {
            arguments = { "sch", "export", "python-bom", "--output", output.string(),
                          aSchematic.GetFullPath().ToStdString() };
        }
        else if( kind == "board_render" )
        {
            arguments = { "pcb", "render", "--output", output.string(),
                          "--width", "1024", "--height", "1024", "--side", "top",
                          "--background", "transparent", "--quality", "basic",
                          "--preset", "follow_plot_settings", "--zoom", "1",
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "assembly_svg" )
        {
            arguments = { "pcb", "export", "svg", "--output", output.string(),
                          "--layers", joinCommaSeparated( job.at( "layers" ) ),
                          "--common-layers",
                          joinCommaSeparated( job.at( "commonLayers" ) ), "--mode-multi",
                          "--black-and-white", "--sketch-pads-on-fab-layers",
                          "--hide-DNP-footprints-on-fab-layers", "--page-size-mode", "2",
                          "--fit-page-to-board", "--exclude-drawing-sheet",
                          "--drill-shape-opt", "2", "--scale", "1", "--check-zones",
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "assembly_dxf" )
        {
            arguments = { "pcb", "export", "dxf", "--output", output.string(),
                          "--layers", joinCommaSeparated( job.at( "layers" ) ),
                          "--common-layers",
                          joinCommaSeparated( job.at( "commonLayers" ) ), "--mode-multi",
                          "--exclude-value", "--sketch-pads-on-fab-layers",
                          "--hide-DNP-footprints-on-fab-layers", "--use-contours",
                          "--output-units", "mm", "--drill-shape-opt", "2", "--scale", "1",
                          "--check-zones", aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "gencad" )
        {
            arguments = { "pcb", "export", "gencad", "--output", output.string(),
                          "--unique-pins", "--unique-footprints", "--store-origin-coord",
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "vrml" )
        {
            arguments = { "pcb", "export", "vrml", "--output", output.string(),
                          "--force", "--no-dnp", "--no-unspecified", "--units", "mm",
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "board_stats" )
        {
            arguments = { "pcb", "export", "stats", "--output", output.string(),
                          "--format", "json", "--units", "mm",
                          "--exclude-footprints-without-pads", "--subtract-holes-from-board",
                          "--subtract-holes-from-copper", aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "netlist" )
        {
            arguments = { "sch", "export", "netlist", "--output", output.string(),
                          "--format", "kicadsexpr",
                          aSchematic.GetFullPath().ToStdString() };
        }
        else
        {
            aError = "fabrication plan contains an unsupported native job";
            break;
        }

        if( !runNativeFabricationCommand( cli, arguments, logDirectory, commandIndex++, kind,
                                          aError ) )
        {
            break;
        }
    }

    std::filesystem::remove_all( logs, filesystemError );
    return aError.empty();
}


JSON buildFabricationPlan( const JSON& aIr, const std::string& aFileStem )
{
    static constexpr const char* CHECK_ORDER[] = {
        "erc", "electrical", "drc", "layout", "sourcing", "fabrication"
    };
    static constexpr const char* OUTPUT_ORDER[] = {
        "gerbers", "drill", "ipcd356", "netlist", "ipc2581", "odbpp", "pick_place", "bom",
        "schematic_bom", "legacy_bom_xml",
        "step", "stepz", "brep", "glb", "stl", "u3d", "xao", "3d_pdf", "pdf",
        "board_ps", "board_render", "schematic_pdf", "schematic_svg", "schematic_dxf",
        "schematic_ps",
        "assembly_svg", "assembly_dxf", "gencad", "vrml", "board_stats"
    };
    static const std::set<std::string> PRODUCTION_OUTPUTS = {
        "gerbers", "drill", "ipcd356", "pick_place", "bom"
    };
    std::set<std::string> checks;
    std::set<std::string> outputs;
    const bool hasElectricalDesign =
            !aIr.at( "schematic" ).at( "components" ).empty()
            || !aIr.at( "schematic" ).at( "nets" ).empty();

    for( const JSON& check : aIr.at( "checks" ) )
        checks.emplace( check.value( "kind", "" ) );

    for( const JSON& output : aIr.at( "outputs" ) )
        outputs.emplace( output.value( "kind", "" ) );

    JSON issues = JSON::array();

    for( const char* required : CHECK_ORDER )
    {
        if( std::string_view( required ) == "electrical" && !hasElectricalDesign )
            continue;

        if( !checks.contains( required ) )
        {
            issues.push_back( { { "type", "missing_check" },
                                { "severity", "error" },
                                { "description",
                                  "Production fabrication intent is missing check "
                                          + std::string( required ) },
                                { "check", required } } );
        }
    }

    for( const std::string& required : PRODUCTION_OUTPUTS )
    {
        if( !outputs.contains( required ) )
        {
            issues.push_back( { { "type", "missing_output" },
                                { "severity", "error" },
                                { "description",
                                  "Production fabrication intent is missing output " + required },
                                { "output", required } } );
        }
    }

    JSON implicitPresentationNets = JSON::array();
    size_t automaticNetCount = 0;
    size_t wiredNetCount = 0;
    size_t hierarchicalNetCount = 0;
    size_t labelNetCount = 0;

    for( const JSON& net : aIr.at( "schematic" ).at( "nets" ) )
    {
        // Label-first presentation is the KiChad default; "auto" is still counted so the
        // reported net-presentation breakdown stays truthful for boards that request it.
        const std::string presentation = net.value( "presentation", "labels" );

        if( presentation == "auto" )
            ++automaticNetCount;
        else if( presentation == "wired" )
            ++wiredNetCount;
        else if( presentation == "hierarchical" )
            ++hierarchicalNetCount;
        else
            ++labelNetCount;

        if( !net.value( "presentationExplicit", false ) )
            implicitPresentationNets.push_back( net.value( "name", "" ) );
    }

    if( !implicitPresentationNets.empty() )
    {
        issues.push_back(
                { { "type", "missing_schematic_presentation" },
                  { "severity", "error" },
                  { "description",
                    "Production fabrication requires every KDS net to declare "
                    "(presentation wired) or (presentation labels); implicit label stubs are "
                    "not reviewable design intent" },
                  { "nets", implicitPresentationNets },
                  { "recovery",
                    "Prefer (presentation labels); use (presentation wired) only for short "
                    "local connections whose generated wires stay clear of symbols." } } );
    }

    // Every fitted physical component must carry a datasheet conformance record: the agent
    // verifies pinout and application-circuit support against the exact part's datasheet
    // and records it; production fabrication is blocked without that evidence.
    std::map<std::string, const JSON*> conformanceRecords;

    if( aIr.contains( "conformance" ) && aIr["conformance"].is_array() )
    {
        for( const JSON& record : aIr["conformance"] )
            conformanceRecords[record.value( "component", "" )] = &record;
    }

    for( const JSON& component : aIr.at( "schematic" ).at( "components" ) )
    {
        const std::string reference = component.value( "reference", "" );

        if( reference.empty() || component.value( "dnp", false )
            || !component.contains( "footprint" ) || component["footprint"].is_null() )
        {
            continue;
        }

        const auto record = conformanceRecords.find( reference );

        if( record == conformanceRecords.end() )
        {
            issues.push_back(
                    { { "type", "missing_datasheet_conformance" },
                      { "severity", "error" },
                      { "description",
                        "Fitted component " + reference
                                + " has no datasheet conformance record; verify its pinout "
                                  "and application circuit against the exact part's datasheet "
                                  "and record a (conformance ...) statement" },
                      { "component", reference } } );
        }
        else
        {
            for( const JSON& deviation :
                 record->second->value( "deviations", JSON::array() ) )
            {
                if( !deviation.is_string() )
                    continue;

                issues.push_back(
                        { { "type", "datasheet_conformance_deviation" },
                          { "severity", "warning" },
                          { "description",
                            "Component " + reference + " deviates from its datasheet: "
                                    + deviation.get<std::string>() },
                          { "component", reference } } );
            }
        }
    }

    KICHAD::DESIGN_SCRIPT_LAYOUT_ANALYZER::RESULT layout =
            KICHAD::DESIGN_SCRIPT_LAYOUT_ANALYZER::Analyze( aIr );

    for( const JSON& layoutIssue : layout.issues )
        issues.push_back( layoutIssue );

    const JSON* stackup = nullptr;

    for( const JSON& statement : aIr.at( "pcb" ) )
    {
        if( statement.value( "kind", "" ) == "stackup" )
        {
            stackup = &statement;
            break;
        }
    }

    JSON layers = JSON::array();

    if( stackup == nullptr )
    {
        issues.push_back( { { "type", "missing_stackup" },
                            { "severity", "error" },
                            { "description",
                              "Production fabrication requires one explicit KDS stackup" } } );
    }
    else
    {
        for( const JSON& entry : stackup->at( "layers" ) )
        {
            const std::string category = entry.value( "category", "" );
            std::string layer = entry.value( "layer", "" );

            if( category != "copper" && category != "soldermask"
                && category != "solderpaste" && category != "silkscreen" )
            {
                continue;
            }

            if( layer == "F.SilkS" )
                layer = "F.Silkscreen";
            else if( layer == "B.SilkS" )
                layer = "B.Silkscreen";

            layers.push_back( std::move( layer ) );
        }

        layers.push_back( "Edge.Cuts" );
    }

    JSON jobs = JSON::array();
    std::set<std::string> bomReferences;
    std::set<std::string> placementReferences;
    std::set<std::string> netlistReferences;
    JSON expectedSchematicBomRows = JSON::array();
    JSON expectedNetlistNets = JSON::array();
    JSON expectedNetlistNoConnects = JSON::array();
    std::string expectedSchematicPdfTitle = aFileStem + ".pdf";

    if( aIr.at( "project" ).contains( "titleBlock" ) )
    {
        const std::string title =
                aIr.at( "project" ).at( "titleBlock" ).value( "title", "" );

        if( !title.empty() )
            expectedSchematicPdfTitle = title;
    }
    else
    {
        for( const JSON& sheet : aIr.at( "schematic" ).at( "sheets" ) )
        {
            if( sheet.at( "parent" ).is_null() )
            {
                expectedSchematicPdfTitle = sheet.at( "title" ).get<std::string>();
                break;
            }
        }
    }

    for( const JSON& component : aIr.at( "schematic" ).at( "components" ) )
    {
        const std::string reference = component.at( "reference" ).get<std::string>();
        netlistReferences.emplace( reference );
        expectedSchematicBomRows.push_back(
                { { "reference", reference },
                  { "value", component.at( "value" ) },
                  { "footprint", component.at( "footprint" ).is_string()
                                             ? component.at( "footprint" )
                                                       .get<std::string>()
                                             : std::string() },
                  { "dnp", component.value( "dnp", false ) } } );

        if( !component.contains( "footprint" ) || !component.at( "footprint" ).is_string() )
            continue;

        bomReferences.emplace( reference );

        if( !component.value( "dnp", false ) )
            placementReferences.emplace( reference );
    }

    for( const JSON& net : aIr.at( "schematic" ).at( "nets" ) )
    {
        std::set<std::pair<std::string, std::string>> nodes;
        JSON plannedNodes = JSON::array();

        for( const JSON& pin : net.at( "pins" ) )
        {
            nodes.emplace( pin.at( "component" ).get<std::string>(),
                           pin.at( "number" ).get<std::string>() );
        }

        for( const auto& [reference, pin] : nodes )
            plannedNodes.push_back( { { "reference", reference }, { "pin", pin } } );

        expectedNetlistNets.push_back( { { "name", net.at( "name" ) },
                                         { "nodes", std::move( plannedNodes ) } } );
    }

    std::set<std::pair<std::string, std::string>> noConnects;

    for( const JSON& noConnect : aIr.at( "schematic" ).at( "noConnects" ) )
    {
        noConnects.emplace( noConnect.at( "component" ).get<std::string>(),
                            noConnect.at( "number" ).get<std::string>() );
    }

    for( const auto& [reference, pin] : noConnects )
    {
        expectedNetlistNoConnects.push_back(
                { { "reference", reference }, { "pin", pin } } );
    }

    for( const char* kind : OUTPUT_ORDER )
    {
        if( !outputs.contains( kind ) )
            continue;

        JSON job = { { "kind", kind }, { "directory", false } };

        if( std::string_view( kind ) == "gerbers" )
        {
            job["relativePath"] = "gerbers";
            job["directory"] = true;
            job["layers"] = layers;
        }
        else if( std::string_view( kind ) == "drill" )
        {
            job["relativePath"] = "drill";
            job["directory"] = true;
        }
        else if( std::string_view( kind ) == "ipcd356" )
        {
            job["relativePath"] = "electrical-test/" + aFileStem + ".d356";
        }
        else if( std::string_view( kind ) == "netlist" )
        {
            job["relativePath"] = "netlist/" + aFileStem + ".net";
        }
        else if( std::string_view( kind ) == "ipc2581" )
        {
            job["relativePath"] = "manufacturing/" + aFileStem + ".ipc2581.xml";
        }
        else if( std::string_view( kind ) == "odbpp" )
        {
            job["relativePath"] = "manufacturing/" + aFileStem + ".odb.zip";
        }
        else if( std::string_view( kind ) == "pick_place" )
        {
            job["relativePath"] = "assembly/" + aFileStem + "-positions.csv";
        }
        else if( std::string_view( kind ) == "bom" )
        {
            job["relativePath"] = "assembly/" + aFileStem + "-bom.csv";
        }
        else if( std::string_view( kind ) == "schematic_bom" )
        {
            job["relativePath"] = "interchange/" + aFileStem + "-schematic-bom.csv";
        }
        else if( std::string_view( kind ) == "legacy_bom_xml" )
        {
            job["relativePath"] = "interchange/" + aFileStem + "-legacy-bom.xml";
        }
        else if( std::string_view( kind ) == "step" )
        {
            job["relativePath"] = "model/" + aFileStem + ".step";
        }
        else if( std::string_view( kind ) == "stepz" )
        {
            job["relativePath"] = "model/" + aFileStem + ".stpz";
        }
        else if( std::string_view( kind ) == "brep"
                 || std::string_view( kind ) == "glb"
                 || std::string_view( kind ) == "stl"
                 || std::string_view( kind ) == "xao"
                 || std::string_view( kind ) == "u3d" )
        {
            job["relativePath"] = "model/" + aFileStem + "." + kind;
        }
        else if( std::string_view( kind ) == "3d_pdf" )
        {
            job["relativePath"] = "model/" + aFileStem + ".3d.pdf";
        }
        else if( std::string_view( kind ) == "pdf" )
        {
            job["relativePath"] = "documentation/" + aFileStem + ".pdf";
            job["layers"] = layers;
        }
        else if( std::string_view( kind ) == "board_ps" )
        {
            job["relativePath"] = "documentation/board_ps";
            job["directory"] = true;
            job["layers"] = layers;
        }
        else if( std::string_view( kind ) == "board_render" )
        {
            job["relativePath"] = "documentation/" + aFileStem + "-board-render.png";
        }
        else if( std::string_view( kind ) == "schematic_pdf" )
        {
            job["relativePath"] = "documentation/schematic/" + aFileStem + ".pdf";
        }
        else if( std::string_view( kind ) == "schematic_svg"
                 || std::string_view( kind ) == "schematic_dxf"
                 || std::string_view( kind ) == "schematic_ps" )
        {
            job["relativePath"] = "documentation/" + std::string( kind );
            job["directory"] = true;
        }
        else if( std::string_view( kind ) == "assembly_svg" )
        {
            job["relativePath"] = "assembly/drawings-svg";
            job["directory"] = true;
            job["layers"] = JSON::array( { "F.Fab", "B.Fab" } );
            job["commonLayers"] = JSON::array( { "Edge.Cuts" } );
            job["layersRequireExplicitEnable"] = false;
        }
        else if( std::string_view( kind ) == "assembly_dxf" )
        {
            job["relativePath"] = "assembly/drawings-dxf";
            job["directory"] = true;
            job["layers"] = JSON::array( { "F.Fab", "B.Fab" } );
            job["commonLayers"] = JSON::array( { "Edge.Cuts" } );
            job["layersRequireExplicitEnable"] = false;
        }
        else if( std::string_view( kind ) == "gencad" )
        {
            job["relativePath"] = "interchange/" + aFileStem + ".cad";
        }
        else if( std::string_view( kind ) == "vrml" )
        {
            job["relativePath"] = "model/" + aFileStem + ".wrl";
        }
        else if( std::string_view( kind ) == "board_stats" )
        {
            job["relativePath"] = "reports/" + aFileStem + "-board-stats.json";
        }

        jobs.push_back( std::move( job ) );
    }

    JSON productionPlan = KICHAD::PRODUCTION_PACKAGE_WRITER::BuildPlan(
            aIr.value( "production", JSON( nullptr ) ) );
    JSON runningIssues = issues;

    if( productionPlan.is_null() )
    {
        runningIssues.push_back( { { "type", "missing_production_handoff" },
                                   { "severity", "error" },
                                   { "description",
                                     "A running-board release requires KDS firmware, programming, power, and bring-up intent" } } );
    }

    return { { "profile", "kichad-production-10.0.4-v17" },
             { "targetDirectory", "fabrication" },
             { "fileStem", aFileStem },
             { "productionReady", issues.empty() },
             { "runningReady", runningIssues.empty() },
             { "issues", std::move( issues ) },
             { "runningIssues", std::move( runningIssues ) },
             { "schematicPresentation",
                 { { "automaticNets", automaticNetCount },
                   { "wiredNets", wiredNetCount },
                   { "hierarchicalNets", hierarchicalNetCount },
                   { "labelNets", labelNetCount },
                   { "implicitNets", implicitPresentationNets.size() } } },
             { "layout", std::move( layout.summary ) },
             { "productionPlan", std::move( productionPlan ) },
             { "expectedBomReferences", bomReferences },
             { "expectedPlacementReferences", placementReferences },
             { "expectedNetlistReferences", netlistReferences },
             { "expectedNetlistNets", std::move( expectedNetlistNets ) },
             { "expectedNetlistNoConnects", std::move( expectedNetlistNoConnects ) },
             { "expectedSchematicBomRows", std::move( expectedSchematicBomRows ) },
             { "expectedSchematicPdfTitle", std::move( expectedSchematicPdfTitle ) },
             { "jobs", std::move( jobs ) } };
}


bool buildNativeNetlistValidationPlan(
        const JSON& aIr, const JSON& aResolvedSymbols, const std::string& aFileStem,
        JSON& aPlan, std::string& aError )
{
    if( !aIr.is_object() || !aIr.contains( "schematic" )
        || !aIr["schematic"].is_object()
        || !aIr["schematic"].contains( "components" )
        || !aIr["schematic"]["components"].is_array()
        || !aResolvedSymbols.is_object() )
    {
        aError = "native netlist eligibility input is malformed";
        return false;
    }

    std::set<std::string> exportReferences;
    std::set<std::string> allReferences;

    for( const JSON& component : aIr["schematic"]["components"] )
    {
        if( !component.is_object() || !component.contains( "reference" )
            || !component["reference"].is_string()
            || !component.contains( "symbol" ) || !component["symbol"].is_string() )
        {
            aError = "compiled KDS contains a malformed component identity";
            return false;
        }

        const std::string reference = component["reference"].get<std::string>();
        const std::string libraryId = component["symbol"].get<std::string>();

        if( reference.empty() || !allReferences.emplace( reference ).second )
        {
            aError = "compiled KDS contains an empty or duplicate component reference";
            return false;
        }

        const auto resolved = aResolvedSymbols.find( libraryId );

        if( resolved == aResolvedSymbols.end() || !resolved->is_object()
            || !resolved->contains( "flags" ) || !resolved->at( "flags" ).is_object()
            || !resolved->at( "flags" ).contains( "onBoard" )
            || !resolved->at( "flags" ).at( "onBoard" ).is_boolean() )
        {
            aError = "resolved symbol " + libraryId
                     + " has no exact native on-board eligibility";
            return false;
        }

        if( resolved->at( "flags" ).at( "onBoard" ).get<bool>() )
            exportReferences.emplace( reference );
    }

    aPlan = buildFabricationPlan( aIr, aFileStem );
    aPlan["expectedNetlistReferences"] = JSON::array();

    for( const std::string& reference : exportReferences )
        aPlan["expectedNetlistReferences"].push_back( reference );

    JSON filteredNets = JSON::array();

    for( const JSON& net : aPlan.at( "expectedNetlistNets" ) )
    {
        JSON filteredNodes = JSON::array();

        for( const JSON& node : net.at( "nodes" ) )
        {
            const std::string reference = node.at( "reference" ).get<std::string>();

            if( exportReferences.contains( reference ) )
                filteredNodes.push_back( node );
        }

        if( !filteredNodes.empty() )
        {
            JSON filtered = net;
            filtered["nodes"] = std::move( filteredNodes );
            filteredNets.push_back( std::move( filtered ) );
        }
    }

    aPlan["expectedNetlistNets"] = std::move( filteredNets );
    JSON filteredNoConnects = JSON::array();

    for( const JSON& endpoint : aPlan.at( "expectedNetlistNoConnects" ) )
    {
        if( exportReferences.contains(
                    endpoint.at( "reference" ).get<std::string>() ) )
        {
            filteredNoConnects.push_back( endpoint );
        }
    }

    aPlan["expectedNetlistNoConnects"] = std::move( filteredNoConnects );
    return true;
}


std::string csvField( const std::string& aValue )
{
    std::string escaped;
    escaped.reserve( aValue.size() + 2 );
    escaped.push_back( '"' );

    for( char character : aValue )
    {
        if( character == '"' )
            escaped.push_back( '"' );

        escaped.push_back( character );
    }

    escaped.push_back( '"' );
    return escaped;
}


bool writeFabricationBom( const JSON& aIr, const wxFileName& aStaging,
                          const JSON& aPlan, size_t& aRows, std::string& aError )
{
    const JSON* bomJob = nullptr;

    for( const JSON& job : aPlan.at( "jobs" ) )
    {
        if( job.at( "kind" ) == "bom" )
        {
            bomJob = &job;
            break;
        }
    }

    if( bomJob == nullptr )
        return true;

    std::map<std::string, const JSON*> sourcing;

    for( const JSON& source : aIr.at( "sourcing" ) )
        sourcing.emplace( source.at( "component" ).get<std::string>(), &source );

    std::vector<const JSON*> components;

    for( const JSON& component : aIr.at( "schematic" ).at( "components" ) )
    {
        if( component.contains( "footprint" ) && !component.at( "footprint" ).is_null() )
            components.push_back( &component );
    }

    std::sort( components.begin(), components.end(),
               []( const JSON* aLeft, const JSON* aRight )
               {
                   return aLeft->at( "reference" ).get<std::string>()
                          < aRight->at( "reference" ).get<std::string>();
               } );
    const std::filesystem::path path =
            std::filesystem::path( aStaging.GetFullPath().ToStdString() )
            / bomJob->at( "relativePath" ).get<std::string>();
    std::error_code filesystemError;
    std::filesystem::create_directories( path.parent_path(), filesystemError );

    if( filesystemError )
    {
        aError = "could not create the private BOM staging directory";
        return false;
    }

    std::ofstream output( path, std::ios::binary | std::ios::trunc );

    if( !output )
    {
        aError = "could not create the KDS fabrication BOM";
        return false;
    }

    output << "Reference,Value,Footprint,Quantity,Manufacturer,MPN,Datasheet,Lifecycle,"
              "Supplier,SKU,Product URL,Available,Verified On,Unit Price,DNP\n";

    for( const JSON* component : components )
    {
        const std::string reference = component->at( "reference" ).get<std::string>();
        const auto source = sourcing.find( reference );

        if( source == sourcing.end() )
        {
            aError = "physical component " + reference + " has no KDS sourcing record";
            return false;
        }

        const JSON& evidence = *source->second;
        const auto text = [&]( const char* aField )
        {
            return evidence.contains( aField ) ? evidence.at( aField ).get<std::string>()
                                                : std::string();
        };
        output << csvField( reference ) << ','
               << csvField( component->at( "value" ).get<std::string>() ) << ','
               << csvField( component->at( "footprint" ).get<std::string>() ) << ','
               << evidence.at( "quantity" ).get<int64_t>() << ','
               << csvField( evidence.at( "manufacturer" ).get<std::string>() ) << ','
               << csvField( evidence.at( "mpn" ).get<std::string>() ) << ','
               << csvField( evidence.at( "datasheet" ).get<std::string>() ) << ','
               << csvField( evidence.at( "lifecycle" ).get<std::string>() ) << ','
               << csvField( evidence.at( "supplier" ).get<std::string>() ) << ','
               << csvField( evidence.at( "sku" ).get<std::string>() ) << ','
               << csvField( evidence.at( "product_url" ).get<std::string>() ) << ','
               << evidence.at( "available" ).get<int64_t>() << ','
               << csvField( evidence.at( "verified_on" ).get<std::string>() ) << ','
               << csvField( text( "unit_price" ) ) << ','
               << ( component->value( "dnp", false ) ? "true" : "false" ) << '\n';
        ++aRows;
    }

    output.flush();

    if( !output )
    {
        aError = "could not write the complete KDS fabrication BOM";
        return false;
    }

    return true;
}


bool readFileEdges( const std::filesystem::path& aPath, std::string& aPrefix,
                    std::string& aSuffix )
{
    std::ifstream input( aPath, std::ios::binary );

    if( !input )
        return false;

    constexpr std::streamsize EDGE_BYTES = 8192;
    aPrefix.resize( static_cast<size_t>( EDGE_BYTES ) );
    input.read( aPrefix.data(), EDGE_BYTES );
    aPrefix.resize( static_cast<size_t>( input.gcount() ) );
    input.clear();
    input.seekg( 0, std::ios::end );
    const std::streamoff length = input.tellg();
    const std::streamoff start = std::max<std::streamoff>( 0, length - EDGE_BYTES );
    input.seekg( start, std::ios::beg );
    aSuffix.resize( static_cast<size_t>( length - start ) );
    input.read( aSuffix.data(), static_cast<std::streamsize>( aSuffix.size() ) );
    return input.good() || input.eof();
}


bool validateIpc2581Artifact( const std::filesystem::path& aPath, const JSON& aPlan,
                              std::string& aError )
{
    std::ifstream input( aPath, std::ios::binary );

    if( !input )
    {
        aError = "could not read IPC-2581 artifact";
        return false;
    }

    std::array<char, 64 * 1024> buffer;
    std::string                 scanWindow;

    while( input )
    {
        input.read( buffer.data(), static_cast<std::streamsize>( buffer.size() ) );
        const std::streamsize bytes = input.gcount();

        if( bytes <= 0 )
            break;

        scanWindow.append( buffer.data(), static_cast<size_t>( bytes ) );

        if( scanWindow.find( "<!DOCTYPE" ) != std::string::npos
            || scanWindow.find( "<!ENTITY" ) != std::string::npos )
        {
            aError = "IPC-2581 artifact contains a forbidden document or entity declaration";
            return false;
        }

        constexpr size_t DECLARATION_OVERLAP = 16;

        if( scanWindow.size() > DECLARATION_OVERLAP )
            scanWindow.erase( 0, scanWindow.size() - DECLARATION_OVERLAP );
    }

    if( input.bad() )
    {
        aError = "could not inspect the complete IPC-2581 artifact";
        return false;
    }

    wxXmlDocument document;
    const std::string nativePath = aPath.string();

    if( !document.Load( wxString::FromUTF8( nativePath.c_str() ) ) )
    {
        aError = "IPC-2581 artifact is not well-formed XML";
        return false;
    }

    wxXmlNode* root = document.GetRoot();

    if( !root || root->GetName() != wxS( "IPC-2581" )
        || root->GetAttribute( wxS( "revision" ) ) != wxS( "C" )
        || root->GetAttribute( wxS( "xmlns" ) ) != wxS( "http://webstds.ipc.org/2581" )
        || root->GetAttribute( wxS( "xsi:schemaLocation" ) )
                   != wxS( "http://webstds.ipc.org/2581 "
                           "http://webstds.ipc.org/2581/IPC-2581C.xsd" ) )
    {
        aError = "IPC-2581 artifact has the wrong revision or root schema";
        return false;
    }

    static const std::set<wxString> ALLOWED_TOP_LEVEL = {
        wxS( "Content" ), wxS( "LogisticHeader" ), wxS( "HistoryRecord" ),
        wxS( "Bom" ), wxS( "Ecad" ), wxS( "Avl" )
    };
    std::map<wxString, size_t> topLevelCounts;
    wxXmlNode*                 content = nullptr;

    for( wxXmlNode* child = root->GetChildren(); child; child = child->GetNext() )
    {
        if( child->GetType() != wxXML_ELEMENT_NODE )
            continue;

        if( !ALLOWED_TOP_LEVEL.contains( child->GetName() ) )
        {
            aError = "IPC-2581 artifact contains an unexpected top-level element";
            return false;
        }

        if( ++topLevelCounts[child->GetName()] > 1 )
        {
            aError = "IPC-2581 artifact contains a duplicate top-level section";
            return false;
        }

        if( child->GetName() == wxS( "Content" ) )
            content = child;
    }

    if( topLevelCounts[wxS( "Content" )] != 1
        || topLevelCounts[wxS( "LogisticHeader" )] != 1
        || topLevelCounts[wxS( "HistoryRecord" )] != 1
        || topLevelCounts[wxS( "Ecad" )] != 1 )
    {
        aError = "IPC-2581 artifact is missing a required top-level section";
        return false;
    }

    const wxString expectedStep = wxString::FromUTF8( aPlan.at( "fileStem" )
                                                               .get<std::string>()
                                                               .c_str() );
    bool contentStepMatches = false;

    for( wxXmlNode* child = content->GetChildren(); child; child = child->GetNext() )
    {
        if( child->GetType() == wxXML_ELEMENT_NODE && child->GetName() == wxS( "StepRef" )
            && child->GetAttribute( wxS( "name" ) ) == expectedStep )
        {
            contentStepMatches = true;
        }
    }

    if( !contentStepMatches )
    {
        aError = "IPC-2581 Content does not reference the planned board step";
        return false;
    }

    constexpr size_t MAX_IPC2581_XML_NODES = 2'000'000;
    constexpr size_t MAX_IPC2581_XML_DEPTH = 256;
    std::vector<std::pair<wxXmlNode*, size_t>> pending = { { root, 0 } };
    std::set<std::string> componentReferences;
    std::set<std::string> bomReferences;
    size_t nodeCount = 0;
    size_t cadHeaders = 0;
    size_t boardSteps = 0;
    size_t stackups = 0;
    size_t conductorLayers = 0;
    size_t outlineLayers = 0;
    size_t softwarePackages = 0;

    while( !pending.empty() )
    {
        const auto [first, depth] = pending.back();
        pending.pop_back();

        if( depth > MAX_IPC2581_XML_DEPTH )
        {
            aError = "IPC-2581 artifact exceeds the XML nesting limit";
            return false;
        }

        for( wxXmlNode* node = first; node; node = node->GetNext() )
        {
            if( ++nodeCount > MAX_IPC2581_XML_NODES )
            {
                aError = "IPC-2581 artifact exceeds the XML node limit";
                return false;
            }

            if( node->GetChildren() )
                pending.emplace_back( node->GetChildren(), depth + 1 );

            if( node->GetType() != wxXML_ELEMENT_NODE )
                continue;

            const wxString name = node->GetName();

            if( name == wxS( "SoftwarePackage" ) )
            {
                ++softwarePackages;

                if( node->GetAttribute( wxS( "name" ) ) != wxS( "KiCad" )
                    || node->GetAttribute( wxS( "revision" ) ) != wxS( "10.0.4" )
                    || node->GetAttribute( wxS( "vendor" ) ) != wxS( "KiCad EDA" ) )
                {
                    aError = "IPC-2581 artifact has the wrong native producer identity";
                    return false;
                }
            }
            else if( name == wxS( "CadHeader" ) )
            {
                ++cadHeaders;

                if( node->GetAttribute( wxS( "units" ) ) != wxS( "MILLIMETER" ) )
                {
                    aError = "IPC-2581 artifact is not expressed in millimetres";
                    return false;
                }
            }
            else if( name == wxS( "Step" ) )
            {
                if( node->GetAttribute( wxS( "name" ) ) == expectedStep
                    && node->GetAttribute( wxS( "type" ) ) == wxS( "BOARD" ) )
                {
                    ++boardSteps;
                }
            }
            else if( name == wxS( "Stackup" ) )
            {
                ++stackups;
            }
            else if( name == wxS( "Layer" ) )
            {
                if( node->GetAttribute( wxS( "layerFunction" ) ) == wxS( "CONDUCTOR" ) )
                    ++conductorLayers;
                else if( node->GetAttribute( wxS( "layerFunction" ) )
                         == wxS( "BOARD_OUTLINE" ) )
                    ++outlineLayers;
            }
            else if( name == wxS( "Component" ) )
            {
                const std::string reference =
                        node->GetAttribute( wxS( "refDes" ) ).ToStdString();

                if( reference.empty() || !componentReferences.emplace( reference ).second )
                {
                    aError = "IPC-2581 artifact has an empty or duplicate component reference";
                    return false;
                }
            }
            else if( name == wxS( "RefDes" ) )
            {
                const std::string reference =
                        node->GetAttribute( wxS( "name" ) ).ToStdString();

                if( reference.empty() || !bomReferences.emplace( reference ).second )
                {
                    aError = "IPC-2581 artifact has an empty or duplicate BOM reference";
                    return false;
                }
            }
        }
    }

    if( softwarePackages != 1 || cadHeaders != 1 || boardSteps != 1 || stackups != 1
        || conductorLayers < 2 || outlineLayers != 1 )
    {
        aError = "IPC-2581 artifact is missing its native board, stackup, or producer structure";
        return false;
    }

    if( componentReferences != bomReferences )
    {
        aError = "IPC-2581 component and native BOM reference sets differ";
        return false;
    }

    for( const JSON& expected : aPlan.at( "expectedBomReferences" ) )
    {
        if( !componentReferences.contains( expected.get<std::string>() ) )
        {
            aError = "IPC-2581 artifact is missing a compiled KDS component reference";
            return false;
        }
    }

    return true;
}


bool validateOdbArchive( const std::filesystem::path& aPath, const JSON& aPlan,
                         std::string& aError )
{
    constexpr size_t MAX_ODB_ARCHIVE_ENTRIES = 20'000;
    constexpr uintmax_t MAX_ODB_CONTROL_FILE_BYTES = 32ULL * 1024ULL * 1024ULL;
    constexpr size_t MAX_ODB_PATH_BYTES = 1024;
    constexpr size_t MAX_ODB_PATH_PARTS = 32;
    static const std::set<std::string> ALLOWED_ROOTS = {
        "matrix", "steps", "input", "symbols", "fonts", "misc", "wheels", "user"
    };
    static const std::set<std::string> REQUIRED_DIRECTORIES = {
        "matrix/", "steps/", "steps/pcb/", "steps/pcb/layers/",
        "steps/pcb/netlists/", "steps/pcb/netlists/cadnet/", "steps/pcb/eda/",
        "fonts/", "misc/"
    };
    static const std::set<std::string> REQUIRED_FILES = {
        "matrix/matrix",
        "steps/pcb/stephdr",
        "steps/pcb/profile",
        "steps/pcb/eda/data",
        "steps/pcb/netlists/cadnet/netlist",
        "steps/pcb/layers/f.cu/features",
        "steps/pcb/layers/b.cu/features",
        "steps/pcb/layers/edge.cuts/features",
        "fonts/standard",
        "misc/info"
    };
    const auto safePath = [&]( const std::string& aName, bool aDirectory )
    {
        if( aName.empty() || aName.size() > MAX_ODB_PATH_BYTES || aName.front() == '/'
            || aName.find( '\\' ) != std::string::npos
            || aName.find( ':' ) != std::string::npos
            || aName.find( "//" ) != std::string::npos
            || aName.ends_with( '/' ) != aDirectory )
        {
            return false;
        }

        for( unsigned char character : aName )
        {
            if( character < 0x20 || character > 0x7e )
                return false;
        }

        size_t parts = 0;
        size_t start = 0;
        std::string root;

        while( start < aName.size() )
        {
            const size_t end = aName.find( '/', start );
            const size_t length = ( end == std::string::npos ? aName.size() : end ) - start;
            const std::string_view part( aName.data() + start, length );

            if( part.empty() || part == "." || part == ".." || ++parts > MAX_ODB_PATH_PARTS )
                return false;

            if( root.empty() )
                root.assign( part );

            if( end == std::string::npos )
                break;

            start = end + 1;
        }

        return ALLOWED_ROOTS.contains( root );
    };
    const auto lowerAscii = []( std::string aValue )
    {
        std::transform( aValue.begin(), aValue.end(), aValue.begin(),
                        []( unsigned char aCharacter )
                        {
                            return static_cast<char>( std::tolower( aCharacter ) );
                        } );
        return aValue;
    };
    const std::string nativePath = aPath.string();
    wxFFileInputStream archiveInput( wxString::FromUTF8( nativePath.c_str() ) );

    if( !archiveInput.IsOk() )
    {
        aError = "could not read ODB++ archive";
        return false;
    }

    wxZipInputStream archive( archiveInput, wxConvUTF8 );
    const int advertisedEntries = archive.GetTotalEntries();

    if( !archive.IsOk() || advertisedEntries <= 0
        || static_cast<size_t>( advertisedEntries ) > MAX_ODB_ARCHIVE_ENTRIES
        || !archive.GetComment().empty() )
    {
        aError = "ODB++ ZIP has invalid archive metadata or entry count";
        return false;
    }

    std::set<std::string> archivePaths;
    std::set<std::string> caseFoldedPaths;
    std::map<std::string, std::string> inspectedFiles;
    uintmax_t totalBytes = 0;
    size_t entryCount = 0;
    std::array<char, 64 * 1024> readBuffer;

    while( auto entry = std::unique_ptr<wxZipEntry>( archive.GetNextEntry() ) )
    {
        if( ++entryCount > MAX_ODB_ARCHIVE_ENTRIES )
        {
            aError = "ODB++ ZIP exceeds the bounded entry count";
            return false;
        }

        const bool directory = entry->IsDir();
        std::string path = entry->GetInternalName().ToStdString();

        if( directory && !path.ends_with( '/' ) )
            path.push_back( '/' );

        const int flags = entry->GetFlags();
        constexpr int ALLOWED_FLAGS = wxZIP_DEFLATE_MASK | wxZIP_SUMS_FOLLOW
                                      | wxZIP_LANG_ENC_UTF8;

        if( !safePath( path, directory ) )
        {
            aError = "ODB++ ZIP contains an unsafe entry path: " + path;
            return false;
        }

        if( !archivePaths.emplace( path ).second
            || !caseFoldedPaths.emplace( lowerAscii( path ) ).second )
        {
            aError = "ODB++ ZIP contains a duplicate or case-ambiguous entry: " + path;
            return false;
        }

        if( ( flags & ~ALLOWED_FLAGS ) != 0
            || ( flags & ( wxZIP_ENCRYPTED | wxZIP_STRONG_ENC ) ) != 0 )
        {
            aError = "ODB++ ZIP contains unsupported or encrypted flags: " + path;
            return false;
        }

        if( ( entry->GetMethod() != wxZIP_METHOD_STORE
              && entry->GetMethod() != wxZIP_METHOD_DEFLATE )
            || !entry->GetComment().empty() || entry->GetExtraLen() != 0
            || entry->GetLocalExtraLen() != 0 )
        {
            aError = "ODB++ ZIP contains unsupported compression or entry metadata: " + path;
            return false;
        }

        const wxFileOffset size = entry->GetSize();
        const wxFileOffset compressedSize = entry->GetCompressedSize();

        if( size < 0 || compressedSize < 0
            || static_cast<uintmax_t>( size ) > MAX_FABRICATION_FILE_BYTES
            || static_cast<uintmax_t>( compressedSize ) > MAX_FABRICATION_FILE_BYTES
            || totalBytes > MAX_FABRICATION_TOTAL_BYTES - static_cast<uintmax_t>( size )
            || ( directory && size != 0 ) )
        {
            aError = "ODB++ ZIP exceeds the bounded entry or expanded package size";
            return false;
        }

        if( directory )
            continue;

        const bool inspect = REQUIRED_FILES.contains( path ) || path.ends_with( "/components" );

        if( inspect && static_cast<uintmax_t>( size ) > MAX_ODB_CONTROL_FILE_BYTES )
        {
            aError = "ODB++ control or component file exceeds its inspection limit";
            return false;
        }

        std::string content;

        if( inspect )
            content.reserve( static_cast<size_t>( size ) );

        uintmax_t actualBytes = 0;

        while( true )
        {
            archive.Read( readBuffer.data(), readBuffer.size() );
            const size_t read = archive.LastRead();

            if( read == 0 )
                break;

            if( actualBytes > MAX_FABRICATION_FILE_BYTES - read
                || totalBytes > MAX_FABRICATION_TOTAL_BYTES - actualBytes - read )
            {
                aError = "ODB++ ZIP expanded data exceeds the bounded package size";
                return false;
            }

            actualBytes += read;

            if( inspect )
                content.append( readBuffer.data(), read );
        }

        if( actualBytes != static_cast<uintmax_t>( size ) || !archive.CloseEntry() )
        {
            aError = "ODB++ ZIP entry is truncated or failed its integrity check";
            return false;
        }

        totalBytes += actualBytes;

        if( inspect )
            inspectedFiles.emplace( path, std::move( content ) );
    }

    if( entryCount != static_cast<size_t>( advertisedEntries ) || !archive.Eof() )
    {
        aError = "ODB++ ZIP central directory differs from its readable entries";
        return false;
    }

    for( const std::string& required : REQUIRED_DIRECTORIES )
    {
        if( !archivePaths.contains( required ) )
        {
            aError = "ODB++ ZIP is missing a required directory";
            return false;
        }
    }

    for( const std::string& required : REQUIRED_FILES )
    {
        if( !inspectedFiles.contains( required ) )
        {
            aError = "ODB++ ZIP is missing a required manufacturing file";
            return false;
        }
    }

    const std::string& matrix = inspectedFiles.at( "matrix/matrix" );
    const std::string& info = inspectedFiles.at( "misc/info" );
    const std::string& stepHeader = inspectedFiles.at( "steps/pcb/stephdr" );
    const std::string& profile = inspectedFiles.at( "steps/pcb/profile" );
    const std::string& eda = inspectedFiles.at( "steps/pcb/eda/data" );
    const std::string& netlist = inspectedFiles.at( "steps/pcb/netlists/cadnet/netlist" );
    const auto validFeatureFile = [&]( const char* aName )
    {
        const std::string& source = inspectedFiles.at( aName );
        return source.starts_with( "UNITS=MM\n" )
               && source.find( "#Num Features" ) != std::string::npos;
    };

    if( matrix.find( "STEP {" ) == std::string::npos
        || matrix.find( "NAME=PCB" ) == std::string::npos
        || matrix.find( "NAME=F.CU" ) == std::string::npos
        || matrix.find( "NAME=B.CU" ) == std::string::npos
        || matrix.find( "NAME=EDGE.CUTS" ) == std::string::npos
        || !info.starts_with( "JOB_NAME=job\nUNITS=MM\nODB_VERSION_MAJOR=8\n"
                              "ODB_VERSION_MINOR=1\nODB_SOURCE=KiCad EDA\n" )
        || info.find( "SAVE_APP=KiCad EDA 10.0.4" ) == std::string::npos
        || stepHeader.find( "LEFT_ACTIVE=" ) == std::string::npos
        || stepHeader.find( "X_ORIGIN=" ) == std::string::npos
        || !stepHeader.ends_with( "UNITS=MM\n" )
        || !profile.starts_with( "UNITS=MM\n" )
        || profile.find( "#Num Features" ) == std::string::npos
        || eda.find( "HDR KiCad EDA 10.0.4" ) == std::string::npos
        || eda.find( "UNITS=MM\n" ) == std::string::npos
        || !netlist.starts_with( "H optimize " )
        || netlist.find( "#Netlist points" ) == std::string::npos
        || inspectedFiles.at( "fonts/standard" ).find( "XSIZE" ) == std::string::npos
        || !validFeatureFile( "steps/pcb/layers/f.cu/features" )
        || !validFeatureFile( "steps/pcb/layers/b.cu/features" )
        || !validFeatureFile( "steps/pcb/layers/edge.cuts/features" ) )
    {
        aError = "ODB++ ZIP failed its native 8.1 millimetre manufacturing structure checks";
        return false;
    }

    std::set<std::string> componentReferences;

    for( const auto& [path, source] : inspectedFiles )
    {
        if( !path.ends_with( "/components" ) )
            continue;

        std::istringstream lines( source );
        std::string line;

        while( std::getline( lines, line ) )
        {
            if( !line.starts_with( "CMP " ) )
                continue;

            std::istringstream fields( line );
            std::string record;
            std::string index;
            std::string x;
            std::string y;
            std::string rotation;
            std::string mirror;
            std::string reference;
            std::string package;

            if( !( fields >> record >> index >> x >> y >> rotation >> mirror >> reference
                   >> package )
                || record != "CMP" || reference.empty()
                || !componentReferences.emplace( reference ).second )
            {
                aError = "ODB++ ZIP has a malformed or duplicate component reference";
                return false;
            }
        }
    }

    for( const JSON& expected : aPlan.at( "expectedBomReferences" ) )
    {
        if( !componentReferences.contains( expected.get<std::string>() ) )
        {
            aError = "ODB++ ZIP is missing a compiled KDS component reference";
            return false;
        }
    }

    return true;
}


bool readCsvReferences( const std::filesystem::path& aPath,
                        std::set<std::string>& aReferences, std::string& aError )
{
    std::ifstream input( aPath, std::ios::binary );
    std::string line;

    if( !input || !std::getline( input, line ) )
    {
        aError = "could not read fabrication CSV header";
        return false;
    }

    while( std::getline( input, line ) )
    {
        if( !line.empty() && line.back() == '\r' )
            line.pop_back();

        if( line.empty() )
            continue;

        std::string reference;

        if( line.front() == '"' )
        {
            bool closed = false;

            for( size_t index = 1; index < line.size(); ++index )
            {
                if( line[index] != '"' )
                {
                    reference.push_back( line[index] );
                    continue;
                }

                if( index + 1 < line.size() && line[index + 1] == '"' )
                {
                    reference.push_back( '"' );
                    ++index;
                    continue;
                }

                closed = index + 1 < line.size() && line[index + 1] == ',';
                break;
            }

            if( !closed )
            {
                aError = "fabrication CSV has a malformed quoted reference";
                return false;
            }
        }
        else
        {
            const size_t comma = line.find( ',' );

            if( comma == std::string::npos )
            {
                aError = "fabrication CSV row has no reference column";
                return false;
            }

            reference = line.substr( 0, comma );
        }

        if( reference.empty() || !aReferences.emplace( std::move( reference ) ).second )
        {
            aError = "fabrication CSV has an empty or duplicate reference";
            return false;
        }
    }

    if( input.bad() )
    {
        aError = "could not read the complete fabrication CSV";
        return false;
    }

    return true;
}


bool hashFabricationFile( const std::filesystem::path& aPath, std::string& aDigest )
{
    std::ifstream input( aPath, std::ios::binary );

    if( !input )
        return false;

    std::array<picosha2::byte_t, picosha2::k_digest_size> digest{};
    picosha2::hash256( input, digest.begin(), digest.end() );
    aDigest = picosha2::bytes_to_hex_string( digest );
    return !input.bad() && aDigest.size() == 64;
}


bool validateExactNativeFormat( const wxFileName& aPath, const std::string& aRoot,
                                const std::string& aVersion, std::string& aError )
{
    wxFile input( aPath.GetFullPath(), wxFile::read );

    if( !input.IsOpened() )
    {
        aError = "could not open native KiCad input " + aPath.GetFullName().ToStdString();
        return false;
    }

    const wxFileOffset length = input.Length();

    if( length <= 0 )
    {
        aError = "native KiCad input is empty: " + aPath.GetFullName().ToStdString();
        return false;
    }

    const size_t headerBytes =
            std::min<size_t>( static_cast<size_t>( length ), 16 * 1024 );
    std::string header( headerBytes, '\0' );

    if( input.Read( header.data(), header.size() )
        != static_cast<wxFileOffset>( header.size() ) )
    {
        aError = "could not read native KiCad input header";
        return false;
    }

    const size_t first = header.find_first_not_of( " \t\r\n" );
    const std::string root = "(" + aRoot;
    const std::string version = "(version " + aVersion + ")";

    if( first == std::string::npos || header.compare( first, root.size(), root ) != 0
        || header.find( version, first + root.size() ) == std::string::npos )
    {
        aError = aPath.GetFullName().ToStdString()
                 + " is not in the exact native KiCad 10.0.4 " + aRoot
                 + " format (expected version " + aVersion + ")";
        return false;
    }

    return true;
}


bool validateBoardFabricationIntent( const wxFileName& aBoard, const JSON& aIr,
                                     const JSON& aPlan, std::string& aError )
{
    wxFile input( aBoard.GetFullPath(), wxFile::read );

    if( !input.IsOpened() )
    {
        aError = "could not open the native board for fabrication-intent validation";
        return false;
    }

    const wxFileOffset length = input.Length();

    if( length <= 0 || length > static_cast<wxFileOffset>( MAX_DESIGN_STATE_BYTES ) )
    {
        aError = "native board must contain 1 byte to 64 MiB for fabrication";
        return false;
    }

    std::string source( static_cast<size_t>( length ), '\0' );

    if( input.Read( source.data(), source.size() ) != length )
    {
        aError = "could not read the complete native board for fabrication";
        return false;
    }

    std::unique_ptr<KICHAD::LOSSLESS_SEXPR_DOCUMENT> document =
            KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( std::move( source ), &aError );

    if( !document || document->Roots().size() != 1
        || document->ListHead( document->Roots().front() ) != "kicad_pcb" )
    {
        if( aError.empty() )
            aError = "native board has no unique kicad_pcb root";

        return false;
    }

    const auto directList = [&]( size_t aParent, const std::string& aHead )
    {
        size_t match = KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE;

        for( size_t child : document->Nodes().at( aParent ).children )
        {
            if( document->Nodes().at( child ).kind
                        != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
                || document->ListHead( child ) != aHead )
            {
                continue;
            }

            if( match != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE )
                return KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE;

            match = child;
        }

        return match;
    };
    const auto scalar = [&]( size_t aParent, const std::string& aHead )
    {
        const size_t field = directList( aParent, aHead );

        if( field == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
            || document->Nodes().at( field ).children.size() < 2
            || document->Nodes().at( document->Nodes().at( field ).children[1] ).kind
                       == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST )
        {
            return std::string();
        }

        return document->AtomText( document->Nodes().at( field ).children[1] );
    };
    const auto numberMatches = []( const std::string& aText, double aExpected )
    {
        JSON number = JSON::parse( aText, nullptr, false );

        return !number.is_discarded() && number.is_number()
               && std::isfinite( number.get<double>() )
               && std::abs( number.get<double>() - aExpected ) <= 1e-9;
    };
    const size_t root = document->Roots().front();
    const size_t general = directList( root, "general" );
    const size_t layerTable = directList( root, "layers" );
    const size_t setup = directList( root, "setup" );
    const size_t stackup = setup == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
                                   ? KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
                                   : directList( setup, "stackup" );

    if( general == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
        || layerTable == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
        || stackup == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE )
    {
        aError = "native board is missing its general, layer-table, or physical stackup data";
        return false;
    }

    const JSON* desiredStackup = nullptr;

    for( const JSON& statement : aIr.at( "pcb" ) )
    {
        if( statement.value( "kind", "" ) == "stackup" )
        {
            desiredStackup = &statement;
            break;
        }
    }

    if( desiredStackup == nullptr )
    {
        aError = "compiled KDS has no physical stackup to validate";
        return false;
    }

    const double expectedBoardThickness =
            desiredStackup->at( "thicknessNm" ).get<double>() / 1000000.0;

    if( !numberMatches( scalar( general, "thickness" ), expectedBoardThickness ) )
    {
        aError = "native board thickness does not match the compiled KDS stackup";
        return false;
    }

    std::set<std::string> enabledLayers;

    for( size_t child : document->Nodes().at( layerTable ).children )
    {
        const auto& node = document->Nodes().at( child );

        if( node.kind != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
            || node.children.size() < 3 )
        {
            continue;
        }

        enabledLayers.emplace( document->AtomText( node.children[1] ) );

        if( node.children.size() >= 4
            && document->Nodes().at( node.children[3] ).kind
                       != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST )
        {
            enabledLayers.emplace( document->AtomText( node.children[3] ) );
        }
    }

    for( const JSON& job : aPlan.at( "jobs" ) )
    {
        if( !job.contains( "layers" )
            || !job.value( "layersRequireExplicitEnable", true ) )
            continue;

        for( const JSON& layer : job.at( "layers" ) )
        {
            if( !enabledLayers.contains( layer.get<std::string>() ) )
            {
                aError = "native board does not enable KDS fabrication layer "
                         + layer.get<std::string>();
                return false;
            }
        }
    }

    std::vector<size_t> nativeLayers;

    for( size_t child : document->Nodes().at( stackup ).children )
    {
        if( document->Nodes().at( child ).kind
                    == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
            && document->ListHead( child ) == "layer" )
        {
            nativeLayers.push_back( child );
        }
    }

    const JSON& desiredLayers = desiredStackup->at( "layers" );

    if( nativeLayers.size() != desiredLayers.size() )
    {
        aError = "native board physical stackup layer count does not match compiled KDS";
        return false;
    }

    for( size_t index = 0; index < nativeLayers.size(); ++index )
    {
        const size_t nativeLayer = nativeLayers[index];
        const auto& node = document->Nodes().at( nativeLayer );
        const JSON& desired = desiredLayers[index];
        const std::string category = desired.at( "category" ).get<std::string>();
        const std::string identity = node.children.size() >= 2
                                             ? document->AtomText( node.children[1] )
                                             : std::string();
        const bool identityMatches = category == "dielectric"
                                             ? identity.starts_with( "dielectric " )
                                             : identity == desired.value( "layer", "" );

        if( !identityMatches
            || scalar( nativeLayer, "type" )
                       != desired.at( "typeName" ).get<std::string>() )
        {
            aError = "native board physical stackup order or layer type differs from KDS";
            return false;
        }

        const int64_t thicknessNm = desired.at( "thicknessNm" ).get<int64_t>();

        if( thicknessNm > 0 )
        {
            const size_t thickness = directList( nativeLayer, "thickness" );

            if( thickness == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
                || document->Nodes().at( thickness ).children.size() < 2
                || !numberMatches(
                        document->AtomText( document->Nodes().at( thickness ).children[1] ),
                        static_cast<double>( thicknessNm ) / 1000000.0 ) )
            {
                aError = "native board physical layer thickness differs from KDS";
                return false;
            }

            if( category == "dielectric" )
            {
                const auto& thicknessNode = document->Nodes().at( thickness );
                const bool nativeLocked =
                        thicknessNode.children.size() == 3
                        && document->AtomText( thicknessNode.children[2] ) == "locked";

                if( nativeLocked != desired.at( "locked" ).get<bool>() )
                {
                    aError = "native board dielectric thickness lock differs from KDS";
                    return false;
                }
            }
        }

        for( const auto& [nativeName, desiredName] :
             std::array<std::pair<const char*, const char*>, 2>{
                     std::pair{ "material", "material" },
                     std::pair{ "color", "color" } } )
        {
            const std::string desiredText = desired.value( desiredName, "" );

            if( !desiredText.empty() && scalar( nativeLayer, nativeName ) != desiredText )
            {
                aError = std::string( "native board physical layer " ) + nativeName
                         + " differs from KDS";
                return false;
            }
        }

        for( const auto& [nativeName, desiredName] :
             std::array<std::pair<const char*, const char*>, 2>{
                     std::pair{ "epsilon_r", "epsilonR" },
                     std::pair{ "loss_tangent", "lossTangent" } } )
        {
            const double desiredNumber = desired.value( desiredName, 0.0 );

            if( desiredNumber > 0.0
                && !numberMatches( scalar( nativeLayer, nativeName ), desiredNumber ) )
            {
                aError = std::string( "native board physical layer " ) + nativeName
                         + " differs from KDS";
                return false;
            }
        }
    }

    const std::string expectedImpedance =
            desiredStackup->at( "impedanceControlled" ).get<bool>() ? "yes" : "no";
    const std::string expectedConnector = desiredStackup->at( "edgeConnector" );
    const std::string nativeConnector = scalar( stackup, "edge_connector" );
    const std::string nativePlating = scalar( stackup, "edge_plating" );

    if( scalar( stackup, "copper_finish" )
                    != desiredStackup->at( "finish" ).get<std::string>()
        || scalar( stackup, "dielectric_constraints" ) != expectedImpedance
        || ( expectedConnector == "none" ? !nativeConnector.empty()
                                          : nativeConnector != expectedConnector )
        || ( desiredStackup->at( "edgePlating" ).get<bool>() ? nativePlating != "yes"
                                                              : !nativePlating.empty() ) )
    {
        aError = "native board fabrication policies differ from compiled KDS stackup";
        return false;
    }

    std::map<std::string, std::string> expectedFootprints;

    for( const JSON& component : aIr.at( "schematic" ).at( "components" ) )
    {
        if( component.contains( "footprint" ) && component.at( "footprint" ).is_string() )
        {
            expectedFootprints.emplace( component.at( "reference" ).get<std::string>(),
                                        component.at( "footprint" ).get<std::string>() );
        }
    }

    std::map<std::string, std::string> nativeFootprints;

    for( size_t child : document->Nodes().at( root ).children )
    {
        if( document->Nodes().at( child ).kind
                    != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
            || document->ListHead( child ) != "footprint" )
        {
            continue;
        }

        const auto& footprintNode = document->Nodes().at( child );
        bool boardOnly = false;
        std::string reference;

        for( size_t footprintChild : footprintNode.children )
        {
            if( document->Nodes().at( footprintChild ).kind
                != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST )
            {
                continue;
            }

            const std::string head = document->ListHead( footprintChild );
            const auto& field = document->Nodes().at( footprintChild );

            if( head == "attr" )
            {
                for( size_t value = 1; value < field.children.size(); ++value )
                    boardOnly = boardOnly || document->AtomText( field.children[value] )
                                                     == "board_only";
            }
            else if( head == "property" && field.children.size() >= 3
                     && document->AtomText( field.children[1] ) == "Reference" )
            {
                reference = document->AtomText( field.children[2] );
            }
        }

        if( boardOnly )
            continue;

        if( footprintNode.children.size() < 2 || reference.empty()
            || !nativeFootprints
                        .emplace( reference, document->AtomText( footprintNode.children[1] ) )
                        .second )
        {
            aError = "native board has an unreferenced or duplicate schematic footprint";
            return false;
        }
    }

    if( nativeFootprints != expectedFootprints )
    {
        aError = "native board schematic-footprint inventory differs from compiled KDS";
        return false;
    }

    return true;
}


bool validateFabricationArtifacts( const wxFileName& aStaging, const JSON& aPlan,
                                   JSON& aArtifacts, std::string& aError )
{
    const std::filesystem::path root( aStaging.GetFullPath().ToStdString() );
    std::map<std::string, size_t> counts;
    std::set<std::string> actualGerberFiles;
    std::set<std::string> jobGerberFiles;
    std::set<std::string> assemblySvgFiles;
    std::set<std::string> assemblyDxfFiles;
    std::set<std::string> schematicSvgFiles;
    std::set<std::string> schematicDxfFiles;
    std::set<std::string> schematicPsFiles;
    std::set<std::string> boardPsFiles;
    std::set<std::string> bomReferences;
    std::set<std::string> placementReferences;
    std::set<std::string> allowedDirectories;
    size_t files = 0;
    uintmax_t totalBytes = 0;
    std::error_code filesystemError;
    const std::string fileStem = aPlan.at( "fileStem" ).get<std::string>();
    const std::set<std::string> expectedAssemblySvgFiles = {
        fileStem + "-F_Fab.svg", fileStem + "-B_Fab.svg"
    };
    const std::set<std::string> expectedAssemblyDxfFiles = {
        fileStem + "-F_Fab.dxf", fileStem + "-B_Fab.dxf"
    };
    std::map<std::string, std::string> expectedBoardPsFiles;
    aArtifacts = JSON::array();

    for( const JSON& job : aPlan.at( "jobs" ) )
    {
        if( job.at( "kind" ) == "board_ps" )
        {
            for( const JSON& layerValue : job.at( "layers" ) )
            {
                const std::string layer = layerValue.get<std::string>();
                std::string filenameLayer = layer;
                std::replace( filenameLayer.begin(), filenameLayer.end(), '.', '_' );
                expectedBoardPsFiles.emplace(
                        fileStem + "-" + filenameLayer + ".ps", layer );
            }
        }

        std::filesystem::path directory =
                job.at( "directory" ).get<bool>()
                        ? std::filesystem::path( job.at( "relativePath" ).get<std::string>() )
                        : std::filesystem::path( job.at( "relativePath" ).get<std::string>() )
                                  .parent_path();

        while( !directory.empty() )
        {
            allowedDirectories.emplace( directory.generic_string() );
            directory = directory.parent_path();
        }
    }

    for( std::filesystem::recursive_directory_iterator iterator( root, filesystemError ), end;
         !filesystemError && iterator != end; iterator.increment( filesystemError ) )
    {
        const std::filesystem::file_status status = iterator->symlink_status( filesystemError );

        if( filesystemError )
            break;

        if( std::filesystem::is_symlink( status ) )
        {
            aError = "fabrication staging contains a symbolic link";
            return false;
        }

        if( std::filesystem::is_directory( status ) )
        {
            const std::filesystem::path relative =
                    std::filesystem::relative( iterator->path(), root, filesystemError );

            if( filesystemError || !allowedDirectories.contains( relative.generic_string() ) )
            {
                aError = "fabrication staging contains an unexpected directory";
                return false;
            }

            continue;
        }

        if( !std::filesystem::is_regular_file( status ) )
        {
            aError = "fabrication staging contains a non-regular file";
            return false;
        }

        const uintmax_t bytes = iterator->file_size( filesystemError );

        if( filesystemError || bytes == 0 || bytes > MAX_FABRICATION_FILE_BYTES
            || ++files > MAX_FABRICATION_FILES
            || totalBytes > MAX_FABRICATION_TOTAL_BYTES - bytes )
        {
            aError = "fabrication artifacts exceed the bounded file or package limits";
            return false;
        }

        totalBytes += bytes;
        const std::filesystem::path relative =
                std::filesystem::relative( iterator->path(), root, filesystemError );

        if( filesystemError || relative.empty() || relative.is_absolute() )
        {
            aError = "fabrication artifact path is not project-relative";
            return false;
        }

        const std::string path = relative.generic_string();
        const std::string extension = iterator->path().extension().string();
        std::string kind;
        std::string prefix;
        std::string suffix;

        if( !readFileEdges( iterator->path(), prefix, suffix ) )
        {
            aError = "could not inspect fabrication artifact " + path;
            return false;
        }

        if( path.starts_with( "gerbers/" ) && extension == ".gbr" )
        {
            kind = "gerbers";
            actualGerberFiles.emplace( iterator->path().filename().string() );

            if( prefix.find( "G04" ) == std::string::npos
                || prefix.find( "TF.GenerationSoftware,KiCad" ) == std::string::npos
                || suffix.find( "M02*" ) == std::string::npos )
            {
                aError = "Gerber artifact failed native signature validation: " + path;
                return false;
            }
        }
        else if( path.starts_with( "gerbers/" ) && extension == ".gbrjob" )
        {
            kind = "gerber_job";
            std::ifstream jobInput( iterator->path(), std::ios::binary );
            JSON job = JSON::parse( jobInput, nullptr, false );

            if( job.is_discarded() || !job.is_object() || !job.contains( "Header" )
                || !job["Header"].is_object()
                || !job["Header"].contains( "GenerationSoftware" )
                || !job["Header"]["GenerationSoftware"].is_object()
                || job["Header"]["GenerationSoftware"].value( "Vendor", "" ) != "KiCad"
                || job["Header"]["GenerationSoftware"].value( "Application", "" )
                           != "Pcbnew"
                || !job.contains( "FilesAttributes" )
                || !job["FilesAttributes"].is_array() )
            {
                aError = "Gerber job artifact failed native schema validation";
                return false;
            }

            std::set<std::string> jobFiles;

            for( const JSON& attribute : job["FilesAttributes"] )
            {
                const std::string jobPath =
                        attribute.is_object() && attribute.contains( "Path" )
                                && attribute["Path"].is_string()
                                ? attribute["Path"].get<std::string>()
                                : std::string();
                const std::filesystem::path jobFile( jobPath );

                if( !attribute.is_object() || !attribute.contains( "Path" )
                    || !attribute["Path"].is_string()
                    || jobFile.empty() || jobFile.is_absolute() || jobFile.has_parent_path()
                    || jobFile.extension() != ".gbr"
                    || !jobFiles.emplace( jobPath ).second )
                {
                    aError = "Gerber job contains an invalid or duplicate file entry";
                    return false;
                }
            }

            if( jobFiles.size() == 0 )
            {
                aError = "Gerber job contains no production layer files";
                return false;
            }

            jobGerberFiles = std::move( jobFiles );
        }
        else if( path.starts_with( "drill/" ) && extension == ".drl" )
        {
            kind = "drill_file";

            if( !prefix.starts_with( "M48" ) || suffix.find( "M30" ) == std::string::npos )
            {
                aError = "Excellon artifact failed signature validation: " + path;
                return false;
            }
        }
        else if( path.starts_with( "drill/" ) && extension == ".pdf" )
        {
            kind = "drill_map";

            if( !prefix.starts_with( "%PDF-" ) || suffix.find( "%%EOF" ) == std::string::npos )
            {
                aError = "drill map failed PDF signature validation: " + path;
                return false;
            }
        }
        else if( path == "drill/drill-report.rpt" )
        {
            kind = "drill_report";

            if( prefix.find( "Drill report" ) == std::string::npos )
            {
                aError = "drill report failed native signature validation";
                return false;
            }
        }
        else if( path.starts_with( "electrical-test/" ) && extension == ".d356" )
        {
            kind = "ipcd356";

            if( !prefix.starts_with( "P  CODE 00" )
                || prefix.find( "P  UNITS CUST 0" ) == std::string::npos
                || ( !suffix.ends_with( "999\n" ) && !suffix.ends_with( "999\r\n" ) ) )
            {
                aError = "IPC-D-356 artifact failed native signature validation";
                return false;
            }
        }
        else if( path == "netlist/" + fileStem + ".net" )
        {
            kind = "netlist";

            if( !KICHAD::FABRICATION_ARTIFACT_VALIDATOR::ValidateKiCadNetlist(
                        iterator->path(), aPlan, aError ) )
            {
                return false;
            }
        }
        else if( path.starts_with( "manufacturing/" )
                 && path.ends_with( ".ipc2581.xml" ) )
        {
            kind = "ipc2581";

            if( !validateIpc2581Artifact( iterator->path(), aPlan, aError ) )
                return false;
        }
        else if( path.starts_with( "manufacturing/" ) && path.ends_with( ".odb.zip" ) )
        {
            kind = "odbpp";

            if( !prefix.starts_with( "PK\x03\x04" )
                || suffix.find( "PK\x05\x06" ) == std::string::npos )
            {
                aError = "ODB++ artifact failed ZIP signature validation";
                return false;
            }

            if( !validateOdbArchive( iterator->path(), aPlan, aError ) )
                return false;
        }
        else if( path.starts_with( "assembly/" ) && path.ends_with( "-positions.csv" ) )
        {
            kind = "pick_place";

            if( !prefix.starts_with( "Ref,Val,Package,PosX,PosY,Rot,Side" ) )
            {
                aError = "position artifact has the wrong KiCad CSV schema";
                return false;
            }

            if( !readCsvReferences( iterator->path(), placementReferences, aError ) )
                return false;
        }
        else if( path.starts_with( "assembly/" ) && path.ends_with( "-bom.csv" ) )
        {
            kind = "bom";

            if( !prefix.starts_with( "Reference,Value,Footprint,Quantity,Manufacturer,MPN," ) )
            {
                aError = "BOM artifact has the wrong KDS sourcing schema";
                return false;
            }

            if( !readCsvReferences( iterator->path(), bomReferences, aError ) )
                return false;
        }
        else if( path == "interchange/" + fileStem + "-schematic-bom.csv" )
        {
            kind = "schematic_bom";

            if( !KICHAD::SCHEMATIC_BOM_ARTIFACT_VALIDATOR::ValidateFile(
                        iterator->path(), aPlan, aError ) )
            {
                return false;
            }
        }
        else if( path == "interchange/" + fileStem + "-legacy-bom.xml" )
        {
            kind = "legacy_bom_xml";

            if( !KICHAD::LEGACY_BOM_ARTIFACT_VALIDATOR::ValidateFile(
                        iterator->path(), aPlan, aError ) )
            {
                return false;
            }
        }
        else if( path == "documentation/" + fileStem + "-board-render.png" )
        {
            kind = "board_render";

            if( !KICHAD::BOARD_RENDER_ARTIFACT_VALIDATOR::ValidateFile(
                        iterator->path(), fileStem, aError ) )
            {
                return false;
            }
        }
        else if( path.starts_with( "assembly/drawings-svg/" ) && extension == ".svg" )
        {
            kind = "assembly_svg";
            const std::string filename = iterator->path().filename().string();

            if( !expectedAssemblySvgFiles.contains( filename )
                || !assemblySvgFiles.emplace( filename ).second
                || !KICHAD::FABRICATION_ARTIFACT_VALIDATOR::ValidateAssemblySvg(
                        iterator->path(), aError ) )
            {
                if( aError.empty() )
                    aError = "assembly SVG has an unexpected or duplicate filename";

                return false;
            }
        }
        else if( path.starts_with( "assembly/drawings-dxf/" ) && extension == ".dxf" )
        {
            kind = "assembly_dxf";
            const std::string filename = iterator->path().filename().string();

            if( !expectedAssemblyDxfFiles.contains( filename )
                || !assemblyDxfFiles.emplace( filename ).second
                || !KICHAD::FABRICATION_ARTIFACT_VALIDATOR::ValidateAssemblyDxf(
                        iterator->path(), aError ) )
            {
                if( aError.empty() )
                    aError = "assembly DXF has an unexpected or duplicate filename";

                return false;
            }
        }
        else if( path == "interchange/" + fileStem + ".cad" )
        {
            kind = "gencad";

            if( !KICHAD::FABRICATION_ARTIFACT_VALIDATOR::ValidateGenCad(
                        iterator->path(), aError ) )
            {
                return false;
            }
        }
        else if( path.starts_with( "model/" ) && extension == ".step" )
        {
            kind = "step";

            if( !prefix.starts_with( "ISO-10303-21;" )
                || suffix.find( "END-ISO-10303-21;" ) == std::string::npos )
            {
                aError = "STEP artifact failed signature validation";
                return false;
            }
        }
        else if( path == "model/" + fileStem + ".brep" )
        {
            kind = "brep";

            if( !KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateBrep(
                        iterator->path(), aError ) )
            {
                return false;
            }
        }
        else if( path == "model/" + fileStem + ".glb" )
        {
            kind = "glb";

            if( !KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateGlb(
                        iterator->path(), fileStem, aError ) )
            {
                return false;
            }
        }
        else if( path == "model/" + fileStem + ".stl" )
        {
            kind = "stl";

            if( !KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateStl(
                        iterator->path(), aError ) )
            {
                return false;
            }
        }
        else if( path == "model/" + fileStem + ".xao" )
        {
            kind = "xao";

            if( !KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateXao(
                        iterator->path(), fileStem, aError ) )
            {
                return false;
            }
        }
        else if( path == "model/" + fileStem + ".stpz" )
        {
            kind = "stepz";

            if( !KICHAD::STEPZ_ARTIFACT_VALIDATOR::ValidateFile(
                        iterator->path(), fileStem, aError ) )
            {
                return false;
            }
        }
        else if( path == "model/" + fileStem + ".u3d" )
        {
            kind = "u3d";

            if( !KICHAD::U3D_ARTIFACT_VALIDATOR::ValidateFile(
                        iterator->path(), aError ) )
            {
                return false;
            }
        }
        else if( path == "model/" + fileStem + ".3d.pdf" )
        {
            kind = "3d_pdf";

            if( !KICHAD::THREE_D_PDF_ARTIFACT_VALIDATOR::ValidateFile(
                        iterator->path(), aError ) )
            {
                return false;
            }
        }
        else if( path == "model/" + fileStem + ".wrl" )
        {
            kind = "vrml";

            if( !KICHAD::FABRICATION_ARTIFACT_VALIDATOR::ValidateVrml(
                        iterator->path(), aError ) )
            {
                return false;
            }
        }
        else if( path == "documentation/schematic/" + fileStem + ".pdf" )
        {
            kind = "schematic_pdf";

            if( !KICHAD::SCHEMATIC_PDF_ARTIFACT_VALIDATOR::ValidateFile(
                        iterator->path(),
                        aPlan.at( "expectedSchematicPdfTitle" ).get<std::string>(),
                        aError ) )
            {
                return false;
            }
        }
        else if( path.starts_with( "documentation/board_ps/" )
                 && extension == ".ps" )
        {
            kind = "board_ps";
            const std::string filename = iterator->path().filename().string();
            const auto expected = expectedBoardPsFiles.find( filename );

            if( expected == expectedBoardPsFiles.end()
                || !boardPsFiles.emplace( filename ).second
                || !KICHAD::BOARD_PS_ARTIFACT_VALIDATOR::ValidateFile(
                        iterator->path(), expected->second, aError ) )
            {
                if( aError.empty() )
                    aError = "board PostScript export has an unexpected filename";

                return false;
            }
        }
        else if( path.starts_with( "documentation/schematic_svg/" )
                 && extension == ".svg" )
        {
            kind = "schematic_svg";
            const std::string filename = iterator->path().filename().string();

            if( !schematicSvgFiles.emplace( filename ).second
                || !KICHAD::SCHEMATIC_SVG_ARTIFACT_VALIDATOR::ValidateFile(
                        iterator->path(), fileStem, aError ) )
            {
                if( aError.empty() )
                    aError = "schematic SVG export contains a duplicate filename";

                return false;
            }
        }
        else if( path.starts_with( "documentation/schematic_dxf/" )
                 && extension == ".dxf" )
        {
            kind = "schematic_dxf";
            const std::string filename = iterator->path().filename().string();

            if( !schematicDxfFiles.emplace( filename ).second
                || !KICHAD::SCHEMATIC_DXF_ARTIFACT_VALIDATOR::ValidateFile(
                        iterator->path(), fileStem, aError ) )
            {
                if( aError.empty() )
                    aError = "schematic DXF export contains a duplicate filename";

                return false;
            }
        }
        else if( path.starts_with( "documentation/schematic_ps/" )
                 && extension == ".ps" )
        {
            kind = "schematic_ps";
            const std::string filename = iterator->path().filename().string();

            if( !schematicPsFiles.emplace( filename ).second
                || !KICHAD::SCHEMATIC_PS_ARTIFACT_VALIDATOR::ValidateFile(
                        iterator->path(), fileStem, aError ) )
            {
                if( aError.empty() )
                    aError = "schematic PostScript export contains a duplicate filename";

                return false;
            }
        }
        else if( path.starts_with( "documentation/" ) && extension == ".pdf" )
        {
            kind = "pdf";

            if( !prefix.starts_with( "%PDF-" ) || suffix.find( "%%EOF" ) == std::string::npos )
            {
                aError = "documentation artifact failed PDF signature validation";
                return false;
            }
        }
        else if( path == "reports/" + fileStem + "-board-stats.json" )
        {
            kind = "board_stats";

            if( !KICHAD::FABRICATION_ARTIFACT_VALIDATOR::ValidateBoardStatistics(
                        iterator->path(), fileStem, aError ) )
            {
                return false;
            }
        }
        else
        {
            aError = "fabrication staging contains an unexpected artifact: " + path;
            return false;
        }

        std::string digest;

        if( !hashFabricationFile( iterator->path(), digest ) )
        {
            aError = "could not hash fabrication artifact " + path;
            return false;
        }

        ++counts[kind];
        aArtifacts.push_back( { { "kind", kind },
                                { "path", path },
                                { "bytes", bytes },
                                { "sha256", std::move( digest ) } } );
    }

    if( filesystemError )
    {
        aError = "could not enumerate the complete fabrication staging directory";
        return false;
    }

    if( actualGerberFiles != jobGerberFiles )
    {
        aError = "Gerber job file does not enumerate the exact generated layer files";
        return false;
    }

    std::set<std::string> expectedBomReferences;
    std::set<std::string> expectedPlacementReferences;

    for( const JSON& reference : aPlan.at( "expectedBomReferences" ) )
        expectedBomReferences.emplace( reference.get<std::string>() );

    for( const JSON& reference : aPlan.at( "expectedPlacementReferences" ) )
        expectedPlacementReferences.emplace( reference.get<std::string>() );

    if( bomReferences != expectedBomReferences
        || placementReferences != expectedPlacementReferences )
    {
        aError = "fabrication BOM or placement references differ from compiled KDS";
        return false;
    }

    std::sort( aArtifacts.begin(), aArtifacts.end(),
               []( const JSON& aLeft, const JSON& aRight )
               {
                   return aLeft.at( "path" ).get<std::string>()
                          < aRight.at( "path" ).get<std::string>();
               } );

    for( const JSON& job : aPlan.at( "jobs" ) )
    {
        const std::string kind = job.at( "kind" ).get<std::string>();

        if( kind == "gerbers"
            && ( counts["gerbers"] != job.at( "layers" ).size()
                 || counts["gerber_job"] != 1 ) )
        {
            aError = "Gerber export did not produce one layer file per layer and one job file";
        }
        else if( kind == "drill"
                 && ( counts["drill_file"] == 0 || counts["drill_map"] == 0
                      || counts["drill_report"] != 1 ) )
            aError = "drill export is missing Excellon, map, or report artifacts";
        else if( kind == "ipcd356" && counts["ipcd356"] != 1 )
            aError = "IPC-D-356 export did not produce exactly one electrical-test artifact";
        else if( kind == "netlist" && counts["netlist"] != 1 )
            aError = "netlist export did not produce exactly one connectivity artifact";
        else if( kind == "ipc2581" && counts["ipc2581"] != 1 )
            aError = "IPC-2581 export did not produce exactly one manufacturing artifact";
        else if( kind == "odbpp" && counts["odbpp"] != 1 )
            aError = "ODB++ export did not produce exactly one manufacturing archive";
        else if( kind == "pick_place" && counts["pick_place"] != 1 )
            aError = "position export did not produce exactly one CSV artifact";
        else if( kind == "bom" && counts["bom"] != 1 )
            aError = "BOM export did not produce exactly one CSV artifact";
        else if( kind == "schematic_bom" && counts["schematic_bom"] != 1 )
            aError = "schematic BOM export did not produce exactly one native CSV artifact";
        else if( kind == "legacy_bom_xml" && counts["legacy_bom_xml"] != 1 )
            aError = "legacy BOM export did not produce exactly one XML interchange artifact";
        else if( kind == "board_render" && counts["board_render"] != 1 )
            aError = "board render did not produce exactly one validated PNG image";
        else if( kind == "step" && counts["step"] != 1 )
            aError = "STEP export did not produce exactly one model artifact";
        else if( kind == "brep" && counts["brep"] != 1 )
            aError = "BREP export did not produce exactly one model artifact";
        else if( kind == "glb" && counts["glb"] != 1 )
            aError = "GLB export did not produce exactly one model artifact";
        else if( kind == "stl" && counts["stl"] != 1 )
            aError = "STL export did not produce exactly one model artifact";
        else if( kind == "xao" && counts["xao"] != 1 )
            aError = "XAO export did not produce exactly one model artifact";
        else if( kind == "stepz" && counts["stepz"] != 1 )
            aError = "STEPZ export did not produce exactly one model artifact";
        else if( kind == "u3d" && counts["u3d"] != 1 )
            aError = "U3D export did not produce exactly one model artifact";
        else if( kind == "3d_pdf" && counts["3d_pdf"] != 1 )
            aError = "3D PDF export did not produce exactly one interactive model artifact";
        else if( kind == "pdf" && counts["pdf"] != 1 )
            aError = "documentation export did not produce exactly one PDF artifact";
        else if( kind == "board_ps"
                 && ( counts["board_ps"] != expectedBoardPsFiles.size()
                      || boardPsFiles.size() != expectedBoardPsFiles.size() ) )
            aError = "board PostScript export did not produce exactly one drawing per layer";
        else if( kind == "schematic_pdf" && counts["schematic_pdf"] != 1 )
            aError = "schematic PDF export did not produce exactly one release drawing";
        else if( kind == "schematic_svg"
                 && ( counts["schematic_svg"] == 0
                      || !schematicSvgFiles.contains( fileStem + ".svg" ) ) )
            aError = "schematic SVG export is missing its root-sheet drawing";
        else if( kind == "schematic_dxf"
                 && ( counts["schematic_dxf"] == 0
                      || !schematicDxfFiles.contains( fileStem + ".dxf" ) ) )
            aError = "schematic DXF export is missing its root-sheet drawing";
        else if( kind == "schematic_ps"
                 && ( counts["schematic_ps"] == 0
                      || !schematicPsFiles.contains( fileStem + ".ps" ) ) )
            aError = "schematic PostScript export is missing its root-sheet drawing";
        else if( kind == "assembly_svg"
                 && ( counts["assembly_svg"] != 2
                      || assemblySvgFiles != expectedAssemblySvgFiles ) )
            aError = "assembly SVG export did not produce exact front and back drawings";
        else if( kind == "assembly_dxf"
                 && ( counts["assembly_dxf"] != 2
                      || assemblyDxfFiles != expectedAssemblyDxfFiles ) )
            aError = "assembly DXF export did not produce exact front and back drawings";
        else if( kind == "gencad" && counts["gencad"] != 1 )
            aError = "GenCAD export did not produce exactly one interchange artifact";
        else if( kind == "vrml" && counts["vrml"] != 1 )
            aError = "VRML export did not produce exactly one model artifact";
        else if( kind == "board_stats" && counts["board_stats"] != 1 )
            aError = "board statistics export did not produce exactly one JSON report";

        if( !aError.empty() )
            return false;
    }

    return true;
}


bool validateProjectLibraryTable( const std::string& aKind, const std::string& aSource,
                                  size_t& aRows, std::string& aError )
{
    if( aSource.empty() || aSource.size() > MAX_PROJECT_LIBRARY_TABLE_BYTES )
    {
        aError = "project library table must contain 1 byte to 1 MiB";
        return false;
    }

    LIBRARY_TABLE_PARSER parser;
    auto                 parsed = parser.ParseBuffer( aSource );

    if( !parsed.has_value() )
    {
        aError = "native library parser rejected the generated table: "
                 + parsed.error().description.ToStdString();
        return false;
    }

    const LIBRARY_TABLE_TYPE expected = aKind == "symbol" ? LIBRARY_TABLE_TYPE::SYMBOL
                                                           : LIBRARY_TABLE_TYPE::FOOTPRINT;

    if( parsed->type != expected || parsed->version != "7"
        || parsed->rows.size() > MAX_PROJECT_LIBRARY_ROWS )
    {
        aError = "generated project library table has the wrong type, version, or row count";
        return false;
    }

    std::set<std::string> nicknames;

    for( const LIBRARY_TABLE_ROW_IR& row : parsed->rows )
    {
        const bool safeUri = row.uri.starts_with( "${KIPRJMOD}/" )
                             && row.uri.find( "/../" ) == std::string::npos
                             && !row.uri.ends_with( "/.." );
        const bool suffix = aKind == "symbol" ? row.uri.ends_with( ".kicad_sym" )
                                               : row.uri.ends_with( ".pretty" );

        if( row.nickname.empty() || row.nickname.size() > 128
            || !nicknames.emplace( row.nickname ).second || row.type != "KiCad"
            || !safeUri || !suffix || !row.options.empty() || !row.description.empty()
            || row.disabled || row.hidden )
        {
            aError = "generated project library table contains an invalid native row";
            return false;
        }
    }

    aRows = parsed->rows.size();
    return true;
}


std::string expectedRootHead( const wxString& aExtension )
{
    if( aExtension == wxS( "kicad_sch" ) )
        return "kicad_sch";

    if( aExtension == wxS( "kicad_pcb" ) )
        return "kicad_pcb";

    if( aExtension == wxS( "kicad_sym" ) )
        return "kicad_symbol_lib";

    if( aExtension == wxS( "kicad_mod" ) )
        return "footprint";

    return {};
}


bool resolveProjectFile( const wxString& aProjectPath, const std::string& aRelativePath,
                         wxFileName& aResolved, std::string& aError )
{
    wxString   relative = wxString::FromUTF8( aRelativePath );
    wxFileName candidate( relative );

    if( aRelativePath.size() > 4096 || aRelativePath.find( '\0' ) != std::string::npos
        || relative.IsEmpty() || candidate.IsAbsolute() )
    {
        aError = "path must be project-relative";
        return false;
    }

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( root, true ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    candidate.MakeAbsolute( root.GetFullPath() );
    candidate.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !candidate.FileExists() )
    {
        aError = "file does not exist";
        return false;
    }

    if( !canonicalizeExisting( candidate ) )
    {
        aError = "path could not be resolved";
        return false;
    }

    wxString candidatePath = candidate.GetFullPath();
    wxString rootPath = root.GetPathWithSep();

#ifdef __WXMSW__
    candidatePath.MakeLower();
    rootPath.MakeLower();
#endif

    if( !candidatePath.StartsWith( rootPath ) )
    {
        aError = "path resolves outside the active project";
        return false;
    }

    if( !isInspectableExtension( candidate.GetExt() ) )
    {
        aError = "file type is not a supported KiCad s-expression format";
        return false;
    }

    aResolved = candidate;
    return true;
}


bool resolveProjectSidecar( const wxString& aProjectPath, const std::string& aRelativePath,
                            wxFileName& aResolved, std::string& aError )
{
    wxString   relative = wxString::FromUTF8( aRelativePath );
    wxFileName candidate( relative );

    if( aRelativePath.size() > 4096 || aRelativePath.find( '\0' ) != std::string::npos
        || relative.IsEmpty() || candidate.IsAbsolute()
        || candidate.GetExt() != wxS( "kicad_kds" ) )
    {
        aError = "path must be a project-relative .kicad_kds file";
        return false;
    }

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( root, true ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    candidate.MakeAbsolute( root.GetFullPath() );
    candidate.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    wxFileName parent = wxFileName::DirName( candidate.GetPath() );

    if( !canonicalizeExisting( parent, true ) )
    {
        aError = "sidecar parent directory does not exist";
        return false;
    }

    wxString parentPath = parent.GetPathWithSep();
    wxString rootPath = root.GetPathWithSep();

#ifdef __WXMSW__
    parentPath.MakeLower();
    rootPath.MakeLower();
#endif

    if( !parentPath.StartsWith( rootPath ) && parent.GetFullPath() != root.GetFullPath() )
    {
        aError = "path resolves outside the active project";
        return false;
    }

    aResolved.Assign( parent.GetFullPath(), candidate.GetFullName() );

    if( aResolved.FileExists() )
    {
        wxFileName canonicalTarget = aResolved;

        if( !canonicalizeExisting( canonicalTarget ) )
        {
            aError = "existing sidecar path could not be resolved";
            return false;
        }

        wxString targetPath = canonicalTarget.GetFullPath();

#ifdef __WXMSW__
        targetPath.MakeLower();
#endif

        if( !targetPath.StartsWith( rootPath ) )
        {
            aError = "existing sidecar resolves outside the active project";
            return false;
        }

        aResolved = canonicalTarget;
    }

    return true;
}


bool readDesignScriptSidecar( const wxFileName& aFile, std::string& aSource,
                              std::string& aError )
{
    wxFile file( aFile.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
    {
        aError = "could not open the KiChad Design Script sidecar";
        return false;
    }

    const wxFileOffset length = file.Length();

    if( length <= 0 || length > static_cast<wxFileOffset>( MAX_DESIGN_SCRIPT_BYTES ) )
    {
        aError = "KiChad Design Script sidecars must contain 1 byte to 16 MiB";
        return false;
    }

    aSource.assign( static_cast<size_t>( length ), '\0' );

    if( file.Read( aSource.data(), static_cast<size_t>( length ) ) != length )
    {
        aError = "could not read the complete KiChad Design Script sidecar";
        aSource.clear();
        return false;
    }

    return true;
}


bool installDesignScriptSidecarAtomically( const wxFileName& aFile,
                                            const std::string& aSource,
                                            std::string& aError )
{
    if( aSource.empty() || aSource.size() > MAX_DESIGN_SCRIPT_BYTES )
    {
        aError = "KiChad Design Script sidecars must contain 1 byte to 16 MiB";
        return false;
    }

    const wxString temporaryPath =
            aFile.GetFullPath() + wxS( ".tmp-" ) + KIID().AsString();
    wxFile temporary;

    if( !temporary.Create( temporaryPath, true )
        || temporary.Write( aSource.data(), aSource.size() ) != aSource.size()
        || !temporary.Flush() )
    {
        temporary.Close();
        wxRemoveFile( temporaryPath );
        aError = "could not durably write the KiChad Design Script sidecar";
        return false;
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, aFile.GetFullPath(), true ) )
    {
        wxRemoveFile( temporaryPath );
        aError = "could not atomically install the KiChad Design Script sidecar";
        return false;
    }

    std::string installed;

    if( !readDesignScriptSidecar( aFile, installed, aError ) || installed != aSource )
    {
        aError = "KiChad Design Script sidecar verification failed after installation";
        return false;
    }

    return true;
}


bool readJsonFile( const wxFileName& aPath, nlohmann::json& aDocument, std::string& aError )
{
    if( !aPath.FileExists() )
    {
        aDocument = nullptr;
        return true;
    }

    wxFile file( aPath.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
    {
        aError = "could not open KiChad managed-state data";
        return false;
    }

    const wxFileOffset length = file.Length();

    if( length <= 0 || length > static_cast<wxFileOffset>( MAX_DESIGN_STATE_BYTES ) )
    {
        aError = "KiChad managed-state data must contain 1 byte to 64 MiB";
        return false;
    }

    std::string text( static_cast<size_t>( length ), '\0' );

    if( file.Read( text.data(), text.size() ) != length )
    {
        aError = "could not read complete KiChad managed-state data";
        return false;
    }

    aDocument = nlohmann::json::parse( text, nullptr, false );

    if( aDocument.is_discarded() )
    {
        aError = "KiChad managed-state data is not valid JSON";
        return false;
    }

    return true;
}


bool writeJsonAtomically( const wxFileName& aPath, const nlohmann::json& aDocument,
                          std::string& aError )
{
    const std::string serialized = aDocument.dump( 2 ) + "\n";

    if( serialized.size() > MAX_DESIGN_STATE_BYTES )
    {
        aError = "KiChad managed-state data exceeds 64 MiB";
        return false;
    }

    const wxString temporaryPath =
            aPath.GetFullPath() + wxS( ".tmp-" ) + KIID().AsString();
    wxFile temporary;

    if( !temporary.Create( temporaryPath, true )
        || temporary.Write( serialized.data(), serialized.size() ) != serialized.size()
        || !temporary.Flush() )
    {
        temporary.Close();
        wxRemoveFile( temporaryPath );
        aError = "could not durably write KiChad managed-state data";
        return false;
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, aPath.GetFullPath(), true ) )
    {
        wxRemoveFile( temporaryPath );
        aError = "could not atomically install KiChad managed-state data";
        return false;
    }

    nlohmann::json installed;

    if( !readJsonFile( aPath, installed, aError ) || installed != aDocument )
    {
        aError = "KiChad managed-state verification failed after installation";
        return false;
    }

    return true;
}


bool mergeRecoveryJournal( const nlohmann::json& aJournal,
                           const KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::CONTEXT& aContext,
                           nlohmann::json& aPreviousState, std::string& aError )
{
    if( aJournal.is_null() )
        return true;

    if( !aJournal.is_object() || aJournal.value( "format", "" ) != "kichad-kds-apply-journal"
        || aJournal.value( "version", 0 ) != 1
        || aJournal.value( "sourcePath", "" ) != aContext.sourcePath
        || aJournal.value( "boardPath", "" ) != aContext.boardPath
        || aJournal.value( "projectName", "" ) != aContext.projectName
        || !aJournal.contains( "previousState" ) || !aJournal.contains( "preparedState" )
        || !aJournal["preparedState"].is_object()
        || !aJournal["preparedState"].contains( "managedPcbItems" )
        || !aJournal["preparedState"]["managedPcbItems"].is_array() )
    {
        aError = "KiChad apply journal is malformed or belongs to a different design";
        return false;
    }

    nlohmann::json merged = aJournal["preparedState"];
    std::map<std::string, nlohmann::json> items;

    for( const nlohmann::json& item : merged["managedPcbItems"] )
    {
        if( !item.is_object() || !item.contains( "itemId" ) || !item["itemId"].is_string()
            || !items.emplace( item["itemId"].get<std::string>(), item ).second )
        {
            aError = "KiChad apply journal contains invalid prepared ownership";
            return false;
        }
    }

    const nlohmann::json& previous = aJournal["previousState"];

    if( !previous.is_null() )
    {
        if( !previous.is_object() || !previous.contains( "managedPcbItems" )
            || !previous["managedPcbItems"].is_array() )
        {
            aError = "KiChad apply journal contains invalid previous ownership";
            return false;
        }

        for( const nlohmann::json& item : previous["managedPcbItems"] )
        {
            if( !item.is_object() || !item.contains( "itemId" ) || !item["itemId"].is_string() )
            {
                aError = "KiChad apply journal contains invalid previous ownership";
                return false;
            }

            const std::string itemId = item["itemId"].get<std::string>();
            auto              existing = items.find( itemId );

            if( existing != items.end() && existing->second != item )
            {
                nlohmann::json preparedIdentity = existing->second;
                nlohmann::json previousIdentity = item;
                preparedIdentity.erase( "footprintSourceSha256" );
                previousIdentity.erase( "footprintSourceSha256" );

                if( preparedIdentity != previousIdentity )
                {
                    aError = "KiChad apply journal contains conflicting ownership";
                    return false;
                }

                // The editor transaction outcome is unknown.  Retain ownership but clear its
                // source revision so the next apply must perform a verified replacement.
                existing->second = std::move( preparedIdentity );
                continue;
            }

            items.emplace( itemId, item );
        }
    }

    merged["sourcePath"] = aContext.sourcePath;
    merged["boardPath"] = aContext.boardPath;
    merged["projectName"] = aContext.projectName;
    merged["sourceSha256"] = aContext.sourceSha256;
    merged["managedPcbItems"] = nlohmann::json::array();

    for( const auto& [itemId, item] : items )
        merged["managedPcbItems"].push_back( item );

    std::map<std::string, nlohmann::json> schematicItems;
    const auto mergeSchematicItems = [&]( const nlohmann::json& aState ) -> bool
    {
        if( !aState.is_object() || !aState.contains( "managedSchematicItems" ) )
            return true;

        if( !aState["managedSchematicItems"].is_array() )
            return false;

        for( const nlohmann::json& item : aState["managedSchematicItems"] )
        {
            if( !item.is_object() || !item.contains( "file" ) || !item["file"].is_string()
                || !item.contains( "kind" ) || !item["kind"].is_string()
                || !item.contains( "logicalId" ) || !item["logicalId"].is_string()
                || !item.contains( "uuid" ) || !item["uuid"].is_string() )
            {
                return false;
            }

            const std::string key = item["file"].get<std::string>() + "\n"
                                    + item["uuid"].get<std::string>();
            auto existing = schematicItems.find( key );

            if( existing != schematicItems.end() && existing->second != item )
                return false;

            schematicItems.emplace( key, item );
        }

        return true;
    };

    if( !mergeSchematicItems( aJournal["preparedState"] )
        || !mergeSchematicItems( previous ) )
    {
        aError = "KiChad apply journal contains invalid schematic ownership";
        return false;
    }

    merged["managedSchematicItems"] = nlohmann::json::array();

    for( const auto& entry : schematicItems )
        merged["managedSchematicItems"].push_back( entry.second );

    aPreviousState = std::move( merged );
    return true;
}


kiapi::common::types::KiCadObjectType pcbObjectType( const std::string& aItemType )
{
    using namespace kiapi::common::types;

    if( aItemType == "footprint" )
        return KOT_PCB_FOOTPRINT;
    if( aItemType == "trace" )
        return KOT_PCB_TRACE;
    if( aItemType == "via" )
        return KOT_PCB_VIA;
    if( aItemType == "arc" )
        return KOT_PCB_ARC;
    if( aItemType == "zone" || aItemType == "rule_area" )
        return KOT_PCB_ZONE;
    if( aItemType == "shape" )
        return KOT_PCB_SHAPE;
    if( aItemType == "text" )
        return KOT_PCB_TEXT;
    if( aItemType == "textbox" )
        return KOT_PCB_TEXTBOX;
    if( aItemType == "table" )
        return KOT_PCB_TABLE;
    if( aItemType == "dimension" )
        return KOT_PCB_DIMENSION;
    if( aItemType == "reference_image" )
        return KOT_PCB_REFERENCE_IMAGE;
    if( aItemType == "barcode" )
        return KOT_PCB_BARCODE;

    return KOT_UNKNOWN;
}


std::unique_ptr<google::protobuf::Message> newPcbItem( const std::string& aItemType )
{
    using namespace kiapi::board::types;

    if( aItemType == "footprint" )
        return std::make_unique<FootprintInstance>();
    if( aItemType == "trace" )
        return std::make_unique<Track>();
    if( aItemType == "via" )
        return std::make_unique<Via>();
    if( aItemType == "arc" )
        // Qualified because <wingdi.h> declares a global Arc() function that
        // otherwise wins name lookup over the unqualified type.
        return std::make_unique<kiapi::board::types::Arc>();
    if( aItemType == "zone" || aItemType == "rule_area" )
        return std::make_unique<Zone>();
    if( aItemType == "shape" )
        return std::make_unique<BoardGraphicShape>();
    if( aItemType == "text" )
        return std::make_unique<BoardText>();
    if( aItemType == "textbox" )
        return std::make_unique<BoardTextBox>();
    if( aItemType == "table" )
        return std::make_unique<BoardTable>();
    if( aItemType == "dimension" )
        return std::make_unique<Dimension>();
    if( aItemType == "reference_image" )
        return std::make_unique<ReferenceImage>();
    if( aItemType == "barcode" )
        return std::make_unique<Barcode>();

    return {};
}


std::string protobufFieldType( const google::protobuf::FieldDescriptor& aField )
{
    using TYPE = google::protobuf::FieldDescriptor::Type;

    switch( aField.type() )
    {
    case TYPE::TYPE_DOUBLE:   return "double";
    case TYPE::TYPE_FLOAT:    return "float";
    case TYPE::TYPE_INT64:    return "int64";
    case TYPE::TYPE_UINT64:   return "uint64";
    case TYPE::TYPE_INT32:    return "int32";
    case TYPE::TYPE_FIXED64:  return "fixed64";
    case TYPE::TYPE_FIXED32:  return "fixed32";
    case TYPE::TYPE_BOOL:     return "bool";
    case TYPE::TYPE_STRING:   return "string";
    case TYPE::TYPE_GROUP:    return "group";
    case TYPE::TYPE_MESSAGE:  return "message";
    case TYPE::TYPE_BYTES:    return "bytes";
    case TYPE::TYPE_UINT32:   return "uint32";
    case TYPE::TYPE_ENUM:     return "enum";
    case TYPE::TYPE_SFIXED32: return "sfixed32";
    case TYPE::TYPE_SFIXED64: return "sfixed64";
    case TYPE::TYPE_SINT32:   return "sint32";
    case TYPE::TYPE_SINT64:   return "sint64";
    }

    return "unknown";
}


nlohmann::json describePcbMessage( const std::string& aItemType,
                                   const std::string& aMessagePath, std::string& aError )
{
    std::unique_ptr<google::protobuf::Message> message = newPcbItem( aItemType );

    if( !message )
    {
        aError = "unsupported PCB item type";
        return {};
    }

    const google::protobuf::Descriptor* descriptor = message->GetDescriptor();
    std::string                         normalizedPath;
    size_t                              begin = 0;
    size_t                              depth = 0;

    while( begin < aMessagePath.size() )
    {
        size_t end = aMessagePath.find( '.', begin );

        if( end == std::string::npos )
            end = aMessagePath.size();

        std::string segment = aMessagePath.substr( begin, end - begin );

        if( segment.empty() || ++depth > 8 )
        {
            aError = "pcb.messagePath must contain at most 8 non-empty field names";
            return {};
        }

        const google::protobuf::FieldDescriptor* selected = nullptr;

        for( int i = 0; i < descriptor->field_count(); ++i )
        {
            const google::protobuf::FieldDescriptor* field = descriptor->field( i );

            if( field->name() == segment || field->json_name() == segment )
            {
                selected = field;
                break;
            }
        }

        if( !selected || selected->cpp_type()
                                 != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE )
        {
            aError = "pcb.messagePath does not identify a message-typed field";
            return {};
        }

        if( !normalizedPath.empty() )
            normalizedPath += '.';

        normalizedPath += selected->json_name();
        descriptor = selected->message_type();
        begin = end + 1;
    }

    nlohmann::json fields = nlohmann::json::array();

    for( int i = 0; i < descriptor->field_count(); ++i )
    {
        const google::protobuf::FieldDescriptor* field = descriptor->field( i );
        nlohmann::json fieldDescription = {
            { "name", field->json_name() },
            { "protoName", field->name() },
            { "number", field->number() },
            { "type", protobufFieldType( *field ) },
            { "cardinality", field->is_repeated() ? "repeated" : "singular" }
        };

        if( field->containing_oneof() )
            fieldDescription["oneof"] = field->containing_oneof()->name();

        if( field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE )
        {
            fieldDescription["messageType"] = field->message_type()->full_name();
            fieldDescription["expandWith"] =
                    normalizedPath.empty()
                            ? std::string( field->json_name() )
                            : normalizedPath + '.' + std::string( field->json_name() );
        }
        else if( field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM )
        {
            nlohmann::json values = nlohmann::json::array();

            for( int valueIndex = 0; valueIndex < field->enum_type()->value_count(); ++valueIndex )
            {
                const google::protobuf::EnumValueDescriptor* value =
                        field->enum_type()->value( valueIndex );
                values.push_back( { { "name", value->name() }, { "number", value->number() } } );
            }

            fieldDescription["enumType"] = field->enum_type()->full_name();
            fieldDescription["values"] = std::move( values );
        }

        if( field->type() == google::protobuf::FieldDescriptor::TYPE_INT64
            || field->type() == google::protobuf::FieldDescriptor::TYPE_UINT64
            || field->type() == google::protobuf::FieldDescriptor::TYPE_SINT64
            || field->type() == google::protobuf::FieldDescriptor::TYPE_FIXED64
            || field->type() == google::protobuf::FieldDescriptor::TYPE_SFIXED64 )
        {
            fieldDescription["jsonEncoding"] = "decimal string";
        }

        fields.emplace_back( std::move( fieldDescription ) );
    }

    return { { "itemType", aItemType },
             { "messagePath", normalizedPath },
             { "messageType", descriptor->full_name() },
             { "fields", std::move( fields ) },
             { "units", "Coordinates and distances are nanometers" } };
}


bool parsePcbItem( const std::string& aItemType, const nlohmann::json& aJson,
                   std::unique_ptr<google::protobuf::Message>& aMessage, std::string& aError )
{
    if( !aJson.is_object() )
    {
        aError = "each PCB item must be a JSON object";
        return false;
    }

    aMessage = newPcbItem( aItemType );

    if( !aMessage )
    {
        aError = "unsupported PCB item type";
        return false;
    }

    google::protobuf::util::JsonParseOptions options;
    options.ignore_unknown_fields = false;
    KICHAD::PROTOBUF_STATUS status =
            google::protobuf::util::JsonStringToMessage( aJson.dump(), aMessage.get(), options );

    if( !status.ok() )
    {
        aError = status.ToString();
        return false;
    }

    return true;
}


std::string pcbItemId( const google::protobuf::Any& aItem, const std::string& aItemType )
{
    std::unique_ptr<google::protobuf::Message> message = newPcbItem( aItemType );

    if( !message || !aItem.UnpackTo( message.get() ) )
        return {};

    const google::protobuf::FieldDescriptor* idField =
            message->GetDescriptor()->FindFieldByName( "id" );

    if( !idField || idField->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE )
        return {};

    const google::protobuf::Message& idMessage =
            message->GetReflection()->GetMessage( *message, idField );
    const google::protobuf::FieldDescriptor* valueField =
            idMessage.GetDescriptor()->FindFieldByName( "value" );

    if( !valueField || valueField->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_STRING )
        return {};

    return idMessage.GetReflection()->GetString( idMessage, valueField );
}


std::string pcbAnyType( const google::protobuf::Any& aItem )
{
    using namespace kiapi::board::types;

    if( aItem.Is<BoardGraphicShape>() )
        return "shape";
    if( aItem.Is<Track>() )
        return "trace";
    if( aItem.Is<kiapi::board::types::Arc>() )  // qualified: see wingdi.h Arc()
        return "arc";
    if( aItem.Is<Via>() )
        return "via";
    if( aItem.Is<BoardText>() )
        return "text";
    if( aItem.Is<BoardTextBox>() )
        return "textbox";
    if( aItem.Is<BoardTable>() )
        return "table";
    if( aItem.Is<Dimension>() )
        return "dimension";
    if( aItem.Is<ReferenceImage>() )
        return "reference_image";
    if( aItem.Is<Barcode>() )
        return "barcode";
    if( aItem.Is<FootprintInstance>() )
        return "footprint";
    if( aItem.Is<Zone>() )
    {
        Zone zone;

        if( aItem.UnpackTo( &zone ) && zone.type() == ZT_RULE_AREA )
            return "rule_area";

        return "zone";
    }

    return "other:" + aItem.type_url();
}


std::string pcbAnyId( const google::protobuf::Any& aItem )
{
    std::string itemType = pcbAnyType( aItem );

    if( itemType.starts_with( "other:" ) )
    {
        size_t separator = aItem.type_url().rfind( '/' );
        std::string typeName = separator == std::string::npos
                                       ? aItem.type_url()
                                       : aItem.type_url().substr( separator + 1 );
        const google::protobuf::Descriptor* descriptor =
                google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName( typeName );

        if( !descriptor )
            return {};

        const google::protobuf::Message* prototype =
                google::protobuf::MessageFactory::generated_factory()->GetPrototype( descriptor );

        if( !prototype )
            return {};

        std::unique_ptr<google::protobuf::Message> message( prototype->New() );

        if( !aItem.UnpackTo( message.get() ) )
            return {};

        const google::protobuf::FieldDescriptor* idField = descriptor->FindFieldByName( "id" );

        if( !idField || idField->cpp_type()
                               != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE )
        {
            return {};
        }

        const google::protobuf::Message& idMessage =
                message->GetReflection()->GetMessage( *message, idField );
        const google::protobuf::FieldDescriptor* valueField =
                idMessage.GetDescriptor()->FindFieldByName( "value" );

        if( !valueField || valueField->cpp_type()
                                  != google::protobuf::FieldDescriptor::CPPTYPE_STRING )
        {
            return {};
        }

        return idMessage.GetReflection()->GetString( idMessage, valueField );
    }

    return pcbItemId( aItem, itemType );
}


bool queryPcbInventory( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                        const std::set<std::string>& aIds, nlohmann::json& aInventory,
                        std::string& aError )
{
    aInventory = nlohmann::json::array();

    if( aIds.empty() )
        return true;

    kiapi::common::commands::GetItemsById request;
    request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );

    for( const std::string& id : aIds )
        request.add_items()->set_value( id );

    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::common::commands::GetItemsResponse items;

    if( !response.message().UnpackTo( &items )
        || items.status() != kiapi::common::types::IRS_OK )
    {
        aError = "KiCad returned an invalid managed-item inventory";
        return false;
    }

    std::set<std::string> returnedIds;

    for( const google::protobuf::Any& item : items.items() )
    {
        std::string itemId = pcbAnyId( item );

        if( itemId.empty() || !aIds.contains( itemId ) || !returnedIds.emplace( itemId ).second )
        {
            aError = "KiCad returned an unexpected managed-item identity";
            return false;
        }

        aInventory.push_back( { { "itemId", itemId }, { "itemType", pcbAnyType( item ) } } );
    }

    return true;
}


bool queryPcbStackup( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                      kiapi::board::BoardStackup& aStackup, std::string& aError )
{
    kiapi::board::commands::GetBoardStackup request;
    request.mutable_board()->CopyFrom( aTarget.document );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::board::commands::BoardStackupResponse stackupResponse;

    if( !response.message().UnpackTo( &stackupResponse )
        || !stackupResponse.has_stackup() )
    {
        aError = "KiCad returned an invalid board stackup";
        return false;
    }

    aStackup.CopyFrom( stackupResponse.stackup() );
    return true;
}


bool queryPcbTitleBlock( const KICHAD_IPC_CLIENT& aClient,
                         const KICHAD_IPC_TARGET& aTarget,
                         kiapi::common::types::TitleBlockInfo& aTitleBlock,
                         std::string& aError )
{
    kiapi::common::commands::GetTitleBlockInfo request;
    request.mutable_document()->CopyFrom( aTarget.document );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError )
        || !response.message().UnpackTo( &aTitleBlock ) )
    {
        if( aError.empty() )
            aError = "KiCad returned an invalid board title block";

        return false;
    }

    return true;
}


bool updatePcbTitleBlock( const KICHAD_IPC_CLIENT& aClient,
                          const KICHAD_IPC_TARGET& aTarget,
                          const kiapi::common::types::TitleBlockInfo& aTitleBlock,
                          std::string& aError )
{
    kiapi::common::commands::SetTitleBlockInfo request;
    request.mutable_document()->CopyFrom( aTarget.document );
    request.mutable_title_block()->CopyFrom( aTitleBlock );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::common::types::TitleBlockInfo readback;

    if( !queryPcbTitleBlock( aClient, aTarget, readback, aError ) )
        return false;

    if( readback.SerializeAsString() != aTitleBlock.SerializeAsString() )
    {
        aError = "KiCad title-block readback does not match the requested KDS metadata";
        return false;
    }

    return true;
}


bool updatePcbStackup( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                       const kiapi::board::BoardStackup& aStackup, std::string& aError )
{
    kiapi::board::commands::UpdateBoardStackup request;
    request.mutable_board()->CopyFrom( aTarget.document );
    request.mutable_stackup()->CopyFrom( aStackup );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::board::commands::BoardStackupResponse stackupResponse;

    if( !response.message().UnpackTo( &stackupResponse )
        || !stackupResponse.has_stackup() )
    {
        aError = "KiCad returned an invalid updated board stackup";
        return false;
    }

    return true;
}


bool queryPcbRules( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                    kiapi::board::BoardDesignRules& aRules, std::string& aError )
{
    kiapi::board::commands::GetBoardDesignRules request;
    request.mutable_board()->CopyFrom( aTarget.document );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::board::commands::BoardDesignRulesResponse rulesResponse;

    if( !response.message().UnpackTo( &rulesResponse ) || !rulesResponse.has_rules() )
    {
        aError = "KiCad returned invalid board design rules";
        return false;
    }

    aRules.CopyFrom( rulesResponse.rules() );
    return true;
}


bool updatePcbRules( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                     const kiapi::board::BoardDesignRules& aRules, std::string& aError )
{
    kiapi::board::commands::UpdateBoardDesignRules request;
    request.mutable_board()->CopyFrom( aTarget.document );
    request.mutable_rules()->CopyFrom( aRules );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::board::commands::BoardDesignRulesResponse rulesResponse;

    if( !response.message().UnpackTo( &rulesResponse ) || !rulesResponse.has_rules() )
    {
        aError = "KiCad returned invalid updated board design rules";
        return false;
    }

    return true;
}


bool queryPcbCustomRules( const KICHAD_IPC_CLIENT& aClient,
                          const KICHAD_IPC_TARGET& aTarget, bool& aPresent,
                          std::string& aSource, std::string& aError )
{
    kiapi::board::commands::GetBoardCustomRules request;
    request.mutable_board()->CopyFrom( aTarget.document );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::board::commands::BoardCustomRulesResponse rulesResponse;

    if( !response.message().UnpackTo( &rulesResponse ) )
    {
        aError = "KiCad returned invalid board custom rules";
        return false;
    }

    aPresent = rulesResponse.present();
    aSource = rulesResponse.source();
    return true;
}


bool updatePcbCustomRules( const KICHAD_IPC_CLIENT& aClient,
                           const KICHAD_IPC_TARGET& aTarget, bool aPresent,
                           const std::string& aSource, std::string& aError )
{
    kiapi::board::commands::UpdateBoardCustomRules request;
    request.mutable_board()->CopyFrom( aTarget.document );
    request.set_present( aPresent );
    request.set_source( aSource );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::board::commands::BoardCustomRulesResponse rulesResponse;

    if( !response.message().UnpackTo( &rulesResponse )
        || rulesResponse.present() != aPresent || rulesResponse.source() != aSource )
    {
        aError = "KiCad returned invalid updated board custom rules";
        return false;
    }

    return true;
}


bool queryNetClassSettings(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        kiapi::common::project::NetClassSettings& aSettings, std::string& aError )
{
    kiapi::common::commands::GetNetClassSettings request;
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::common::commands::NetClassSettingsResponse settingsResponse;

    if( !response.message().UnpackTo( &settingsResponse )
        || !settingsResponse.has_settings() )
    {
        aError = "KiCad returned invalid netclass settings";
        return false;
    }

    aSettings.CopyFrom( settingsResponse.settings() );
    return true;
}


bool updateNetClassSettings(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const kiapi::common::project::NetClassSettings& aSettings, std::string& aError )
{
    kiapi::common::commands::UpdateNetClassSettings request;
    request.mutable_settings()->CopyFrom( aSettings );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::common::commands::NetClassSettingsResponse settingsResponse;

    if( !response.message().UnpackTo( &settingsResponse )
        || !settingsResponse.has_settings() )
    {
        aError = "KiCad returned invalid updated netclass settings";
        return false;
    }

    return true;
}


bool queryPcbFootprintInventory( const KICHAD_IPC_CLIENT& aClient,
                                 const KICHAD_IPC_TARGET& aTarget,
                                 const std::set<std::string>& aReferences,
                                 nlohmann::json& aInventory, std::string& aError )
{
    if( aReferences.empty() )
        return true;

    kiapi::common::commands::GetItems request;
    request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );
    request.add_types( kiapi::common::types::KOT_PCB_FOOTPRINT );

    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::common::commands::GetItemsResponse items;

    if( !response.message().UnpackTo( &items )
        || items.status() != kiapi::common::types::IRS_OK
        || items.items_size() > static_cast<int>( MAX_PCB_FOOTPRINTS ) )
    {
        aError = "KiCad returned an invalid or excessive footprint inventory";
        return false;
    }

    std::set<std::string> returnedIds;

    for( const google::protobuf::Any& packed : items.items() )
    {
        kiapi::board::types::FootprintInstance footprint;

        if( !packed.UnpackTo( &footprint ) )
        {
            aError = "KiCad returned a non-footprint in the footprint inventory";
            return false;
        }

        const std::string reference = footprint.reference_field().text().text().text();

        if( !aReferences.contains( reference ) )
            continue;

        const std::string itemId = footprint.id().value();
        const std::string libraryNickname =
                footprint.definition().id().library_nickname();
        const std::string entryName = footprint.definition().id().entry_name();

        const bool hasLibraryId = !libraryNickname.empty() && !entryName.empty();

        if( !KIID::SniffTest( wxString::FromUTF8( itemId ) )
            || ( hasLibraryId && ( libraryNickname.find( ':' ) != std::string::npos
                                   || entryName.find( ':' ) != std::string::npos ) )
            || !returnedIds.emplace( itemId ).second )
        {
            aError = "KiCad returned an invalid or duplicate footprint identity";
            return false;
        }

        nlohmann::json inventory = {
            { "itemId", itemId },
            { "itemType", "footprint" },
            { "reference", reference },
            { "schematicLinked", !footprint.attributes().not_in_schematic()
                                          && footprint.symbol_path().path_size() > 0 }
        };

        if( hasLibraryId )
            inventory["libraryId"] = libraryNickname + ":" + entryName;

        aInventory.push_back( std::move( inventory ) );
    }

    return true;
}


bool refillPcbZones( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                     const std::set<std::string>& aExpectedZoneIds, std::string& aError )
{
    kiapi::board::commands::RefillZones refill;
    refill.mutable_board()->CopyFrom( aTarget.document );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, refill, response, aError ) )
        return false;

    // RefillZones is accepted synchronously but KiCad performs the fill on the editor thread and
    // reports AS_BUSY in the interim. Poll the authoritative zone objects until every desired KDS
    // zone is present and filled. This also closes the race before the queued fill action starts.
    std::this_thread::sleep_for( std::chrono::milliseconds( 25 ) );
    const auto deadline = std::chrono::steady_clock::now() + MAX_ZONE_REFILL_WAIT;
    std::string lastError;

    do
    {
        kiapi::common::commands::GetItems query;
        query.mutable_header()->mutable_document()->CopyFrom( aTarget.document );
        query.add_types( kiapi::common::types::KOT_PCB_ZONE );
        kiapi::common::ApiResponse queryResponse;

        if( aClient.Call( aTarget, query, queryResponse, lastError ) )
        {
            kiapi::common::commands::GetItemsResponse items;

            if( !queryResponse.message().UnpackTo( &items )
                || items.status() != kiapi::common::types::IRS_OK
                || items.items_size() > static_cast<int>( MAX_PCB_ZONES ) )
            {
                aError = "KiCad returned an invalid or excessive zone inventory after refill";
                return false;
            }

            std::set<std::string> filled;

            for( const google::protobuf::Any& packed : items.items() )
            {
                kiapi::board::types::Zone zone;

                if( !packed.UnpackTo( &zone ) )
                {
                    aError = "KiCad returned a non-zone in the zone refill inventory";
                    return false;
                }

                if( aExpectedZoneIds.contains( zone.id().value() ) && zone.filled() )
                    filled.emplace( zone.id().value() );
            }

            if( filled.size() == aExpectedZoneIds.size() )
                return true;
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 25 ) );
    } while( std::chrono::steady_clock::now() < deadline );

    aError = "KiCad did not finish filling every managed KDS zone within 30 seconds";

    if( !lastError.empty() )
        aError += ": " + lastError;

    return false;
}


bool validateManagedAssetReadback( const google::protobuf::Any& aActive,
                                   const nlohmann::json& aAction,
                                   const char* aMutation, std::string& aError )
{
    const std::string itemType = aAction.at( "itemType" ).get<std::string>();

    if( itemType != "reference_image" && itemType != "barcode" )
        return true;

    std::unique_ptr<google::protobuf::Message> expected;
    std::unique_ptr<google::protobuf::Message> active = newPcbItem( itemType );

    if( !parsePcbItem( itemType, aAction.at( "item" ), expected, aError )
        || !active || !aActive.UnpackTo( active.get() )
        || !google::protobuf::util::MessageDifferencer::Equals( *expected, *active ) )
    {
        if( aError.empty() )
            aError = "KiCad " + itemType + " " + aMutation + " readback did not match KDS";

        return false;
    }

    return true;
}


bool validateCreateUpdateResponse( const google::protobuf::RepeatedPtrField<
                                           kiapi::common::commands::ItemCreationResult>& aResults,
                                   const std::vector<const nlohmann::json*>& aActions,
                                   std::string& aError )
{
    if( aResults.size() != static_cast<int>( aActions.size() ) )
    {
        aError = "KiCad returned an incomplete create-items response";
        return false;
    }

    for( int i = 0; i < aResults.size(); ++i )
    {
        if( aResults[i].status().code() != kiapi::common::commands::ISC_OK
            || pcbAnyId( aResults[i].item() ) != aActions[i]->at( "itemId" ).get<std::string>() )
        {
            aError = aResults[i].status().error_message();

            if( aError.empty() )
                aError = "KiCad rejected or changed a deterministic PCB item UUID";

            return false;
        }

        if( !validateManagedAssetReadback( aResults[i].item(), *aActions[i], "create", aError ) )
            return false;
    }

    return true;
}


bool validateCreateUpdateResponse( const google::protobuf::RepeatedPtrField<
                                           kiapi::common::commands::ItemUpdateResult>& aResults,
                                   const std::vector<const nlohmann::json*>& aActions,
                                   std::string& aError )
{
    if( aResults.size() != static_cast<int>( aActions.size() ) )
    {
        aError = "KiCad returned an incomplete update-items response";
        return false;
    }

    for( int i = 0; i < aResults.size(); ++i )
    {
        if( aResults[i].status().code() != kiapi::common::commands::ISC_OK
            || pcbAnyId( aResults[i].item() ) != aActions[i]->at( "itemId" ).get<std::string>() )
        {
            aError = aResults[i].status().error_message();

            if( aError.empty() )
                aError = "KiCad rejected or changed a deterministic PCB item UUID";

            return false;
        }

        if( !validateManagedAssetReadback( aResults[i].item(), *aActions[i], "update", aError ) )
            return false;
    }

    return true;
}


kiapi::board::types::BoardLayer footprintPresentationLayer( const std::string& aLayer );

void populateFootprintPresentationRequest(
        const nlohmann::json& aPresentation,
        kiapi::board::types::FootprintInstance& aRequested,
        google::protobuf::FieldMask& aFieldMask );

bool footprintFieldMatches( const kiapi::board::types::Field& aField,
                            const nlohmann::json& aPresentation );


bool footprintSourceInventory( const std::string& aSource,
                               std::multiset<std::string>& aPads,
                               std::multiset<std::string>& aModels,
                               std::string& aError )
{
    std::string parseError;
    std::unique_ptr<KICHAD::LOSSLESS_SEXPR_DOCUMENT> document =
            KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( aSource, &parseError );

    if( !document || document->Roots().size() != 1
        || document->ListHead( document->Roots().front() ) != "footprint" )
    {
        aError = "resolved footprint source is not a valid native footprint: " + parseError;
        return false;
    }

    const auto& nodes = document->Nodes();
    const auto& root = nodes.at( document->Roots().front() );

    for( size_t child : root.children )
    {
        const auto& node = nodes.at( child );
        const std::string head = document->ListHead( child );

        if( ( head != "pad" && head != "model" ) || node.children.size() < 2 )
            continue;

        const std::string value = document->AtomText( node.children[1] );

        if( head == "pad" )
            aPads.emplace( value );
        else
            aModels.emplace( value );
    }

    return true;
}


bool validateCreatedFootprint( const kiapi::board::types::FootprintInstance& aActive,
                               const kiapi::board::types::FootprintInstance& aRequested,
                               const nlohmann::json& aAction,
                               const std::string& aSource, std::string& aError )
{
    const nlohmann::json& instance = aAction.at( "instance" );

    const auto textMismatch = [&]( const char* aField, const std::string& aRequestedValue,
                                   const std::string& aActiveValue ) -> bool
    {
        if( aActiveValue == aRequestedValue )
            return false;

        // Keep tool failures useful without dumping an unbounded datasheet, description, or
        // user-defined value into the model context.
        constexpr size_t maxDiagnosticBytes = 160;
        const auto bounded = []( const std::string& aValue )
        {
            std::string value = aValue.substr( 0, maxDiagnosticBytes );

            if( aValue.size() > maxDiagnosticBytes )
                value += "...";

            return nlohmann::json( value ).dump();
        };
        aError = std::string( "KiCad footprint replacement readback mismatch for " ) + aField
                 + ": requested " + bounded( aRequestedValue ) + ", returned "
                 + bounded( aActiveValue );
        return true;
    };
    const auto integerMismatch = [&]( const char* aField, int64_t aRequestedValue,
                                      int64_t aActiveValue ) -> bool
    {
        if( aActiveValue == aRequestedValue )
            return false;

        aError = std::string( "KiCad footprint replacement readback mismatch for " ) + aField
                 + ": requested " + std::to_string( aRequestedValue ) + ", returned "
                 + std::to_string( aActiveValue );
        return true;
    };

    if( textMismatch( "id", aRequested.id().value(), aActive.id().value() )
        || textMismatch( "reference", aRequested.reference_field().text().text().text(),
                         aActive.reference_field().text().text().text() )
        || textMismatch( "value", aRequested.value_field().text().text().text(),
                         aActive.value_field().text().text().text() )
        || textMismatch( "datasheet", aRequested.datasheet_field().text().text().text(),
                         aActive.datasheet_field().text().text().text() )
        || textMismatch( "description", aRequested.description_field().text().text().text(),
                         aActive.description_field().text().text().text() )
        || textMismatch( "library_nickname",
                         aRequested.definition().id().library_nickname(),
                         aActive.definition().id().library_nickname() )
        || textMismatch( "library_entry", aRequested.definition().id().entry_name(),
                         aActive.definition().id().entry_name() ) )
    {
        return false;
    }

    if( aActive.attributes().not_in_schematic() )
    {
        aError = "KiCad footprint replacement readback mismatch for not_in_schematic: "
                 "requested false, returned true";
        return false;
    }

    if( aActive.attributes().do_not_populate()
        != aRequested.attributes().do_not_populate() )
    {
        aError = "KiCad footprint replacement readback mismatch for do_not_populate: requested "
                 + std::string( aRequested.attributes().do_not_populate() ? "true" : "false" )
                 + ", returned "
                 + std::string( aActive.attributes().do_not_populate() ? "true" : "false" );
        return false;
    }

    if( integerMismatch( "position.x_nm", aRequested.position().x_nm(),
                         aActive.position().x_nm() )
        || integerMismatch( "position.y_nm", aRequested.position().y_nm(),
                            aActive.position().y_nm() ) )
    {
        return false;
    }

    const double orientationDelta = std::remainder(
            aActive.orientation().value_degrees()
                    - aRequested.orientation().value_degrees(),
            360.0 );

    if( std::abs( orientationDelta ) > 1e-9 )
    {
        aError = "KiCad footprint replacement readback mismatch for orientation_degrees: "
                 "requested "
                 + std::to_string( aRequested.orientation().value_degrees() ) + ", returned "
                 + std::to_string( aActive.orientation().value_degrees() );
        return false;
    }

    if( integerMismatch( "layer", static_cast<int64_t>( aRequested.layer() ),
                         static_cast<int64_t>( aActive.layer() ) )
        || integerMismatch( "locked", static_cast<int64_t>( aRequested.locked() ),
                            static_cast<int64_t>( aActive.locked() ) )
        || integerMismatch( "symbol_path.count", aRequested.symbol_path().path_size(),
                            aActive.symbol_path().path_size() ) )
    {
        return false;
    }

    if( textMismatch( "symbol_sheet_name", aRequested.symbol_sheet_name(),
                      aActive.symbol_sheet_name() )
        || textMismatch( "symbol_sheet_filename", aRequested.symbol_sheet_filename(),
                         aActive.symbol_sheet_filename() ) )
    {
        return false;
    }

    for( int i = 0; i < aRequested.symbol_path().path_size(); ++i )
    {
        if( aActive.symbol_path().path( i ).value()
            != aRequested.symbol_path().path( i ).value() )
        {
            aError = "KiCad footprint replacement readback mismatch for symbol_path["
                     + std::to_string( i ) + "]: requested "
                     + nlohmann::json( aRequested.symbol_path().path( i ).value() ).dump()
                     + ", returned "
                     + nlohmann::json( aActive.symbol_path().path( i ).value() ).dump();
            return false;
        }
    }

    std::multiset<std::string> expectedPads;
    std::multiset<std::string> expectedModels;

    if( !footprintSourceInventory( aSource, expectedPads, expectedModels, aError ) )
        return false;

    std::multiset<std::string> activePads;
    std::multiset<std::string> activeModels;
    std::map<std::string, std::string> activeFields;
    std::set<std::string> assignedPads;

    for( const google::protobuf::Any& packed : aActive.definition().items() )
    {
        kiapi::board::types::Pad pad;
        kiapi::board::types::Footprint3DModel model;
        kiapi::board::types::Field field;

        if( packed.UnpackTo( &pad ) )
        {
            activePads.emplace( pad.number() );
            auto expectedNet = instance.at( "padNets" ).find( pad.number() );

            if( expectedNet != instance.at( "padNets" ).end() )
            {
                if( pad.net().name() != expectedNet.value().get<std::string>() )
                {
                    aError = "KiCad footprint pad-net readback did not match KDS for pad "
                             + pad.number();
                    return false;
                }

                assignedPads.emplace( pad.number() );
            }
        }
        else if( packed.UnpackTo( &model ) )
        {
            activeModels.emplace( model.filename() );
        }
        else if( packed.UnpackTo( &field ) )
        {
            if( !activeFields.emplace( field.name(), field.text().text().text() ).second )
            {
                aError = "KiCad footprint readback contains duplicate custom fields";
                return false;
            }
        }
    }

    if( activePads != expectedPads )
    {
        aError = "KiCad footprint pad inventory did not match the parsed replacement source";
        return false;
    }

    if( activeModels != expectedModels )
    {
        aError = "KiCad footprint 3D-model inventory did not match the replacement source";
        return false;
    }

    if( assignedPads.size() != instance.at( "padNets" ).size() )
    {
        aError = "KiCad footprint replacement did not materialize every assigned KDS pad net";
        return false;
    }

    static const std::set<std::string> mandatory = {
        "Reference", "Value", "Footprint", "Datasheet", "Description"
    };

    for( auto field = instance.at( "fields" ).begin();
         field != instance.at( "fields" ).end(); ++field )
    {
        if( mandatory.contains( field.key() ) )
            continue;

        auto active = activeFields.find( field.key() );

        if( active == activeFields.end() || active->second != field.value().get<std::string>() )
        {
            aError = "KiCad footprint custom-field readback did not match KDS field "
                     + field.key();
            return false;
        }
    }

    if( aAction.contains( "presentation" )
        && aAction.at( "presentation" ).contains( "reference" )
        && !footprintFieldMatches( aActive.reference_field(),
                                   aAction.at( "presentation" ).at( "reference" ) ) )
    {
        aError = "KiCad footprint reference presentation readback did not match KDS";
        return false;
    }

    if( aAction.contains( "presentation" )
        && aAction.at( "presentation" ).contains( "value" )
        && !footprintFieldMatches( aActive.value_field(),
                                   aAction.at( "presentation" ).at( "value" ) ) )
    {
        aError = "KiCad footprint value presentation readback did not match KDS";
        return false;
    }

    return true;
}


bool createFootprintFromSource( const KICHAD_IPC_CLIENT& aClient,
                                const KICHAD_IPC_TARGET& aTarget,
                                const nlohmann::json& aAction,
                                const nlohmann::json& aFootprintSources,
                                std::string& aError )
{
    const nlohmann::json& instance = aAction.at( "instance" );
    const std::string libraryId = instance.at( "libraryId" ).get<std::string>();

    if( !aFootprintSources.is_object() || !aFootprintSources.contains( libraryId )
        || !aFootprintSources[libraryId].is_string() )
    {
        aError = "placed footprint " + libraryId
                 + " is not available from a resolved project or installed stock library";
        return false;
    }

    const size_t separator = libraryId.find( ':' );
    kiapi::board::types::FootprintInstance requested;
    requested.mutable_id()->set_value( aAction.at( "itemId" ).get<std::string>() );
    requested.mutable_definition()->mutable_id()->set_library_nickname(
            libraryId.substr( 0, separator ) );
    requested.mutable_definition()->mutable_id()->set_entry_name(
            libraryId.substr( separator + 1 ) );
    requested.mutable_reference_field()->mutable_text()->mutable_text()->set_text(
            aAction.at( "component" ).get<std::string>() );
    requested.mutable_value_field()->mutable_text()->mutable_text()->set_text(
            instance.at( "value" ).get<std::string>() );
    const nlohmann::json& fields = instance.at( "fields" );
    requested.mutable_datasheet_field()->mutable_text()->mutable_text()->set_text(
            fields.value( "Datasheet", "" ) );
    requested.mutable_description_field()->mutable_text()->mutable_text()->set_text(
            fields.value( "Description", "" ) );
    requested.mutable_attributes()->set_not_in_schematic( false );
    requested.mutable_attributes()->set_do_not_populate( instance.at( "dnp" ).get<bool>() );

    for( const nlohmann::json& id : instance.at( "symbolPath" ) )
    {
        requested.mutable_symbol_path()->add_path()->set_value( id.get<std::string>() );
    }

    requested.set_symbol_sheet_name( instance.at( "symbolSheetName" ).get<std::string>() );
    requested.set_symbol_sheet_filename(
            instance.at( "symbolSheetFilename" ).get<std::string>() );
    requested.mutable_position()->set_x_nm(
            aAction.at( "position" ).at( "xNm" ).get<int64_t>() );
    requested.mutable_position()->set_y_nm(
            aAction.at( "position" ).at( "yNm" ).get<int64_t>() );
    requested.mutable_orientation()->set_value_degrees(
            aAction.at( "rotationDegrees" ).get<double>() );
    requested.set_layer( aAction.at( "side" ) == "front"
                                 ? kiapi::board::types::BL_F_Cu
                                 : kiapi::board::types::BL_B_Cu );
    requested.set_locked( aAction.at( "locked" ).get<bool>()
                                  ? kiapi::common::types::LS_LOCKED
                                  : kiapi::common::types::LS_UNLOCKED );
    google::protobuf::FieldMask presentationMask;

    if( aAction.contains( "presentation" )
        && !aAction.at( "presentation" ).empty() )
    {
        populateFootprintPresentationRequest( aAction.at( "presentation" ), requested,
                                              presentationMask );
    }

    for( auto net = instance.at( "padNets" ).begin();
         net != instance.at( "padNets" ).end(); ++net )
    {
        kiapi::board::types::Pad pad;
        pad.set_number( net.key() );
        pad.mutable_net()->set_name( net.value().get<std::string>() );
        requested.mutable_definition()->add_items()->PackFrom( pad );
    }

    static const std::set<std::string> mandatory = {
        "Reference", "Value", "Footprint", "Datasheet", "Description"
    };

    for( auto field = fields.begin(); field != fields.end(); ++field )
    {
        if( mandatory.contains( field.key() ) )
            continue;

        kiapi::board::types::Field requestedField;
        requestedField.set_name( field.key() );
        requestedField.mutable_text()->mutable_text()->set_text(
                field.value().get<std::string>() );
        requestedField.set_visible( false );
        requested.mutable_definition()->add_items()->PackFrom( requestedField );
    }

    kiapi::common::commands::ParseAndCreateItemsFromString parse;
    parse.mutable_document()->CopyFrom( aTarget.document );
    parse.set_contents( aFootprintSources[libraryId].get<std::string>() );
    parse.mutable_item()->PackFrom( requested );
    parse.mutable_field_mask()->CopyFrom( presentationMask );

    if( aAction.at( "action" ) == "replace_footprint" )
    {
        parse.mutable_replace_item_id()->set_value(
                aAction.at( "replacedItemId" ).get<std::string>() );
    }

    kiapi::common::ApiResponse parseResponse;

    if( !aClient.Call( aTarget, parse, parseResponse, aError ) )
        return false;

    kiapi::common::commands::CreateItemsResponse created;

    if( !parseResponse.message().UnpackTo( &created )
        || created.status() != kiapi::common::types::IRS_OK
        || created.created_items_size() != 1
        || created.created_items( 0 ).status().code() != kiapi::common::commands::ISC_OK )
    {
        aError = created.created_items_size() == 1
                         ? created.created_items( 0 ).status().error_message()
                         : "KiCad could not create exactly one footprint from " + libraryId;
        return false;
    }

    kiapi::board::types::FootprintInstance footprint;

    if( !created.created_items( 0 ).item().UnpackTo( &footprint ) )
    {
        aError = "KiCad returned an unreadable parsed footprint instance for component "
                 + aAction.at( "component" ).get<std::string>();

        return false;
    }

    if( !validateCreatedFootprint( footprint, requested, aAction,
                                   aFootprintSources[libraryId].get<std::string>(), aError ) )
    {
        aError = "component " + aAction.at( "component" ).get<std::string>() + ": "
                 + ( aError.empty() ? "KiCad returned a mismatched parsed footprint instance"
                                    : aError );

        return false;
    }

    return true;
}


bool updateFootprintMetadata( const KICHAD_IPC_CLIENT& aClient,
                              const KICHAD_IPC_TARGET& aTarget,
                              const nlohmann::json& aAction, std::string& aError )
{
    using namespace kiapi::board::types;
    const nlohmann::json& instance = aAction.at( "instance" );
    const nlohmann::json& fields = instance.at( "fields" );
    static const std::set<std::string> mandatory = {
        "Reference", "Value", "Footprint", "Datasheet", "Description"
    };
    FootprintInstance requested;
    requested.mutable_id()->set_value( aAction.at( "itemId" ).get<std::string>() );
    requested.mutable_value_field()->mutable_text()->mutable_text()->set_text(
            instance.at( "value" ).get<std::string>() );
    requested.mutable_datasheet_field()->mutable_text()->mutable_text()->set_text(
            fields.value( "Datasheet", "" ) );
    requested.mutable_description_field()->mutable_text()->mutable_text()->set_text(
            fields.value( "Description", "" ) );
    requested.mutable_attributes()->set_do_not_populate( instance.at( "dnp" ).get<bool>() );
    // definition.items is replacement semantics.  Preserve the message presence even when KDS
    // declares no custom fields so the native handler can distinguish "clear the list" from a
    // malformed request that omitted the replacement value.
    requested.mutable_definition();
    std::map<std::string, std::string> expectedFields;

    for( auto field = fields.begin(); field != fields.end(); ++field )
    {
        if( mandatory.contains( field.key() ) )
            continue;

        Field requestedField;
        requestedField.set_name( field.key() );
        requestedField.mutable_text()->mutable_text()->set_text(
                field.value().get<std::string>() );
        requested.mutable_definition()->add_items()->PackFrom( requestedField );
        expectedFields.emplace( field.key(), field.value().get<std::string>() );
    }

    kiapi::common::commands::UpdateItems request;
    request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );

    for( const char* path : { "value_field", "datasheet_field", "description_field",
                              "attributes.do_not_populate", "definition.items" } )
    {
        request.mutable_header()->mutable_field_mask()->add_paths( path );
    }

    request.add_items()->PackFrom( requested );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::common::commands::UpdateItemsResponse updated;

    if( !response.message().UnpackTo( &updated )
        || updated.status() != kiapi::common::types::IRS_OK
        || updated.updated_items_size() != 1
        || updated.updated_items( 0 ).status().code()
                   != kiapi::common::commands::ISC_OK )
    {
        if( updated.updated_items_size() == 1 )
            aError = updated.updated_items( 0 ).status().error_message();

        if( aError.empty() )
            aError = "KiCad rejected the managed footprint metadata replacement";

        return false;
    }

    FootprintInstance active;

    if( !updated.updated_items( 0 ).item().UnpackTo( &active )
        || active.id().value() != requested.id().value()
        || active.value_field().text().text().text()
                   != requested.value_field().text().text().text()
        || active.datasheet_field().text().text().text()
                   != requested.datasheet_field().text().text().text()
        || active.description_field().text().text().text()
                   != requested.description_field().text().text().text()
        || active.attributes().do_not_populate()
                   != requested.attributes().do_not_populate() )
    {
        aError = "KiCad footprint metadata readback did not match KDS";
        return false;
    }

    std::map<std::string, std::string> activeFields;

    for( const google::protobuf::Any& packed : active.definition().items() )
    {
        Field field;

        if( !packed.UnpackTo( &field ) )
            continue;

        if( !activeFields.emplace( field.name(), field.text().text().text() ).second )
        {
            aError = "KiCad footprint metadata readback contains duplicate fields";
            return false;
        }
    }

    if( activeFields != expectedFields )
    {
        aError = "KiCad footprint custom-field readback did not match KDS";
        return false;
    }

    return true;
}


kiapi::board::types::BoardLayer footprintPresentationLayer( const std::string& aLayer )
{
    using kiapi::board::types::BoardLayer;

    if( aLayer == "F.SilkS" )
        return BoardLayer::BL_F_SilkS;

    if( aLayer == "B.SilkS" )
        return BoardLayer::BL_B_SilkS;

    if( aLayer == "F.Fab" )
        return BoardLayer::BL_F_Fab;

    return BoardLayer::BL_B_Fab;
}


void populateFootprintPresentationRequest(
        const nlohmann::json& aPresentation,
        kiapi::board::types::FootprintInstance& aRequested,
        google::protobuf::FieldMask& aFieldMask )
{
    using namespace kiapi;
    const auto populate = [&]( const char* aName, board::types::Field* aField )
    {
        const nlohmann::json& source = aPresentation.at( aName );
        const std::string prefix = aName == std::string( "reference" )
                                           ? "reference_field"
                                           : "value_field";
        common::types::TextAttributes* attributes =
                aField->mutable_text()->mutable_text()->mutable_attributes();

        if( source.contains( "visible" ) )
        {
            aField->set_visible( source.at( "visible" ).get<bool>() );
            aFieldMask.add_paths( prefix + ".visible" );
        }

        if( source.contains( "layer" ) )
        {
            aField->mutable_text()->set_layer( footprintPresentationLayer(
                    source.at( "layer" ).get<std::string>() ) );
            aFieldMask.add_paths( prefix + ".text.layer" );
        }

        if( source.contains( "position" ) )
        {
            common::types::Vector2* position =
                    aField->mutable_text()->mutable_text()->mutable_position();
            position->set_x_nm( source.at( "position" ).at( "xNm" ).get<int64_t>() );
            position->set_y_nm( source.at( "position" ).at( "yNm" ).get<int64_t>() );
            aFieldMask.add_paths( prefix + ".text.text.position" );
        }

        if( source.contains( "size" ) )
        {
            attributes->mutable_size()->set_x_nm(
                    source.at( "size" ).at( "xNm" ).get<int64_t>() );
            attributes->mutable_size()->set_y_nm(
                    source.at( "size" ).at( "yNm" ).get<int64_t>() );
            aFieldMask.add_paths( prefix + ".text.text.attributes.size" );
        }

        if( source.contains( "strokeNm" ) )
        {
            attributes->mutable_stroke_width()->set_value_nm(
                    source.at( "strokeNm" ).get<int64_t>() );
            aFieldMask.add_paths( prefix + ".text.text.attributes.stroke_width" );
        }

        if( source.contains( "angleDegrees" ) )
        {
            attributes->mutable_angle()->set_value_degrees(
                    source.at( "angleDegrees" ).get<double>() );
            aFieldMask.add_paths( prefix + ".text.text.attributes.angle" );
        }

        if( source.contains( "horizontalJustification" ) )
        {
            const std::string value = source.at( "horizontalJustification" )
                                              .get<std::string>();
            attributes->set_horizontal_alignment(
                    value == "left" ? common::types::HA_LEFT
                    : value == "right" ? common::types::HA_RIGHT
                                         : common::types::HA_CENTER );
            aFieldMask.add_paths(
                    prefix + ".text.text.attributes.horizontal_alignment" );
        }

        if( source.contains( "verticalJustification" ) )
        {
            const std::string value = source.at( "verticalJustification" )
                                              .get<std::string>();
            attributes->set_vertical_alignment(
                    value == "top" ? common::types::VA_TOP
                    : value == "bottom" ? common::types::VA_BOTTOM
                                          : common::types::VA_CENTER );
            aFieldMask.add_paths(
                    prefix + ".text.text.attributes.vertical_alignment" );
        }

        if( source.contains( "fontName" ) )
        {
            attributes->set_font_name( source.at( "fontName" ).get<std::string>() );
            aFieldMask.add_paths( prefix + ".text.text.attributes.font_name" );
        }

        for( const auto& [sourceName, protoName] :
             { std::pair{ "bold", "bold" }, std::pair{ "italic", "italic" },
               std::pair{ "underlined", "underlined" },
               std::pair{ "mirrored", "mirrored" },
               std::pair{ "keepUpright", "keep_upright" } } )
        {
            if( !source.contains( sourceName ) )
                continue;

            const bool value = source.at( sourceName ).get<bool>();

            if( sourceName == std::string( "bold" ) )
                attributes->set_bold( value );
            else if( sourceName == std::string( "italic" ) )
                attributes->set_italic( value );
            else if( sourceName == std::string( "underlined" ) )
                attributes->set_underlined( value );
            else if( sourceName == std::string( "mirrored" ) )
                attributes->set_mirrored( value );
            else
                attributes->set_keep_upright( value );

            aFieldMask.add_paths( prefix + ".text.text.attributes." + protoName );
        }
    };

    if( aPresentation.contains( "reference" ) )
        populate( "reference", aRequested.mutable_reference_field() );

    if( aPresentation.contains( "value" ) )
        populate( "value", aRequested.mutable_value_field() );
}


bool footprintFieldMatches( const kiapi::board::types::Field& aField,
                            const nlohmann::json& aPresentation )
{
    using namespace kiapi::common::types;
    const TextAttributes& attributes = aField.text().text().attributes();

    if( aPresentation.contains( "visible" )
        && aField.visible() != aPresentation.at( "visible" ).get<bool>() )
    {
        return false;
    }

    if( aPresentation.contains( "layer" )
        && aField.text().layer()
                   != footprintPresentationLayer(
                           aPresentation.at( "layer" ).get<std::string>() ) )
    {
        return false;
    }

    if( aPresentation.contains( "position" )
        && ( aField.text().text().position().x_nm()
                     != aPresentation.at( "position" ).at( "xNm" ).get<int64_t>()
             || aField.text().text().position().y_nm()
                        != aPresentation.at( "position" ).at( "yNm" ).get<int64_t>() ) )
    {
        return false;
    }

    if( aPresentation.contains( "size" )
        && ( attributes.size().x_nm()
                     != aPresentation.at( "size" ).at( "xNm" ).get<int64_t>()
             || attributes.size().y_nm()
                        != aPresentation.at( "size" ).at( "yNm" ).get<int64_t>() ) )
    {
        return false;
    }

    if( aPresentation.contains( "strokeNm" )
        && attributes.stroke_width().value_nm()
                   != aPresentation.at( "strokeNm" ).get<int64_t>() )
    {
        return false;
    }

    if( aPresentation.contains( "angleDegrees" )
        && std::abs( std::remainder(
                            attributes.angle().value_degrees()
                                    - aPresentation.at( "angleDegrees" ).get<double>(),
                            360.0 ) ) > 1e-9 )
    {
        return false;
    }

    if( aPresentation.contains( "horizontalJustification" ) )
    {
        const std::string value = aPresentation.at( "horizontalJustification" )
                                          .get<std::string>();
        const HorizontalAlignment expected =
                value == "left" ? HorizontalAlignment::HA_LEFT
                : value == "right" ? HorizontalAlignment::HA_RIGHT
                                     : HorizontalAlignment::HA_CENTER;

        if( attributes.horizontal_alignment() != expected )
            return false;
    }

    if( aPresentation.contains( "verticalJustification" ) )
    {
        const std::string value = aPresentation.at( "verticalJustification" )
                                          .get<std::string>();
        const VerticalAlignment expected =
                value == "top" ? VerticalAlignment::VA_TOP
                : value == "bottom" ? VerticalAlignment::VA_BOTTOM
                                      : VerticalAlignment::VA_CENTER;

        if( attributes.vertical_alignment() != expected )
            return false;
    }

    for( const auto& [name, active] :
         { std::pair{ "bold", attributes.bold() },
           std::pair{ "italic", attributes.italic() },
           std::pair{ "underlined", attributes.underlined() },
           std::pair{ "mirrored", attributes.mirrored() },
           std::pair{ "keepUpright", attributes.keep_upright() } } )
    {
        if( aPresentation.contains( name )
            && active != aPresentation.at( name ).get<bool>() )
        {
            return false;
        }
    }

    return !aPresentation.contains( "fontName" )
           || attributes.font_name()
                      == aPresentation.at( "fontName" ).get<std::string>();
}


bool updateFootprintPresentation( const KICHAD_IPC_CLIENT& aClient,
                                  const KICHAD_IPC_TARGET& aTarget,
                                  const nlohmann::json& aAction, std::string& aError )
{
    using namespace kiapi;
    const nlohmann::json& presentation = aAction.at( "presentation" );
    board::types::FootprintInstance requested;
    requested.mutable_id()->set_value( aAction.at( "itemId" ).get<std::string>() );
    common::commands::UpdateItems request;
    request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );

    const auto populate = [&]( const char* aName, board::types::Field* aField )
    {
        const nlohmann::json& source = presentation.at( aName );
        const std::string prefix = std::string( aName == std::string( "reference" )
                                                       ? "reference_field"
                                                       : "value_field" );
        common::types::TextAttributes* attributes =
                aField->mutable_text()->mutable_text()->mutable_attributes();

        if( source.contains( "visible" ) )
        {
            aField->set_visible( source.at( "visible" ).get<bool>() );
            request.mutable_header()->mutable_field_mask()->add_paths( prefix + ".visible" );
        }

        if( source.contains( "layer" ) )
        {
            aField->mutable_text()->set_layer( footprintPresentationLayer(
                    source.at( "layer" ).get<std::string>() ) );
            request.mutable_header()->mutable_field_mask()->add_paths( prefix + ".text.layer" );
        }

        if( source.contains( "position" ) )
        {
            common::types::Vector2* position =
                    aField->mutable_text()->mutable_text()->mutable_position();
            position->set_x_nm( source.at( "position" ).at( "xNm" ).get<int64_t>() );
            position->set_y_nm( source.at( "position" ).at( "yNm" ).get<int64_t>() );
            request.mutable_header()->mutable_field_mask()->add_paths(
                    prefix + ".text.text.position" );
        }

        if( source.contains( "size" ) )
        {
            attributes->mutable_size()->set_x_nm(
                    source.at( "size" ).at( "xNm" ).get<int64_t>() );
            attributes->mutable_size()->set_y_nm(
                    source.at( "size" ).at( "yNm" ).get<int64_t>() );
            request.mutable_header()->mutable_field_mask()->add_paths(
                    prefix + ".text.text.attributes.size" );
        }

        if( source.contains( "strokeNm" ) )
        {
            attributes->mutable_stroke_width()->set_value_nm(
                    source.at( "strokeNm" ).get<int64_t>() );
            request.mutable_header()->mutable_field_mask()->add_paths(
                    prefix + ".text.text.attributes.stroke_width" );
        }

        if( source.contains( "angleDegrees" ) )
        {
            attributes->mutable_angle()->set_value_degrees(
                    source.at( "angleDegrees" ).get<double>() );
            request.mutable_header()->mutable_field_mask()->add_paths(
                    prefix + ".text.text.attributes.angle" );
        }

        if( source.contains( "horizontalJustification" ) )
        {
            const std::string value = source.at( "horizontalJustification" )
                                              .get<std::string>();
            attributes->set_horizontal_alignment(
                    value == "left" ? common::types::HA_LEFT
                    : value == "right" ? common::types::HA_RIGHT
                                         : common::types::HA_CENTER );
            request.mutable_header()->mutable_field_mask()->add_paths(
                    prefix + ".text.text.attributes.horizontal_alignment" );
        }

        if( source.contains( "verticalJustification" ) )
        {
            const std::string value = source.at( "verticalJustification" )
                                              .get<std::string>();
            attributes->set_vertical_alignment(
                    value == "top" ? common::types::VA_TOP
                    : value == "bottom" ? common::types::VA_BOTTOM
                                          : common::types::VA_CENTER );
            request.mutable_header()->mutable_field_mask()->add_paths(
                    prefix + ".text.text.attributes.vertical_alignment" );
        }

        if( source.contains( "fontName" ) )
        {
            attributes->set_font_name( source.at( "fontName" ).get<std::string>() );
            request.mutable_header()->mutable_field_mask()->add_paths(
                    prefix + ".text.text.attributes.font_name" );
        }

        for( const auto& [sourceName, protoName] :
             { std::pair{ "bold", "bold" }, std::pair{ "italic", "italic" },
               std::pair{ "underlined", "underlined" },
               std::pair{ "mirrored", "mirrored" },
               std::pair{ "keepUpright", "keep_upright" } } )
        {
            if( !source.contains( sourceName ) )
                continue;

            const bool value = source.at( sourceName ).get<bool>();

            if( sourceName == std::string( "bold" ) )
                attributes->set_bold( value );
            else if( sourceName == std::string( "italic" ) )
                attributes->set_italic( value );
            else if( sourceName == std::string( "underlined" ) )
                attributes->set_underlined( value );
            else if( sourceName == std::string( "mirrored" ) )
                attributes->set_mirrored( value );
            else
                attributes->set_keep_upright( value );

            request.mutable_header()->mutable_field_mask()->add_paths(
                    prefix + ".text.text.attributes." + protoName );
        }
    };

    if( presentation.contains( "reference" ) )
        populate( "reference", requested.mutable_reference_field() );

    if( presentation.contains( "value" ) )
        populate( "value", requested.mutable_value_field() );

    request.add_items()->PackFrom( requested );
    common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    common::commands::UpdateItemsResponse updated;

    if( !response.message().UnpackTo( &updated )
        || updated.status() != common::types::IRS_OK
        || updated.updated_items_size() != 1
        || updated.updated_items( 0 ).status().code()
                   != common::commands::ISC_OK )
    {
        if( updated.updated_items_size() == 1 )
            aError = updated.updated_items( 0 ).status().error_message();

        if( aError.empty() )
            aError = "KiCad rejected the managed footprint presentation";

        return false;
    }

    board::types::FootprintInstance active;

    if( !updated.updated_items( 0 ).item().UnpackTo( &active )
        || active.id().value() != requested.id().value()
        || ( presentation.contains( "reference" )
             && !footprintFieldMatches( active.reference_field(),
                                        presentation.at( "reference" ) ) )
        || ( presentation.contains( "value" )
             && !footprintFieldMatches( active.value_field(), presentation.at( "value" ) ) ) )
    {
        aError = "KiCad footprint presentation readback did not match KDS";
        return false;
    }

    return true;
}


bool executePcbActions( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                        const nlohmann::json& aActions,
                        const nlohmann::json& aFootprintSources, std::string& aError )
{
    std::vector<const nlohmann::json*> creates;
    std::vector<const nlohmann::json*> footprintCreates;
    std::vector<const nlohmann::json*> footprintReplacements;
    std::vector<const nlohmann::json*> footprintMetadataUpdates;
    std::vector<const nlohmann::json*> footprintPresentationUpdates;
    std::map<std::pair<std::string, std::string>,
             std::vector<const nlohmann::json*>> updates;
    std::vector<const nlohmann::json*> deletes;

    for( const nlohmann::json& action : aActions )
    {
        const std::string kind = action.at( "action" ).get<std::string>();

        if( kind == "create" )
            creates.emplace_back( &action );
        else if( kind == "create_footprint" )
            footprintCreates.emplace_back( &action );
        else if( kind == "replace_footprint" )
            footprintReplacements.emplace_back( &action );
        else if( kind == "update_footprint_metadata" )
            footprintMetadataUpdates.emplace_back( &action );
        else if( kind == "update_footprint_presentation" )
            footprintPresentationUpdates.emplace_back( &action );
        else if( kind == "update" )
            updates[{ action.at( "itemType" ).get<std::string>(),
                      action.at( "fieldMask" ).dump() }].emplace_back( &action );
        else if( kind == "delete" )
            deletes.emplace_back( &action );
        else
        {
            aError = "PCB reconcile produced an unsupported executable action";
            return false;
        }
    }

    for( const nlohmann::json* action : footprintReplacements )
    {
        if( !createFootprintFromSource( aClient, aTarget, *action,
                                        aFootprintSources, aError ) )
        {
            return false;
        }
    }

    for( const nlohmann::json* action : footprintCreates )
    {
        if( !createFootprintFromSource( aClient, aTarget, *action,
                                        aFootprintSources, aError ) )
        {
            return false;
        }
    }

    for( size_t begin = 0; begin < creates.size(); begin += 200 )
    {
        const size_t end = std::min( begin + 200, creates.size() );
        kiapi::common::commands::CreateItems request;
        request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );
        std::vector<const nlohmann::json*> batch( creates.begin() + begin, creates.begin() + end );

        for( const nlohmann::json* action : batch )
        {
            std::unique_ptr<google::protobuf::Message> item;

            if( !parsePcbItem( action->at( "itemType" ).get<std::string>(),
                               action->at( "item" ), item, aError ) )
            {
                return false;
            }

            request.add_items()->PackFrom( *item );
        }

        kiapi::common::ApiResponse response;

        if( !aClient.Call( aTarget, request, response, aError ) )
            return false;

        kiapi::common::commands::CreateItemsResponse created;

        if( !response.message().UnpackTo( &created )
            || created.status() != kiapi::common::types::IRS_OK
            || !validateCreateUpdateResponse( created.created_items(), batch, aError ) )
        {
            if( aError.empty() )
                aError = "KiCad returned an invalid create-items response";

            return false;
        }
    }

    for( const auto& [key, actions] : updates )
    {
        const std::string& itemType = key.first;

        for( size_t begin = 0; begin < actions.size(); begin += 200 )
        {
            const size_t end = std::min( begin + 200, actions.size() );
            std::vector<const nlohmann::json*> batch( actions.begin() + begin,
                                                       actions.begin() + end );
            kiapi::common::commands::UpdateItems request;
            request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );

            for( const nlohmann::json& field : batch.front()->at( "fieldMask" ) )
                request.mutable_header()->mutable_field_mask()->add_paths( field.get<std::string>() );

            for( const nlohmann::json* action : batch )
            {
                std::unique_ptr<google::protobuf::Message> item;

                if( !parsePcbItem( itemType, action->at( "item" ), item, aError ) )
                    return false;

                request.add_items()->PackFrom( *item );
            }

            kiapi::common::ApiResponse response;

            if( !aClient.Call( aTarget, request, response, aError ) )
                return false;

            kiapi::common::commands::UpdateItemsResponse updated;

            if( !response.message().UnpackTo( &updated )
                || updated.status() != kiapi::common::types::IRS_OK
                || !validateCreateUpdateResponse( updated.updated_items(), batch, aError ) )
            {
                if( aError.empty() )
                    aError = "KiCad returned an invalid update-items response";

                return false;
            }
        }
    }

    for( const nlohmann::json* action : footprintPresentationUpdates )
    {
        if( !updateFootprintPresentation( aClient, aTarget, *action, aError ) )
            return false;
    }

    for( const nlohmann::json* action : footprintMetadataUpdates )
    {
        if( !updateFootprintMetadata( aClient, aTarget, *action, aError ) )
            return false;
    }

    for( size_t begin = 0; begin < deletes.size(); begin += 500 )
    {
        const size_t end = std::min( begin + 500, deletes.size() );
        kiapi::common::commands::DeleteItems request;
        request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );

        for( size_t i = begin; i < end; ++i )
            request.add_item_ids()->set_value( deletes[i]->at( "itemId" ).get<std::string>() );

        kiapi::common::ApiResponse response;

        if( !aClient.Call( aTarget, request, response, aError ) )
            return false;

        kiapi::common::commands::DeleteItemsResponse deleted;

        if( !response.message().UnpackTo( &deleted )
            || deleted.status() != kiapi::common::types::IRS_OK
            || deleted.deleted_items_size() != static_cast<int>( end - begin ) )
        {
            aError = "KiCad returned an invalid delete-items response";
            return false;
        }

        for( int i = 0; i < deleted.deleted_items_size(); ++i )
        {
            if( deleted.deleted_items( i ).status() != kiapi::common::commands::IDS_OK
                || deleted.deleted_items( i ).id().value()
                           != deletes[begin + static_cast<size_t>( i )]
                                      ->at( "itemId" ).get<std::string>() )
            {
                aError = "KiCad rejected or changed a managed PCB deletion";
                return false;
            }
        }
    }

    return true;
}

} // namespace


KICHAD::CODEX_TOOLS::PRIVATE_TEMPORARY_DIRECTORY::~PRIVATE_TEMPORARY_DIRECTORY()
{
    if( !m_path.empty() )
    {
        std::error_code ignored;
        std::filesystem::remove_all( m_path, ignored );
    }
}


bool KICHAD::CODEX_TOOLS::PRIVATE_TEMPORARY_DIRECTORY::Create(
        const std::string& aPrefix, std::string& aError )
{
    std::error_code filesystemError;
    const std::filesystem::path temporaryRoot =
            std::filesystem::temp_directory_path( filesystemError );

    if( filesystemError )
    {
        aError = "could not resolve the private temporary directory";
        return false;
    }

    m_path = temporaryRoot / ( aPrefix + KIID().AsString().ToStdString() );
    std::filesystem::create_directory( m_path, filesystemError );

    if( filesystemError )
    {
        m_path.clear();
        aError = "could not create the private temporary directory";
        return false;
    }

    std::filesystem::permissions( m_path, std::filesystem::perms::owner_all,
                                  std::filesystem::perm_options::replace,
                                  filesystemError );

    if( filesystemError )
    {
        std::error_code ignored;
        std::filesystem::remove_all( m_path, ignored );
        m_path.clear();
        aError = "could not secure the private temporary directory";
        return false;
    }

    return true;
}


const std::filesystem::path&
KICHAD::CODEX_TOOLS::PRIVATE_TEMPORARY_DIRECTORY::Path() const
{
    return m_path;
}


bool KICHAD::CODEX_TOOLS::ResolveProjectFile( const wxString& aProjectPath,
                                              const std::string& aRelativePath,
                                              wxFileName& aResolved, std::string& aError )
{
    return resolveProjectFile( aProjectPath, aRelativePath, aResolved, aError );
}


std::string KICHAD::CODEX_TOOLS::ExpectedRootHead( const wxString& aExtension )
{
    return expectedRootHead( aExtension );
}


kiapi::common::types::KiCadObjectType KICHAD::CODEX_TOOLS::PcbObjectType(
        const std::string& aItemType )
{
    return pcbObjectType( aItemType );
}


std::unique_ptr<google::protobuf::Message> KICHAD::CODEX_TOOLS::NewPcbItem(
        const std::string& aItemType )
{
    return newPcbItem( aItemType );
}


nlohmann::json KICHAD::CODEX_TOOLS::DescribePcbMessage(
        const std::string& aItemType, const std::string& aMessagePath, std::string& aError )
{
    return describePcbMessage( aItemType, aMessagePath, aError );
}


bool KICHAD::CODEX_TOOLS::ParsePcbItem(
        const std::string& aItemType, const nlohmann::json& aJson,
        std::unique_ptr<google::protobuf::Message>& aMessage, std::string& aError )
{
    return parsePcbItem( aItemType, aJson, aMessage, aError );
}


std::string KICHAD::CODEX_TOOLS::PcbItemId( const google::protobuf::Any& aItem,
                                            const std::string& aItemType )
{
    return pcbItemId( aItem, aItemType );
}


std::string KICHAD::CODEX_TOOLS::PcbAnyType( const google::protobuf::Any& aItem )
{
    return pcbAnyType( aItem );
}


bool KICHAD::CODEX_TOOLS::ResolveProjectLibraryTable(
        const wxString& aProjectPath, const std::string& aName,
        wxFileName& aResolved, std::string& aError )
{
    return resolveProjectLibraryTable( aProjectPath, aName, aResolved, aError );
}


bool KICHAD::CODEX_TOOLS::ResolveProjectSchematic(
        const wxString& aProjectPath, const std::string& aRelativePath,
        wxFileName& aResolved, std::string& aError )
{
    return resolveProjectSchematic( aProjectPath, aRelativePath, aResolved, aError );
}


bool KICHAD::CODEX_TOOLS::ResolveProjectSidecar(
        const wxString& aProjectPath, const std::string& aRelativePath,
        wxFileName& aResolved, std::string& aError )
{
    return resolveProjectSidecar( aProjectPath, aRelativePath, aResolved, aError );
}


bool KICHAD::CODEX_TOOLS::ReadOptionalTextFile(
        const wxFileName& aPath, bool& aPresent, std::string& aSource, std::string& aError )
{
    return readOptionalTextFile( aPath, aPresent, aSource, aError );
}


bool KICHAD::CODEX_TOOLS::InstallTextFileAtomically(
        const wxFileName& aPath, bool aPresent, const std::string& aSource,
        std::string& aError )
{
    return installTextFileAtomically( aPath, aPresent, aSource, aError );
}


bool KICHAD::CODEX_TOOLS::ReadOptionalSchematic(
        const wxFileName& aPath, bool& aPresent, std::string& aSource, std::string& aError )
{
    return readOptionalSchematic( aPath, aPresent, aSource, aError );
}


bool KICHAD::CODEX_TOOLS::InstallSchematicAtomically(
        const wxFileName& aPath, bool aPresent, const std::string& aSource,
        std::string& aError )
{
    return installSchematicAtomically( aPath, aPresent, aSource, aError );
}


bool KICHAD::CODEX_TOOLS::InventoryProjectSymbolLibraries(
        const wxString& aProjectPath, const nlohmann::json& aCompilerIr,
        nlohmann::json& aSources, std::string& aError )
{
    return inventoryProjectSymbolLibraries( aProjectPath, aCompilerIr, aSources, aError );
}


bool KICHAD::CODEX_TOOLS::InventoryProjectFootprints(
        const wxString& aProjectPath, const nlohmann::json& aCompilerIr,
        nlohmann::json& aSources, std::string& aError )
{
    return inventoryProjectFootprints( aProjectPath, aCompilerIr, aSources, aError );
}


bool KICHAD::CODEX_TOOLS::ValidateFootprintModelAssets(
        const wxString& aProjectPath, const nlohmann::json& aCompilerIr,
        nlohmann::json& aSummary, std::string& aError )
{
    return validateFootprintModelAssets( aProjectPath, aCompilerIr, aSummary, aError );
}


bool KICHAD::CODEX_TOOLS::SchematicScreenUuid(
        const std::string& aSource, std::string& aUuid, std::string& aError )
{
    return schematicScreenUuid( aSource, aUuid, aError );
}


bool KICHAD::CODEX_TOOLS::ValidateNativeSchematicHierarchy(
        const wxFileName& aRootSchematic, std::string& aError )
{
    return validateNativeSchematicHierarchy( aRootSchematic, nullptr, nullptr, aError );
}


bool KICHAD::CODEX_TOOLS::ValidateNativeSchematicHierarchy(
        const wxFileName& aRootSchematic, const nlohmann::json& aCompilerIr,
        std::string& aError )
{
    return validateNativeSchematicHierarchy( aRootSchematic, &aCompilerIr, nullptr,
                                             aError );
}


bool KICHAD::CODEX_TOOLS::ValidateNativeSchematicHierarchy(
        const wxFileName& aRootSchematic, const nlohmann::json& aCompilerIr,
        const nlohmann::json& aResolvedSymbols, std::string& aError )
{
    return validateNativeSchematicHierarchy( aRootSchematic, &aCompilerIr,
                                             &aResolvedSymbols, aError );
}


bool KICHAD::CODEX_TOOLS::ValidateProjectLibraryTable(
        const std::string& aKind, const std::string& aSource,
        size_t& aRows, std::string& aError )
{
    return validateProjectLibraryTable( aKind, aSource, aRows, aError );
}


bool KICHAD::CODEX_TOOLS::ReadDesignScriptSidecar(
        const wxFileName& aFile, std::string& aSource, std::string& aError )
{
    return readDesignScriptSidecar( aFile, aSource, aError );
}


bool KICHAD::CODEX_TOOLS::InstallDesignScriptSidecarAtomically(
        const wxFileName& aFile, const std::string& aSource, std::string& aError )
{
    return installDesignScriptSidecarAtomically( aFile, aSource, aError );
}


bool KICHAD::CODEX_TOOLS::ReadJsonFile(
        const wxFileName& aPath, nlohmann::json& aDocument, std::string& aError )
{
    return readJsonFile( aPath, aDocument, aError );
}


bool KICHAD::CODEX_TOOLS::WriteJsonAtomically(
        const wxFileName& aPath, const nlohmann::json& aDocument, std::string& aError )
{
    return writeJsonAtomically( aPath, aDocument, aError );
}


bool KICHAD::CODEX_TOOLS::MergeRecoveryJournal(
        const nlohmann::json& aJournal,
        const KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::CONTEXT& aContext,
        nlohmann::json& aPreviousState, std::string& aError )
{
    return mergeRecoveryJournal( aJournal, aContext, aPreviousState, aError );
}


bool KICHAD::CODEX_TOOLS::QueryPcbInventory(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const std::set<std::string>& aIds, nlohmann::json& aInventory, std::string& aError )
{
    return queryPcbInventory( aClient, aTarget, aIds, aInventory, aError );
}


bool KICHAD::CODEX_TOOLS::QueryPcbStackup(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        kiapi::board::BoardStackup& aStackup, std::string& aError )
{
    return queryPcbStackup( aClient, aTarget, aStackup, aError );
}


bool KICHAD::CODEX_TOOLS::QueryPcbTitleBlock(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        kiapi::common::types::TitleBlockInfo& aTitleBlock, std::string& aError )
{
    return queryPcbTitleBlock( aClient, aTarget, aTitleBlock, aError );
}


bool KICHAD::CODEX_TOOLS::UpdatePcbTitleBlock(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const kiapi::common::types::TitleBlockInfo& aTitleBlock, std::string& aError )
{
    return updatePcbTitleBlock( aClient, aTarget, aTitleBlock, aError );
}


bool KICHAD::CODEX_TOOLS::UpdatePcbStackup(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const kiapi::board::BoardStackup& aStackup, std::string& aError )
{
    return updatePcbStackup( aClient, aTarget, aStackup, aError );
}


bool KICHAD::CODEX_TOOLS::QueryPcbRules(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        kiapi::board::BoardDesignRules& aRules, std::string& aError )
{
    return queryPcbRules( aClient, aTarget, aRules, aError );
}


bool KICHAD::CODEX_TOOLS::UpdatePcbRules(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const kiapi::board::BoardDesignRules& aRules, std::string& aError )
{
    return updatePcbRules( aClient, aTarget, aRules, aError );
}


bool KICHAD::CODEX_TOOLS::QueryPcbCustomRules(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        bool& aPresent, std::string& aSource, std::string& aError )
{
    return queryPcbCustomRules( aClient, aTarget, aPresent, aSource, aError );
}


bool KICHAD::CODEX_TOOLS::UpdatePcbCustomRules(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        bool aPresent, const std::string& aSource, std::string& aError )
{
    return updatePcbCustomRules( aClient, aTarget, aPresent, aSource, aError );
}


bool KICHAD::CODEX_TOOLS::QueryNetClassSettings(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        kiapi::common::project::NetClassSettings& aSettings, std::string& aError )
{
    return queryNetClassSettings( aClient, aTarget, aSettings, aError );
}


bool KICHAD::CODEX_TOOLS::UpdateNetClassSettings(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const kiapi::common::project::NetClassSettings& aSettings, std::string& aError )
{
    return updateNetClassSettings( aClient, aTarget, aSettings, aError );
}


bool KICHAD::CODEX_TOOLS::QueryPcbFootprintInventory(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const std::set<std::string>& aReferences, nlohmann::json& aInventory,
        std::string& aError )
{
    return queryPcbFootprintInventory( aClient, aTarget, aReferences, aInventory, aError );
}


bool KICHAD::CODEX_TOOLS::RefillPcbZones(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const std::set<std::string>& aExpectedZoneIds, std::string& aError )
{
    return refillPcbZones( aClient, aTarget, aExpectedZoneIds, aError );
}


bool KICHAD::CODEX_TOOLS::ExecutePcbActions(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const nlohmann::json& aActions, const nlohmann::json& aFootprintSources,
        std::string& aError )
{
    return executePcbActions( aClient, aTarget, aActions, aFootprintSources, aError );
}


bool KICHAD::CODEX_TOOLS::RunNativeKiCadCheck(
        const std::string& aCheck, const wxFileName& aInput,
        std::string& aReport, std::string& aError )
{
    return runNativeKiCadCheck( aCheck, aInput, aReport, aError );
}


bool KICHAD::CODEX_TOOLS::RunNativeKiCadPreview(
        const std::string& aView, const wxFileName& aInput,
        const std::vector<int>& aPages, const std::vector<wxFileName>& aOutputs,
        std::string& aError )
{
    return runNativeKiCadPreview( aView, aInput, aPages, aOutputs, aError );
}


bool KICHAD::CODEX_TOOLS::CanonicalizeExisting( wxFileName& aPath, bool aDirectory )
{
    return canonicalizeExisting( aPath, aDirectory );
}


bool KICHAD::CODEX_TOOLS::CreateFabricationVerificationSnapshot(
        const wxFileName& aProjectRoot, const wxFileName& aBoard,
        const wxFileName& aSchematic, const wxFileName& aSidecar,
        const std::array<std::string, 3>& aRelativePaths,
        const std::vector<std::string>& aAdditionalRelativePaths,
        PRIVATE_TEMPORARY_DIRECTORY& aSnapshot, std::string& aError )
{
    return createFabricationVerificationSnapshot( aProjectRoot, aBoard, aSchematic,
                                                   aSidecar, aRelativePaths,
                                                   aAdditionalRelativePaths, aSnapshot, aError );
}


nlohmann::json KICHAD::CODEX_TOOLS::BuildFabricationPlan(
        const nlohmann::json& aIr, const std::string& aFileStem )
{
    return buildFabricationPlan( aIr, aFileStem );
}


bool KICHAD::CODEX_TOOLS::BuildNativeNetlistValidationPlan(
        const nlohmann::json& aIr, const nlohmann::json& aResolvedSymbols,
        const std::string& aFileStem, nlohmann::json& aPlan, std::string& aError )
{
    return buildNativeNetlistValidationPlan( aIr, aResolvedSymbols, aFileStem,
                                             aPlan, aError );
}


bool KICHAD::CODEX_TOOLS::RunNativeKiCadFabrication(
        const wxFileName& aBoard, const wxFileName& aSchematic,
        const nlohmann::json& aPlan, const wxFileName& aStaging,
        std::string& aError )
{
    return runNativeKiCadFabrication( aBoard, aSchematic, aPlan, aStaging, aError );
}


bool KICHAD::CODEX_TOOLS::ValidateExactNativeFormat(
        const wxFileName& aPath, const std::string& aRoot,
        const std::string& aVersion, std::string& aError )
{
    return validateExactNativeFormat( aPath, aRoot, aVersion, aError );
}


bool KICHAD::CODEX_TOOLS::ValidateBoardFabricationIntent(
        const wxFileName& aBoard, const nlohmann::json& aIr,
        const nlohmann::json& aPlan, std::string& aError )
{
    return validateBoardFabricationIntent( aBoard, aIr, aPlan, aError );
}


bool KICHAD::CODEX_TOOLS::WriteFabricationBom(
        const nlohmann::json& aIr, const wxFileName& aStaging,
        const nlohmann::json& aPlan, size_t& aRows, std::string& aError )
{
    return writeFabricationBom( aIr, aStaging, aPlan, aRows, aError );
}


bool KICHAD::CODEX_TOOLS::ValidateFabricationArtifacts(
        const wxFileName& aStaging, const nlohmann::json& aPlan,
        nlohmann::json& aArtifacts, std::string& aError )
{
    return validateFabricationArtifacts( aStaging, aPlan, aArtifacts, aError );
}


bool KICHAD::CODEX_TOOLS::HashFabricationFile(
        const std::filesystem::path& aPath, std::string& aDigest )
{
    return hashFabricationFile( aPath, aDigest );
}
