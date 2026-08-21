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

#include <exception>
#include <initializer_list>
#include <utility>


namespace
{

using JSON = nlohmann::json;


bool codeIsOneOf( const std::string& aCode, std::initializer_list<const char*> aCodes )
{
    for( const char* code : aCodes )
    {
        if( aCode == code )
            return true;
    }

    return false;
}


std::string failureState( const std::string& aCode, const std::string& aTool,
                          const std::string& aOperation )
{
    const bool mutating = ( aTool == "design"
                            && ( aOperation == "apply" || aOperation == "save"
                                 || aOperation == "patch" ) )
                          || ( aTool == "pcb" && aOperation == "mutate" )
                          || ( aTool == "layout"
                               && ( aOperation == "adopt" || aOperation == "revert"
                                    || aOperation == "reconcile" ) );

    if( !mutating || codeIsOneOf( aCode, { "unknown_tool", "invalid_arguments",
                                           "project_unavailable", "invalid_path", "read_failed",
                                           "file_too_large", "parse_failed", "format_mismatch",
                                           "invalid_source", "compile_failed", "stale_source",
                                           "patch_context_not_found",
                                           "patch_context_ambiguous",
                                           "snapshot_required", "dependency_unavailable",
                                           "pcb_editor_unavailable", "backend_incomplete",
                                           "invalid_plan", "symbol_generation_failed",
                                           "footprint_generation_failed" } )
        || aCode.find( "inventory_failed" ) != std::string::npos )
    {
        return "none";
    }

    return "unknown; inspect the affected document before retrying";
}


bool failureIsRetryable( const std::string& aCode )
{
    return codeIsOneOf( aCode, { "stale_source", "patch_context_not_found",
                                 "patch_context_ambiguous", "dependency_unavailable",
                                 "pcb_editor_unavailable", "ipc_failed", "read_failed",
                                 "write_failed", "check_failed", "transaction_failed" } );
}


JSON safeContext( const std::string& aTool, const JSON& aArguments )
{
    JSON context = { { "tool", aTool } };

    if( !aArguments.is_object() )
        return context;

    for( const char* name : { "operation", "path", "boardPath", "schematicPath", "action",
                              "itemType" } )
    {
        auto value = aArguments.find( name );

        if( value != aArguments.end() && ( value->is_string() || value->is_number()
                                            || value->is_boolean() ) )
        {
            context[name] = *value;
        }
    }

    return context;
}


std::string stringArgument( const JSON& aArguments, const char* aName )
{
    if( !aArguments.is_object() )
        return {};

    auto value = aArguments.find( aName );
    return value != aArguments.end() && value->is_string()
                   ? value->get<std::string>()
                   : std::string();
}


JSON retryStep( const std::string& aTool, const JSON& aArguments,
                const std::string& aPurpose )
{
    return { { "action", "call_tool" }, { "tool", aTool },
             { "arguments", aArguments }, { "purpose", aPurpose } };
}


JSON recoveryFor( const std::string& aCode, const std::string& aTool,
                  const JSON& aArguments )
{
    const std::string operation = stringArgument( aArguments, "operation" );
    const std::string path = stringArgument( aArguments, "path" );
    JSON recovery = { { "summary", "Do not retry this request unchanged." },
                      { "steps", JSON::array() } };
    JSON& steps = recovery["steps"];

    if( aCode == "stale_source" && aTool == "design" && operation == "apply" && !path.empty() )
    {
        recovery["summary"] =
                "Recompile the current KDS source and retry apply with the returned digest.";
        steps.push_back( retryStep( "design", { { "operation", "compile" }, { "path", path } },
                                    "Read and compile the current sidecar revision." ) );
        JSON retryArguments = { { "operation", "apply" }, { "path", path },
                                { "expectedSha256", "$previous.data.sourceSha256" } };

        if( aArguments.contains( "boardPath" ) && aArguments["boardPath"].is_string() )
            retryArguments["boardPath"] = aArguments["boardPath"];

        steps.push_back( retryStep( "design", retryArguments,
                                    "Apply exactly the revision returned by design.compile." ) );
    }
    else if( aCode == "stale_source" && aTool == "design" && operation == "patch"
             && !path.empty() )
    {
        recovery["summary"] =
                "Search the current sidecar, reconcile the intended edits, and retry against its digest.";
        steps.push_back( retryStep(
                "design",
                { { "operation", "search" }, { "path", path },
                  { "query", "$original.arguments.edits[0].oldText" } },
                "Locate exact current source text and obtain sourceSha256." ) );
        steps.push_back( { { "action", "retry_tool" }, { "tool", "design" },
                           { "useOriginalArguments", JSON::array( { "path", "edits" } ) },
                           { "arguments",
                             { { "operation", "patch" },
                               { "expectedSha256", "$previous.data.sourceSha256" } } },
                           { "purpose", "Patch only after reconciling concurrent changes." } } );
    }
    else if( aCode == "stale_source" && aTool == "design" && operation == "save"
             && !path.empty() )
    {
        recovery["summary"] =
                "Read the current sidecar, reconcile the intended replacement, and save against its digest.";
        steps.push_back( retryStep( "design", { { "operation", "read" }, { "path", path } },
                                    "Obtain the current source and sourceSha256." ) );
        steps.push_back( { { "action", "retry_tool" }, { "tool", "design" },
                           { "useOriginalArguments", JSON::array( { "path", "source" } ) },
                           { "arguments",
                             { { "operation", "save" },
                               { "expectedSha256", "$previous.data.sourceSha256" } } },
                           { "purpose", "Save only after reconciling concurrent changes." } } );
    }
    else if( codeIsOneOf( aCode,
                          { "patch_context_not_found", "patch_context_ambiguous" } )
             && aTool == "design" && operation == "patch" && !path.empty() )
    {
        recovery["summary"] =
                "Search or read the current source and retry with oldText that occurs exactly once.";
        steps.push_back( retryStep( "design",
                                    { { "operation", "search" }, { "path", path },
                                      { "query",
                                        "$original.arguments.edits[$error.details.editIndex].oldText" } },
                                    "Locate exact current source text and obtain sourceSha256." ) );
        steps.push_back( { { "action", "retry_tool" }, { "tool", "design" },
                           { "useOriginalArguments", JSON::array( { "path", "edits" } ) },
                           { "arguments",
                             { { "operation", "patch" },
                               { "expectedSha256", "$previous.data.sourceSha256" } } },
                           { "purpose",
                             "Use enough exact surrounding text to make every edit unique." } } );
    }
    else if( codeIsOneOf( aCode, { "compile_failed", "invalid_source" } )
             && aTool == "design" && operation == "patch" )
    {
        recovery["summary"] =
                "The patch candidate was not committed; correct its edits using the returned diagnostics.";
        steps.push_back( { { "action", "inspect_failure_details" },
                           { "purpose",
                             "Use every returned candidate diagnostic without changing the current file." } } );
        steps.push_back( { { "action", "retry_tool" }, { "tool", "design" },
                           { "useOriginalArguments",
                             JSON::array( { "path", "expectedSha256" } ) },
                           { "arguments", { { "operation", "patch" } } },
                           { "purpose",
                             "Submit corrected ordered edits against the unchanged source revision." } } );
    }
    else if( aCode == "compile_failed" || aCode == "invalid_source" )
    {
        recovery["summary"] =
                "Correct every returned KDS diagnostic, then compile again before saving or applying.";
        steps.push_back( { { "action", "edit_kds" },
                           { "purpose", "Fix the reported diagnostic locations and messages." } } );

        if( !path.empty() )
        {
            steps.push_back( retryStep( "design",
                                        { { "operation", "compile" }, { "path", path } },
                                        "Confirm that the corrected sidecar is valid." ) );
        }
    }
    else if( codeIsOneOf( aCode, { "dependency_unavailable", "pcb_editor_unavailable",
                                   "ipc_failed" } ) )
    {
        recovery["summary"] =
                "Inspect the required editor connection, then retry only after it reports ready.";

        std::string boardPath = stringArgument( aArguments, "boardPath" );

        if( boardPath.empty() )
            boardPath = path;

        if( !boardPath.empty() )
        {
            steps.push_back( retryStep( "pcb",
                                        { { "operation", "status" }, { "path", boardPath } },
                                        "Confirm that the exact PCB is open through KiCad IPC." ) );
        }

        steps.push_back( { { "action", "retry_original_tool" },
                           { "purpose", "Retry once after the dependency is ready." } } );
    }
    else if( aCode == "invalid_arguments" || aCode == "unknown_tool" )
    {
        recovery["summary"] =
                "Correct the request using the advertised native tool schema before retrying.";
        steps.push_back( { { "action", "inspect_tool_schema" }, { "tool", aTool },
                           { "purpose", "Use only supported operations and argument types." } } );
    }
    else if( codeIsOneOf( aCode, { "invalid_path", "read_failed", "project_unavailable" } ) )
    {
        recovery["summary"] =
                "Refresh project context and use an existing project-relative path.";
        steps.push_back( retryStep( "project", { { "operation", "context" } },
                                    "Refresh the active project's files and paths." ) );
    }
    else if( aCode == "snapshot_required" )
    {
        recovery["summary"] =
                "The host must capture a pre-turn project snapshot before mutation; retrying in the same unprotected state is unsafe.";
        steps.push_back( { { "action", "start_protected_turn" },
                           { "purpose", "Have KiChad create the required undo snapshot." } } );
    }
    else if( aCode.find( "gate_failed" ) != std::string::npos
             || aCode.find( "check_failed" ) != std::string::npos )
    {
        recovery["summary"] =
                "Run the failing verification operation, correct every reported violation, and rerun the gate.";
        steps.push_back( { { "action", "inspect_failure_details" },
                           { "purpose", "Use the returned verification diagnostics as the repair list." } } );
    }
    else if( aCode == "backend_incomplete" )
    {
        recovery["summary"] =
                "This KDS feature has no complete native backend. Do not retry or silently omit it.";
        steps.push_back( { { "action", "report_capability_gap" },
                           { "purpose", "Preserve the design intent and identify the missing backend." } } );
    }
    else
    {
        steps.push_back( { { "action", "inspect_failure_details" },
                           { "purpose", "Resolve the reported condition before retrying." } } );
    }

    return recovery;
}

} // namespace


