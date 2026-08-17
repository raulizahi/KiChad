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

#include "design_script_electrical_analyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>

#include "kichad_wide_int.h"


namespace
{

using JSON = nlohmann::json;
constexpr int64_t ONE_MILLION = 1'000'000;


void issue( JSON& aIssues, const std::string& aType, const std::string& aDescription,
            const JSON& aDetails = JSON::object() )
{
    JSON value = { { "type", aType }, { "severity", "error" },
                   { "description", aDescription } };

    for( const auto& [name, detail] : aDetails.items() )
        value[name] = detail;

    aIssues.push_back( std::move( value ) );
}


std::string integerString( WIDE_INT aValue )
{
    if( aValue == 0 )
        return "0";

    const bool negative = aValue < 0;
    WIDE_UINT   magnitude = negative ? static_cast<WIDE_UINT>( -aValue )
                                     : static_cast<WIDE_UINT>( aValue );
    std::string result;

    while( magnitude > 0 )
    {
        const unsigned digit = static_cast<unsigned>( magnitude % 10 );
        result.push_back( static_cast<char>( '0' + digit ) );
        magnitude /= 10;
    }

    if( negative )
        result.push_back( '-' );

    std::reverse( result.begin(), result.end() );
    return result;
}


JSON boundedInteger( WIDE_INT aValue )
{
    if( aValue >= std::numeric_limits<int64_t>::min()
        && aValue <= std::numeric_limits<int64_t>::max() )
    {
        return static_cast<int64_t>( aValue );
    }

    return integerString( aValue );
}

} // namespace


