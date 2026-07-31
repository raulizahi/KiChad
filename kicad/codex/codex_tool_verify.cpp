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
#include "design_script_compiler.h"
#include "design_script_electrical_analyzer.h"
#include "design_script_footprint_library_generator.h"
#include "design_script_layout_analyzer.h"
#include "design_script_physical_synthesizer.h"
#include "design_script_simulation_runner.h"

#include <build_version.h>

#include <array>
#include <map>
#include <set>
#include <string>
#include <tuple>

#include <wx/datetime.h>
#include <wx/filename.h>


namespace
{

constexpr wxFileOffset MAX_VERIFICATION_REPORT_BYTES = 64 * 1024 * 1024;
constexpr size_t       MAX_VERIFICATION_RESULT_BYTES = 240 * 1024;
constexpr size_t       MAX_VERIFICATION_IGNORED_CHECKS = 200;
constexpr size_t       MAX_VERIFICATION_IGNORED_BYTES = 64 * 1024;

} // namespace


namespace KICHAD::CODEX_TOOLS
{

nlohmann::json VerifySpec()
{
    nlohmann::json schema = { { "type", "object" },
                              { "additionalProperties", false },
                              { "required", nlohmann::json::array( { "operation", "path" } ) } };
    schema["properties"]["operation"] =
            { { "type", "string" },
              { "enum", nlohmann::json::array(
                                { "erc", "electrical", "drc", "layout", "sourcing" } ) } };
    schema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description",
                "Project-relative .kicad_sch for ERC, .kicad_pcb for DRC, or .kicad_kds for "
                "electrical, layout, and sourcing." } };
    schema["properties"]["offset"] =
            { { "type", "integer" }, { "minimum", 0 }, { "maximum", 1000000 },
              { "description", "Zero-based violation offset; defaults to 0." } };
    schema["properties"]["limit"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 },
              { "description", "Maximum violations returned; defaults to 100." } };
    schema["properties"]["maxAgeDays"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 90 },
              { "description", "Maximum sourcing-evidence age in days; defaults to 7." } };

    return { { "type", "function" },
             { "name", "verify" },
             { "description",
               "Run read-only native design gates. ERC and DRC use the matching KiCad 10 "
               "backend; DRC also checks schematic parity. Electrical evaluates typed rail, "
               "rating, thermal, and logic contracts plus declared operating-point, transient, "
               "DC-sweep, and AC-sweep simulations and numerical assertions. Layout evaluates the canonical KDS "
               "physical acceptance contract. Sourcing checks the single KDS evidence cache "
               "for completeness, orderability, and freshness. Results have complete counts "
               "and bounded pageable issues." },
             { "inputSchema", std::move( schema ) } };
}

} // namespace KICHAD::CODEX_TOOLS


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleElectricalVerify(
        const JSON& aArguments, const wxString& aProjectPath ) const
{
    int offset = 0;
    int limit = 100;

    for( const auto& [name, minimum, maximum, destination] :
         std::array<std::tuple<const char*, int64_t, int64_t, int*>, 2>{
                 std::tuple{ "offset", 0, 1000000, &offset },
                 std::tuple{ "limit", 1, 200, &limit } } )
    {
        if( !aArguments.contains( name ) )
            continue;

        if( !aArguments[name].is_number_integer() )
            return failure( "invalid_arguments", std::string( "verify." ) + name
                                                         + " must be an integer" );

        const int64_t parsed = aArguments[name].get<int64_t>();

        if( parsed < minimum || parsed > maximum )
            return failure( "invalid_arguments", std::string( "verify." ) + name
                                                         + " is outside its allowed range" );

        *destination = static_cast<int>( parsed );
    }

    if( !wxFileName::DirExists( aProjectPath ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    const std::string relativePath = aArguments["path"].get<std::string>();
    wxFileName sidecar;
    std::string pathError;

    if( !KICHAD::CODEX_TOOLS::ResolveProjectSidecar( aProjectPath, relativePath,
                                                      sidecar, pathError ) )
    {
        return failure( "invalid_path", pathError );
    }

    if( !sidecar.FileExists() )
        return failure( "read_failed", "KiChad Design Script sidecar does not exist" );

    std::string source;

    if( !KICHAD::CODEX_TOOLS::ReadDesignScriptSidecar( sidecar, source, pathError ) )
        return failure( "read_failed", pathError );

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    if( !compiled.ok )
    {
        return failure( "compile_failed",
                        "KDS must compile before electrical verification; use design.compile "
                        "for structured diagnostics" );
    }

    KICHAD::DESIGN_SCRIPT_ELECTRICAL_ANALYZER::RESULT analyzed =
            KICHAD::DESIGN_SCRIPT_ELECTRICAL_ANALYZER::Analyze( compiled.ir );
    KICHAD::DESIGN_SCRIPT_SIMULATION_RUNNER::RESULT simulated =
            KICHAD::DESIGN_SCRIPT_SIMULATION_RUNNER::Run( compiled.ir );

    if( !simulated.ok )
        return failure( "simulation_failed", simulated.error );

    for( JSON& issue : simulated.issues )
        analyzed.issues.push_back( std::move( issue ) );

    analyzed.clean = analyzed.clean && simulated.clean;
    const size_t totalIssues = analyzed.issues.size();
    JSON page = JSON::array();

    for( size_t index = static_cast<size_t>( offset );
         index < totalIssues && page.size() < static_cast<size_t>( limit ); ++index )
    {
        JSON item = analyzed.issues[index];
        item["category"] = "electrical";
        page.push_back( std::move( item ) );
    }

    const size_t returned = page.size();
    const bool hasMore = static_cast<size_t>( offset ) + returned < totalIssues;
    JSON payload = {
        { "operation", "electrical" },
        { "path", relativePath },
        { "sourceSha256", compiled.sourceSha256 },
        { "clean", analyzed.clean },
        { "waiversPresent", false },
        { "counts",
          { { "total", totalIssues }, { "errors", totalIssues }, { "warnings", 0 },
            { "exclusions", 0 }, { "other", 0 },
            { "categories", { { "electrical", totalIssues } } } } },
        { "electrical",
          { { "contracts", std::move( analyzed.summary ) },
            { "simulation", std::move( simulated.summary ) } } },
        { "ignoredChecksCount", 0 },
        { "ignoredChecks", JSON::array() },
        { "ignoredChecksTruncated", false },
        { "offset", offset },
        { "returnedViolations", returned },
        { "violations", std::move( page ) },
        { "resultTruncated", hasMore }
    };

    if( hasMore )
        payload["nextOffset"] = static_cast<size_t>( offset ) + returned;

    return success( payload );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleLayoutVerify(
        const JSON& aArguments, const wxString& aProjectPath ) const
{
    int offset = 0;
    int limit = 100;

    for( const auto& [name, minimum, maximum, destination] :
         std::array<std::tuple<const char*, int64_t, int64_t, int*>, 2>{
                 std::tuple{ "offset", 0, 1000000, &offset },
                 std::tuple{ "limit", 1, 200, &limit } } )
    {
        if( !aArguments.contains( name ) )
            continue;

        if( !aArguments[name].is_number_integer() )
            return failure( "invalid_arguments", std::string( "verify." ) + name
                                                         + " must be an integer" );

        const int64_t parsed = aArguments[name].get<int64_t>();

        if( parsed < minimum || parsed > maximum )
            return failure( "invalid_arguments", std::string( "verify." ) + name
                                                         + " is outside its allowed range" );

        *destination = static_cast<int>( parsed );
    }

    if( !wxFileName::DirExists( aProjectPath ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    const std::string relativePath = aArguments["path"].get<std::string>();
    wxFileName sidecar;
    std::string pathError;

    if( !KICHAD::CODEX_TOOLS::ResolveProjectSidecar( aProjectPath, relativePath,
                                                      sidecar, pathError ) )
    {
        return failure( "invalid_path", pathError );
    }

    if( !sidecar.FileExists() )
        return failure( "read_failed", "KiChad Design Script sidecar does not exist" );

    std::string source;

    if( !KICHAD::CODEX_TOOLS::ReadDesignScriptSidecar( sidecar, source, pathError ) )
        return failure( "read_failed", pathError );

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    if( !compiled.ok )
    {
        return failure( "compile_failed",
                        "KDS must compile before layout verification; use design.compile "
                        "for structured diagnostics" );
    }

    if( compiled.ir.contains( "synthesis" ) && compiled.ir["synthesis"].is_object() )
    {
        KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::RESULT generated =
                KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::Generate( compiled.ir );
        JSON footprintSources;

        if( !generated.ok )
            return failure( "footprint_generation_failed",
                            "managed footprints required by physical synthesis could not be generated" );

        if( !KICHAD::CODEX_TOOLS::InventoryProjectFootprints(
                    aProjectPath, compiled.ir, footprintSources, pathError ) )
        {
            return failure( "footprint_inventory_failed", pathError );
        }

        for( const auto& [id, nativeSource] : generated.sources.items() )
            footprintSources[id] = nativeSource;

        KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIZER::RESULT synthesized =
                KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIZER::Synthesize(
                        compiled.ir, footprintSources );

        if( !synthesized.ok )
            return failure( "synthesis_failed", synthesized.error );

        compiled.ir = std::move( synthesized.ir );
    }

    KICHAD::DESIGN_SCRIPT_LAYOUT_ANALYZER::RESULT analyzed =
            KICHAD::DESIGN_SCRIPT_LAYOUT_ANALYZER::Analyze( compiled.ir );
    const size_t totalIssues = analyzed.issues.size();
    JSON page = JSON::array();

    for( size_t index = static_cast<size_t>( offset );
         index < totalIssues && page.size() < static_cast<size_t>( limit ); ++index )
    {
        page.push_back( analyzed.issues[index] );
    }

    const size_t returned = page.size();
    const bool hasMore = static_cast<size_t>( offset ) + returned < totalIssues;
    JSON payload = {
        { "operation", "layout" },
        { "path", relativePath },
        { "sourceSha256", compiled.sourceSha256 },
        { "clean", analyzed.clean },
        { "waiversPresent", false },
        { "counts",
          { { "total", totalIssues }, { "errors", totalIssues }, { "warnings", 0 },
            { "exclusions", 0 }, { "other", 0 },
            { "categories", { { "layout", totalIssues } } } } },
        { "layout", std::move( analyzed.summary ) },
        { "ignoredChecksCount", 0 },
        { "ignoredChecks", JSON::array() },
        { "ignoredChecksTruncated", false },
        { "offset", offset },
        { "returnedViolations", returned },
        { "violations", std::move( page ) },
        { "resultTruncated", hasMore }
    };

    if( hasMore )
        payload["nextOffset"] = static_cast<size_t>( offset ) + returned;

    return success( payload );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleSourcingVerify(
        const JSON& aArguments, const wxString& aProjectPath ) const
{
    int offset = 0;
    int limit = 100;
    int maxAgeDays = 7;

    const auto boundedInteger = [&]( const char* aName, int64_t aMinimum, int64_t aMaximum,
                                     int& aValue ) -> JSON
    {
        if( !aArguments.contains( aName ) )
            return nullptr;

        if( !aArguments[aName].is_number_integer() )
            return failure( "invalid_arguments", std::string( "verify." ) + aName
                                                         + " must be an integer" );

        const int64_t parsed = aArguments[aName].get<int64_t>();

        if( parsed < aMinimum || parsed > aMaximum )
        {
            return failure( "invalid_arguments", std::string( "verify." ) + aName
                                                         + " is outside its allowed range" );
        }

        aValue = static_cast<int>( parsed );
        return nullptr;
    };

    for( const auto& [name, minimum, maximum, destination] :
         std::array<std::tuple<const char*, int64_t, int64_t, int*>, 3>{
                 std::tuple{ "offset", 0, 1000000, &offset },
                 std::tuple{ "limit", 1, 200, &limit },
                 std::tuple{ "maxAgeDays", 1, 90, &maxAgeDays } } )
    {
        JSON error = boundedInteger( name, minimum, maximum, *destination );

        if( !error.is_null() )
            return error;
    }

    if( !wxFileName::DirExists( aProjectPath ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    const std::string relativePath = aArguments["path"].get<std::string>();
    wxFileName sidecar;
    std::string pathError;

    if( !KICHAD::CODEX_TOOLS::ResolveProjectSidecar( aProjectPath, relativePath, sidecar, pathError ) )
        return failure( "invalid_path", pathError );

    if( !sidecar.FileExists() )
        return failure( "read_failed", "KiChad Design Script sidecar does not exist" );

    std::string source;

    if( !KICHAD::CODEX_TOOLS::ReadDesignScriptSidecar( sidecar, source, pathError ) )
        return failure( "read_failed", pathError );

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    if( !compiled.ok )
    {
        return failure( "compile_failed",
                        "KDS must compile before sourcing verification; use design.compile "
                        "for structured diagnostics" );
    }

    std::set<std::string> requiredComponents;

    for( const JSON& component : compiled.ir["schematic"]["components"] )
    {
        if( !component.is_object() || !component.contains( "reference" )
            || !component["reference"].is_string() )
        {
            return failure( "compile_failed", "KDS compiler returned invalid component data" );
        }

        const std::string reference = component["reference"].get<std::string>();

        if( component.contains( "footprint" ) && !component["footprint"].is_null() )
            requiredComponents.emplace( reference );
    }

    std::map<std::string, const JSON*> records;

    for( const JSON& record : compiled.ir["sourcing"] )
    {
        if( !record.is_object() || !record.contains( "component" )
            || !record["component"].is_string() )
        {
            return failure( "compile_failed", "KDS compiler returned invalid sourcing data" );
        }

        records.emplace( record["component"].get<std::string>(), &record );
    }

    JSON issues = JSON::array();
    std::set<std::string> componentsWithIssues;
    const auto addIssue = [&]( const std::string& aComponent, const std::string& aType,
                               const std::string& aSeverity, const std::string& aDescription,
                               JSON aDetail = JSON::object() )
    {
        JSON issue = { { "category", "sourcing" },
                       { "type", aType },
                       { "severity", aSeverity },
                       { "description", aDescription },
                       { "component", aComponent } };

        if( aDetail.is_object() )
            issue.update( aDetail );

        issues.push_back( std::move( issue ) );
        componentsWithIssues.emplace( aComponent );
    };

    static constexpr const char* REQUIRED_FIELDS[] = {
        "manufacturer", "mpn",       "datasheet",  "lifecycle", "supplier",
        "sku",          "product_url", "available", "verified_on", "quantity"
    };
    const wxDateTime today = wxDateTime::Today();

    for( const std::string& reference : requiredComponents )
    {
        auto recordIt = records.find( reference );

        if( recordIt == records.end() )
        {
            addIssue( reference, "missing_source", "error",
                      "Physical component has no cached sourcing evidence" );
            continue;
        }

        const JSON& record = *recordIt->second;
        JSON missing = JSON::array();

        for( const char* field : REQUIRED_FIELDS )
        {
            if( !record.contains( field ) )
                missing.push_back( field );
        }

        if( !missing.empty() )
        {
            addIssue( reference, "missing_evidence_field", "error",
                      "Sourcing evidence is incomplete", { { "missingFields", missing } } );
            continue;
        }

        const std::string lifecycle = record["lifecycle"].get<std::string>();

        if( lifecycle != "active" )
        {
            addIssue( reference, "non_active_lifecycle",
                      lifecycle == "nrnd" ? "warning" : "error",
                      "Component lifecycle is not active", { { "lifecycle", lifecycle } } );
        }

        const int64_t available = record["available"].get<int64_t>();

        if( available <= 0 )
        {
            addIssue( reference, "not_orderable", "error",
                      "Distributor evidence reports no available stock",
                      { { "available", available }, { "supplier", record["supplier"] } } );
        }

        // Stocking policy: every fitted part must be stocked at DigiKey, Mouser, or Newark;
        // any other distributor requires an explicit user-approved exception.
        std::string supplierKey = record["supplier"].get<std::string>();
        supplierKey.erase( std::remove_if( supplierKey.begin(), supplierKey.end(),
                                           []( unsigned char c )
                                           {
                                               return !std::isalnum( c );
                                           } ),
                           supplierKey.end() );
        std::transform( supplierKey.begin(), supplierKey.end(), supplierKey.begin(),
                        []( unsigned char c )
                        {
                            return std::tolower( c );
                        } );

        if( supplierKey != "digikey" && supplierKey != "mouser" && supplierKey != "newark" )
        {
            addIssue( reference, "unapproved_distributor", "error",
                      "Sourcing evidence must come from DigiKey, Mouser, or Newark unless the "
                      "user explicitly approves an exception",
                      { { "supplier", record["supplier"] } } );
        }

        const std::string verifiedOn = record["verified_on"].get<std::string>();
        const int year = std::stoi( verifiedOn.substr( 0, 4 ) );
        const int month = std::stoi( verifiedOn.substr( 5, 2 ) );
        const int day = std::stoi( verifiedOn.substr( 8, 2 ) );
        const wxDateTime verified( day, static_cast<wxDateTime::Month>( month - 1 ), year );
        const int ageDays = ( today - verified ).GetDays();

        if( ageDays < 0 )
        {
            addIssue( reference, "future_evidence", "error",
                      "Sourcing evidence date is in the future",
                      { { "verifiedOn", verifiedOn } } );
        }
        else if( ageDays > maxAgeDays )
        {
            addIssue( reference, "stale_evidence", "error",
                      "Sourcing evidence is older than the allowed age",
                      { { "verifiedOn", verifiedOn }, { "ageDays", ageDays } } );
        }
    }

    for( const auto& entry : records )
    {
        const std::string& reference = entry.first;

        if( !requiredComponents.contains( reference ) )
        {
            addIssue( reference, "non_physical_source", "warning",
                      "Sourcing evidence belongs to a component with no physical footprint" );
        }
    }

    size_t errorCount = 0;
    size_t warningCount = 0;

    for( const JSON& issue : issues )
    {
        if( issue["severity"] == "error" )
            ++errorCount;
        else
            ++warningCount;
    }

    size_t completeComponents = 0;

    for( const std::string& reference : requiredComponents )
    {
        if( !componentsWithIssues.contains( reference ) )
            ++completeComponents;
    }

    const size_t totalIssues = issues.size();
    JSON page = JSON::array();

    for( size_t i = static_cast<size_t>( offset ); i < totalIssues
                                                      && page.size() < static_cast<size_t>( limit );
         ++i )
    {
        page.push_back( issues[i] );
    }

    const size_t returned = page.size();
    const bool hasMore = static_cast<size_t>( offset ) + returned < totalIssues;
    JSON payload = {
        { "operation", "sourcing" },
        { "path", relativePath },
        { "sourceSha256", compiled.sourceSha256 },
        { "verifiedOn", std::string( today.FormatISODate().ToUTF8() ) },
        { "maxAgeDays", maxAgeDays },
        { "clean", totalIssues == 0 },
        { "waiversPresent", false },
        { "counts",
          { { "total", totalIssues },
            { "errors", errorCount },
            { "warnings", warningCount },
            { "exclusions", 0 },
            { "other", 0 },
            { "categories", { { "sourcing", totalIssues } } } } },
        { "sourcing",
          { { "requiredComponents", requiredComponents.size() },
            { "sourceRecords", records.size() },
            { "completeComponents", completeComponents } } },
        { "ignoredChecksCount", 0 },
        { "ignoredChecks", JSON::array() },
        { "ignoredChecksTruncated", false },
        { "offset", offset },
        { "returnedViolations", returned },
        { "violations", std::move( page ) },
        { "resultTruncated", hasMore }
    };

    if( hasMore )
        payload["nextOffset"] = static_cast<size_t>( offset ) + returned;

    return success( payload );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleVerify(
        const JSON& aArguments, const wxString& aProjectPath ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string() || !aArguments.contains( "path" )
        || !aArguments["path"].is_string() )
    {
        return failure( "invalid_arguments", "verify.operation and verify.path must be strings" );
    }

    const std::string operation = aArguments["operation"].get<std::string>();
    const std::string relativePath = aArguments["path"].get<std::string>();

    if( operation != "erc" && operation != "electrical" && operation != "drc"
        && operation != "layout"
        && operation != "sourcing" )
    {
        return failure( "invalid_arguments",
                        "verify.operation must be 'erc', 'electrical', 'drc', 'layout', "
                        "or 'sourcing'" );
    }

    if( operation == "electrical" || operation == "layout" )
    {
        if( aArguments.contains( "maxAgeDays" ) )
            return failure( "invalid_arguments", "verify.maxAgeDays applies only to sourcing" );

        return operation == "electrical"
                       ? handleElectricalVerify( aArguments, aProjectPath )
                       : handleLayoutVerify( aArguments, aProjectPath );
    }

    if( operation == "sourcing" )
        return handleSourcingVerify( aArguments, aProjectPath );

    if( aArguments.contains( "maxAgeDays" ) )
        return failure( "invalid_arguments", "verify.maxAgeDays applies only to sourcing" );

    int offset = 0;
    int limit = 100;

    if( aArguments.contains( "offset" ) )
    {
        if( !aArguments["offset"].is_number_integer() )
            return failure( "invalid_arguments", "verify.offset must be an integer" );

        const int64_t parsedOffset = aArguments["offset"].get<int64_t>();

        if( parsedOffset < 0 || parsedOffset > 1000000 )
            return failure( "invalid_arguments", "verify.offset must be between 0 and 1000000" );

        offset = static_cast<int>( parsedOffset );
    }

    if( aArguments.contains( "limit" ) )
    {
        if( !aArguments["limit"].is_number_integer() )
            return failure( "invalid_arguments", "verify.limit must be an integer" );

        const int64_t parsedLimit = aArguments["limit"].get<int64_t>();

        if( parsedLimit < 1 || parsedLimit > 200 )
            return failure( "invalid_arguments", "verify.limit must be between 1 and 200" );

        limit = static_cast<int>( parsedLimit );
    }

    if( !wxFileName::DirExists( aProjectPath ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    wxFileName resolved;
    std::string pathError;

    if( !KICHAD::CODEX_TOOLS::ResolveProjectFile( aProjectPath, relativePath, resolved, pathError ) )
        return failure( "invalid_path", pathError );

    const wxString expectedExtension = operation == "erc" ? wxS( "kicad_sch" )
                                                            : wxS( "kicad_pcb" );

    if( resolved.GetExt() != expectedExtension )
    {
        return failure( "invalid_path", operation == "erc"
                                                ? "ERC requires a .kicad_sch file"
                                                : "DRC requires a .kicad_pcb file" );
    }

    std::string reportSource;
    std::string checkError;
    const bool ran = m_nativeCheckRunner
                             ? m_nativeCheckRunner( operation, resolved, reportSource, checkError )
                             : KICHAD::CODEX_TOOLS::RunNativeKiCadCheck( operation, resolved, reportSource, checkError );

    if( !ran )
    {
        if( checkError.empty() )
            checkError = "native KiCad verification failed without an error message";

        return failure( "check_failed", checkError );
    }

    if( reportSource.empty() || reportSource.size() > MAX_VERIFICATION_REPORT_BYTES )
    {
        return failure( "invalid_report",
                        "native verification report must contain 1 byte to 64 MiB" );
    }

    JSON report = JSON::parse( reportSource, nullptr, false );

    if( report.is_discarded() || !report.is_object() )
        return failure( "invalid_report", "native verification report is not valid JSON" );

    const auto hasString = [&]( const char* aName )
    {
        return report.contains( aName ) && report[aName].is_string();
    };

    if( !hasString( "$schema" ) || !hasString( "source" ) || !hasString( "date" )
        || !hasString( "kicad_version" ) || !hasString( "coordinate_units" )
        || !report.contains( "included_severities" )
        || !report["included_severities"].is_array() || !report.contains( "ignored_checks" )
        || !report["ignored_checks"].is_array() )
    {
        return failure( "invalid_report", "native verification report is missing required fields" );
    }

    const std::string expectedSchema = operation == "erc"
                                               ? "https://schemas.kicad.org/erc.v1.json"
                                               : "https://schemas.kicad.org/drc.v1.json";
    const std::string nativeVersion( GetMajorMinorPatchVersion().ToUTF8() );

    if( report["$schema"].get<std::string>() != expectedSchema
        || report["coordinate_units"].get<std::string>() != "mm"
        || report["source"].get<std::string>()
                   != std::string( resolved.GetFullName().ToUTF8() ) )
    {
        return failure( "invalid_report",
                        "native verification report schema, units, or source does not match" );
    }

    if( report["kicad_version"].get<std::string>() != nativeVersion )
    {
        return failure( "version_mismatch",
                        "native verification report was not produced by this KiChad version" );
    }

    std::set<std::string> includedSeverities;

    for( const JSON& severity : report["included_severities"] )
    {
        if( !severity.is_string() )
            return failure( "invalid_report", "included severity is not a string" );

        includedSeverities.emplace( severity.get<std::string>() );
    }

    if( report["included_severities"].size() != 3
        || includedSeverities != std::set<std::string>{ "error", "exclusion", "warning" } )
    {
        return failure( "invalid_report",
                        "native verification report does not include every severity" );
    }

    JSON ignoredChecks = JSON::array();
    size_t ignoredCheckBytes = 0;

    for( const JSON& ignored : report["ignored_checks"] )
    {
        if( !ignored.is_object() || !ignored.contains( "key" ) || !ignored["key"].is_string()
            || !ignored.contains( "description" ) || !ignored["description"].is_string() )
        {
            return failure( "invalid_report", "ignored check entry is malformed" );
        }

        const size_t entryBytes = ignored.dump().size();

        if( ignoredChecks.size() < MAX_VERIFICATION_IGNORED_CHECKS
            && ignoredCheckBytes + entryBytes <= MAX_VERIFICATION_IGNORED_BYTES )
        {
            ignoredChecks.push_back( ignored );
            ignoredCheckBytes += entryBytes;
        }
    }

    JSON violations = JSON::array();
    JSON categoryCounts = JSON::object();
    size_t total = 0;
    size_t errors = 0;
    size_t warnings = 0;
    size_t exclusions = 0;
    size_t other = 0;
    size_t resultBytes = ignoredCheckBytes;
    bool   resultByteLimited = false;
    bool   malformedViolation = false;

    const auto visitViolation = [&]( const std::string& aCategory, const JSON& aViolation,
                                     const std::string& aSheetPath = std::string(),
                                     const std::string& aSheetUuidPath = std::string() )
    {
        if( !aViolation.is_object() || !aViolation.contains( "type" )
            || !aViolation["type"].is_string() || !aViolation.contains( "description" )
            || !aViolation["description"].is_string() || !aViolation.contains( "severity" )
            || !aViolation["severity"].is_string() || !aViolation.contains( "items" )
            || !aViolation["items"].is_array()
            || ( aViolation.contains( "excluded" ) && !aViolation["excluded"].is_boolean() )
            || ( aViolation.contains( "comment" ) && !aViolation["comment"].is_string() ) )
        {
            malformedViolation = true;
            return;
        }

        for( const JSON& item : aViolation["items"] )
        {
            if( !item.is_object() || !item.contains( "uuid" ) || !item["uuid"].is_string()
                || !item.contains( "description" ) || !item["description"].is_string()
                || !item.contains( "pos" ) || !item["pos"].is_object()
                || !item["pos"].contains( "x" ) || !item["pos"]["x"].is_number()
                || !item["pos"].contains( "y" ) || !item["pos"]["y"].is_number() )
            {
                malformedViolation = true;
                return;
            }
        }

        const std::string severity = aViolation["severity"].get<std::string>();

        if( severity != "error" && severity != "warning" && severity != "exclusion" )
        {
            malformedViolation = true;
            return;
        }

        const bool excluded = ( aViolation.contains( "excluded" )
                                && aViolation["excluded"].get<bool>() )
                              || severity == "exclusion";

        if( excluded )
            ++exclusions;
        else if( severity == "error" )
            ++errors;
        else if( severity == "warning" )
            ++warnings;
        else
            ++other;

        categoryCounts[aCategory] = categoryCounts[aCategory].get<size_t>() + 1;
        const size_t index = total++;

        if( index < static_cast<size_t>( offset )
            || violations.size() >= static_cast<size_t>( limit ) || resultByteLimited )
        {
            return;
        }

        JSON item = aViolation;
        item["category"] = aCategory;

        if( !aSheetPath.empty() )
        {
            item["sheetPath"] = aSheetPath;
            item["sheetUuidPath"] = aSheetUuidPath;
        }

        const size_t itemBytes = item.dump().size();

        if( resultBytes + itemBytes > MAX_VERIFICATION_RESULT_BYTES )
        {
            resultByteLimited = true;
            return;
        }

        resultBytes += itemBytes;
        violations.push_back( std::move( item ) );
    };

    if( operation == "erc" )
    {
        if( !report.contains( "sheets" ) || !report["sheets"].is_array() )
            return failure( "invalid_report", "native ERC report has no sheets array" );

        categoryCounts["erc"] = 0;

        for( const JSON& sheet : report["sheets"] )
        {
            if( !sheet.is_object() || !sheet.contains( "path" ) || !sheet["path"].is_string()
                || !sheet.contains( "uuid_path" ) || !sheet["uuid_path"].is_string()
                || !sheet.contains( "violations" ) || !sheet["violations"].is_array() )
            {
                return failure( "invalid_report", "native ERC sheet entry is malformed" );
            }

            for( const JSON& violation : sheet["violations"] )
            {
                visitViolation( "erc", violation, sheet["path"].get<std::string>(),
                                sheet["uuid_path"].get<std::string>() );
            }
        }
    }
    else
    {
        const std::array<std::pair<const char*, const char*>, 3> categories = {
            std::pair{ "violations", "drc" },
            std::pair{ "unconnected_items", "unconnectedItem" },
            std::pair{ "schematic_parity", "schematicParity" }
        };

        for( const auto& [field, category] : categories )
        {
            if( !report.contains( field ) || !report[field].is_array() )
                return failure( "invalid_report", "native DRC report category is malformed" );

            categoryCounts[category] = 0;

            for( const JSON& violation : report[field] )
                visitViolation( category, violation );
        }
    }

    if( malformedViolation )
        return failure( "invalid_report", "native verification violation is malformed" );

    if( resultByteLimited && violations.empty() && total > static_cast<size_t>( offset ) )
    {
        return failure( "result_too_large",
                        "one native verification violation exceeds the tool result limit" );
    }

    const size_t returned = violations.size();
    const bool hasMore = static_cast<size_t>( offset ) + returned < total;
    const bool ignoredChecksTruncated =
            ignoredChecks.size() < report["ignored_checks"].size();
    JSON payload = {
        { "operation", operation },
        { "path", relativePath },
        { "schema", expectedSchema },
        { "kicadVersion", nativeVersion },
        { "coordinateUnits", "mm" },
        { "clean", errors == 0 && warnings == 0 },
        { "waiversPresent", exclusions > 0 || !report["ignored_checks"].empty() },
        { "counts",
          { { "total", total },
            { "errors", errors },
            { "warnings", warnings },
            { "exclusions", exclusions },
            { "other", other },
            { "categories", std::move( categoryCounts ) } } },
        { "ignoredChecksCount", report["ignored_checks"].size() },
        { "ignoredChecks", std::move( ignoredChecks ) },
        { "ignoredChecksTruncated", ignoredChecksTruncated },
        { "offset", offset },
        { "returnedViolations", returned },
        { "violations", std::move( violations ) },
        { "resultTruncated", hasMore }
    };

    if( hasMore )
        payload["nextOffset"] = static_cast<size_t>( offset ) + returned;

    return success( payload );
}