CODEX_TOOL_REGISTRY::CODEX_TOOL_REGISTRY( std::function<wxString()> aProjectPathProvider,
                                          std::function<bool()> aMutationGuard,
                                          std::function<wxString()> aIpcSocketDirectoryProvider,
                                          std::function<bool( const wxFileName&, std::string& )>
                                                  aSchematicValidator,
                                          NATIVE_CHECK_RUNNER aNativeCheckRunner,
                                          NATIVE_FABRICATION_RUNNER aNativeFabricationRunner,
                                          std::function<bool( const wxFileName&, std::string& )>
                                                  aSymbolLibraryValidator,
                                          std::function<bool( const wxFileName&, std::string& )>
                                                  aFootprintLibraryValidator,
                                          NATIVE_PREVIEW_RUNNER aNativePreviewRunner ) :
        m_projectPathProvider( std::move( aProjectPathProvider ) ),
        m_mutationGuard( std::move( aMutationGuard ) ),
        m_ipcSocketDirectoryProvider( std::move( aIpcSocketDirectoryProvider ) ),
        m_schematicValidator( std::move( aSchematicValidator ) ),
        m_nativeCheckRunner( std::move( aNativeCheckRunner ) ),
        m_nativeFabricationRunner( std::move( aNativeFabricationRunner ) ),
        m_symbolLibraryValidator( std::move( aSymbolLibraryValidator ) ),
        m_footprintLibraryValidator( std::move( aFootprintLibraryValidator ) ),
        m_nativePreviewRunner( std::move( aNativePreviewRunner ) )
{}


