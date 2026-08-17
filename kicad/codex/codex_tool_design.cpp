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
#include "kichad_remove_file.h"
#include "codex_tool_registry.h"

#include "codex_tool_internal.h"
#include "design_script_compiler.h"
#include "design_script_context_builder.h"
#include "design_script_footprint_library_generator.h"
#include "design_script_pcb_planner.h"
#include "design_script_physical_synthesizer.h"
#include "design_script_pcb_reconciler.h"
#include "design_script_schematic_planner.h"
#include "design_script_schematic_reconciler.h"
#include "design_script_symbol_library_generator.h"
#include "design_script_symbol_resolver.h"
#include "kicad_ipc_client.h"
#include "kicad_ipc_transaction.h"
#include "managed_footprint_library_io.h"
#include "managed_symbol_library_io.h"
#include "project_settings_ipc.h"

#include <kiid.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <api/board/board.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/types/project_settings.pb.h>
#include <google/protobuf/util/json_util.h>
#include <picosha2.h>
#include <wx/base64.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/utils.h>


namespace
{

using JSON = nlohmann::json;

constexpr size_t MAX_DESIGN_SCRIPT_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_DESIGN_STATE_BYTES = 64 * 1024 * 1024;
constexpr size_t MAX_SCHEMATIC_INVENTORY_BYTES = 32 * 1024 * 1024;
constexpr size_t MAX_DESIGN_PATCH_EDITS = 128;
constexpr size_t MAX_DESIGN_PATCH_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_DESIGN_READ_LINES = 1000;
constexpr size_t MAX_DESIGN_SEARCH_CONTEXT_BYTES = 4096;


bool attachFootprintSourceDigests( JSON& aOperations, const JSON& aFootprintSources,
                                   std::string& aError )
{
    if( !aOperations.is_array() || !aFootprintSources.is_object() )
    {
        aError = "footprint source digest input is malformed";
        return false;
    }

    for( JSON& operation : aOperations )
    {
        if( !operation.is_object() || operation.value( "action", "" ) != "place_by_reference"
            || !operation.contains( "instance" ) )
        {
            continue;
        }

        JSON& instance = operation["instance"];

        if( !instance.is_object() || !instance.contains( "libraryId" )
            || !instance["libraryId"].is_string() )
        {
            aError = "planned footprint instance has no resolved library identity";
            return false;
        }

        const std::string libraryId = instance["libraryId"].get<std::string>();
        auto source = aFootprintSources.find( libraryId );

        if( source == aFootprintSources.end() || !source->is_string()
            || source->get_ref<const std::string&>().empty() )
        {
            aError = "planned footprint " + libraryId
                     + " has no exact resolved native source";
            return false;
        }

        std::string digest;
        picosha2::hash256_hex_string( source->get_ref<const std::string&>(), digest );
        instance["footprintSourceSha256"] = std::move( digest );
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


struct MANAGED_SYMBOL_LIBRARY_UPDATE
{
    std::string nickname;
    std::string relativePath;
    wxFileName  path;
    std::string source;
    bool        previousPresent = false;
    std::string previousSource;
    bool        applied = false;
};


struct MANAGED_FOOTPRINT_LIBRARY_UPDATE
{
    std::string nickname;
    std::string relativePath;
    wxFileName  path;
    KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::FILES files;
    bool        previousPresent = false;
    KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::FILES previousFiles;
    bool        applied = false;
};


bool validatePlannedNativeSchematic(
        const JSON& aOperation, const JSON& aCompilerIr,
        const JSON& aResolvedSymbols,
        const CODEX_TOOL_REGISTRY::NATIVE_SCHEMATIC_VALIDATOR& aValidator,
        std::string& aError )
{
    if( !aValidator )
        return true;

    if( !aOperation.is_object() || !aOperation.contains( "rootFile" )
        || !aOperation["rootFile"].is_string() || !aOperation.contains( "files" )
        || !aOperation["files"].is_array() )
    {
        aError = "planned schematic validation input is malformed";
        return false;
    }

    KICHAD::CODEX_TOOLS::PRIVATE_TEMPORARY_DIRECTORY staging;

    if( !staging.Create( "kichad-schematic-preview-", aError ) )
        return false;

    const std::string rootRelative = aOperation["rootFile"].get<std::string>();
    std::set<std::filesystem::path> stagedPaths;
    wxFileName rootSchematic;
    size_t totalBytes = 0;

    for( const JSON& file : aOperation["files"] )
    {
        if( !file.is_object() || !file.contains( "path" ) || !file["path"].is_string()
            || !file.contains( "newDocumentSource" )
            || !file["newDocumentSource"].is_string() )
        {
            aError = "planned schematic file is malformed";
            return false;
        }

        const std::string relativeText = file["path"].get<std::string>();
        const std::u8string relativeUtf8(
                reinterpret_cast<const char8_t*>( relativeText.data() ),
                reinterpret_cast<const char8_t*>( relativeText.data() + relativeText.size() ) );
        const std::filesystem::path original( relativeUtf8 );
        const std::filesystem::path relative = original.lexically_normal();

        if( relativeText.empty() || relative.is_absolute() || relative.has_root_path()
            || relative.extension() != ".kicad_sch" )
        {
            aError = "planned schematic path is not a confined relative .kicad_sch file";
            return false;
        }

        for( const std::filesystem::path& part : original )
        {
            if( part == ".." )
            {
                aError = "planned schematic path escapes its private validation directory";
                return false;
            }
        }

        if( !stagedPaths.emplace( relative ).second )
        {
            aError = "planned schematic contains a duplicate file path";
            return false;
        }

        const std::string& source = file["newDocumentSource"].get_ref<const std::string&>();

        if( source.empty() || source.size() > MAX_DESIGN_SCRIPT_BYTES
            || totalBytes > MAX_SCHEMATIC_INVENTORY_BYTES - source.size() )
        {
            aError = "planned schematic validation input exceeds its bounded size";
            return false;
        }

        totalBytes += source.size();
        const std::filesystem::path target = staging.Path() / relative;
        std::error_code filesystemError;
        std::filesystem::create_directories( target.parent_path(), filesystemError );

        if( filesystemError )
        {
            aError = "could not create the private schematic validation hierarchy";
            return false;
        }

        const wxString targetString = wxString::FromUTF8( target.string() );
        wxFile output;

        if( !output.Create( targetString, true )
            || output.Write( source.data(), source.size() ) != source.size()
            || !output.Flush() )
        {
            aError = "could not write the private schematic validation hierarchy";
            return false;
        }

        if( relative.generic_string() == rootRelative )
            rootSchematic.Assign( targetString );
    }

    if( !rootSchematic.IsOk() || !rootSchematic.FileExists() )
    {
        aError = "planned root schematic is missing from its file set";
        return false;
    }

    return aValidator( rootSchematic, aCompilerIr, aResolvedSymbols, aError );
}


bool saveBoardDocument( const KICHAD_IPC_CLIENT& aClient,
                        const KICHAD_IPC_TARGET& aTarget, std::string& aError )
{
    kiapi::common::commands::SaveDocument request;
    request.mutable_document()->CopyFrom( aTarget.document );
    kiapi::common::ApiResponse response;
    return aClient.Call( aTarget, request, response, aError );
}

} // namespace


namespace KICHAD::CODEX_TOOLS
{

nlohmann::json DesignSpec()
{
    nlohmann::json schema = { { "type", "object" },
                              { "additionalProperties", false },
                              { "required", nlohmann::json::array( { "operation" } ) } };
    schema["properties"]["operation"] =
            { { "type", "string" },
              { "enum", nlohmann::json::array(
                                { "describe", "context", "search", "read", "compile",
                                  "preview", "save", "patch", "apply" } ) } };
    schema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative reusable .kicad_kds sidecar path." } };
    schema["properties"]["source"] =
            { { "type", "string" }, { "maxLength", MAX_DESIGN_SCRIPT_BYTES },
              { "description", "Inline KiChad Design Script s-expression source." } };
    schema["properties"]["edits"] =
            { { "type", "array" },
              { "minItems", 1 },
              { "maxItems", MAX_DESIGN_PATCH_EDITS },
              { "description",
                "Ordered exact-text edits for design.patch. Each oldText must match exactly once "
                "in the source produced by the preceding edit; combined edit text is limited to "
                "16 MiB." },
              { "items",
                { { "type", "object" },
                  { "additionalProperties", false },
                  { "required", nlohmann::json::array( { "oldText", "newText" } ) },
                  { "properties",
                    { { "oldText",
                        { { "type", "string" }, { "minLength", 1 },
                          { "maxLength", MAX_DESIGN_SCRIPT_BYTES },
                          { "description", "Exact unique source text to replace." } } },
                      { "newText",
                        { { "type", "string" },
                          { "maxLength", MAX_DESIGN_SCRIPT_BYTES },
                          { "description", "Replacement text; empty deletes oldText." } } } } } } } };
    schema["properties"]["boardPath"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative .kicad_pcb target required by apply." } };
    schema["properties"]["expectedSha256"] =
            { { "type", "string" }, { "minLength", 64 }, { "maxLength", 64 },
              { "description",
                "Required to patch or replace an existing sidecar and to apply the exact compiled revision." } };
    schema["properties"]["domain"] =
            { { "type", "string" },
              { "enum", nlohmann::json::array(
                                { "all", "project", "libraries", "schematic", "pcb",
                                  "manufacturing" } ) },
              { "description", "Optional semantic domain for design.context." } };
    schema["properties"]["query"] =
            { { "type", "string" }, { "maxLength", 256 },
              { "description",
                "Case-insensitive semantic filter for design.context or exact-source text for design.search." } };
    schema["properties"]["offset"] =
            { { "type", "integer" }, { "minimum", 0 }, { "maximum", 1000000 },
              { "description", "Zero-based design.context item or design.search match offset." } };
    schema["properties"]["limit"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 },
              { "description", "Maximum design.context semantic items or design.search matches." } };
    schema["properties"]["startLine"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 1000000 },
              { "description", "Optional one-based first source line for bounded design.read." } };
    schema["properties"]["lineCount"] =
            { { "type", "integer" }, { "minimum", 1 },
              { "maximum", MAX_DESIGN_READ_LINES },
              { "description", "Maximum exact source lines returned by bounded design.read." } };
    schema["properties"]["contextBytes"] =
            { { "type", "integer" }, { "minimum", 0 },
              { "maximum", MAX_DESIGN_SEARCH_CONTEXT_BYTES },
              { "description", "Exact source bytes included on each side of a design.search match." } };

    return { { "type", "function" },
             { "name", "design" },
             { "description",
               "Describe, search or read bounded exact source and semantic context, compile, "
               "preview, atomically save or patch, or transactionally apply a "
               "reusable KiChad Design Script sidecar. KDS programs declare the complete "
               "project—schematic, libraries, PCB intent, sourcing, verification, and "
               "fabrication outputs—for deterministic execution by KiChad compiler backends." },
             { "inputSchema", std::move( schema ) } };
}

} // namespace KICHAD::CODEX_TOOLS


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleDesign(
        const JSON& aArguments, const wxString& aProjectPath, bool aMutationAvailable,
        const wxString& aIpcSocketDirectory, std::chrono::milliseconds aIpcTimeout,
        const RUNTIME_DEPENDENCY_RESOLVER& aDependencyResolver ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string() )
    {
        return failure( "invalid_arguments", "design.operation must be a string" );
    }

    const std::string operation = aArguments["operation"].get<std::string>();

    if( operation != "describe" && operation != "context" && operation != "search"
        && operation != "read" && operation != "compile" && operation != "preview"
        && operation != "save" && operation != "patch" && operation != "apply" )
    {
        return failure( "invalid_arguments",
                        "design.operation must be 'describe', 'context', 'search', 'read', 'compile', 'preview', "
                        "'save', 'patch', or 'apply'" );
    }

    if( operation == "describe" )
        return success( KICHAD::DESIGN_SCRIPT_COMPILER::Describe() );

    const bool hasSource = aArguments.contains( "source" ) && aArguments["source"].is_string();
    const bool hasPath = aArguments.contains( "path" ) && aArguments["path"].is_string();
    const auto nonNegativeInteger = []( const JSON& aValue )
    {
        return aValue.is_number_unsigned()
               || ( aValue.is_number_integer() && aValue.get<int64_t>() >= 0 );
    };
    const auto integerArgument = [&]( const char* aName, uint64_t aDefault )
    {
        return aArguments.contains( aName ) ? aArguments[aName].get<uint64_t>() : aDefault;
    };

