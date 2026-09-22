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

#include "design_script_escape_analyzer.h"

#include "kichad_from_chars.h"
#include "lossless_sexpr_document.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;

constexpr int64_t MICRON = 1000;
constexpr size_t  MAX_PADS = 20000;
/// Below this pitch a package is worth analyzing at all; coarser parts never set the floor.
constexpr int64_t FINE_PITCH_LIMIT_NM = 1200000;

struct PAD
{
    int64_t x = 0;
    int64_t y = 0;
    int64_t width = 0;
    int64_t height = 0;
};


bool millimetres( const std::string& aText, int64_t& aNanometers )
{
    long double value = 0.0L;
    const std::from_chars_result parsed =
            KICHAD::FromChars( aText.data(), aText.data() + aText.size(), value );

    if( parsed.ec != std::errc() || parsed.ptr != aText.data() + aText.size()
        || !std::isfinite( value ) || std::fabsl( value ) > 10000.0L )
        return false;

    aNanometers = static_cast<int64_t>( std::llroundl( value * 1000000.0L ) );
    return true;
}


std::vector<size_t> directLists( const DOCUMENT& aDocument, size_t aParent,
                                 const std::string& aHead )
{
    std::vector<size_t> result;

    for( size_t child : aDocument.Nodes().at( aParent ).children )
    {
        if( aDocument.Nodes().at( child ).kind == DOCUMENT::NODE_KIND::LIST
            && aDocument.ListHead( child ) == aHead )
            result.push_back( child );
    }

    return result;
}


/** Copper pads with a position and size; everything else is ignored. */
bool parsePads( const std::string& aSource, std::vector<PAD>& aPads )
{
    std::string parseError;
    std::unique_ptr<DOCUMENT> document = DOCUMENT::Parse( aSource, &parseError );

    if( !document || document->Roots().empty()
        || document->ListHead( document->Roots().front() ) != "footprint" )
        return false;

    for( size_t padNode : directLists( *document, document->Roots().front(), "pad" ) )
    {
        const std::vector<size_t> positions = directLists( *document, padNode, "at" );
        const std::vector<size_t> sizes = directLists( *document, padNode, "size" );

        if( positions.size() != 1 || sizes.size() != 1 )
            continue;

        const DOCUMENT::NODE& position = document->Nodes().at( positions.front() );
        const DOCUMENT::NODE& size = document->Nodes().at( sizes.front() );

        if( position.children.size() < 3 || size.children.size() < 3 )
            continue;

        PAD pad;

        if( !millimetres( document->AtomText( position.children[1] ), pad.x )
            || !millimetres( document->AtomText( position.children[2] ), pad.y )
            || !millimetres( document->AtomText( size.children[1] ), pad.width )
            || !millimetres( document->AtomText( size.children[2] ), pad.height )
            || pad.width <= 0 || pad.height <= 0 )
            continue;

        aPads.push_back( pad );

        if( aPads.size() > MAX_PADS )
            return false;
    }

    return !aPads.empty();
}


/** Round a nanometre floor down to whole microns so a reported value is orderable at a fab. */
int64_t wholeMicronsDown( int64_t aNanometers )
{
    return aNanometers <= 0 ? 0 : ( aNanometers / MICRON ) * MICRON;
}


struct ARRAY_GEOMETRY
{
    bool    isArray = false;
    bool    hasInteriorPads = false;
    int64_t pitch = 0;        ///< smallest centre-to-centre spacing between neighbours
    int64_t padExtent = 0;    ///< largest pad dimension facing that channel
    size_t  columns = 0;
    size_t  rows = 0;
};


/**
 * Decide whether the pads form a grid array and, if so, its pitch and pad size.
 *
 * Distinct pad coordinates are collected per axis; a grid array has several rows and several
 * columns, and its pitch is the smallest spacing between adjacent coordinates.  Interior pads
 * (any pad not on the outermost row or column) are the ones that must escape through a channel.
 */