void CODEX_TOOL_REGISTRY::SetExternalLayoutTool( const wxString& aPath )
{
    std::lock_guard<std::mutex> lock( m_externalLayoutToolMutex );
    m_externalLayoutTool = aPath.Clone();
}


wxString CODEX_TOOL_REGISTRY::ExternalLayoutTool() const
{
    std::lock_guard<std::mutex> lock( m_externalLayoutToolMutex );
    return m_externalLayoutTool.Clone();
}


void CODEX_TOOL_REGISTRY::SetExternalLayoutEnabled( bool aEnabled )
{
    std::lock_guard<std::mutex> lock( m_externalLayoutToolMutex );
    m_externalLayoutEnabled = aEnabled;
}


bool CODEX_TOOL_REGISTRY::ExternalLayoutEnabled() const
{
    std::lock_guard<std::mutex> lock( m_externalLayoutToolMutex );
    return m_externalLayoutEnabled;
}


void CODEX_TOOL_REGISTRY::SetExternalLayoutLayers( int aLayers )
{
    std::lock_guard<std::mutex> lock( m_externalLayoutToolMutex );
    m_externalLayoutLayers = aLayers;
}


int CODEX_TOOL_REGISTRY::ExternalLayoutLayers() const
{
    std::lock_guard<std::mutex> lock( m_externalLayoutToolMutex );
    return m_externalLayoutLayers;
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::Specs() const
{
    JSON specs = JSON::array( { KICHAD::CODEX_TOOLS::ProjectSpec(),
                                KICHAD::CODEX_TOOLS::InspectSpec(),
                                KICHAD::CODEX_TOOLS::DesignSpec(),
                                KICHAD::CODEX_TOOLS::PcbSpec(),
                                KICHAD::CODEX_TOOLS::VerifySpec(),
                                KICHAD::CODEX_TOOLS::FabricateSpec() } );

    // Advertising the layout tool while the user has external layout disabled invites
    // the agent to treat the switched-off tool as a blocker instead of routing in the
    // KDS itself.  Threads bind tools at start, so a preference flip takes effect on
    // the next new conversation.
    if( ExternalLayoutEnabled() )
        specs.push_back( KICHAD::CODEX_TOOLS::LayoutSpec() );

    return specs;
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::Handle( const std::string& aTool,
                                                        const JSON& aArguments ) const
{
    wxString socketDirectory =
            m_ipcSocketDirectoryProvider ? m_ipcSocketDirectoryProvider() : wxString();
    return HandleWithContext( aTool, aArguments, projectPath(),
                              m_mutationGuard && m_mutationGuard(), socketDirectory );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::HandleWithContext(
        const std::string& aTool, const JSON& aArguments, const wxString& aProjectPath,
        bool aMutationAvailable, const wxString& aIpcSocketDirectory,
        bool aFinalActionApproved, std::chrono::milliseconds aIpcTimeout,
        RUNTIME_DEPENDENCY_RESOLVER aDependencyResolver ) const
{
    JSON result;

    try
    {
        if( aTool == "project" )
            result = handleProject( aArguments, aProjectPath, aMutationAvailable );
        else if( aTool == "inspect" )
            result = handleInspect( aArguments, aProjectPath );
        else if( aTool == "design" )
            result = handleDesign( aArguments, aProjectPath, aMutationAvailable,
                                   aIpcSocketDirectory, aIpcTimeout, aDependencyResolver );
        else if( aTool == "pcb" )
            result = handlePcb( aArguments, aProjectPath, aMutationAvailable,
                                aIpcSocketDirectory, aIpcTimeout, aDependencyResolver );
        else if( aTool == "verify" )
            result = handleVerify( aArguments, aProjectPath );
        else if( aTool == "layout" )
            result = handleLayout( aArguments, aProjectPath, aMutationAvailable );
        else if( aTool == "fabricate" )
        {
            result = handleFabricate( aArguments, aProjectPath, aMutationAvailable,
                                      aFinalActionApproved );
        }
        else
            result = failure( "unknown_tool", "The requested tool is not advertised by KiChad" );
    }
    catch( const std::exception& error )
    {
        result = failure( "tool_failed", error.what() );
    }

    return addFailureContext( std::move( result ), aTool, aArguments );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::success( const JSON& aPayload ) const
{
    JSON envelope = { { "ok", true }, { "data", aPayload } };
    return { { "contentItems", JSON::array( { { { "type", "inputText" },
                                                { "text", envelope.dump() } } } ) },
             { "success", true } };
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::failure( const std::string& aCode,
                                                        const std::string& aMessage,
                                                        const JSON& aDetails ) const
{
    JSON error = { { "code", aCode }, { "message", aMessage } };

    if( !aDetails.empty() )
        error["details"] = aDetails;

    JSON envelope = { { "ok", false }, { "error", std::move( error ) } };
    return { { "contentItems", JSON::array( { { { "type", "inputText" },
                                                { "text", envelope.dump() } } } ) },
             { "success", false } };
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::addFailureContext(
        JSON aResult, const std::string& aTool, const JSON& aArguments ) const
{
    if( !aResult.is_object() || aResult.value( "success", true )
        || !aResult.contains( "contentItems" ) || !aResult["contentItems"].is_array()
        || aResult["contentItems"].empty()
        || !aResult["contentItems"][0].contains( "text" )
        || !aResult["contentItems"][0]["text"].is_string() )
    {
        return aResult;
    }

    JSON envelope = JSON::parse(
            aResult["contentItems"][0]["text"].get<std::string>(), nullptr, false );

    if( !envelope.is_object() || !envelope.contains( "error" )
        || !envelope["error"].is_object() )
    {
        return aResult;
    }

    JSON& error = envelope["error"];
    const std::string code = error.value( "code", "tool_failed" );
    const std::string operation = stringArgument( aArguments, "operation" );
    error["contractVersion"] = 1;
    error["stage"] = operation.empty() ? aTool : aTool + "." + operation;
    error["context"] = safeContext( aTool, aArguments );
    error["stateChanged"] = failureState( code, aTool, operation );
    error["retryable"] = failureIsRetryable( code );
    error["recovery"] = recoveryFor( code, aTool, aArguments );
    aResult["contentItems"][0]["text"] = envelope.dump();
    return aResult;
}


wxString CODEX_TOOL_REGISTRY::projectPath() const
{
    wxString path = m_projectPathProvider ? m_projectPathProvider() : wxString();

    return path;
}