    if( ( ( operation == "compile" || operation == "preview" ) && hasSource == hasPath )
        || ( operation == "save" && ( !hasSource || !hasPath ) )
        || ( ( operation == "context" || operation == "search" || operation == "read"
               || operation == "patch" || operation == "apply" )
             && ( hasSource || !hasPath ) ) )
    {
        std::string message;

        if( operation == "context" || operation == "search" || operation == "read"
            || operation == "patch" || operation == "apply" )
            message = "design." + operation + " requires path and does not accept inline source";
        else if( operation == "save" )
            message = "design.save requires source and path";
        else
            message = "design.compile and design.preview require exactly one of source or path";

        return failure( "invalid_arguments", message );
    }

    if( ( aArguments.contains( "source" ) && !aArguments["source"].is_string() )
        || ( aArguments.contains( "path" ) && !aArguments["path"].is_string() )
        || ( aArguments.contains( "boardPath" ) && !aArguments["boardPath"].is_string() )
        || ( aArguments.contains( "domain" ) && !aArguments["domain"].is_string() )
        || ( aArguments.contains( "query" ) && !aArguments["query"].is_string() )
        || ( aArguments.contains( "edits" ) && !aArguments["edits"].is_array() )
        || ( aArguments.contains( "offset" )
             && !nonNegativeInteger( aArguments["offset"] ) )
        || ( aArguments.contains( "limit" )
             && !nonNegativeInteger( aArguments["limit"] ) )
        || ( aArguments.contains( "startLine" )
             && !nonNegativeInteger( aArguments["startLine"] ) )
        || ( aArguments.contains( "lineCount" )
             && !nonNegativeInteger( aArguments["lineCount"] ) )
        || ( aArguments.contains( "contextBytes" )
             && !nonNegativeInteger( aArguments["contextBytes"] ) ) )
    {
        return failure( "invalid_arguments",
                        "design source, path, edits, boardPath, or context filter has the wrong type" );
    }

    if( operation == "patch"
        && ( !aArguments.contains( "edits" ) || !aArguments["edits"].is_array()
             || aArguments["edits"].empty()
             || aArguments["edits"].size() > MAX_DESIGN_PATCH_EDITS ) )
    {
        return failure( "invalid_arguments",
                        "design.patch requires between 1 and 128 ordered edits" );
    }

    if( ( operation != "patch" && aArguments.contains( "edits" ) )
        || ( operation != "read"
             && ( aArguments.contains( "startLine" )
                  || aArguments.contains( "lineCount" ) ) )
        || ( operation != "search" && operation != "context"
             && ( aArguments.contains( "query" ) || aArguments.contains( "offset" )
                  || aArguments.contains( "limit" ) ) )
        || ( operation != "search" && aArguments.contains( "contextBytes" ) )
        || ( operation != "context" && aArguments.contains( "domain" ) ) )
    {
        return failure( "invalid_arguments",
                        "design received arguments that do not apply to this operation" );
    }

    if( operation == "search"
        && ( !aArguments.contains( "query" ) || !aArguments["query"].is_string()
             || aArguments["query"].get_ref<const std::string&>().empty()
             || aArguments["query"].get_ref<const std::string&>().size() > 256 ) )
    {
        return failure( "invalid_arguments",
                        "design.search requires a nonempty query of at most 256 bytes" );
    }

    if( operation == "read"
        && ( ( aArguments.contains( "startLine" )
               && ( integerArgument( "startLine", 1 ) == 0
                    || integerArgument( "startLine", 1 ) > 1000000 ) )
             || ( aArguments.contains( "lineCount" )
                  && ( integerArgument( "lineCount", MAX_DESIGN_READ_LINES ) == 0
                       || integerArgument( "lineCount", MAX_DESIGN_READ_LINES )
                                  > MAX_DESIGN_READ_LINES ) ) ) )
    {
        return failure( "invalid_arguments", "design.read has an invalid source line range" );
    }

    if( operation == "search"
        && ( integerArgument( "offset", 0 ) > 1000000
             || integerArgument( "limit", 20 ) == 0
             || integerArgument( "limit", 20 ) > 200
             || integerArgument( "contextBytes", 1024 )
                        > MAX_DESIGN_SEARCH_CONTEXT_BYTES ) )
    {
        return failure( "invalid_arguments", "design.search has an invalid result page" );
    }

    std::string source;
    wxFileName  sidecar;
    std::string pathError;

    if( hasPath )
    {
        const std::string relativePath = aArguments["path"].get<std::string>();

        if( !KICHAD::CODEX_TOOLS::ResolveProjectSidecar( aProjectPath, relativePath, sidecar, pathError ) )
            return failure( "invalid_path", pathError );

        if( ( operation == "context" || operation == "search" || operation == "read"
              || operation == "compile" || operation == "preview" || operation == "patch"
              || operation == "apply" )
            && !sidecar.FileExists() )
            return failure( "read_failed", "KiChad Design Script sidecar does not exist" );
    }

    if( hasSource )
        source = aArguments["source"].get<std::string>();
    else if( !KICHAD::CODEX_TOOLS::ReadDesignScriptSidecar( sidecar, source, pathError ) )
        return failure( "read_failed", pathError );

    if( source.empty() || source.size() > MAX_DESIGN_SCRIPT_BYTES
        || source.find( '\0' ) != std::string::npos )
    {
        return failure( "invalid_source",
                        "KiChad Design Script source must be UTF-8 text containing 1 byte to 16 MiB" );
    }

    std::string previousSha256;

    if( operation == "patch" )
    {
        picosha2::hash256_hex_string( source, previousSha256 );
        size_t patchBytes = 0;

        if( !aArguments.contains( "expectedSha256" )
            || !aArguments["expectedSha256"].is_string()
            || aArguments["expectedSha256"].get<std::string>().size() != 64 )
        {
            return failure(
                    "stale_source", "design.patch requires the current 64-character sourceSha256",
                    { { "expectedSha256", nullptr }, { "actualSha256", previousSha256 } } );
        }

        if( aArguments["expectedSha256"].get<std::string>() != previousSha256 )
        {
            return failure(
                    "stale_source", "sidecar changed since it was loaded",
                    { { "expectedSha256", aArguments["expectedSha256"] },
                      { "actualSha256", previousSha256 } } );
        }

        for( size_t editIndex = 0; editIndex < aArguments["edits"].size(); ++editIndex )
        {
            const JSON& edit = aArguments["edits"][editIndex];

            if( !edit.is_object() || edit.size() != 2 || !edit.contains( "oldText" )
                || !edit["oldText"].is_string() || !edit.contains( "newText" )
                || !edit["newText"].is_string() )
            {
                return failure(
                        "invalid_arguments",
                        "each design.patch edit must contain only string oldText and newText",
                        { { "editIndex", editIndex } } );
            }

            const std::string oldText = edit["oldText"].get<std::string>();
            const std::string newText = edit["newText"].get<std::string>();

            if( oldText.empty() || oldText == newText
                || oldText.size() > MAX_DESIGN_SCRIPT_BYTES
                || newText.size() > MAX_DESIGN_SCRIPT_BYTES )
            {
                return failure(
                        "invalid_arguments",
                        "design.patch oldText must be nonempty and differ from bounded newText",
                        { { "editIndex", editIndex } } );
            }

            if( oldText.size() > MAX_DESIGN_PATCH_BYTES - patchBytes )
            {
                return failure( "invalid_arguments",
                                "design.patch exact-text payload exceeds the 16 MiB limit",
                                { { "editIndex", editIndex } } );
            }

            patchBytes += oldText.size();

            if( newText.size() > MAX_DESIGN_PATCH_BYTES - patchBytes )
            {
                return failure( "invalid_arguments",
                                "design.patch exact-text payload exceeds the 16 MiB limit",
                                { { "editIndex", editIndex } } );
            }

            patchBytes += newText.size();

            const size_t firstMatch = source.find( oldText );

            if( firstMatch == std::string::npos )
            {
                std::string oldTextSha256;
                picosha2::hash256_hex_string( oldText, oldTextSha256 );
                return failure(
                        "patch_context_not_found",
                        "design.patch oldText does not occur in the current KDS source",
                        { { "editIndex", editIndex }, { "oldTextSha256", oldTextSha256 } } );
            }

            if( source.find( oldText, firstMatch + 1 ) != std::string::npos )
            {
                std::string oldTextSha256;
                picosha2::hash256_hex_string( oldText, oldTextSha256 );
                return failure(
                        "patch_context_ambiguous",
                        "design.patch oldText must identify exactly one source span",
                        { { "editIndex", editIndex }, { "matchesAtLeast", 2 },
                          { "oldTextSha256", oldTextSha256 } } );
            }

            if( source.size() - oldText.size() > MAX_DESIGN_SCRIPT_BYTES - newText.size() )
            {
                return failure(
                        "invalid_source", "patched KDS source would exceed the 16 MiB limit",
                        { { "editIndex", editIndex } } );
            }

            source.replace( firstMatch, oldText.size(), newText );
        }

        if( source.empty() || source.find( '\0' ) != std::string::npos )
        {
            return failure(
                    "invalid_source",
                    "patched KiChad Design Script must be UTF-8 text containing 1 byte to 16 MiB" );
        }
    }

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    const bool invalidUtf8 = std::any_of(
            compiled.diagnostics.begin(), compiled.diagnostics.end(),
            []( const JSON& aDiagnostic )
            {
                return aDiagnostic.value( "code", "" ) == "invalid_encoding";
            } );

    if( operation == "context" )
    {
        const std::string domain = aArguments.value( "domain", "all" );
        const std::string query = aArguments.value( "query", "" );
        const size_t offset = static_cast<size_t>( integerArgument( "offset", 0 ) );
        const size_t limit = static_cast<size_t>( integerArgument( "limit", 50 ) );
        static const std::set<std::string> DOMAINS = {
            "all", "project", "libraries", "schematic", "pcb", "manufacturing"
        };

        if( !DOMAINS.contains( domain ) || query.size() > 256 || offset > 1000000
            || limit == 0 || limit > 200 )
        {
            return failure( "invalid_arguments", "design.context has an invalid domain or page" );
        }

        JSON context = compiled.ok
                               ? KICHAD::DESIGN_SCRIPT_CONTEXT_BUILDER::Build(
                                         compiled.ir, compiled.plan, domain, query, offset, limit )
                               : JSON( nullptr );
        return success( { { "operation", "context" },
                          { "path", aArguments["path"] },
                          { "sourceSha256", compiled.sourceSha256 },
                          { "sourceBytes", source.size() },
                          { "valid", compiled.ok },
                          { "diagnostics", compiled.diagnostics },
                          { "context", std::move( context ) } } );
    }