ARRAY_GEOMETRY arrayGeometry( const std::vector<PAD>& aPads )
{
    ARRAY_GEOMETRY geometry;

    if( aPads.size() < 9 )
        return geometry;

    std::set<int64_t> columnSet;
    std::set<int64_t> rowSet;

    for( const PAD& pad : aPads )
    {
        columnSet.insert( pad.x );
        rowSet.insert( pad.y );
    }

    if( columnSet.size() < 3 || rowSet.size() < 3 )
        return geometry;

    const auto smallestStep = []( const std::set<int64_t>& aValues )
    {
        int64_t step = 0;

        for( auto it = std::next( aValues.begin() ); it != aValues.end(); ++it )
        {
            const int64_t delta = *it - *std::prev( it );

            if( delta > 0 && ( step == 0 || delta < step ) )
                step = delta;
        }

        return step;
    };

    const int64_t columnStep = smallestStep( columnSet );
    const int64_t rowStep = smallestStep( rowSet );

    if( columnStep <= 0 || rowStep <= 0 )
        return geometry;

    geometry.pitch = std::min( columnStep, rowStep );

    if( geometry.pitch > FINE_PITCH_LIMIT_NM )
        return geometry;

    const int64_t minimumX = *columnSet.begin();
    const int64_t maximumX = *columnSet.rbegin();
    const int64_t minimumY = *rowSet.begin();
    const int64_t maximumY = *rowSet.rbegin();

    for( const PAD& pad : aPads )
    {
        geometry.padExtent = std::max( geometry.padExtent, std::max( pad.width, pad.height ) );

        if( pad.x > minimumX && pad.x < maximumX && pad.y > minimumY && pad.y < maximumY )
            geometry.hasInteriorPads = true;
    }

    geometry.columns = columnSet.size();
    geometry.rows = rowSet.size();
    geometry.isArray = geometry.padExtent > 0 && geometry.padExtent < geometry.pitch;
    return geometry;
}

} // namespace


