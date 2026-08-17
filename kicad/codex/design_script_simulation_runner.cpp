/*
 * This program source code file is part of KiChad, a Codex-integrated downstream of KiCad.
 *
 * Copyright (C) 2026 KiChad Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "design_script_simulation_runner.h"

#include <kiid.h>

#include <algorithm>
#include <array>
#include "kichad_boost_process.h"
#include <cctype>
#include <chrono>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <wx/filefn.h>
#include <wx/string.h>
#include <wx/utils.h>


namespace
{

using JSON = nlohmann::json;

constexpr size_t MAX_RAW_BYTES = 64 * 1024 * 1024;
constexpr size_t MAX_POINTS = 1'000'000;
constexpr std::chrono::seconds MAX_RUN_TIME( 30 );


struct TEMP_DIRECTORY
{
    std::filesystem::path path;

    ~TEMP_DIRECTORY()
    {
        if( !path.empty() )
        {
            std::error_code ignored;
            std::filesystem::remove_all( path, ignored );
        }
    }
};


struct RAW_DATA
{
    bool complex = false;
    std::vector<std::string> names;
    std::vector<std::vector<std::complex<double>>> values;
};


std::string lower( std::string aValue )
{
    std::transform( aValue.begin(), aValue.end(), aValue.begin(),
                    []( unsigned char aCharacter )
                    {
                        return static_cast<char>( std::tolower( aCharacter ) );
                    } );
    return aValue;
}


bool parseNumber( const std::string& aText, std::complex<double>& aValue )
{
    const size_t comma = aText.find( ',' );

    try
    {
        size_t consumed = 0;
        const double real = std::stod( comma == std::string::npos
                                              ? aText
                                              : aText.substr( 0, comma ),
                                       &consumed );

        if( consumed != ( comma == std::string::npos ? aText.size() : comma )
            || !std::isfinite( real ) )
        {
            return false;
        }

        double imaginary = 0.0;

        if( comma != std::string::npos )
        {
            const std::string suffix = aText.substr( comma + 1 );
            consumed = 0;
            imaginary = std::stod( suffix, &consumed );

            if( consumed != suffix.size() || !std::isfinite( imaginary ) )
                return false;
        }

        aValue = { real, imaginary };
        return true;
    }
    catch( const std::exception& )
    {
        return false;
    }
}


bool readRawFile( const std::filesystem::path& aPath, RAW_DATA& aData, std::string& aError )
{
    std::error_code filesystemError;
    const uintmax_t bytes = std::filesystem::file_size( aPath, filesystemError );

    if( filesystemError || bytes == 0 || bytes > MAX_RAW_BYTES )
    {
        aError = "ngspice raw result must contain 1 byte to 64 MiB";
        return false;
    }

    std::ifstream input( aPath );

    if( !input )
    {
        aError = "could not open ngspice raw result";
        return false;
    }

    size_t variableCount = 0;
    size_t pointCount = 0;
    std::string line;

    while( std::getline( input, line ) )
    {
        if( line.starts_with( "No. Variables:" ) )
        {
            std::istringstream values( line.substr( 14 ) );
            values >> variableCount;
        }
        else if( line.starts_with( "No. Points:" ) )
        {
            std::istringstream values( line.substr( 11 ) );
            values >> pointCount;
        }
        else if( line.starts_with( "Flags:" ) )
        {
            aData.complex = lower( line ).find( "complex" ) != std::string::npos;
        }
        else if( line == "Variables:" )
        {
            break;
        }
    }

    if( variableCount == 0 || variableCount > 65536 || pointCount == 0
        || pointCount > MAX_POINTS )
    {
        aError = "ngspice raw result has invalid bounded dimensions";
        return false;
    }

    for( size_t index = 0; index < variableCount; ++index )
    {
        if( !std::getline( input, line ) )
        {
            aError = "ngspice raw variable table is truncated";
            return false;
        }

        std::istringstream row( line );
        size_t nativeIndex = 0;
        std::string name;
        std::string type;

        if( !( row >> nativeIndex >> name >> type ) || nativeIndex != index )
        {
            aError = "ngspice raw variable table is malformed";
            return false;
        }

        aData.names.push_back( lower( name ) );
        aData.values.emplace_back();
        aData.values.back().reserve( pointCount );
    }

    while( std::getline( input, line ) && line != "Values:" )
    {
    }

    if( line != "Values:" )
    {
        aError = "ngspice raw result has no values section";
        return false;
    }

    for( size_t point = 0; point < pointCount; ++point )
    {
        for( size_t variable = 0; variable < variableCount; ++variable )
        {
            do
            {
                if( !std::getline( input, line ) )
                {
                    aError = "ngspice raw values are truncated";
                    return false;
                }
            } while( line.find_first_not_of( " \t\r" ) == std::string::npos );

            std::istringstream row( line );
            std::vector<std::string> tokens;
            std::string token;

            while( row >> token )
                tokens.push_back( token );

            if( tokens.empty() )
            {
                aError = "ngspice raw value row is empty";
                return false;
            }

            if( variable == 0 )
            {
                size_t nativePoint = 0;
                std::istringstream indexParser( tokens.front() );

                if( tokens.size() < 2 || !( indexParser >> nativePoint ) || nativePoint != point )
                {
                    aError = "ngspice raw point index is malformed";
                    return false;
                }
            }

            std::complex<double> parsed;

            if( !parseNumber( tokens.back(), parsed ) )
            {
                aError = "ngspice raw value is not finite numeric data";
                return false;
            }

            aData.values[variable].push_back( parsed );
        }
    }

    return true;
}


std::string number( double aValue )
{
    std::ostringstream output;
    output << std::scientific << std::setprecision( 17 ) << aValue;
    return output.str();
}


void addIssue( JSON& aIssues, const std::string& aSimulation,
               const std::string& aAssertion, const std::string& aDescription,
               const JSON& aDetails )
{
    JSON issue = { { "type", "simulation_assertion_failed" }, { "severity", "error" },
                   { "simulation", aSimulation }, { "assertion", aAssertion },
                   { "description", aDescription } };
    issue.update( aDetails );
    aIssues.push_back( std::move( issue ) );
}


bool writeNetlist( const JSON& aSimulation, const JSON& aAnalysis,
                   const std::filesystem::path& aRawPath,
                   const std::filesystem::path& aNetlistPath,
                   std::map<std::string, std::string>& aNets,
                   std::map<std::string, std::string>& aSources,
                   std::string& aError )
{
    const std::string ground = aSimulation.value( "ground", "" );
    aNets[ground] = "0";
    size_t netIndex = 1;

    const auto addNet = [&]( const std::string& aNet )
    {
        if( !aNets.contains( aNet ) )
            aNets[aNet] = "n" + std::to_string( netIndex++ );
    };

    for( const JSON& device : aSimulation.value( "devices", JSON::array() ) )
    {
        for( const JSON& net : device.value( "nodes", JSON::array() ) )
            addNet( net.get<std::string>() );
    }

    for( const JSON& source : aSimulation.value( "sources", JSON::array() ) )
    {
        addNet( source.value( "positive", "" ) );
        addNet( source.value( "negative", "" ) );
    }

    std::ofstream output( aNetlistPath, std::ios::binary | std::ios::trunc );

    if( !output )
    {
        aError = "could not create private ngspice netlist";
        return false;
    }

    output << "KiChad deterministic simulation\n";
    std::map<std::string, std::string> models;
    size_t modelIndex = 1;

    for( const JSON& model : aSimulation.value( "models", JSON::array() ) )
    {
        const std::string native = "KDSMODEL" + std::to_string( modelIndex++ );
        const std::string kind = model.value( "kind", "" );
        const std::map<std::string, std::string> kinds = {
            { "diode", "D" }, { "npn", "NPN" }, { "pnp", "PNP" },
            { "nmos", "NMOS" }, { "pmos", "PMOS" }
        };
        const auto nativeKind = kinds.find( kind );

        if( nativeKind == kinds.end() )
        {
            aError = "compiled simulation contains an unsupported model kind";
            return false;
        }

        models[model.value( "id", "" )] = native;
        output << ".model " << native << ' ' << nativeKind->second << " (";
        bool first = true;
        const JSON parameters = model.value( "parameters", JSON::object() );

        for( const auto& [name, value] : parameters.items() )
        {
            if( !first )
                output << ' ';

            first = false;
            output << name << '=' << number( value.get<double>() );
        }

        output << ")\n";
    }

    size_t deviceIndex = 1;

    for( const JSON& device : aSimulation.value( "devices", JSON::array() ) )
    {
        const std::string kind = device.value( "kind", "" );
        const std::map<std::string, char> prefixes = {
            { "resistor", 'R' }, { "capacitor", 'C' }, { "inductor", 'L' },
            { "diode", 'D' }, { "bjt", 'Q' }, { "mosfet", 'M' }
        };
        const auto prefix = prefixes.find( kind );

        if( prefix == prefixes.end() )
        {
            aError = "compiled simulation contains an unsupported device kind";
            return false;
        }

        output << prefix->second << deviceIndex++;

        for( const JSON& net : device.value( "nodes", JSON::array() ) )
            output << ' ' << aNets.at( net.get<std::string>() );

        if( device.contains( "valueSi" ) )
            output << ' ' << number( device["valueSi"].get<double>() );
        else if( device.contains( "model" ) && models.contains( device["model"].get<std::string>() ) )
            output << ' ' << models.at( device["model"].get<std::string>() );
        else
        {
            aError = "compiled simulation device has no executable value or model";
            return false;
        }

        output << '\n';
    }

    size_t sourceIndex = 1;

    for( const JSON& source : aSimulation.value( "sources", JSON::array() ) )
    {
        const bool voltage = source.value( "kind", "" ) == "voltage";
        const std::string native = std::string( 1, voltage ? 'V' : 'I' )
                                   + std::to_string( sourceIndex++ );
        aSources[source.value( "id", "" )] = native;
        output << native << ' ' << aNets.at( source.value( "positive", "" ) ) << ' '
               << aNets.at( source.value( "negative", "" ) ) << " DC "
               << number( source.value( "dcSi", 0.0 ) );

        if( source.contains( "acMagnitudeSi" ) )
            output << " AC " << number( source["acMagnitudeSi"].get<double>() ) << ' '
                   << number( source["acPhaseDegrees"].get<double>() );

        if( source.contains( "pulseSi" ) )
        {
            output << " PULSE(";

            for( size_t index = 0; index < source["pulseSi"].size(); ++index )
                output << ( index == 0 ? "" : " " )
                       << number( source["pulseSi"][index].get<double>() );

            output << ')';
        }

        output << '\n';
    }

    const std::string kind = aAnalysis.value( "kind", "" );

    if( kind == "operating_point" )
        output << ".op\n";
    else if( kind == "transient" )
        output << ".tran " << number( aAnalysis.value( "stepSi", 0.0 ) ) << ' '
               << number( aAnalysis.value( "stopSi", 0.0 ) ) << ' '
               << number( aAnalysis.value( "startSi", 0.0 ) ) << "\n";
    else if( kind == "dc_sweep" )
    {
        if( !aSources.contains( aAnalysis.value( "source", "" ) ) )
        {
            aError = "compiled dc sweep references an unknown source";
            return false;
        }

        output << ".dc " << aSources.at( aAnalysis.value( "source", "" ) ) << ' '
               << number( aAnalysis.value( "startSi", 0.0 ) ) << ' '
               << number( aAnalysis.value( "stopSi", 0.0 ) ) << ' '
               << number( aAnalysis.value( "stepSi", 0.0 ) ) << "\n";
    }
    else if( kind == "ac_sweep" )
    {
        const std::map<std::string, std::string> scales = {
            { "decade", "dec" }, { "octave", "oct" }, { "linear", "lin" }
        };
        output << ".ac " << scales.at( aAnalysis.value( "scale", "" ) ) << ' '
               << aAnalysis.value( "points", 0 ) << ' '
               << number( aAnalysis.value( "startSi", 0.0 ) ) << ' '
               << number( aAnalysis.value( "stopSi", 0.0 ) ) << "\n";
    }
    else
    {
        aError = "compiled simulation contains an unsupported analysis";
        return false;
    }

    output << ".control\nset filetype=ascii\nrun\nwrite " << aRawPath.filename().string()
           << " all\nquit\n.endc\n.end\n";
    output.flush();

    if( !output )
    {
        aError = "could not write complete private ngspice netlist";
        return false;
    }

    return true;
}


bool runNgspice( const std::filesystem::path& aExecutable,
                 const std::filesystem::path& aNetlist,
                 const std::filesystem::path& aLog, std::string& aError )
{
    namespace bp = KICHAD_BP;
    bool finished = false;
    int exitCode = -1;

    try
    {
        const std::filesystem::path errorLog = aLog.string() + ".err";
        bp::child process(
                aExecutable.string(),
                bp::args( std::vector<std::string>{ "-b", aNetlist.filename().string() } ),
                bp::start_dir = aNetlist.parent_path().string(),
                bp::std_out > aLog.string(), bp::std_err > errorLog.string() );
        const auto deadline = std::chrono::steady_clock::now() + MAX_RUN_TIME;
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
        }
        else
        {
            exitCode = process.exit_code();
        }

        if( processError )
            aError = "ngspice process failed: " + processError.message();
    }
    catch( const std::exception& error )
    {
        aError = std::string( "could not run ngspice: " ) + error.what();
    }

    if( aError.empty() && ( !finished || exitCode != 0 ) )
    {
        aError = !finished ? "ngspice exceeded the 30 second analysis limit"
                           : "ngspice rejected the compiled circuit";
        std::string detail;

        for( const std::filesystem::path& path :
             { aLog, std::filesystem::path( aLog.string() + ".err" ) } )
        {
            std::ifstream input( path );
            detail.append( ( std::istreambuf_iterator<char>( input ) ),
                           std::istreambuf_iterator<char>() );

            if( detail.size() >= 4096 )
                break;
        }

        if( detail.size() > 4096 )
            detail.resize( 4096 );

        if( !detail.empty() )
            aError += ": " + detail;
    }

    return aError.empty();
}

} // namespace


KICHAD::DESIGN_SCRIPT_SIMULATION_RUNNER::RESULT
KICHAD::DESIGN_SCRIPT_SIMULATION_RUNNER::Run( const JSON& aCompilerIr )
{
    RESULT result;

    if( !aCompilerIr.is_object() || !aCompilerIr.contains( "electrical" )
        || !aCompilerIr["electrical"].is_object() )
    {
        result.error = "compiled KDS has no electrical contract";
        return result;
    }

    const JSON simulations =
            aCompilerIr["electrical"].value( "simulations", JSON::array() );

    if( simulations.empty() )
    {
        result.ok = true;
        result.clean = true;
        result.summary = { { "simulations", 0 }, { "analyses", 0 },
                           { "assertions", 0 }, { "points", 0 } };
        return result;
    }

    wxString executablePath;
    wxString environmentPath;
    wxGetEnv( wxS( "PATH" ), &environmentPath );
    wxFindFileInPath( &executablePath, environmentPath, wxS( "ngspice" ) );

#ifdef __WXMSW__
    if( executablePath.empty() )
        wxFindFileInPath( &executablePath, environmentPath, wxS( "ngspice.exe" ) );
#endif

    const std::filesystem::path executable = executablePath.ToStdString();

    if( executable.empty() )
    {
        result.error = "ngspice is unavailable; install the pinned simulation backend";
        return result;
    }

    TEMP_DIRECTORY temporary;
    std::error_code filesystemError;
    temporary.path = std::filesystem::temp_directory_path( filesystemError )
                     / ( "kichad-simulation-" + KIID().AsString().ToStdString() );
    std::filesystem::create_directory( temporary.path, filesystemError );

    if( filesystemError )
    {
        result.error = "could not create a private simulation directory";
        return result;
    }

    std::filesystem::permissions( temporary.path, std::filesystem::perms::owner_all,
                                  std::filesystem::perm_options::replace, filesystemError );

    if( filesystemError )
    {
        result.error = "could not secure the private simulation directory";
        return result;
    }

    size_t analysisCount = 0;
    size_t assertionCount = 0;
    size_t pointCount = 0;
    JSON summaries = JSON::array();

    for( const JSON& simulation : simulations )
    {
        const std::string simulationId = simulation.value( "id", "" );

        for( const JSON& analysis : simulation.value( "analyses", JSON::array() ) )
        {
            const std::string analysisId = analysis.value( "id", "" );
            const std::filesystem::path netlist = temporary.path
                                                  / ( "analysis-" + std::to_string( analysisCount ) + ".cir" );
            const std::filesystem::path raw = temporary.path
                                              / ( "analysis-" + std::to_string( analysisCount ) + ".raw" );
            const std::filesystem::path log = temporary.path
                                              / ( "analysis-" + std::to_string( analysisCount ) + ".log" );
            std::map<std::string, std::string> nets;
            std::map<std::string, std::string> sources;

            if( !writeNetlist( simulation, analysis, raw, netlist, nets, sources, result.error )
                || !runNgspice( executable, netlist, log, result.error ) )
            {
                return result;
            }

            RAW_DATA rawData;

            if( !readRawFile( raw, rawData, result.error ) )
                return result;

            ++analysisCount;
            pointCount += rawData.values.empty() ? 0 : rawData.values.front().size();
            JSON analysisSummary = { { "simulation", simulationId },
                                     { "analysis", analysisId },
                                     { "kind", analysis.value( "kind", "" ) },
                                     { "points", rawData.values.empty()
                                                         ? 0
                                                         : rawData.values.front().size() },
                                     { "assertions", JSON::array() } };

            std::map<std::string, size_t> vectors;

            for( size_t index = 0; index < rawData.names.size(); ++index )
                vectors[rawData.names[index]] = index;

            for( const JSON& assertion : simulation.value( "assertions", JSON::array() ) )
            {
                if( assertion.value( "analysis", "" ) != analysisId )
                    continue;

                ++assertionCount;
                const JSON probe = assertion.value( "probe", JSON::object() );
                const JSON targets = probe.value( "targets", JSON::array() );
                std::vector<double> values;
                std::string vectorName;

                if( probe.value( "kind", "" ) == "voltage" && !targets.empty() )
                {
                    const std::string first = "v(" + lower( nets.at( targets[0].get<std::string>() ) ) + ")";
                    const auto firstVector = vectors.find( first );

                    if( firstVector != vectors.end() )
                    {
                        values.reserve( rawData.values[firstVector->second].size() );

                        for( const std::complex<double>& value : rawData.values[firstVector->second] )
                            values.push_back( rawData.complex ? std::abs( value )
                                                              : value.real() );

                        vectorName = first;

                        if( targets.size() == 2 )
                        {
                            const std::string second = "v(" + lower( nets.at( targets[1].get<std::string>() ) ) + ")";
                            const auto secondVector = vectors.find( second );

                            if( secondVector == vectors.end()
                                || rawData.values[secondVector->second].size() != values.size() )
                            {
                                values.clear();
                            }
                            else
                            {
                                for( size_t index = 0; index < values.size(); ++index )
                                {
                                    const std::complex<double> difference =
                                            rawData.values[firstVector->second][index]
                                            - rawData.values[secondVector->second][index];
                                    values[index] = rawData.complex ? std::abs( difference )
                                                                   : difference.real();
                                }

                                vectorName = first + "-" + second;
                            }
                        }
                    }
                }
                else if( probe.value( "kind", "" ) == "current" && targets.size() == 1
                         && sources.contains( targets[0].get<std::string>() ) )
                {
                    const std::string native = lower( sources.at( targets[0].get<std::string>() ) );
                    const std::array<std::string, 2> candidates = {
                        "i(" + native + ")", native + "#branch"
                    };

                    for( const std::string& candidate : candidates )
                    {
                        const auto vector = vectors.find( candidate );

                        if( vector != vectors.end() )
                        {
                            for( const std::complex<double>& value : rawData.values[vector->second] )
                                values.push_back( rawData.complex ? std::abs( value )
                                                                  : value.real() );

                            vectorName = candidate;
                            break;
                        }
                    }
                }

                const std::string assertionId = assertion.value( "id", "" );

                if( values.empty() )
                {
                    result.error = "ngspice result omitted probe for assertion " + assertionId;
                    return result;
                }

                const bool finalOnly = assertion.value( "scope", "all" ) == "final";
                const auto begin = finalOnly ? values.end() - 1 : values.begin();
                const double measuredMinimum = *std::min_element( begin, values.end() );
                const double measuredMaximum = *std::max_element( begin, values.end() );
                bool passed = true;

                if( assertion.contains( "minimumSi" )
                    && measuredMinimum < assertion["minimumSi"].get<double>() )
                {
                    passed = false;
                }

                if( assertion.contains( "maximumSi" )
                    && measuredMaximum > assertion["maximumSi"].get<double>() )
                {
                    passed = false;
                }

                analysisSummary["assertions"].push_back(
                        { { "id", assertionId }, { "passed", passed },
                          { "vector", vectorName }, { "minimum", measuredMinimum },
                          { "maximum", measuredMaximum }, { "final", values.back() } } );

                if( !passed )
                {
                    addIssue( result.issues, simulationId, assertionId,
                              "simulation result is outside the declared acceptance range",
                              { { "analysis", analysisId }, { "vector", vectorName },
                                { "measuredMinimum", measuredMinimum },
                                { "measuredMaximum", measuredMaximum },
                                { "requiredMinimum", assertion.value( "minimumSi", JSON( nullptr ) ) },
                                { "requiredMaximum", assertion.value( "maximumSi", JSON( nullptr ) ) } } );
                }
            }

            summaries.push_back( std::move( analysisSummary ) );
        }
    }

    result.ok = true;
    result.clean = result.issues.empty();
    result.summary = { { "simulations", simulations.size() }, { "analyses", analysisCount },
                       { "assertions", assertionCount }, { "points", pointCount },
                       { "runs", std::move( summaries ) } };
    return result;
}