KICHAD::DESIGN_SCRIPT_ELECTRICAL_ANALYZER::RESULT
KICHAD::DESIGN_SCRIPT_ELECTRICAL_ANALYZER::Analyze( const JSON& aCompilerIr )
{
    RESULT result;

    if( !aCompilerIr.is_object() || !aCompilerIr.contains( "electrical" )
        || !aCompilerIr["electrical"].is_object() )
    {
        issue( result.issues, "missing_electrical_contract",
               "KDS has no typed electrical-analysis contract" );
        result.summary = { { "rails", 0 }, { "ratings", 0 }, { "thermal", 0 },
                           { "logic", 0 } };
        return result;
    }

    const JSON& electrical = aCompilerIr["electrical"];

    for( const JSON& rail : electrical.value( "rails", JSON::array() ) )
    {
        const std::string id = rail.value( "id", "" );
        WIDE_INT loadNa = 0;

        for( const JSON& load : rail.value( "loads", JSON::array() ) )
            loadNa += load.value( "currentNa", int64_t( 0 ) );

        const int64_t reserve = rail.value( "reservePpm", int64_t( 0 ) );
        const WIDE_INT requiredNa =
                ( loadNa * static_cast<WIDE_INT>( ONE_MILLION + reserve )
                  + ONE_MILLION - 1 )
                / ONE_MILLION;
        const int64_t availableNa = rail.value( "sourceCurrentNa", int64_t( 0 ) );

        if( requiredNa > availableNa )
        {
            issue( result.issues, "rail_current_budget_exceeded",
                   "rail " + id + " load plus reserve exceeds source current",
                   { { "rail", id }, { "loadNa", boundedInteger( loadNa ) },
                     { "reservePpm", reserve },
                     { "requiredNa", boundedInteger( requiredNa ) },
                     { "availableNa", availableNa } } );
        }
    }

    for( const JSON& rating : electrical.value( "ratings", JSON::array() ) )
    {
        const std::string component = rating.value( "component", "" );
        const int64_t derating = rating.value( "deratingPpm", ONE_MILLION );

        for( const auto& [kind, operating, rated] :
             { std::tuple{ "voltage", "operatingVoltageNv", "ratedVoltageNv" },
               std::tuple{ "current", "operatingCurrentNa", "ratedCurrentNa" },
               std::tuple{ "power", "operatingPowerNw", "ratedPowerNw" } } )
        {
            if( !rating.contains( operating ) || !rating.contains( rated ) )
                continue;

            const int64_t operatingValue = rating[operating].get<int64_t>();
            const int64_t ratedValue = rating[rated].get<int64_t>();
            const int64_t allowed = static_cast<int64_t>(
                    static_cast<long double>( ratedValue ) * derating / ONE_MILLION );

            if( operatingValue > allowed )
            {
                issue( result.issues, "component_derating_exceeded",
                       "component " + component + " exceeds its derated " + kind + " limit",
                       { { "component", component }, { "quantity", kind },
                         { "operating", operatingValue }, { "rated", ratedValue },
                         { "deratingPpm", derating }, { "allowed", allowed } } );
            }
        }
    }

    for( const JSON& thermal : electrical.value( "thermal", JSON::array() ) )
    {
        const std::string component = thermal.value( "component", "" );
        const int64_t dissipationNw = thermal.value( "dissipationNw", int64_t( 0 ) );
        const int64_t theta = thermal.value( "thetaJaMicroCPerW", int64_t( 0 ) );
        const int64_t ambient = thermal.value( "ambientMicroC", int64_t( 0 ) );
        const int64_t maximum = thermal.value( "maximumJunctionMicroC", int64_t( 0 ) );
        const long double rise = static_cast<long double>( dissipationNw ) * theta
                                 / 1'000'000'000.0L;
        const long double junction = static_cast<long double>( ambient ) + rise;

        if( junction > maximum )
        {
            issue( result.issues, "junction_temperature_exceeded",
                   "component " + component + " estimated junction temperature exceeds limit",
                   { { "component", component }, { "ambientMicroC", ambient },
                     { "riseMicroC", static_cast<double>( rise ) },
                     { "estimatedJunctionMicroC", static_cast<double>( junction ) },
                     { "maximumJunctionMicroC", maximum } } );
        }
    }

    for( const JSON& logic : electrical.value( "logic", JSON::array() ) )
    {
        const std::string id = logic.value( "id", "" );
        const int64_t outputLow = logic.value( "outputLowNv", int64_t( 0 ) );
        const int64_t outputHigh = logic.value( "outputHighNv", int64_t( 0 ) );
        const int64_t inputLow = logic.value( "inputLowNv", int64_t( 0 ) );
        const int64_t inputHigh = logic.value( "inputHighNv", int64_t( 0 ) );
        const int64_t signalMin = logic.value( "signalMinimumNv", int64_t( 0 ) );
        const int64_t signalMax = logic.value( "signalMaximumNv", int64_t( 0 ) );

        if( outputLow > inputLow )
            issue( result.issues, "logic_low_level_incompatible",
                   "logic " + id + " driver low level exceeds receiver threshold",
                   { { "logic", id }, { "outputLowNv", outputLow },
                     { "inputLowNv", inputLow } } );

        if( outputHigh < inputHigh )
            issue( result.issues, "logic_high_level_incompatible",
                   "logic " + id + " driver high level is below receiver threshold",
                   { { "logic", id }, { "outputHighNv", outputHigh },
                     { "inputHighNv", inputHigh } } );

        if( signalMin > signalMax )
            issue( result.issues, "logic_signal_range_invalid",
                   "logic " + id + " signal range is reversed", { { "logic", id } } );

        if( logic.contains( "absoluteMinimumNv" )
            && ( signalMin < logic["absoluteMinimumNv"].get<int64_t>()
                 || signalMax > logic["absoluteMaximumNv"].get<int64_t>() ) )
        {
            issue( result.issues, "logic_absolute_rating_exceeded",
                   "logic " + id + " signal range exceeds receiver absolute ratings",
                   { { "logic", id }, { "signalMinimumNv", signalMin },
                     { "signalMaximumNv", signalMax },
                     { "absoluteMinimumNv", logic["absoluteMinimumNv"] },
                     { "absoluteMaximumNv", logic["absoluteMaximumNv"] } } );
        }
    }

    result.summary = {
        { "rails", electrical.value( "rails", JSON::array() ).size() },
        { "ratings", electrical.value( "ratings", JSON::array() ).size() },
        { "thermal", electrical.value( "thermal", JSON::array() ).size() },
        { "logic", electrical.value( "logic", JSON::array() ).size() },
        { "issues", result.issues.size() }
    };
    result.clean = result.issues.empty();
    return result;
}