    if( operation == "search" )
    {
        if( invalidUtf8 )
            return failure( "invalid_source", "KiChad Design Script source must be valid UTF-8" );

        const std::string query = aArguments["query"].get<std::string>();
        const size_t offset = static_cast<size_t>( integerArgument( "offset", 0 ) );
        const size_t limit = static_cast<size_t>( integerArgument( "limit", 20 ) );
        const size_t contextBytes =
                static_cast<size_t>( integerArgument( "contextBytes", 1024 ) );
        const auto asciiLower = []( std::string aText )
        {
            std::transform( aText.begin(), aText.end(), aText.begin(),
                            []( unsigned char aCharacter )
                            {
                                return aCharacter >= 'A' && aCharacter <= 'Z'
                                               ? static_cast<char>( aCharacter + ( 'a' - 'A' ) )
                                               : static_cast<char>( aCharacter );
                            } );
            return aText;
        };
        const std::string searchableSource = asciiLower( source );
        const std::string searchableQuery = asciiLower( query );
        std::vector<size_t> lineStarts = { 0 };

        for( size_t index = 0; index < source.size(); ++index )
        {
            if( source[index] == '\n' && index + 1 < source.size() )
                lineStarts.push_back( index + 1 );
        }

        const auto lineIndexAt = [&]( size_t aByteOffset )
        {
            auto after = std::upper_bound( lineStarts.begin(), lineStarts.end(), aByteOffset );
            return static_cast<size_t>( std::distance( lineStarts.begin(), after ) - 1 );
        };
        const auto isUtf8Continuation = []( unsigned char aCharacter )
        {
            return ( aCharacter & 0xC0 ) == 0x80;
        };

        JSON matches = JSON::array();
        size_t totalMatches = 0;
        size_t searchFrom = 0;

        while( searchFrom <= searchableSource.size() )
        {
            const size_t match = searchableSource.find( searchableQuery, searchFrom );

            if( match == std::string::npos )
                break;

            const size_t matchIndex = totalMatches++;

            if( matchIndex >= offset && matches.size() < limit )
            {
                size_t snippetStart = match > contextBytes ? match - contextBytes : 0;
                size_t snippetEnd = std::min( source.size(),
                                              match + query.size() + contextBytes );

                while( snippetStart > 0
                       && isUtf8Continuation(
                               static_cast<unsigned char>( source[snippetStart] ) ) )
                {
                    --snippetStart;
                }

                while( snippetEnd < source.size()
                       && isUtf8Continuation(
                               static_cast<unsigned char>( source[snippetEnd] ) ) )
                {
                    ++snippetEnd;
                }

                const size_t matchLineIndex = lineIndexAt( match );
                const size_t snippetStartLineIndex = lineIndexAt( snippetStart );
                const size_t snippetEndLineIndex = lineIndexAt(
                        snippetEnd == 0 ? 0 : snippetEnd - 1 );
                matches.push_back(
                        { { "matchIndex", matchIndex },
                          { "line", matchLineIndex + 1 },
                          { "columnBytes", match - lineStarts[matchLineIndex] + 1 },
                          { "matchByte", match },
                          { "matchText", source.substr( match, query.size() ) },
                          { "source", source.substr( snippetStart,
                                                      snippetEnd - snippetStart ) },
                          { "sourceStartByte", snippetStart },
                          { "sourceEndByte", snippetEnd },
                          { "sourceStartLine", snippetStartLineIndex + 1 },
                          { "sourceEndLine", snippetEndLineIndex + 1 } } );
            }

            searchFrom = match + searchableQuery.size();
        }

        const size_t nextOffset = offset + matches.size();
        JSON payload = { { "operation", "search" },
                         { "path", aArguments["path"] },
                         { "query", query },
                         { "offset", offset },
                         { "limit", limit },
                         { "totalMatches", totalMatches },
                         { "matches", std::move( matches ) },
                         { "sourceBytes", source.size() },
                         { "sourceLines", lineStarts.size() },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "valid", compiled.ok },
                         { "diagnostics", compiled.diagnostics } };

        if( nextOffset < totalMatches )
            payload["nextOffset"] = nextOffset;
        else
            payload["nextOffset"] = nullptr;

