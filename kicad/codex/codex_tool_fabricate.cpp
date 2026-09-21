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
#include "design_release_writer.h"
#include "design_script_compiler.h"
#include "production_package_writer.h"

#include <build_version.h>
#include <kiid.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

#include <wx/filename.h>


namespace KICHAD::CODEX_TOOLS
{

nlohmann::json FabricateSpec()
{
    nlohmann::json schema = {
        { "type", "object" },
        { "additionalProperties", false },
        { "required", nlohmann::json::array(
                              { "operation", "path", "boardPath", "schematicPath",
                                "expectedSha256" } ) }
    };
    schema["properties"]["operation"] =
            { { "type", "string" }, { "enum", nlohmann::json::array( { "plan", "export" } ) } };
    schema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative canonical .kicad_kds sidecar." } };
    schema["properties"]["boardPath"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative on-disk .kicad_pcb input." } };
    schema["properties"]["schematicPath"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative root .kicad_sch input." } };
    schema["properties"]["expectedSha256"] =
            { { "type", "string" }, { "minLength", 64 }, { "maxLength", 64 },
              { "description", "Exact compiled KDS revision to release." } };
    schema["properties"]["allowWaivers"] =
            { { "type", "boolean" },
              { "description",
                "Allow explicitly ignored/excluded ERC or DRC checks in a confirmed export; "
                "defaults to false." } };

    return { { "type", "function" },
             { "name", "fabricate" },
             { "description",
               "Plan or perform the final KiCad 10 fabrication export declared by the exact "
               "KDS revision. Export reruns ERC, DRC, and sourcing gates, writes a private "
               "staging package, validates every artifact, includes any hash-bound firmware "
               "and programming/bring-up plan, and atomically installs it only after visible "
               "user confirmation. Plan reports manufacturing productionReady separately from "
               "complete runningReady status." },
             { "inputSchema", std::move( schema ) } };
}

} // namespace KICHAD::CODEX_TOOLS


bool CODEX_TOOL_REGISTRY::RequiresFinalConfirmation( const std::string& aTool,
                                                     const JSON& aArguments )
{
    return aTool == "fabricate" && aArguments.is_object()
           && aArguments.contains( "operation" ) && aArguments["operation"].is_string()
           && aArguments["operation"].get<std::string>() == "export";
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleFabricate(
        const JSON& aArguments, const wxString& aProjectPath, bool aMutationAvailable,
        bool aFinalActionApproved ) const
{
    static const std::set<std::string> ALLOWED_ARGUMENTS = {
        "operation", "path", "boardPath", "schematicPath", "expectedSha256", "allowWaivers"
    };

    if( !aArguments.is_object() || aArguments.dump().size() > 32 * 1024 )
        return failure( "invalid_arguments", "fabricate arguments must be a bounded object" );

    for( auto argument = aArguments.begin(); argument != aArguments.end(); ++argument )
    {
        if( !ALLOWED_ARGUMENTS.contains( argument.key() ) )
            return failure( "invalid_arguments", "fabricate contains an unknown argument" );
    }

    for( const char* required :
         { "operation", "path", "boardPath", "schematicPath", "expectedSha256" } )
    {
        if( !aArguments.contains( required ) || !aArguments[required].is_string() )
        {
            return failure( "invalid_arguments",
                            std::string( "fabricate." ) + required + " must be a string" );
        }
    }

    if( aArguments.contains( "allowWaivers" ) && !aArguments["allowWaivers"].is_boolean() )
        return failure( "invalid_arguments", "fabricate.allowWaivers must be a boolean" );

    const std::string operation = aArguments["operation"].get<std::string>();

    if( operation != "plan" && operation != "export" )
        return failure( "invalid_arguments", "fabricate.operation must be 'plan' or 'export'" );

    const std::string expectedSha256 = aArguments["expectedSha256"].get<std::string>();

    if( expectedSha256.size() != 64
        || !std::all_of( expectedSha256.begin(), expectedSha256.end(),
                         []( unsigned char aCharacter )
                         {
                             return std::isdigit( aCharacter )
                                    || ( aCharacter >= 'a' && aCharacter <= 'f' );
                         } ) )
    {
        return failure( "invalid_arguments",
                        "fabricate.expectedSha256 must be one lowercase SHA-256 digest" );
    }

    if( !wxFileName::DirExists( aProjectPath ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    const std::string sourceRelativePath = aArguments["path"].get<std::string>();
    const std::string boardRelativePath = aArguments["boardPath"].get<std::string>();
    const std::string schematicRelativePath =
            aArguments["schematicPath"].get<std::string>();
    wxFileName sidecar;
    wxFileName board;
    wxFileName schematic;
    std::string pathError;

    if( !KICHAD::CODEX_TOOLS::ResolveProjectSidecar( aProjectPath, sourceRelativePath, sidecar, pathError ) )
        return failure( "invalid_path", pathError );

    if( !sidecar.FileExists() )
        return failure( "read_failed", "KiChad Design Script sidecar does not exist" );

    if( !KICHAD::CODEX_TOOLS::ResolveProjectFile( aProjectPath, boardRelativePath, board, pathError ) )
        return failure( "invalid_path", pathError );

    if( board.GetExt() != wxS( "kicad_pcb" ) )
        return failure( "invalid_path", "fabrication requires a .kicad_pcb board" );

    if( !KICHAD::CODEX_TOOLS::ResolveProjectFile( aProjectPath, schematicRelativePath, schematic, pathError ) )
        return failure( "invalid_path", pathError );

    if( schematic.GetExt() != wxS( "kicad_sch" ) )
        return failure( "invalid_path", "fabrication requires a root .kicad_sch schematic" );

    if( !KICHAD::CODEX_TOOLS::ValidateExactNativeFormat( board, "kicad_pcb", "20260206", pathError )
        || !KICHAD::CODEX_TOOLS::ValidateExactNativeFormat( schematic, "kicad_sch", "20260306", pathError ) )
    {
        return failure( "format_version_mismatch", pathError );
    }

    std::string source;

    if( !KICHAD::CODEX_TOOLS::ReadDesignScriptSidecar( sidecar, source, pathError ) )
        return failure( "read_failed", pathError );

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    if( !compiled.ok )
    {
        return failure( "compile_failed",
                        "KDS must compile before fabrication; use design.compile for "
                        "structured diagnostics" );
    }

    if( compiled.sourceSha256 != expectedSha256 )
        return failure( "stale_source", "KDS changed after the approved revision was compiled" );

    const std::string fileStem( board.GetName().ToUTF8() );
    const std::string schematicStem( schematic.GetName().ToUTF8() );
    const std::string projectName = compiled.ir["project"].value( "name", "" );

    if( fileStem.empty() || fileStem != schematicStem || fileStem != projectName )
    {
        return failure( "project_mismatch",
                        "KDS project, root schematic, and board must have the same file stem" );
    }

    JSON plan = KICHAD::CODEX_TOOLS::BuildFabricationPlan( compiled.ir, fileStem );
    plan["operation"] = "plan";
    plan["path"] = sourceRelativePath;
    plan["boardPath"] = boardRelativePath;
    plan["schematicPath"] = schematicRelativePath;
    plan["sourceSha256"] = compiled.sourceSha256;
    plan["kicadVersion"] = std::string( GetMajorMinorPatchVersion().ToUTF8() );
    plan["nativeInputFormats"] = { { "board", "20260206" },
                                    { "schematic", "20260306" } };
    std::string boardIntentError;
    const bool boardIntentValid =
            KICHAD::CODEX_TOOLS::ValidateBoardFabricationIntent( board, compiled.ir, plan, boardIntentError );
    plan["nativeBoardIntentValid"] = boardIntentValid;

    if( !boardIntentValid )
    {
        plan["productionReady"] = false;
        plan["issues"].push_back( { { "type", "native_board_intent_mismatch" },
                                    { "severity", "error" },
                                    { "description", boardIntentError } } );
    }

    if( operation == "plan" )
        return success( plan );

    if( !boardIntentValid )
        return failure( "native_board_intent_mismatch", boardIntentError );

    if( !plan["productionReady"].get<bool>() )
        return failure( "incomplete_fabrication_intent",
                        "KDS is missing one or more production checks, outputs, or stackup" );

    if( !aMutationAvailable )
    {
        return failure( "snapshot_required",
                        "A complete pre-turn project snapshot is required for fabrication" );
    }

    if( !aFinalActionApproved )
    {
        return failure( "permission_required",
                        "Visible user confirmation is required for final fabrication export" );
    }

    const bool allowWaivers = aArguments.value( "allowWaivers", false );
    std::string boardSha256;
    std::string schematicSha256;

    if( !KICHAD::CODEX_TOOLS::HashFabricationFile( board.GetFullPath().ToStdString(), boardSha256 )
        || !KICHAD::CODEX_TOOLS::HashFabricationFile( schematic.GetFullPath().ToStdString(), schematicSha256 ) )
    {
        return failure( "read_failed", "could not hash the complete native design inputs" );
    }

    wxFileName projectRoot = wxFileName::DirName( aProjectPath );
    projectRoot.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !KICHAD::CODEX_TOOLS::CanonicalizeExisting( projectRoot, true ) )
        return failure( "project_unavailable", "active project directory could not be resolved" );

    KICHAD::CODEX_TOOLS::PRIVATE_TEMPORARY_DIRECTORY verificationSnapshot;
    std::vector<std::string> productionInputs;

    if( compiled.ir["production"].is_object() )
    {
        for( const JSON& firmware : compiled.ir["production"]["firmware"] )
        {
            if( firmware.contains( "path" ) )
                productionInputs.emplace_back( firmware["path"].get<std::string>() );
        }
    }

    if( !KICHAD::CODEX_TOOLS::CreateFabricationVerificationSnapshot(
                projectRoot, board, schematic, sidecar,
                { sourceRelativePath, boardRelativePath, schematicRelativePath },
                productionInputs,
                verificationSnapshot, pathError ) )
    {
        return failure( "verification_snapshot_failed", pathError );
    }

    const wxString verificationProjectPath =
            wxString::FromUTF8( verificationSnapshot.Path().string().c_str() );
    wxFileName verificationBoard;
    wxFileName verificationSchematic;

    if( !KICHAD::CODEX_TOOLS::ResolveProjectFile( verificationProjectPath, boardRelativePath,
                             verificationBoard, pathError ) )
    {
        return failure( "verification_snapshot_failed",
                        "private board snapshot could not be resolved: " + pathError );
    }

    if( !KICHAD::CODEX_TOOLS::ResolveProjectFile(
                verificationProjectPath, schematicRelativePath,
                verificationSchematic, pathError ) )
    {
        return failure( "verification_snapshot_failed",
                        "private schematic snapshot could not be resolved: " + pathError );
    }

    const auto decodeResult = [&]( const JSON& aResult, JSON& aData,
                                   std::string& aError )
    {
        if( !aResult.is_object() || !aResult.value( "success", false )
            || !aResult.contains( "contentItems" ) || !aResult["contentItems"].is_array()
            || aResult["contentItems"].empty()
            || !aResult["contentItems"][0].contains( "text" )
            || !aResult["contentItems"][0]["text"].is_string() )
        {
            if( aResult.is_object() && aResult.contains( "contentItems" )
                && aResult["contentItems"].is_array() && !aResult["contentItems"].empty()
                && aResult["contentItems"][0].contains( "text" )
                && aResult["contentItems"][0]["text"].is_string() )
            {
                JSON envelope = JSON::parse(
                        aResult["contentItems"][0]["text"].get<std::string>(), nullptr, false );

                if( envelope.is_object() && envelope.contains( "error" ) )
                    aError = envelope["error"].value( "message", "verification failed" );
            }

            if( aError.empty() )
                aError = "verification tool returned an invalid failure envelope";

            return false;
        }

        JSON envelope = JSON::parse(
                aResult["contentItems"][0]["text"].get<std::string>(), nullptr, false );

        if( !envelope.is_object() || !envelope.value( "ok", false )
            || !envelope.contains( "data" ) || !envelope["data"].is_object() )
        {
            aError = "verification tool returned an invalid success envelope";
            return false;
        }

        aData = std::move( envelope["data"] );
        return true;
    };
    JSON checks = JSON::object();
    const bool hasElectricalDesign =
            !compiled.ir.at( "schematic" ).at( "components" ).empty()
            || !compiled.ir.at( "schematic" ).at( "nets" ).empty();

    for( const auto& [kind, path] :
        std::array<std::pair<const char*, std::string>, 5>{
                 std::pair{ "erc", schematicRelativePath },
                 std::pair{ "electrical", sourceRelativePath },
                 std::pair{ "drc", boardRelativePath },
                 std::pair{ "layout", sourceRelativePath },
                 std::pair{ "sourcing", sourceRelativePath } } )
    {
        if( kind == std::string_view( "electrical" ) && !hasElectricalDesign )
            continue;

        JSON result = handleVerify( { { "operation", kind }, { "path", path } },
                                    verificationProjectPath );
        JSON data;
        std::string checkError;

        if( !decodeResult( result, data, checkError ) )
            return failure( std::string( kind ) + "_check_failed", checkError );

        if( ( kind == std::string_view( "electrical" )
              || kind == std::string_view( "layout" )
              || kind == std::string_view( "sourcing" ) )
            && data.value( "sourceSha256", "" ) != compiled.sourceSha256 )
        {
            return failure( "stale_source", std::string( kind )
                                                    + " gate checked a different KDS revision" );
        }

        const bool clean = data.value( "clean", false );
        const bool waivers = data.value( "waiversPresent", false );

        if( !clean )
        {
            const JSON counts = data.value( "counts", JSON::object() );
            return failure( std::string( kind ) + "_gate_failed",
                            std::string( kind ) + " gate is not clean ("
                                    + std::to_string( counts.value( "errors", 0 ) )
                                    + " errors, "
                                    + std::to_string( counts.value( "warnings", 0 ) )
                                    + " warnings)" );
        }

        if( waivers && !allowWaivers )
        {
            return failure( "waiver_confirmation_required",
                            std::string( kind )
                                    + " has exclusions or ignored checks; set allowWaivers "
                                      "only with explicit release approval" );
        }

        checks[kind] = { { "clean", true },
                         { "waiversPresent", waivers },
                         { "counts", data.value( "counts", JSON::object() ) } };

        // User-approved distributor exceptions are clean, but the release must still name them.
        if( kind == std::string_view( "sourcing" ) && data.contains( "sourcing" )
            && data["sourcing"].value( "distributorExceptionCount", 0 ) > 0 )
        {
            checks[kind]["distributorExceptions"] = data["sourcing"]["distributorExceptions"];
        }

        if( data.contains( "schema" ) )
            checks[kind]["schema"] = data["schema"];

        if( data.contains( "kicadVersion" ) )
            checks[kind]["kicadVersion"] = data["kicadVersion"];
    }

    const auto inputsUnchanged = [&]()
    {
        std::string currentBoardHash;
        std::string currentSchematicHash;
        std::string currentSource;

        return KICHAD::CODEX_TOOLS::HashFabricationFile( board.GetFullPath().ToStdString(), currentBoardHash )
               && KICHAD::CODEX_TOOLS::HashFabricationFile( schematic.GetFullPath().ToStdString(),
                                       currentSchematicHash )
               && currentBoardHash == boardSha256 && currentSchematicHash == schematicSha256
               && KICHAD::CODEX_TOOLS::ReadDesignScriptSidecar( sidecar, currentSource, pathError )
               && currentSource == source;
    };

    if( !inputsUnchanged() )
    {
        return failure( "stale_inputs",
                        "board, schematic, or KDS changed while release gates were running" );
    }

    const std::filesystem::path root( projectRoot.GetFullPath().ToStdString() );
    const std::string transactionId( KIID().AsString().ToUTF8() );
    const std::filesystem::path staging =
            root / ( ".kichad-fabrication-staging-" + transactionId );
    const std::filesystem::path target = root / "fabrication";
    const std::filesystem::path backup =
            root / ( ".kichad-fabrication-backup-" + transactionId );
    std::error_code filesystemError;
    std::filesystem::create_directory( staging, filesystemError );

    if( filesystemError )
        return failure( "staging_failed", "could not create private fabrication staging" );

    std::filesystem::permissions( staging, std::filesystem::perms::owner_all,
                                  std::filesystem::perm_options::replace, filesystemError );

    if( filesystemError )
    {
        std::error_code cleanupError;
        std::filesystem::remove_all( staging, cleanupError );
        return failure( "staging_failed", "could not secure private fabrication staging" );
    }

    const auto cleanupStaging = [&]()
    {
        std::error_code cleanupError;
        std::filesystem::remove_all( staging, cleanupError );
    };
    wxFileName stagingDirectory =
            wxFileName::DirName( wxString::FromUTF8( staging.string().c_str() ) );
    std::string exportError;
    const bool exported = m_nativeFabricationRunner
                                  ? m_nativeFabricationRunner(
                                            verificationBoard, verificationSchematic,
                                            plan, stagingDirectory, exportError )
                                  : KICHAD::CODEX_TOOLS::RunNativeKiCadFabrication(
                                            verificationBoard, verificationSchematic,
                                            plan, stagingDirectory, exportError );

    if( !exported )
    {
        cleanupStaging();

        if( exportError.empty() )
            exportError = "native fabrication backend failed without an error message";

        return failure( "export_failed", exportError );
    }

    size_t bomRows = 0;

    if( !KICHAD::CODEX_TOOLS::WriteFabricationBom( compiled.ir, stagingDirectory, plan, bomRows, exportError ) )
    {
        cleanupStaging();
        return failure( "bom_failed", exportError );
    }

    JSON artifacts;

    if( !KICHAD::CODEX_TOOLS::ValidateFabricationArtifacts( stagingDirectory, plan, artifacts, exportError ) )
    {
        cleanupStaging();
        return failure( "artifact_validation_failed", exportError );
    }

    JSON productionArtifacts;

    if( !KICHAD::PRODUCTION_PACKAGE_WRITER::Write(
                verificationSnapshot.Path(), staging, compiled.ir["production"],
                plan["productionPlan"],
                productionArtifacts, exportError ) )
    {
        cleanupStaging();
        return failure( "production_package_failed", exportError );
    }

    for( JSON& artifact : productionArtifacts )
        artifacts.push_back( std::move( artifact ) );

    JSON designArtifacts;

    if( !KICHAD::DESIGN_RELEASE_WRITER::Write(
                verificationSnapshot.Path(), staging, designArtifacts, exportError ) )
    {
        cleanupStaging();
        return failure( "design_release_failed", exportError );
    }

    for( JSON& artifact : designArtifacts )
        artifacts.push_back( std::move( artifact ) );

    if( !inputsUnchanged() )
    {
        cleanupStaging();
        return failure( "stale_inputs",
                        "board, schematic, or KDS changed during fabrication export" );
    }

    const bool waiversPresent = checks["erc"].value( "waiversPresent", false )
                                || checks["drc"].value( "waiversPresent", false )
                                || checks["layout"].value( "waiversPresent", false );
    JSON manifest = {
        { "schema", "kichad.fabrication-manifest.v3" },
        { "manifestVersion", 3 },
        { "profile", plan["profile"] },
        { "kicadVersion", std::string( GetMajorMinorPatchVersion().ToUTF8() ) },
        { "releaseStatus", waiversPresent ? "waived" : "clean" },
        { "waiversAllowed", allowWaivers },
        { "source",
          { { "kds", { { "path", sourceRelativePath },
                          { "packagePath", "design/" + sourceRelativePath },
                          { "sha256", compiled.sourceSha256 } } },
            { "board", { { "path", boardRelativePath },
                           { "packagePath", "design/" + boardRelativePath },
                           { "sha256", boardSha256 },
                           { "formatVersion", "20260206" } } },
            { "schematic", { { "path", schematicRelativePath },
                               { "packagePath", "design/" + schematicRelativePath },
                               { "sha256", schematicSha256 },
                               { "formatVersion", "20260306" } } } } },
        { "checks", checks },
        { "runningReady", plan["runningReady"] },
        { "productionPlan", plan["productionPlan"] },
        { "bomRows", bomRows },
        { "artifacts", artifacts }
    };
    wxFileName manifestPath( stagingDirectory.GetFullPath(), wxS( "manifest.json" ) );

    if( !KICHAD::CODEX_TOOLS::WriteJsonAtomically( manifestPath, manifest, exportError ) )
    {
        cleanupStaging();
        return failure( "manifest_failed", exportError );
    }

    std::string manifestSha256;

    if( !KICHAD::CODEX_TOOLS::HashFabricationFile( manifestPath.GetFullPath().ToStdString(), manifestSha256 ) )
    {
        cleanupStaging();
        return failure( "manifest_failed", "could not hash the fabrication manifest" );
    }

    const bool targetExists = std::filesystem::exists( target, filesystemError );

    if( filesystemError )
    {
        cleanupStaging();
        return failure( "install_failed", "could not inspect existing fabrication output" );
    }

    if( targetExists )
    {
        const std::filesystem::file_status targetStatus =
                std::filesystem::symlink_status( target, filesystemError );

        if( filesystemError || std::filesystem::is_symlink( targetStatus )
            || !std::filesystem::is_directory( targetStatus ) )
        {
            cleanupStaging();
            return failure( "install_failed",
                            "existing fabrication target must be a real project directory" );
        }

        std::filesystem::rename( target, backup, filesystemError );

        if( filesystemError )
        {
            cleanupStaging();
            return failure( "install_failed", "could not stage existing fabrication output" );
        }
    }

    filesystemError.clear();
    std::filesystem::rename( staging, target, filesystemError );

    if( filesystemError )
    {
        std::error_code restoreError;

        if( targetExists )
            std::filesystem::rename( backup, target, restoreError );

        cleanupStaging();
        return failure( "install_failed",
                        restoreError ? "fabrication installation and rollback both failed"
                                     : "could not atomically install fabrication output" );
    }

    std::string installedManifestSha256;

    if( !KICHAD::CODEX_TOOLS::HashFabricationFile( target / "manifest.json", installedManifestSha256 )
        || installedManifestSha256 != manifestSha256 )
    {
        std::error_code rollbackError;
        std::filesystem::remove_all( target, rollbackError );

        if( targetExists && !rollbackError )
            std::filesystem::rename( backup, target, rollbackError );

        return failure( "install_failed",
                        rollbackError ? "installed manifest verification and rollback failed"
                                      : "installed manifest verification failed; output restored" );
    }

    bool backupRetained = false;

    if( targetExists )
    {
        filesystemError.clear();
        std::filesystem::remove_all( backup, filesystemError );
        backupRetained = static_cast<bool>( filesystemError );
    }

    uintmax_t artifactBytes = 0;

    for( const JSON& artifact : artifacts )
        artifactBytes += artifact.at( "bytes" ).get<uintmax_t>();

    return success( { { "operation", "export" },
                      { "path", sourceRelativePath },
                      { "boardPath", boardRelativePath },
                      { "schematicPath", schematicRelativePath },
                      { "sourceSha256", compiled.sourceSha256 },
                      { "profile", plan["profile"] },
                      { "targetDirectory", "fabrication" },
                      { "releaseStatus", waiversPresent ? "waived" : "clean" },
                      { "runningReady", plan["runningReady"] },
                      { "checks", std::move( checks ) },
                      { "artifactCount", artifacts.size() },
                      { "artifactBytes", artifactBytes },
                      { "bomRows", bomRows },
                      { "manifestSha256", manifestSha256 },
                      { "transaction", "confirmed staged validation and atomic replacement" },
                      { "backupRetained", backupRetained } } );
}