KICHAD::DESIGN_SCRIPT_ESCAPE_ANALYZER::RESULT
KICHAD::DESIGN_SCRIPT_ESCAPE_ANALYZER::Analyze( const JSON& aCompilerIr,
                                                 const JSON& aFootprintSources )
{
    RESULT result;
    size_t analyzedPackages = 0;

    if( !aCompilerIr.is_object() || !aCompilerIr.contains( "schematic" )
        || !aCompilerIr["schematic"].is_object()
        || !aCompilerIr["schematic"].contains( "components" )
        || !aCompilerIr["schematic"]["components"].is_array()
        || !aFootprintSources.is_object() )
    {
        result.summary = { { "analyzedPackages", 0 }, { "finePitchPackages", 0 } };
        return result;
    }

    // Declared floors, when the KDS declares any; a design with no rules still learns what it
    // needs, which is the point of estimating before a fab profile is chosen.
    const JSON rules = aCompilerIr.value( "rules", JSON::object() );
    const bool haveRules = rules.is_object() && rules.contains( "minimumTrackWidthNm" )
                           && rules.contains( "minimumClearanceNm" );
    const int64_t declaredTrack =
            haveRules ? rules.value( "minimumTrackWidthNm", int64_t( 0 ) ) : 0;
    const int64_t declaredClearance =
            haveRules ? rules.value( "minimumClearanceNm", int64_t( 0 ) ) : 0;
    const int64_t declaredVia =
            rules.is_object() ? rules.value( "minimumViaDiameterNm", int64_t( 0 ) ) : 0;

    std::map<std::string, std::vector<std::string>> componentsByFootprint;

    for( const JSON& component : aCompilerIr["schematic"]["components"] )
    {
        if( !component.is_object() || !component.contains( "reference" )
            || !component["reference"].is_string() || !component.contains( "footprint" )
            || !component["footprint"].is_string() )
            continue;

        const std::string footprint = component["footprint"].get<std::string>();

        if( footprint.empty() || footprint == "none" )
            continue;

        componentsByFootprint[footprint].push_back( component["reference"].get<std::string>() );
    }

    for( auto& [libraryId, references] : componentsByFootprint )
    {
        if( !aFootprintSources.contains( libraryId )
            || !aFootprintSources[libraryId].is_string() )
            continue;

        std::vector<PAD> pads;

        if( !parsePads( aFootprintSources[libraryId].get<std::string>(), pads ) )
            continue;

        ++analyzedPackages;
        const ARRAY_GEOMETRY geometry = arrayGeometry( pads );

        if( !geometry.isArray || !geometry.hasInteriorPads )
            continue;

        // One track escaping between two adjacent pads: track + two clearances must fit the
        // channel left between their edges.
        const int64_t channel = geometry.pitch - geometry.padExtent;
        // A dogbone via sits in the diagonal pocket between four pads.
        const long double diagonal = geometry.pitch * std::sqrt( 2.0L );
        const int64_t pocket =
                static_cast<int64_t>( std::llroundl( diagonal - geometry.padExtent ) );
        // Split the channel evenly between the track and its two clearances, then round down to
        // whole microns so the number can be ordered from a fabricator as-is.
        const int64_t requiredFloor = wholeMicronsDown( channel / 3 );
        std::sort( references.begin(), references.end() );

        JSON requirement = { { "footprint", libraryId },
                             { "components", references },
                             { "pitchNm", geometry.pitch },
                             { "padNm", geometry.padExtent },
                             { "columns", geometry.columns },
                             { "rows", geometry.rows },
                             { "escapeChannelNm", channel },
                             { "diagonalPocketNm", pocket },
                             { "requiredTrackNm", requiredFloor },
                             { "requiredClearanceNm", requiredFloor },
                             { "maximumViaDiameterNm", wholeMicronsDown( pocket ) } };

        if( haveRules )
        {
            const int64_t declaredNeed = declaredTrack + 2 * declaredClearance;
            requirement["declaredTrackNm"] = declaredTrack;
            requirement["declaredClearanceNm"] = declaredClearance;
            requirement["declaredEscapeWidthNm"] = declaredNeed;
            requirement["escapes"] = declaredNeed <= channel;

            if( declaredNeed > channel )
            {
                result.feasible = false;
                result.issues.push_back(
                        { { "category", "layout" },
                          { "type", "escape_infeasible" },
                          { "severity", "error" },
                          { "component", references.front() },
                          { "description",
                            "Package " + libraryId + " on "
                                    + ( references.size() == 1
                                                ? references.front()
                                                : references.front() + " and "
                                                          + std::to_string( references.size() - 1 )
                                                          + " more" )
                                    + " cannot be escaped with the declared rules: its "
                                    + std::to_string( geometry.pitch / MICRON )
                                    + " um pitch leaves a "
                                    + std::to_string( channel / MICRON )
                                    + " um channel, but one escape track needs "
                                    + std::to_string( declaredNeed / MICRON )
                                    + " um. Declare rules at or below "
                                    + std::to_string( requiredFloor / MICRON )
                                    + " um track and clearance with a fab that holds them, or "
                                      "choose a coarser package." },
                          { "footprint", libraryId },
                          { "pitchNm", geometry.pitch },
                          { "escapeChannelNm", channel },
                          { "declaredEscapeWidthNm", declaredNeed },
                          { "requiredTrackNm", requiredFloor },
                          { "requiredClearanceNm", requiredFloor } } );
            }

            if( declaredVia > 0 && pocket > 0 && declaredVia > pocket )
            {
                result.feasible = false;
                result.issues.push_back(
                        { { "category", "layout" },
                          { "type", "via_does_not_fit_pocket" },
                          { "severity", "error" },
                          { "component", references.front() },
                          { "description",
                            "Package " + libraryId + " leaves a "
                                    + std::to_string( pocket / MICRON )
                                    + " um diagonal pocket between pads, but the declared "
                                      "minimum via diameter is "
                                    + std::to_string( declaredVia / MICRON )
                                    + " um, so a dogbone escape via cannot be placed. Declare a "
                                      "via at or below "
                                    + std::to_string( wholeMicronsDown( pocket ) / MICRON )
                                    + " um, or use via-in-pad (HDI)." },
                          { "footprint", libraryId },
                          { "diagonalPocketNm", pocket },
                          { "declaredViaDiameterNm", declaredVia },
                          { "maximumViaDiameterNm", wholeMicronsDown( pocket ) } } );
            }
        }
        else
        {
            // No declared rules yet: report the floors the package will demand so they are
            // never declared coarser than the design can route.
            requirement["escapes"] = nullptr;
            result.issues.push_back(
                    { { "category", "layout" },
                      { "type", "fab_features_required" },
                      { "severity", "warning" },
                      { "component", references.front() },
                      { "description",
                        "Package " + libraryId + " needs a fabricator holding "
                                + std::to_string( requiredFloor / MICRON )
                                + " um track and clearance (and a via at or below "
                                + std::to_string( wholeMicronsDown( pocket ) / MICRON )
                                + " um) to escape its "
                                + std::to_string( geometry.pitch / MICRON )
                                + " um pitch. Declare (rules ...) at or below those floors and a "
                                  "(fab ...) profile that meets them before place and route." },
                      { "footprint", libraryId },
                      { "requiredTrackNm", requiredFloor },
                      { "requiredClearanceNm", requiredFloor },
                      { "maximumViaDiameterNm", wholeMicronsDown( pocket ) } } );
        }

        result.requirements.push_back( std::move( requirement ) );
    }

    result.summary = { { "analyzedPackages", analyzedPackages },
                       { "finePitchPackages", result.requirements.size() },
                       { "rulesDeclared", haveRules } };
    return result;
}