        return success( payload );
    }

    if( operation == "read" )
    {
        if( invalidUtf8 )
            return failure( "invalid_source", "KiChad Design Script source must be valid UTF-8" );

        std::vector<size_t> lineStarts = { 0 };

        for( size_t index = 0; index < source.size(); ++index )
        {
            if( source[index] == '\n' && index + 1 < source.size() )
                lineStarts.push_back( index + 1 );
        }

        const bool bounded = aArguments.contains( "startLine" )
                             || aArguments.contains( "lineCount" );
        const size_t startLine = static_cast<size_t>( integerArgument( "startLine", 1 ) );
        const size_t lineCount = static_cast<size_t>(
                integerArgument( "lineCount", MAX_DESIGN_READ_LINES ) );

        if( startLine > lineStarts.size() )
        {
            return failure( "invalid_arguments", "design.read startLine exceeds the KDS source",
                            { { "startLine", startLine },
                              { "totalLines", lineStarts.size() } } );
        }

        const size_t firstLineIndex = startLine - 1;
        const size_t endLineIndex = bounded
                                            ? std::min( firstLineIndex + lineCount,
                                                        lineStarts.size() )
                                            : lineStarts.size();
        const size_t firstByte = lineStarts[firstLineIndex];
        const size_t endByte = endLineIndex < lineStarts.size()
                                       ? lineStarts[endLineIndex]
                                       : source.size();
        const std::string selectedSource = source.substr( firstByte, endByte - firstByte );
        JSON payload = { { "operation", "read" },
                         { "path", aArguments["path"] },
                         { "source", selectedSource },
                         { "bytes", selectedSource.size() },
                         { "sourceBytes", source.size() },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "valid", compiled.ok },
                         { "diagnostics", compiled.diagnostics },
                         { "range",
                           { { "startLine", startLine },
                             { "endLine", endLineIndex },
                             { "totalLines", lineStarts.size() },
                             { "complete", firstByte == 0 && endByte == source.size() } } } };

        if( endLineIndex < lineStarts.size() )
            payload["range"]["nextLine"] = endLineIndex + 1;
        else
            payload["range"]["nextLine"] = nullptr;

        return success( payload );
    }

    if( operation == "compile" )
    {
        JSON payload = { { "operation", "compile" },
                         { "valid", compiled.ok },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "plan", compiled.plan },
                         { "diagnostics", compiled.diagnostics } };

        if( hasPath )
            payload["path"] = aArguments["path"];

        return success( payload );
    }

    if( operation == "preview" )
    {
        if( !compiled.ok )
        {
            std::string message = "KiChad Design Script did not pass compilation";

            if( !compiled.diagnostics.empty() )
                message += ": " + compiled.diagnostics.front().value( "message", "invalid program" );

            return failure( "compile_failed", message,
                            { { "sourceSha256", compiled.sourceSha256 },
                              { "diagnostics", compiled.diagnostics } } );
        }

        KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT planned =
                KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
        KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::RESULT generatedSymbols =
                KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::Generate( compiled.ir );
        KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::RESULT generatedFootprints =
                KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::Generate( compiled.ir );
        JSON symbolLibrarySources;
        JSON footprintSources;
        JSON footprintModelAssets;

        if( !generatedSymbols.ok )
        {
            return failure( "symbol_generation_failed",
                            generatedSymbols.diagnostics.empty()
                                    ? "managed symbol libraries could not be generated"
                                    : generatedSymbols.diagnostics.front().value(
                                              "message", "managed symbol generation failed" ) );
        }

        if( !generatedFootprints.ok )
        {
            return failure( "footprint_generation_failed",
                            generatedFootprints.diagnostics.empty()
                                    ? "managed footprint libraries could not be generated"
                                    : generatedFootprints.diagnostics.front().value(
                                              "message", "managed footprint generation failed" ) );
        }

        if( !KICHAD::CODEX_TOOLS::ValidateFootprintModelAssets(
                    aProjectPath, compiled.ir, footprintModelAssets, pathError ) )
        {
            return failure( "footprint_model_asset_unavailable", pathError );
        }

        if( !KICHAD::CODEX_TOOLS::InventoryProjectSymbolLibraries( aProjectPath, compiled.ir,
                                              symbolLibrarySources, pathError ) )
        {
            return failure( "symbol_inventory_failed", pathError );
        }

        for( const auto& [nickname, nativeSource] : generatedSymbols.sources.items() )
            symbolLibrarySources[nickname] = nativeSource;

        if( !KICHAD::CODEX_TOOLS::InventoryProjectFootprints( aProjectPath, compiled.ir,
                                         footprintSources, pathError ) )
        {
            return failure( "footprint_inventory_failed", pathError );
        }

        for( const auto& [id, nativeSource] : generatedFootprints.sources.items() )
            footprintSources[id] = nativeSource;

        KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIZER::RESULT synthesized =
                KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIZER::Synthesize(
                        compiled.ir, footprintSources );

        if( !synthesized.ok )
            return failure( "synthesis_failed", synthesized.error );

        compiled.ir = std::move( synthesized.ir );

        KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT resolvedSymbols =
                KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve(
                        compiled.ir, symbolLibrarySources );
        KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT schematicPlanned;

        if( resolvedSymbols.ok )
        {
            planned = KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan(
                    compiled.ir, resolvedSymbols.symbols );

            if( !attachFootprintSourceDigests( planned.operations, footprintSources,
                                               pathError ) )
            {
                return failure( "footprint_inventory_failed", pathError );
            }

            schematicPlanned = KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, JSON::object(), resolvedSymbols.symbols );
        }
        else
        {
            schematicPlanned.counts = resolvedSymbols.counts;
            schematicPlanned.diagnostics = resolvedSymbols.diagnostics;
        }

        if( schematicPlanned.fullyLowered && !schematicPlanned.operations.empty() )
        {
            pathError.clear();

            if( !validatePlannedNativeSchematic(
                        schematicPlanned.operations[0], compiled.ir,
                        resolvedSymbols.symbols,
                        m_schematicValidator, pathError ) )
            {
                return failure(
                        "schematic_validation_failed",
                        pathError.empty()
                                ? "native schematic connectivity validation failed"
                                : pathError,
                        { { "sourceSha256", compiled.sourceSha256 } } );
            }
        }

        JSON items = JSON::array();

        for( const JSON& plannedOperation : planned.operations )
        {
            if( items.size() == 500 )
                break;

            const std::string action = plannedOperation.value( "action", "" );

            if( action == "upsert" )
            {
                items.push_back( { { "action", "manage" },
                                   { "logicalId", plannedOperation["logicalId"] },
                                   { "itemType", plannedOperation["itemType"] },
                                   { "targetId", plannedOperation["itemId"] } } );
            }
            else if( action == "place_by_reference" )
            {
                items.push_back( { { "action", "place" },
                                   { "component", plannedOperation["component"] } } );
            }
            else if( action == "update_stackup" )
            {
                items.push_back( { { "action", "configure_stackup" },
                                   { "physicalLayers",
                                     plannedOperation["stackup"]["layers"].size() } } );
            }
            else if( action == "update_title_block" )
            {
                items.push_back( { { "action", "configure_title_block" } } );
            }
            else if( action == "update_rules" )
            {
                items.push_back( { { "action", "configure_global_board_rules" } } );
            }
            else if( action == "update_schematic_rule_severities" )
            {
                items.push_back(
                        { { "action", "configure_erc_severities" },
                          { "overrides",
                            plannedOperation["severities"]["severities"].size() } } );
            }
            else if( action == "update_net_classes" )
            {
                items.push_back(
                        { { "action", "configure_net_classes" },
                          { "classes", plannedOperation["settings"]["netClasses"].size() },
                          { "assignments",
                            plannedOperation["settings"]["assignments"].size() } } );
            }
            else if( action == "update_project_library_table" )
            {
                items.push_back(
                        { { "action", "configure_project_library_table" },
                          { "kind", plannedOperation["kind"] },
                          { "path", plannedOperation["path"] },
                          { "libraries", plannedOperation["entries"] } } );
            }
            else if( action == "update_custom_rules" )
            {
                items.push_back(
                        { { "action", "configure_custom_board_rules" },
                          { "rules", planned.counts.value( "customRules", 0 ) } } );
            }
            else
            {
                items.push_back( { { "action", "unsupported" },
                                   { "statementKind", plannedOperation["statementKind"] },
                                   { "reason", plannedOperation["reason"] } } );
            }
        }

        JSON boardPlan = { { "fullyLowered", planned.fullyLowered },
                           { "counts", std::move( planned.counts ) },
                           { "diagnostics", std::move( planned.diagnostics ) },
                           { "items", std::move( items ) },
                           { "itemsTruncated", planned.operations.size() > 500 } };

        JSON schematicFiles = JSON::array();

        if( !schematicPlanned.operations.empty() )
        {
            for( const JSON& file : schematicPlanned.operations[0]["files"] )
            {
                schematicFiles.push_back( { { "path", file["path"] },
                                            { "sheetId", file["sheetId"] },
                                            { "page", file["page"] },
                                            { "root", file["root"] },
                                            { "managedItems",
                                              file["items"].size()
                                                      + file["libSymbols"].size()
                                                      + file["busAliases"].size() } } );
            }
        }

        JSON schematicPlan = { { "fullyLowered", schematicPlanned.fullyLowered },
                               { "counts", std::move( schematicPlanned.counts ) },
                               { "diagnostics", std::move( schematicPlanned.diagnostics ) },
                               { "files", std::move( schematicFiles ) } };

        JSON payload = { { "operation", "preview" },
                         { "valid", true },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "compilerPlan", std::move( compiled.plan ) },
                         { "boardPlan", std::move( boardPlan ) },
                         { "schematicPlan", std::move( schematicPlan ) } };

        payload["managedSymbolLibraries"] = {
            { "counts", generatedSymbols.counts },
            { "nicknames", JSON::array() }
        };

        for( const auto& entry : generatedSymbols.sources.items() )
            payload["managedSymbolLibraries"]["nicknames"].push_back( entry.key() );

        payload["managedFootprintLibraries"] = {
            { "counts", generatedFootprints.counts },
            { "libraries", generatedFootprints.libraries }
        };
        payload["footprintModelAssets"] = std::move( footprintModelAssets );

        if( hasPath )
            payload["path"] = aArguments["path"];

        return success( payload );
    }

    if( operation == "apply" )
    {
        if( !aMutationAvailable )
        {
            return failure( "snapshot_required",
                            "A complete pre-turn project snapshot is required to apply KDS" );
        }

        if( !compiled.ok )
        {
            std::string message = "KiChad Design Script did not pass compilation";

            if( !compiled.diagnostics.empty() )
                message += ": " + compiled.diagnostics.front().value( "message", "invalid program" );

            return failure( "compile_failed", message );
        }

        if( !aArguments.contains( "expectedSha256" )
            || !aArguments["expectedSha256"].is_string()
            || aArguments["expectedSha256"].get<std::string>() != compiled.sourceSha256 )
        {
            JSON details = { { "actualSha256", compiled.sourceSha256 } };

            if( aArguments.contains( "expectedSha256" )
                && aArguments["expectedSha256"].is_string() )
            {
                details["expectedSha256"] = aArguments["expectedSha256"];
            }
            else
            {
                details["expectedSha256"] = nullptr;
            }

            return failure( "stale_source",
                            "The KDS source does not match the revision supplied to design.apply",
                            details );
        }

        if( !aArguments.contains( "boardPath" ) || !aArguments["boardPath"].is_string() )
            return failure( "invalid_arguments", "design.apply requires boardPath" );

        const std::string boardRelativePath = aArguments["boardPath"].get<std::string>();
        wxFileName       board;

        if( !KICHAD::CODEX_TOOLS::ResolveProjectFile( aProjectPath, boardRelativePath, board, pathError )
            || board.GetExt() != wxS( "kicad_pcb" ) )
        {
            if( pathError.empty() )
                pathError = "boardPath must identify a project .kicad_pcb file";

            return failure( "invalid_path", pathError );
        }

        KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT planned =
                KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
        KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::RESULT generatedSymbols =
                KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::Generate( compiled.ir );
        KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::RESULT generatedFootprints =
                KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::Generate( compiled.ir );
        JSON symbolLibrarySources;
        JSON footprintSources;
        JSON footprintModelAssets;

        if( !generatedSymbols.ok )
        {
            return failure( "symbol_generation_failed",
                            generatedSymbols.diagnostics.empty()
                                    ? "managed symbol libraries could not be generated"
                                    : generatedSymbols.diagnostics.front().value(
                                              "message", "managed symbol generation failed" ) );
        }

        if( !generatedFootprints.ok )
        {
            return failure( "footprint_generation_failed",
                            generatedFootprints.diagnostics.empty()
                                    ? "managed footprint libraries could not be generated"
                                    : generatedFootprints.diagnostics.front().value(
                                              "message", "managed footprint generation failed" ) );
        }

        if( !KICHAD::CODEX_TOOLS::ValidateFootprintModelAssets(
                    aProjectPath, compiled.ir, footprintModelAssets, pathError ) )
        {
            return failure( "footprint_model_asset_unavailable", pathError );
        }

        if( !KICHAD::CODEX_TOOLS::InventoryProjectSymbolLibraries( aProjectPath, compiled.ir,
                                              symbolLibrarySources, pathError ) )
        {
            return failure( "symbol_inventory_failed", pathError );
        }

        for( const auto& [nickname, nativeSource] : generatedSymbols.sources.items() )
            symbolLibrarySources[nickname] = nativeSource;

        if( !KICHAD::CODEX_TOOLS::InventoryProjectFootprints( aProjectPath, compiled.ir,
                                         footprintSources, pathError ) )
        {
            return failure( "footprint_inventory_failed", pathError );
        }

        for( const auto& [id, nativeSource] : generatedFootprints.sources.items() )
            footprintSources[id] = nativeSource;

        KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIZER::RESULT synthesized =
                KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIZER::Synthesize(
                        compiled.ir, footprintSources );

        if( !synthesized.ok )
            return failure( "synthesis_failed", synthesized.error );

        compiled.ir = std::move( synthesized.ir );

        KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT resolvedSymbols =
                KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve(
                        compiled.ir, symbolLibrarySources );

        if( !resolvedSymbols.ok )
        {
            return failure(
                    "backend_incomplete",
                    resolvedSymbols.diagnostics.empty()
                            ? "one or more schematic symbols could not be exactly resolved"
                            : resolvedSymbols.diagnostics.front().value(
                                      "message", "schematic symbol resolution failed" ) );
        }

        planned = KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan(
                compiled.ir, resolvedSymbols.symbols );

        if( !attachFootprintSourceDigests( planned.operations, footprintSources, pathError ) )
            return failure( "footprint_inventory_failed", pathError );

        KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT schematicPreplanned =
                KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                        compiled.ir, JSON::object(), resolvedSymbols.symbols );

        if( !planned.fullyLowered || !schematicPreplanned.fullyLowered )
        {
            std::string message = !planned.fullyLowered
                                          ? "one or more board statements do not have an apply backend"
                                          : "one or more schematic statements do not have an apply backend";

            for( const JSON& action : planned.operations )
            {
                if( action.value( "action", "" ) == "unsupported" )
                {
                    message += ": " + action.value( "reason", "unsupported statement" );
                    break;
                }
            }

            if( !schematicPreplanned.fullyLowered
                && !schematicPreplanned.diagnostics.empty() )
            {
                message += ": " + schematicPreplanned.diagnostics.front().value(
                                           "message", "unsupported schematic statement" );
            }

            return failure( "backend_incomplete", message );
        }

        pathError.clear();

        if( !schematicPreplanned.operations.empty()
            && !validatePlannedNativeSchematic(
                    schematicPreplanned.operations[0], compiled.ir,
                    resolvedSymbols.symbols,
                    m_schematicValidator, pathError ) )
        {
            return failure(
                    "schematic_preflight_failed",
                    pathError.empty()
                            ? "native schematic connectivity validation failed"
                            : pathError,
                    { { "sourceSha256", compiled.sourceSha256 } } );
        }

        std::unique_ptr<kiapi::board::BoardStackup> desiredStackup;
        std::unique_ptr<kiapi::common::types::TitleBlockInfo> desiredTitleBlock;
        std::unique_ptr<kiapi::board::BoardDesignRules> desiredRules;
        std::unique_ptr<kiapi::common::project::NetClassSettings> desiredNetClasses;
        std::unique_ptr<kiapi::common::project::TextVariables> desiredTextVariables;
        std::unique_ptr<kiapi::common::project::SchematicFieldTemplates>
                desiredFieldTemplates;
        std::unique_ptr<kiapi::common::project::SchematicRuleSeverities>
                desiredSchematicRuleSeverities;
        JSON desiredLibraryTables = JSON::array();
        std::set<std::string> desiredLibraryTableKinds;
        bool        hasDesiredCustomRules = false;
        bool        desiredCustomRulesPresent = false;
        std::string desiredCustomRulesSource;

        for( const JSON& action : planned.operations )
        {
            const std::string plannedAction = action.value( "action", "" );

            if( plannedAction != "update_stackup" && plannedAction != "update_title_block"
                && plannedAction != "update_rules"
                && plannedAction != "update_net_classes"
                && plannedAction != "update_text_variables"
                && plannedAction != "update_schematic_field_templates"
                && plannedAction != "update_schematic_rule_severities"
                && plannedAction != "update_custom_rules"
                && plannedAction != "update_project_library_table" )
                continue;

            if( plannedAction == "update_project_library_table" )
            {
                const std::string kind = action.value( "kind", "" );
                const std::string expectedPath = kind == "symbol" ? "sym-lib-table"
                                                   : kind == "footprint" ? "fp-lib-table"
                                                                          : "";

                if( expectedPath.empty() || !desiredLibraryTableKinds.emplace( kind ).second
                    || action.value( "path", "" ) != expectedPath
                    || !action.contains( "present" ) || !action["present"].is_boolean()
                    || !action["present"].get<bool>() || !action.contains( "source" )
                    || !action["source"].is_string() )
                {
                    return failure( "invalid_plan",
                                    "KDS produced an invalid project library table operation" );
                }

                size_t      rows = 0;
                std::string validationError;
                const std::string tableSource = action["source"].get<std::string>();

                if( !KICHAD::CODEX_TOOLS::ValidateProjectLibraryTable( kind, tableSource, rows, validationError )
                    || !action.contains( "entries" ) || !action["entries"].is_number_unsigned()
                    || action["entries"].get<size_t>() != rows )
                {
                    return failure( "invalid_plan",
                                    validationError.empty()
                                            ? "KDS project library table row count is inconsistent"
                                            : validationError );
                }

                desiredLibraryTables.push_back( { { "kind", kind },
                                                  { "path", expectedPath },
                                                  { "source", tableSource },
                                                  { "rows", rows } } );
                continue;
            }

            if( plannedAction == "update_custom_rules" )
            {
                if( hasDesiredCustomRules || !action.contains( "customRules" )
                    || !action["customRules"].is_object()
                    || !action["customRules"].contains( "present" )
                    || !action["customRules"]["present"].is_boolean()
                    || !action["customRules"].contains( "source" )
                    || !action["customRules"]["source"].is_string()
                    || action["customRules"]["source"].get_ref<const std::string&>().size()
                               > MAX_DESIGN_SCRIPT_BYTES )
                {
                    return failure( "invalid_plan",
                                    "KDS produced invalid native custom rules" );
                }

                hasDesiredCustomRules = true;
                desiredCustomRulesPresent = action["customRules"]["present"].get<bool>();
                desiredCustomRulesSource =
                        action["customRules"]["source"].get<std::string>();

                if( desiredCustomRulesPresent != !desiredCustomRulesSource.empty() )
                {
                    return failure( "invalid_plan",
                                    "KDS produced inconsistent native custom rules" );
                }

                continue;
            }

            google::protobuf::util::JsonParseOptions options;
            options.ignore_unknown_fields = false;
            KICHAD::PROTOBUF_STATUS status;

            if( plannedAction == "update_stackup" )
            {
                if( desiredStackup )
                    return failure( "invalid_plan", "KDS planned more than one board stackup" );

                desiredStackup = std::make_unique<kiapi::board::BoardStackup>();
                status = google::protobuf::util::JsonStringToMessage(
                        action.at( "stackup" ).dump(), desiredStackup.get(), options );
            }
            else if( plannedAction == "update_title_block" )
            {
                if( desiredTitleBlock )
                    return failure( "invalid_plan", "KDS planned more than one title block" );

                desiredTitleBlock =
                        std::make_unique<kiapi::common::types::TitleBlockInfo>();
                status = google::protobuf::util::JsonStringToMessage(
                        action.at( "titleBlock" ).dump(), desiredTitleBlock.get(), options );
            }
            else if( plannedAction == "update_rules" )
            {
                if( desiredRules )
                    return failure( "invalid_plan", "KDS planned more than one global rule set" );

                desiredRules = std::make_unique<kiapi::board::BoardDesignRules>();
                status = google::protobuf::util::JsonStringToMessage(
                        action.at( "rules" ).dump(), desiredRules.get(), options );
            }
            else if( plannedAction == "update_net_classes" )
            {
                if( desiredNetClasses )
                    return failure( "invalid_plan", "KDS planned more than one netclass table" );

                desiredNetClasses =
                        std::make_unique<kiapi::common::project::NetClassSettings>();
                status = google::protobuf::util::JsonStringToMessage(
                        action.at( "settings" ).dump(), desiredNetClasses.get(), options );
            }
            else if( plannedAction == "update_text_variables" )
            {
                if( desiredTextVariables )
                    return failure( "invalid_plan", "KDS planned text variables more than once" );

                desiredTextVariables =
                        std::make_unique<kiapi::common::project::TextVariables>();
                status = google::protobuf::util::JsonStringToMessage(
                        action.at( "textVariables" ).dump(), desiredTextVariables.get(), options );
            }
            else if( plannedAction == "update_schematic_field_templates" )
            {
                if( desiredFieldTemplates )
                    return failure( "invalid_plan", "KDS planned field templates more than once" );

                desiredFieldTemplates =
                        std::make_unique<kiapi::common::project::SchematicFieldTemplates>();
                status = google::protobuf::util::JsonStringToMessage(
                        action.at( "fieldTemplates" ).dump(), desiredFieldTemplates.get(), options );
            }
            else
            {
                if( desiredSchematicRuleSeverities )
                {
                    return failure( "invalid_plan",
                                    "KDS planned schematic rule severities more than once" );
                }

                desiredSchematicRuleSeverities =
                        std::make_unique<kiapi::common::project::SchematicRuleSeverities>();
                status = google::protobuf::util::JsonStringToMessage(
                        action.at( "severities" ).dump(),
                        desiredSchematicRuleSeverities.get(), options );
            }

            if( !status.ok() )
                return failure( "invalid_plan", "KDS produced invalid native design settings" );
        }

        if( !desiredLibraryTables.empty() && desiredLibraryTables.size() != 2 )
        {
            return failure( "invalid_plan",
                            "KDS must replace symbol and footprint project tables together" );
        }

        const std::string sourceRelativePath = aArguments["path"].get<std::string>();
        const std::string projectName = compiled.ir["project"]["name"].get<std::string>();
        KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::CONTEXT reconcileContext = {
            sourceRelativePath, boardRelativePath, projectName, compiled.sourceSha256
        };
        wxFileName statePath = sidecar;
        statePath.SetExt( wxS( "kicad_kds_state" ) );
        wxFileName journalPath = sidecar;
        journalPath.SetExt( wxS( "kicad_kds_journal" ) );
        JSON previousState;
        JSON journal;

        if( !KICHAD::CODEX_TOOLS::ReadJsonFile( statePath, previousState, pathError )
            || !KICHAD::CODEX_TOOLS::ReadJsonFile( journalPath, journal, pathError )
            || !KICHAD::CODEX_TOOLS::MergeRecoveryJournal( journal, reconcileContext, previousState, pathError ) )
        {
            return failure( "invalid_managed_state", pathError );
        }

        JSON previousSchematicItems = JSON::array();

        if( previousState.is_object() && previousState.contains( "managedSchematicItems" ) )
        {
            if( !previousState["managedSchematicItems"].is_array() )
            {
                return failure( "invalid_managed_state",
                                "managed schematic ownership must be an array" );
            }

            previousSchematicItems = previousState["managedSchematicItems"];
        }

        std::map<std::string, JSON> preliminarySchematicFiles;

        if( !schematicPreplanned.operations.empty() )
        {
            if( schematicPreplanned.operations.size() != 1
                || schematicPreplanned.operations[0].value( "action", "" )
                           != "reconcile_schematic_hierarchy" )
            {
                return failure( "invalid_plan", "KDS produced an invalid schematic operation" );
            }

            for( const JSON& file : schematicPreplanned.operations[0]["files"] )
                preliminarySchematicFiles.emplace( file["path"].get<std::string>(), file );
        }

        std::set<std::string> schematicInventoryPaths;

        for( const auto& entry : preliminarySchematicFiles )
            schematicInventoryPaths.emplace( entry.first );

        for( const JSON& item : previousSchematicItems )
        {
            if( !item.is_object() || !item.contains( "file" ) || !item["file"].is_string() )
            {
                return failure( "invalid_managed_state",
                                "managed schematic ownership contains an invalid file path" );
            }

            schematicInventoryPaths.emplace( item["file"].get<std::string>() );
        }

        JSON liveSchematicFiles = JSON::array();
        JSON existingScreenUuids = JSON::object();
        std::map<std::string, wxFileName> resolvedSchematicPaths;
        size_t schematicInventoryBytes = 0;

        for( const std::string& relativePath : schematicInventoryPaths )
        {
            wxFileName resolved;
            bool present = false;
            std::string nativeSource;

            if( !KICHAD::CODEX_TOOLS::ResolveProjectSchematic( aProjectPath, relativePath, resolved, pathError )
                || !KICHAD::CODEX_TOOLS::ReadOptionalSchematic( resolved, present, nativeSource, pathError ) )
            {
                return failure( "schematic_inventory_failed", pathError );
            }

            if( nativeSource.size() > MAX_SCHEMATIC_INVENTORY_BYTES
                                             - schematicInventoryBytes )
            {
                return failure( "schematic_inventory_failed",
                                "managed schematic inventory exceeds 32 MiB" );
            }

            schematicInventoryBytes += nativeSource.size();

            resolvedSchematicPaths.emplace( relativePath, resolved );
            JSON live = { { "path", relativePath }, { "present", present } };

            if( present )
                live["source"] = nativeSource;

            liveSchematicFiles.emplace_back( std::move( live ) );

            if( present && preliminarySchematicFiles.contains( relativePath ) )
            {
                std::string screenUuid;

                if( !KICHAD::CODEX_TOOLS::SchematicScreenUuid( nativeSource, screenUuid, pathError ) )
                    return failure( "schematic_inventory_failed", pathError );

                existingScreenUuids[relativePath] = screenUuid;
            }
        }

        KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT schematicPlanned =
                KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                        compiled.ir, existingScreenUuids, resolvedSymbols.symbols );

        if( !schematicPlanned.fullyLowered )
        {
            return failure(
                    "backend_incomplete",
                    schematicPlanned.diagnostics.empty()
                            ? "schematic hierarchy could not be lowered against live files"
                            : schematicPlanned.diagnostics.front().value(
                                      "message", "schematic hierarchy planning failed" ) );
        }

        JSON schematicOperation = {
            { "action", "reconcile_schematic_hierarchy" },
            { "project", projectName },
            { "rootFile", "" },
            { "files", JSON::array() },
            { "managedItems", JSON::array() }
        };

        if( !schematicPlanned.operations.empty() )
            schematicOperation = schematicPlanned.operations[0];

        KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT schematicReconciled =
                KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                        schematicOperation, previousSchematicItems, liveSchematicFiles );

        if( !schematicReconciled.ok )
        {
            return failure(
                    "schematic_reconcile_failed",
                    schematicReconciled.diagnostics.empty()
                            ? "managed schematic reconciliation failed"
                            : schematicReconciled.diagnostics.front().value(
                                      "message", "managed schematic reconciliation failed" ) );
        }

        std::vector<SCHEMATIC_FILE_UPDATE> schematicUpdates;

        for( const JSON& action : schematicReconciled.fileActions )
        {
            const std::string relativePath = action["path"].get<std::string>();
            auto resolved = resolvedSchematicPaths.find( relativePath );

            if( resolved == resolvedSchematicPaths.end() )
                return failure( "invalid_plan", "schematic action has no bounded inventory" );

            SCHEMATIC_FILE_UPDATE update;
            update.relativePath = relativePath;
            update.path = resolved->second;
            update.source = action["source"].get<std::string>();
            update.previousPresent = action["previousPresent"].get<bool>();
            update.previousSource = action["previousSource"].get<std::string>();
            schematicUpdates.emplace_back( std::move( update ) );
        }

        JSON managedOperations = JSON::array();
        JSON reconcileOperations = JSON::array();
        std::set<std::string> placementReferences;

        for( const JSON& plannedOperation : planned.operations )
        {
            const std::string action = plannedOperation.value( "action", "" );

            if( action == "upsert" )
            {
                managedOperations.push_back( plannedOperation );
                reconcileOperations.push_back( plannedOperation );
            }
            else if( action == "place_by_reference" )
            {
                placementReferences.emplace( plannedOperation["component"].get<std::string>() );
                reconcileOperations.push_back( plannedOperation );
            }
        }

        KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::RESULT preflight =
                KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::Reconcile(
                        managedOperations, previousState, JSON::array(), reconcileContext );

        if( !preflight.ok )
        {
            return failure( "reconcile_failed",
                            preflight.diagnostics.empty()
                                    ? "managed PCB state failed validation"
                                    : preflight.diagnostics.front().value(
                                              "message", "managed PCB state failed validation" ) );
        }

        std::set<std::string> relevantIds;

        for( const JSON& plannedOperation : managedOperations )
            relevantIds.emplace( plannedOperation["itemId"].get<std::string>() );

        for( const std::string& reference : placementReferences )
        {
            relevantIds.emplace( KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                    projectName, "footprint", reference ) );
        }

        if( !previousState.is_null() )
        {
            for( const JSON& item : previousState["managedPcbItems"] )
                relevantIds.emplace( item["itemId"].get<std::string>() );
        }

        std::vector<PROJECT_LIBRARY_TABLE_UPDATE> libraryTableUpdates;

        for( const JSON& desired : desiredLibraryTables )
        {
            PROJECT_LIBRARY_TABLE_UPDATE update;
            update.kind = desired["kind"].get<std::string>();
            update.source = desired["source"].get<std::string>();
            update.rows = desired["rows"].get<size_t>();

            if( !KICHAD::CODEX_TOOLS::ResolveProjectLibraryTable( aProjectPath,
                                             desired["path"].get<std::string>(),
                                             update.path, pathError )
                || !KICHAD::CODEX_TOOLS::ReadOptionalTextFile( update.path, update.previousPresent,
                                          update.previousSource, pathError ) )
            {
                return failure( "library_table_inventory_failed", pathError );
            }

            libraryTableUpdates.emplace_back( std::move( update ) );
        }

        std::vector<MANAGED_SYMBOL_LIBRARY_UPDATE> managedSymbolLibraryUpdates;

        for( const JSON& library : compiled.ir["libraries"] )
        {
            if( !library.is_object() || library.value( "kind", "" ) != "symbol"
                || library.value( "table", "" ) != "project"
                || !library.value( "managed", false ) )
            {
                continue;
            }

            MANAGED_SYMBOL_LIBRARY_UPDATE update;
            update.nickname = library.value( "id", "" );

            if( update.nickname.empty() || !generatedSymbols.sources.contains( update.nickname )
                || !generatedSymbols.sources[update.nickname].is_string()
                || !library.contains( "uri" ) || !library["uri"].is_string() )
            {
                return failure( "invalid_plan",
                                "KDS produced an inconsistent managed symbol library" );
            }

            update.source = generatedSymbols.sources[update.nickname].get<std::string>();

            if( !KICHAD::MANAGED_SYMBOL_LIBRARY_IO::Resolve(
                        aProjectPath, library["uri"].get<std::string>(), update.path,
                        update.relativePath, pathError )
                || !KICHAD::MANAGED_SYMBOL_LIBRARY_IO::ReadOptional(
                        update.path, update.previousPresent, update.previousSource, pathError ) )
            {
                return failure( "managed_symbol_library_inventory_failed", pathError );
            }

            managedSymbolLibraryUpdates.emplace_back( std::move( update ) );
        }

        std::vector<MANAGED_FOOTPRINT_LIBRARY_UPDATE> managedFootprintLibraryUpdates;

        for( const JSON& library : compiled.ir["libraries"] )
        {
            if( !library.is_object() || library.value( "kind", "" ) != "footprint"
                || library.value( "table", "" ) != "project"
                || !library.value( "managed", false ) )
            {
                continue;
            }

            MANAGED_FOOTPRINT_LIBRARY_UPDATE update;
            update.nickname = library.value( "id", "" );

            if( update.nickname.empty()
                || !generatedFootprints.libraries.contains( update.nickname )
                || !generatedFootprints.libraries[update.nickname].is_array()
                || generatedFootprints.libraries[update.nickname].empty()
                || !library.contains( "uri" ) || !library["uri"].is_string() )
            {
                return failure( "invalid_plan",
                                "KDS produced an inconsistent managed footprint library" );
            }

            for( const JSON& nameValue : generatedFootprints.libraries[update.nickname] )
            {
                if( !nameValue.is_string() )
                {
                    return failure( "invalid_plan",
                                    "KDS produced a malformed managed footprint name" );
                }

                const std::string name = nameValue.get<std::string>();
                const std::string id = update.nickname + ":" + name;

                if( name.empty() || !generatedFootprints.sources.contains( id )
                    || !generatedFootprints.sources[id].is_string()
                    || !update.files.emplace(
                            name + ".kicad_mod",
                            generatedFootprints.sources[id].get<std::string>() ).second )
                {
                    return failure( "invalid_plan",
                                    "KDS produced inconsistent managed footprint source files" );
                }
            }

            if( !KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::Resolve(
                        aProjectPath, library["uri"].get<std::string>(), update.path,
                        update.relativePath, pathError )
                || !KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::ReadOptional(
                        update.path, update.previousPresent, update.previousFiles, pathError ) )
            {
                return failure( "managed_footprint_library_inventory_failed", pathError );
            }

            managedFootprintLibraryUpdates.emplace_back( std::move( update ) );
        }

        if( aDependencyResolver
            && !aDependencyResolver( { RUNTIME_APPLICATION::PCB_EDITOR, board }, pathError ) )
        {
            return failure( "dependency_unavailable", pathError );
        }

        KICHAD_IPC_CLIENT client( "org.kichad.codex.design", aIpcSocketDirectory,
                                  aIpcTimeout );
        KICHAD_IPC_TARGET target;

        if( !client.FindOpenPcb( aProjectPath, board.GetFullPath(), target, pathError ) )
            return failure( "pcb_not_open", pathError );

        kiapi::board::BoardStackup previousStackup;
        kiapi::common::types::TitleBlockInfo previousTitleBlock;
        kiapi::board::BoardDesignRules previousRules;
        kiapi::common::project::NetClassSettings previousNetClasses;
        kiapi::common::project::TextVariables previousTextVariables;
        kiapi::common::project::SchematicFieldTemplates previousFieldTemplates;
        kiapi::common::project::SchematicRuleSeverities previousSchematicRuleSeverities;
        bool        previousCustomRulesPresent = false;
        std::string previousCustomRulesSource;

        if( desiredStackup && !KICHAD::CODEX_TOOLS::QueryPcbStackup( client, target, previousStackup, pathError ) )
            return failure( "stackup_inventory_failed", pathError );

        if( desiredTitleBlock
            && !KICHAD::CODEX_TOOLS::QueryPcbTitleBlock(
                    client, target, previousTitleBlock, pathError ) )
        {
            return failure( "title_block_inventory_failed", pathError );
        }

        if( desiredRules && !KICHAD::CODEX_TOOLS::QueryPcbRules( client, target, previousRules, pathError ) )
            return failure( "rules_inventory_failed", pathError );

        if( desiredNetClasses
            && !KICHAD::CODEX_TOOLS::QueryNetClassSettings( client, target, previousNetClasses, pathError ) )
        {
            return failure( "netclass_inventory_failed", pathError );
        }

        if( desiredTextVariables
            && !KICHAD::PROJECT_SETTINGS_IPC::QueryTextVariables(
                    client, target, previousTextVariables, pathError ) )
        {
            return failure( "text_variable_inventory_failed", pathError );
        }

        if( desiredFieldTemplates
            && !KICHAD::PROJECT_SETTINGS_IPC::QuerySchematicFieldTemplates(
                    client, target, previousFieldTemplates, pathError ) )
        {
            return failure( "field_template_inventory_failed", pathError );
        }

        if( desiredSchematicRuleSeverities
            && !KICHAD::PROJECT_SETTINGS_IPC::QuerySchematicRuleSeverities(
                    client, target, previousSchematicRuleSeverities, pathError ) )
        {
            return failure( "erc_severity_inventory_failed", pathError );
        }

        if( hasDesiredCustomRules
            && !KICHAD::CODEX_TOOLS::QueryPcbCustomRules( client, target, previousCustomRulesPresent,
                                     previousCustomRulesSource, pathError ) )
        {
            return failure( "custom_rules_inventory_failed", pathError );
        }

        JSON liveInventory;

        if( !KICHAD::CODEX_TOOLS::QueryPcbInventory( client, target, relevantIds, liveInventory, pathError ) )
            return failure( "inventory_failed", pathError );

        if( !KICHAD::CODEX_TOOLS::QueryPcbFootprintInventory( client, target, placementReferences,
                                         liveInventory, pathError ) )
        {
            return failure( "inventory_failed", pathError );
        }

        KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::RESULT reconciled =
                KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::Reconcile(
                        reconcileOperations, previousState, liveInventory, reconcileContext );

        if( !reconciled.ok )
        {
            return failure( "reconcile_failed",
                            reconciled.diagnostics.empty()
                                    ? "managed PCB reconciliation failed"
                                    : reconciled.diagnostics.front().value(
                                              "message", "managed PCB reconciliation failed" ) );
        }

        reconciled.nextState["managedSchematicItems"] = schematicReconciled.managedItems;

        bool zoneMutation = false;
        std::set<std::string> expectedZoneIds;

        for( const JSON& managedOperation : managedOperations )
        {
            if( managedOperation.value( "itemType", "" ) == "zone" )
                expectedZoneIds.emplace( managedOperation["itemId"].get<std::string>() );
        }

        for( const JSON& action : reconciled.actions )
        {
            if( action.value( "itemType", "" ) == "zone" )
                zoneMutation = true;
        }

        JSON applyJournal = { { "format", "kichad-kds-apply-journal" },
                              { "version", 1 },
                              { "sourcePath", sourceRelativePath },
                              { "boardPath", boardRelativePath },
                              { "projectName", projectName },
                              { "sourceSha256", compiled.sourceSha256 },
                              { "previousState", previousState },
                              { "preparedState", reconciled.nextState } };

        if( desiredStackup )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            KICHAD::SetAlwaysPrintDefaultValuedFields( options, true );
            KICHAD::PROTOBUF_STATUS status =
                    google::protobuf::util::MessageToJsonString(
                            previousStackup, &serializedPrevious, options );

            if( !status.ok() )
                return failure( "journal_failed", "could not serialize the prior board stackup" );

            applyJournal["previousStackup"] = JSON::parse( serializedPrevious );
        }

        if( desiredTitleBlock )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            KICHAD::SetAlwaysPrintDefaultValuedFields( options, true );
            KICHAD::PROTOBUF_STATUS status =
                    google::protobuf::util::MessageToJsonString(
                            previousTitleBlock, &serializedPrevious, options );

            if( !status.ok() )
            {
                return failure( "journal_failed",
                                "could not serialize the prior board title block" );
            }

            applyJournal["previousTitleBlock"] = JSON::parse( serializedPrevious );
        }

        if( desiredRules )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            KICHAD::SetAlwaysPrintDefaultValuedFields( options, true );
            KICHAD::PROTOBUF_STATUS status =
                    google::protobuf::util::MessageToJsonString(
                            previousRules, &serializedPrevious, options );

            if( !status.ok() )
                return failure( "journal_failed", "could not serialize the prior board rules" );

            applyJournal["previousRules"] = JSON::parse( serializedPrevious );
        }

        if( desiredNetClasses )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            KICHAD::SetAlwaysPrintDefaultValuedFields( options, true );
            KICHAD::PROTOBUF_STATUS status =
                    google::protobuf::util::MessageToJsonString(
                            previousNetClasses, &serializedPrevious, options );

            if( !status.ok() )
            {
                return failure( "journal_failed",
                                "could not serialize the prior netclass settings" );
            }

            applyJournal["previousNetClassSettings"] = JSON::parse( serializedPrevious );
        }

        if( desiredTextVariables )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            KICHAD::SetAlwaysPrintDefaultValuedFields( options, true );
            KICHAD::PROTOBUF_STATUS status =
                    google::protobuf::util::MessageToJsonString(
                            previousTextVariables, &serializedPrevious, options );

            if( !status.ok() )
            {
                return failure( "journal_failed",
                                "could not serialize the prior project text variables" );
            }

            applyJournal["previousTextVariables"] = JSON::parse( serializedPrevious );
        }

        if( desiredFieldTemplates )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            KICHAD::SetAlwaysPrintDefaultValuedFields( options, true );
            KICHAD::PROTOBUF_STATUS status =
                    google::protobuf::util::MessageToJsonString(
                            previousFieldTemplates, &serializedPrevious, options );

            if( !status.ok() )
            {
                return failure( "journal_failed",
                                "could not serialize the prior schematic field templates" );
            }

            applyJournal["previousSchematicFieldTemplates"] =
                    JSON::parse( serializedPrevious );
        }

        if( desiredSchematicRuleSeverities )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            KICHAD::SetAlwaysPrintDefaultValuedFields( options, true );
            KICHAD::PROTOBUF_STATUS status =
                    google::protobuf::util::MessageToJsonString(
                            previousSchematicRuleSeverities, &serializedPrevious, options );

            if( !status.ok() )
            {
                return failure( "journal_failed",
                                "could not serialize the prior schematic rule severities" );
            }

            applyJournal["previousSchematicRuleSeverities"] =
                    JSON::parse( serializedPrevious );
        }

        if( hasDesiredCustomRules )
        {
            applyJournal["previousCustomRules"] = {
                { "present", previousCustomRulesPresent },
                { "source", previousCustomRulesSource }
            };
        }

        if( !libraryTableUpdates.empty() )
        {
            applyJournal["previousProjectLibraryTables"] = JSON::array();

            for( const PROJECT_LIBRARY_TABLE_UPDATE& update : libraryTableUpdates )
            {
                applyJournal["previousProjectLibraryTables"].push_back(
                        { { "kind", update.kind },
                          { "path", update.path.GetFullName().ToStdString() },
                          { "present", update.previousPresent },
                          { "sourceBase64",
                            wxBase64Encode( update.previousSource.data(),
                                            update.previousSource.size() )
                                    .ToStdString() } } );
            }
        }

        if( !managedSymbolLibraryUpdates.empty() )
        {
            applyJournal["previousManagedSymbolLibraries"] = JSON::array();

            for( const MANAGED_SYMBOL_LIBRARY_UPDATE& update : managedSymbolLibraryUpdates )
            {
                applyJournal["previousManagedSymbolLibraries"].push_back(
                        { { "nickname", update.nickname },
                          { "path", update.relativePath },
                          { "present", update.previousPresent },
                          { "sourceBase64",
                            wxBase64Encode( update.previousSource.data(),
                                            update.previousSource.size() )
                                    .ToStdString() } } );
            }
        }

        if( !managedFootprintLibraryUpdates.empty() )
        {
            applyJournal["previousManagedFootprintLibraries"] = JSON::array();

            for( const MANAGED_FOOTPRINT_LIBRARY_UPDATE& update :
                 managedFootprintLibraryUpdates )
            {
                JSON entry = { { "nickname", update.nickname },
                               { "path", update.relativePath },
                               { "present", update.previousPresent },
                               { "files", JSON::array() } };

                for( const auto& [filename, previousSource] : update.previousFiles )
                {
                    entry["files"].push_back(
                            { { "name", filename },
                              { "sourceBase64",
                                wxBase64Encode( previousSource.data(), previousSource.size() )
                                        .ToStdString() } } );
                }

                applyJournal["previousManagedFootprintLibraries"].push_back(
                        std::move( entry ) );
            }
        }

        if( !schematicUpdates.empty() )
        {
            applyJournal["previousSchematicFiles"] = JSON::array();

            for( const SCHEMATIC_FILE_UPDATE& update : schematicUpdates )
            {
                applyJournal["previousSchematicFiles"].push_back(
                        { { "path", update.relativePath },
                          { "present", update.previousPresent },
                          { "sourceBase64",
                            wxBase64Encode( update.previousSource.data(),
                                            update.previousSource.size() )
                                    .ToStdString() } } );
            }
        }

        if( !KICHAD::CODEX_TOOLS::WriteJsonAtomically( journalPath, applyJournal, pathError ) )
            return failure( "journal_failed", pathError );

        bool titleBlockApplied = false;

        if( desiredTitleBlock )
        {
            if( !KICHAD::CODEX_TOOLS::UpdatePcbTitleBlock(
                        client, target, *desiredTitleBlock, pathError ) )
            {
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdatePcbTitleBlock(
                            client, target, previousTitleBlock, rollbackError ) )
                {
                    pathError += "; title-block rollback also failed: " + rollbackError;
                }

                return failure( "title_block_apply_failed",
                                pathError
                                        + "; the apply journal was retained for safe recovery" );
            }

            titleBlockApplied = true;
        }

        auto rollbackTitleBlock = [&]( std::string& aMessage )
        {
            if( !titleBlockApplied )
                return;

            std::string rollbackError;

            if( !KICHAD::CODEX_TOOLS::UpdatePcbTitleBlock(
                        client, target, previousTitleBlock, rollbackError ) )
            {
                aMessage += "; title-block rollback also failed: " + rollbackError;
            }
        };

        bool stackupApplied = false;

        if( desiredStackup )
        {
            if( !KICHAD::CODEX_TOOLS::UpdatePcbStackup( client, target, *desiredStackup, pathError ) )
            {
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdatePcbStackup( client, target, previousStackup, rollbackError ) )
                    pathError += "; stackup rollback also failed: " + rollbackError;

                rollbackTitleBlock( pathError );

                return failure( "stackup_apply_failed",
                                pathError + "; the apply journal was retained for safe recovery" );
            }

            stackupApplied = true;
        }

        auto rollbackStackup = [&]( std::string& aMessage )
        {
            if( stackupApplied )
            {
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdatePcbStackup(
                            client, target, previousStackup, rollbackError ) )
                {
                    aMessage += "; stackup rollback also failed: " + rollbackError;
                }
            }

            rollbackTitleBlock( aMessage );
        };

        bool rulesApplied = false;

        if( desiredRules )
        {
            if( !KICHAD::CODEX_TOOLS::UpdatePcbRules( client, target, *desiredRules, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdatePcbRules( client, target, previousRules, rollbackError ) )
                    message += "; board-rules rollback also failed: " + rollbackError;

                rollbackStackup( message );
                return failure( "rules_apply_failed", message );
            }

            rulesApplied = true;
        }

        auto rollbackBoardSettings = [&]( std::string& aMessage )
        {
            if( rulesApplied )
            {
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdatePcbRules( client, target, previousRules, rollbackError ) )
                    aMessage += "; board-rules rollback also failed: " + rollbackError;
            }

            rollbackStackup( aMessage );
        };

        bool netClassesApplied = false;

        if( desiredNetClasses )
        {
            if( !KICHAD::CODEX_TOOLS::UpdateNetClassSettings( client, target, *desiredNetClasses, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdateNetClassSettings( client, target, previousNetClasses, rollbackError ) )
                    message += "; netclass rollback also failed: " + rollbackError;

                rollbackBoardSettings( message );
                return failure( "netclass_apply_failed", message );
            }

            netClassesApplied = true;
        }

        auto rollbackNetClasses = [&]( std::string& aMessage )
        {
            if( netClassesApplied )
            {
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdateNetClassSettings(
                            client, target, previousNetClasses, rollbackError ) )
                {
                    aMessage += "; netclass rollback also failed: " + rollbackError;
                }
            }

            rollbackBoardSettings( aMessage );
        };

        bool textVariablesApplied = false;

        if( desiredTextVariables )
        {
            if( !KICHAD::PROJECT_SETTINGS_IPC::ReplaceTextVariables(
                        client, target, *desiredTextVariables, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::PROJECT_SETTINGS_IPC::ReplaceTextVariables(
                            client, target, previousTextVariables, rollbackError ) )
                {
                    message += "; text-variable rollback also failed: " + rollbackError;
                }

                rollbackNetClasses( message );
                return failure( "text_variable_apply_failed", message );
            }

            textVariablesApplied = true;
        }

        auto rollbackTextVariables = [&]( std::string& aMessage )
        {
            if( textVariablesApplied )
            {
                std::string rollbackError;

                if( !KICHAD::PROJECT_SETTINGS_IPC::ReplaceTextVariables(
                            client, target, previousTextVariables, rollbackError ) )
                {
                    aMessage += "; text-variable rollback also failed: " + rollbackError;
                }
            }

            rollbackNetClasses( aMessage );
        };

        bool schematicRuleSeveritiesApplied = false;

        if( desiredSchematicRuleSeverities )
        {
            if( !KICHAD::PROJECT_SETTINGS_IPC::ReplaceSchematicRuleSeverities(
                        client, target, *desiredSchematicRuleSeverities, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::PROJECT_SETTINGS_IPC::ReplaceSchematicRuleSeverities(
                            client, target, previousSchematicRuleSeverities,
                            rollbackError ) )
                {
                    message += "; ERC-severity rollback also failed: " + rollbackError;
                }

                rollbackTextVariables( message );
                return failure( "erc_severity_apply_failed", message );
            }

            schematicRuleSeveritiesApplied = true;
        }

        auto rollbackSchematicRuleSeverities = [&]( std::string& aMessage )
        {
            if( schematicRuleSeveritiesApplied )
            {
                std::string rollbackError;

                if( !KICHAD::PROJECT_SETTINGS_IPC::ReplaceSchematicRuleSeverities(
                            client, target, previousSchematicRuleSeverities,
                            rollbackError ) )
                {
                    aMessage += "; ERC-severity rollback also failed: " + rollbackError;
                }
            }

            rollbackTextVariables( aMessage );
        };

        bool fieldTemplatesApplied = false;

        if( desiredFieldTemplates )
        {
            if( !KICHAD::PROJECT_SETTINGS_IPC::ReplaceSchematicFieldTemplates(
                        client, target, *desiredFieldTemplates, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::PROJECT_SETTINGS_IPC::ReplaceSchematicFieldTemplates(
                            client, target, previousFieldTemplates, rollbackError ) )
                {
                    message += "; field-template rollback also failed: " + rollbackError;
                }

                rollbackSchematicRuleSeverities( message );
                return failure( "field_template_apply_failed", message );
            }

            fieldTemplatesApplied = true;
        }

        auto rollbackProjectSettings = [&]( std::string& aMessage )
        {
            if( fieldTemplatesApplied )
            {
                std::string rollbackError;

                if( !KICHAD::PROJECT_SETTINGS_IPC::ReplaceSchematicFieldTemplates(
                            client, target, previousFieldTemplates, rollbackError ) )
                {
                    aMessage += "; field-template rollback also failed: " + rollbackError;
                }
            }

            rollbackSchematicRuleSeverities( aMessage );
        };

        bool customRulesApplied = false;

        if( hasDesiredCustomRules )
        {
            if( !KICHAD::CODEX_TOOLS::UpdatePcbCustomRules( client, target, desiredCustomRulesPresent,
                                       desiredCustomRulesSource, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdatePcbCustomRules( client, target, previousCustomRulesPresent,
                                           previousCustomRulesSource, rollbackError ) )
                {
                    message += "; custom-rules rollback also failed: " + rollbackError;
                }

                rollbackProjectSettings( message );
                return failure( "custom_rules_apply_failed", message );
            }

            customRulesApplied = true;
        }

        auto rollbackAllSettings = [&]( std::string& aMessage )
        {
            if( customRulesApplied )
            {
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdatePcbCustomRules( client, target, previousCustomRulesPresent,
                                           previousCustomRulesSource, rollbackError ) )
                {
                    aMessage += "; custom-rules rollback also failed: " + rollbackError;
                }
            }

            rollbackProjectSettings( aMessage );
        };

        size_t libraryTablesApplied = 0;

        for( PROJECT_LIBRARY_TABLE_UPDATE& update : libraryTableUpdates )
        {
            if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically( update.path, true, update.source, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically( update.path, update.previousPresent,
                                                update.previousSource, rollbackError ) )
                {
                    message += "; current library-table rollback also failed: "
                               + rollbackError;
                }

                for( auto prior = libraryTableUpdates.rbegin();
                     prior != libraryTableUpdates.rend(); ++prior )
                {
                    if( !prior->applied )
                        continue;

                    rollbackError.clear();

                    if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically( prior->path, prior->previousPresent,
                                                    prior->previousSource, rollbackError ) )
                    {
                        message += "; library-table rollback also failed: " + rollbackError;
                    }
                }

                rollbackAllSettings( message );
                return failure( "library_table_apply_failed", message );
            }

            update.applied = true;
            ++libraryTablesApplied;
        }

        size_t managedSymbolLibrariesApplied = 0;

        auto rollbackManagedSymbolLibraries = [&]( std::string& aMessage )
        {
            for( auto update = managedSymbolLibraryUpdates.rbegin();
                 update != managedSymbolLibraryUpdates.rend(); ++update )
            {
                if( !update->applied )
                    continue;

                std::string rollbackError;

                if( !KICHAD::MANAGED_SYMBOL_LIBRARY_IO::InstallAtomically(
                            update->path, update->previousPresent,
                            update->previousSource, rollbackError ) )
                {
                    aMessage += "; managed symbol library rollback also failed: "
                                + rollbackError;
                }
            }
        };

        for( MANAGED_SYMBOL_LIBRARY_UPDATE& update : managedSymbolLibraryUpdates )
        {
            const bool installed = KICHAD::MANAGED_SYMBOL_LIBRARY_IO::InstallAtomically(
                    update.path, true, update.source, pathError );
            const bool nativeValid = installed
                                     && ( m_symbolLibraryValidator
                                                  ? m_symbolLibraryValidator( update.path,
                                                                              pathError )
                                                  : KICHAD::MANAGED_SYMBOL_LIBRARY_IO::ValidateNative(
                                                            update.path, pathError ) );

            if( !nativeValid )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::MANAGED_SYMBOL_LIBRARY_IO::InstallAtomically(
                            update.path, update.previousPresent,
                            update.previousSource, rollbackError ) )
                {
                    message += "; current managed symbol library rollback also failed: "
                               + rollbackError;
                }

                rollbackManagedSymbolLibraries( message );

                for( auto library = libraryTableUpdates.rbegin();
                     library != libraryTableUpdates.rend(); ++library )
                {
                    if( !library->applied )
                        continue;

                    rollbackError.clear();

                    if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically(
                                library->path, library->previousPresent,
                                library->previousSource, rollbackError ) )
                    {
                        message += "; library-table rollback also failed: " + rollbackError;
                    }
                }

                rollbackAllSettings( message );
                return failure( installed ? "managed_symbol_library_validation_failed"
                                          : "managed_symbol_library_apply_failed",
                                message );
            }

            update.applied = true;
            ++managedSymbolLibrariesApplied;
        }

        size_t managedFootprintLibrariesApplied = 0;

        auto rollbackManagedFootprintLibraries = [&]( std::string& aMessage )
        {
            for( auto update = managedFootprintLibraryUpdates.rbegin();
                 update != managedFootprintLibraryUpdates.rend(); ++update )
            {
                if( !update->applied )
                    continue;

                std::string rollbackError;

                if( !KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::InstallAtomically(
                            update->path, update->previousPresent,
                            update->previousFiles, rollbackError ) )
                {
                    aMessage += "; managed footprint library rollback also failed: "
                                + rollbackError;
                }
            }
        };

        for( MANAGED_FOOTPRINT_LIBRARY_UPDATE& update : managedFootprintLibraryUpdates )
        {
            const bool installed = KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::InstallAtomically(
                    update.path, true, update.files, pathError );
            const bool nativeValid = installed
                                     && ( m_footprintLibraryValidator
                                                  ? m_footprintLibraryValidator( update.path,
                                                                                 pathError )
                                                  : KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::ValidateNative(
                                                            update.path, pathError ) );

            if( !nativeValid )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::InstallAtomically(
                            update.path, update.previousPresent,
                            update.previousFiles, rollbackError ) )
                {
                    message += "; current managed footprint library rollback also failed: "
                               + rollbackError;
                }

                rollbackManagedFootprintLibraries( message );
                rollbackManagedSymbolLibraries( message );

                for( auto library = libraryTableUpdates.rbegin();
                     library != libraryTableUpdates.rend(); ++library )
                {
                    if( !library->applied )
                        continue;

                    rollbackError.clear();

                    if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically(
                                library->path, library->previousPresent,
                                library->previousSource, rollbackError ) )
                    {
                        message += "; library-table rollback also failed: " + rollbackError;
                    }
                }

                rollbackAllSettings( message );
                return failure( installed ? "managed_footprint_library_validation_failed"
                                          : "managed_footprint_library_apply_failed",
                                message );
            }

            update.applied = true;
            ++managedFootprintLibrariesApplied;
        }

        size_t schematicFilesApplied = 0;

        auto rollbackSchematicFiles = [&]( std::string& aMessage )
        {
            for( auto update = schematicUpdates.rbegin();
                 update != schematicUpdates.rend(); ++update )
            {
                if( !update->applied )
                    continue;

                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::InstallSchematicAtomically( update->path, update->previousPresent,
                                                 update->previousSource, rollbackError ) )
                {
                    aMessage += "; schematic rollback also failed: " + rollbackError;
                }
            }
        };

        for( SCHEMATIC_FILE_UPDATE& update : schematicUpdates )
        {
            if( !KICHAD::CODEX_TOOLS::InstallSchematicAtomically( update.path, true, update.source, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::InstallSchematicAtomically( update.path, update.previousPresent,
                                                 update.previousSource, rollbackError ) )
                {
                    message += "; current schematic rollback also failed: " + rollbackError;
                }

                rollbackSchematicFiles( message );
                rollbackManagedFootprintLibraries( message );
                rollbackManagedSymbolLibraries( message );

                for( auto library = libraryTableUpdates.rbegin();
                     library != libraryTableUpdates.rend(); ++library )
                {
                    if( !library->applied )
                        continue;

                    rollbackError.clear();

                    if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically( library->path, library->previousPresent,
                                                    library->previousSource, rollbackError ) )
                    {
                        message += "; library-table rollback also failed: " + rollbackError;
                    }
                }

                rollbackAllSettings( message );
                return failure( "schematic_apply_failed", message );
            }

            update.applied = true;
            ++schematicFilesApplied;
        }

        if( schematicFilesApplied > 0 )
        {
            pathError.clear();
            std::vector<wxFileName> validationRoots;
            const std::string rootRelativePath = schematicOperation.value( "rootFile", "" );

            if( !rootRelativePath.empty() )
            {
                auto root = resolvedSchematicPaths.find( rootRelativePath );

                if( root == resolvedSchematicPaths.end() )
                    pathError = "planned root schematic has no bounded path";
                else
                    validationRoots.emplace_back( root->second );
            }
            else
            {
                for( const SCHEMATIC_FILE_UPDATE& update : schematicUpdates )
                    validationRoots.emplace_back( update.path );
            }

            for( const wxFileName& validationRoot : validationRoots )
            {
                const bool nativeValid = pathError.empty()
                                         && ( m_schematicValidator
                                                      ? m_schematicValidator(
                                                                validationRoot, compiled.ir,
                                                                resolvedSymbols.symbols,
                                                                pathError )
                                                      : KICHAD::CODEX_TOOLS::ValidateNativeSchematicHierarchy(
                                                                validationRoot, compiled.ir,
                                                                resolvedSymbols.symbols,
                                                                pathError ) );

                if( !nativeValid )
                {
                    std::string message = pathError
                                          + "; the apply journal was retained for safe recovery";
                    rollbackSchematicFiles( message );
                    rollbackManagedFootprintLibraries( message );
                    rollbackManagedSymbolLibraries( message );

                    for( auto library = libraryTableUpdates.rbegin();
                         library != libraryTableUpdates.rend(); ++library )
                    {
                        if( !library->applied )
                            continue;

                        std::string rollbackError;

                        if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically(
                                    library->path, library->previousPresent,
                                    library->previousSource, rollbackError ) )
                        {
                            message += "; library-table rollback also failed: "
                                       + rollbackError;
                        }
                    }

                    rollbackAllSettings( message );
                    return failure( "schematic_validation_failed", message );
                }
            }
        }

        auto rollbackAllArtifacts = [&]( std::string& aMessage )
        {
            rollbackSchematicFiles( aMessage );
            rollbackManagedFootprintLibraries( aMessage );
            rollbackManagedSymbolLibraries( aMessage );

            for( auto update = libraryTableUpdates.rbegin();
                 update != libraryTableUpdates.rend(); ++update )
            {
                if( !update->applied )
                    continue;

                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically( update->path, update->previousPresent,
                                                update->previousSource, rollbackError ) )
                {
                    aMessage += "; library-table rollback also failed: " + rollbackError;
                }
            }

            rollbackAllSettings( aMessage );
        };

        if( reconciled.actions.empty() )
        {
            const bool boardDocumentChanged = titleBlockApplied || stackupApplied || rulesApplied;

            if( boardDocumentChanged && !saveBoardDocument( client, target, pathError ) )
            {
                return failure(
                        "board_save_failed",
                        pathError
                                + "; the live board may contain unsaved changes and the apply "
                                  "journal was retained for safe recovery" );
            }

            if( !KICHAD::CODEX_TOOLS::WriteJsonAtomically( statePath, reconciled.nextState, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                rollbackAllArtifacts( message );
                return failure( "state_write_failed", message );
            }

            const bool journalRemoved = KICHAD::RemoveFileWithRetry( journalPath.GetFullPath() );
            JSON payload = { { "operation", "apply" },
                             { "path", sourceRelativePath },
                             { "boardPath", boardRelativePath },
                             { "sourceSha256", compiled.sourceSha256 },
                             { "counts", reconciled.counts },
                             { "transaction",
                               titleBlockApplied || stackupApplied || rulesApplied
                                               || netClassesApplied
                                               || textVariablesApplied
                                               || fieldTemplatesApplied
                                               || customRulesApplied || libraryTablesApplied > 0
                                               || managedSymbolLibrariesApplied > 0
                                               || managedFootprintLibrariesApplied > 0
                                               || schematicFilesApplied > 0
                                       ? "design settings applied"
                                       : "no board changes" },
                             { "titleBlockApplied", titleBlockApplied },
                             { "stackupApplied", stackupApplied },
                             { "rulesApplied", rulesApplied },
                             { "netClassesApplied", netClassesApplied },
                             { "textVariablesApplied", textVariablesApplied },
                             { "fieldTemplatesApplied", fieldTemplatesApplied },
                             { "customRulesApplied", customRulesApplied },
                             { "libraryTablesApplied", libraryTablesApplied },
                             { "managedSymbolLibrariesApplied", managedSymbolLibrariesApplied },
                             { "managedFootprintLibrariesApplied",
                               managedFootprintLibrariesApplied },
                             { "footprintModelAssets", footprintModelAssets },
                             { "schematicFilesApplied", schematicFilesApplied },
                             { "schematicCounts", schematicReconciled.counts },
                             { "boardSaved", boardDocumentChanged },
                             { "verification",
                               { { "status", "not_run" },
                                 { "required", JSON::array( { "erc", "drc" } ) } } },
                             { "journalRetained", !journalRemoved } };
            return success( payload );
        }

        KICHAD_IPC_COMMIT_GUARD commit( client, target );

        if( !commit.Begin( pathError ) )
        {
            std::string message = pathError
                                  + "; the apply journal was retained for safe recovery";
            rollbackAllArtifacts( message );
            return failure( "transaction_failed", message );
        }

        if( !KICHAD::CODEX_TOOLS::ExecutePcbActions( client, target, reconciled.actions,
                                footprintSources, pathError ) )
        {
            std::string dropError;
            const bool  dropped = commit.Drop( dropError );
            std::string message = pathError + "; the apply journal was retained for safe recovery";

            if( !dropped && !dropError.empty() )
                message += "; transaction drop also failed: " + dropError;

            rollbackAllArtifacts( message );

            return failure( "apply_failed", message );
        }

        if( !commit.Commit( "Apply KiChad Design Script " + sourceRelativePath, pathError ) )
        {
            std::string dropError;
            const bool  dropped = commit.Drop( dropError );
            std::string message = pathError + "; the apply journal was retained for safe recovery";

            if( !dropped && !dropError.empty() )
                message += "; transaction drop also failed: " + dropError;

            rollbackAllArtifacts( message );

            return failure( "transaction_failed", message );
        }

        if( zoneMutation && !KICHAD::CODEX_TOOLS::RefillPcbZones( client, target, expectedZoneIds, pathError ) )
        {
            return failure( "zone_refill_failed",
                            pathError + "; the committed board and retained journal can be "
                                        "reconciled safely on the next apply" );
        }

        if( !saveBoardDocument( client, target, pathError ) )
        {
            return failure(
                    "board_save_failed",
                    pathError
                            + "; the committed board remains live but may be unsaved, and the "
                              "apply journal was retained for safe recovery" );
        }

        if( !KICHAD::CODEX_TOOLS::WriteJsonAtomically( statePath, reconciled.nextState, pathError ) )
        {
            return failure( "state_write_failed",
                            pathError + "; the committed board and retained journal can be "
                                        "reconciled safely on the next apply" );
        }

        const bool journalRemoved = KICHAD::RemoveFileWithRetry( journalPath.GetFullPath() );
        JSON payload = { { "operation", "apply" },
                         { "path", sourceRelativePath },
                         { "boardPath", boardRelativePath },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "counts", reconciled.counts },
                         { "managedItems",
                           reconciled.nextState["managedPcbItems"].size() },
                         { "transaction", "committed" },
                         { "titleBlockApplied", titleBlockApplied },
                         { "stackupApplied", stackupApplied },
                         { "rulesApplied", rulesApplied },
                         { "netClassesApplied", netClassesApplied },
                         { "textVariablesApplied", textVariablesApplied },
                         { "fieldTemplatesApplied", fieldTemplatesApplied },
                         { "customRulesApplied", customRulesApplied },
                         { "libraryTablesApplied", libraryTablesApplied },
                         { "managedSymbolLibrariesApplied", managedSymbolLibrariesApplied },
                         { "managedFootprintLibrariesApplied",
                           managedFootprintLibrariesApplied },
                         { "footprintModelAssets", footprintModelAssets },
                         { "schematicFilesApplied", schematicFilesApplied },
                         { "schematicCounts", schematicReconciled.counts },
                         { "zonesRefilled", zoneMutation ? expectedZoneIds.size() : 0 },
                         { "boardSaved", true },
                         { "verification",
                           { { "status", "not_run" },
                             { "required", JSON::array( { "erc", "drc" } ) } } },
                         { "statePath", statePath.GetFullName().ToStdString() },
                         { "journalRetained", !journalRemoved } };
        return success( payload );
    }

    if( !aMutationAvailable )
        return failure( "snapshot_required",
                        "A pre-turn project snapshot is required to save or patch KDS" );

    if( !compiled.ok )
    {
        std::string message = "KiChad Design Script did not pass compilation";

        if( !compiled.diagnostics.empty() )
            message += ": " + compiled.diagnostics.front().value( "message", "invalid program" );

        return failure( "compile_failed", message,
                        { { "sourceSha256", compiled.sourceSha256 },
                          { "diagnostics", compiled.diagnostics } } );
    }

    if( sidecar.FileExists() )
    {
        if( !aArguments.contains( "expectedSha256" )
            || !aArguments["expectedSha256"].is_string() )
        {
            return failure(
                    "stale_source",
                    "expectedSha256 is required when replacing or patching an existing sidecar",
                    { { "expectedSha256", nullptr },
                      { "observed", "an existing sidecar requires a revision guard" } } );
        }

        std::string existing;

        if( !KICHAD::CODEX_TOOLS::ReadDesignScriptSidecar( sidecar, existing, pathError ) )
            return failure( "read_failed", pathError );

        std::string existingSha256;
        picosha2::hash256_hex_string( existing, existingSha256 );

        if( aArguments["expectedSha256"].get<std::string>() != existingSha256 )
        {
            return failure(
                    "stale_source", "sidecar changed since it was loaded",
                    { { "expectedSha256", aArguments["expectedSha256"] },
                      { "actualSha256", existingSha256 } } );
        }
    }

    if( !KICHAD::CODEX_TOOLS::InstallDesignScriptSidecarAtomically(
                sidecar, source, pathError ) )
    {
        return failure( "write_failed", pathError );
    }

    JSON payload = { { "operation", operation },
                     { "path", aArguments["path"] },
                     { "bytes", source.size() },
                     { "sourceSha256", compiled.sourceSha256 },
                     { "valid", true },
                     { "plan", compiled.plan },
                     { "transaction",
                       operation == "patch" ? "snapshot-backed atomic patch"
                                            : "snapshot-backed atomic save" } };

    if( operation == "patch" )
    {
        payload["previousSha256"] = previousSha256;
        payload["editsApplied"] = aArguments["edits"].size();
    }

    return success( payload );
}
