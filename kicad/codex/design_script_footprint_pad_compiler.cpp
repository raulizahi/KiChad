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

#include "design_script_footprint_pad_compiler.h"
#include "kichad_from_chars.h"

#include "design_script_footprint_custom_pad_compiler.h"
#include "design_script_footprint_hole_treatment_compiler.h"
#include "design_script_footprint_padstack_compiler.h"
#include "design_script_teardrop_compiler.h"
#include "design_script_via_backdrill_compiler.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_PAD_COMPILER::RESULT;
using CUSTOM_COMPILER = KICHAD::DESIGN_SCRIPT_FOOTPRINT_CUSTOM_PAD_COMPILER;
using HOLE_TREATMENT_COMPILER = KICHAD::DESIGN_SCRIPT_FOOTPRINT_HOLE_TREATMENT_COMPILER;
using PADSTACK_COMPILER = KICHAD::DESIGN_SCRIPT_FOOTPRINT_PADSTACK_COMPILER;
using BACKDRILL_COMPILER = KICHAD::DESIGN_SCRIPT_VIA_BACKDRILL_COMPILER;
using TEARDROP_COMPILER = KICHAD::DESIGN_SCRIPT_TEARDROP_COMPILER;

constexpr int64_t MAX_COORDINATE_NM = 2'000'000'000LL;
constexpr int64_t MAX_PAD_DIMENSION_NM = 1'000'000'000LL;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


bool scalar( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    if( aNode >= aDocument.Nodes().size()
        || aDocument.Nodes()[aNode].kind == DOCUMENT::NODE_KIND::LIST )
    {
        return false;
    }

    aValue = aDocument.AtomText( aNode );
    return true;
}


bool identifier( const std::string& aValue )
{
    if( aValue.empty() || aValue.size() > 128 )
        return false;

    for( unsigned char character : aValue )
    {
        if( !( std::isalnum( character ) || character == '_' || character == '-'
               || character == '.' || character == '+' ) )
        {
            return false;
        }
    }

    return true;
}


bool boolean( const std::string& aText, bool& aValue )
{
    if( aText == "true" )
    {
        aValue = true;
        return true;
    }

    if( aText == "false" )
    {
        aValue = false;
        return true;
    }

    return false;
}


bool distance( const std::string& aText, int64_t& aNanometers )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = KICHAD::FromChars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr == begin || !std::isfinite( value ) )
        return false;

    const std::string_view unit( converted.ptr, static_cast<size_t>( end - converted.ptr ) );
    long double scale = 0.0L;

    if( unit == "mm" )
        scale = 1'000'000.0L;
    else if( unit == "mil" )
        scale = 25'400.0L;
    else if( unit == "um" )
        scale = 1'000.0L;
    else if( unit == "nm" )
        scale = 1.0L;
    else if( unit == "in" )
        scale = 25'400'000.0L;
    else
        return false;

    const long double rounded = std::round( value * scale );

    if( !std::isfinite( rounded ) || rounded < -MAX_COORDINATE_NM
        || rounded > MAX_COORDINATE_NM )
    {
        return false;
    }

    aNanometers = static_cast<int64_t>( rounded );
    return true;
}


bool angle( const std::string& aText, int64_t& aTenths )
{
    if( !aText.ends_with( "deg" ) )
        return false;

    const std::string_view number( aText.data(), aText.size() - 3 );
    long double degrees = 0.0L;
    const std::from_chars_result converted =
            KICHAD::FromChars( number.data(), number.data() + number.size(), degrees );

    if( converted.ec != std::errc() || converted.ptr != number.data() + number.size()
        || !std::isfinite( degrees ) )
    {
        return false;
    }

    const long double tenths = std::round( degrees * 10.0L );

    if( tenths < -3600.0L || tenths > 3600.0L
        || std::fabs( tenths - degrees * 10.0L ) > 0.000001L )
    {
        return false;
    }

    aTenths = static_cast<int64_t>( tenths );
    return true;
}


bool decimalPpm( const std::string& aText, int64_t aMinimum, int64_t aMaximum,
                 int64_t& aPartsPerMillion )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = KICHAD::FromChars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr != end || !std::isfinite( value ) )
        return false;

    const long double ppm = std::round( value * 1'000'000.0L );

    if( ppm < static_cast<long double>( aMinimum )
        || ppm > static_cast<long double>( aMaximum ) )
    {
        return false;
    }

    aPartsPerMillion = static_cast<int64_t>( ppm );
    return true;
}


bool oneValue( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], aValue );
}


bool point( const DOCUMENT& aDocument, size_t aNode, JSON& aPoint )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string xText;
    std::string yText;
    int64_t x = 0;
    int64_t y = 0;

    if( node.kind != DOCUMENT::NODE_KIND::LIST || node.children.size() != 3
        || !scalar( aDocument, node.children[1], xText )
        || !scalar( aDocument, node.children[2], yText )
        || !distance( xText, x ) || !distance( yText, y ) )
    {
        return false;
    }

    aPoint = { { "xNm", x }, { "yNm", y } };
    return true;
}


bool nullableDistance( const DOCUMENT& aDocument, size_t aNode, JSON& aValue,
                       bool aNonNegative )
{
    std::string text;
    int64_t parsed = 0;

    if( !oneValue( aDocument, aNode, text ) )
        return false;

    if( text == "inherit" )
    {
        aValue = nullptr;
        return true;
    }

    if( !distance( text, parsed ) || ( aNonNegative && parsed < 0 )
        || parsed < -100'000'000 || parsed > 100'000'000 )
    {
        return false;
    }

    aValue = parsed;
    return true;
}


bool nullableDecimal( const DOCUMENT& aDocument, size_t aNode, JSON& aValue )
{
    std::string text;
    int64_t parsed = 0;

    if( !oneValue( aDocument, aNode, text ) )
        return false;

    if( text == "inherit" )
    {
        aValue = nullptr;
        return true;
    }

    if( !decimalPpm( text, -1'000'000, 1'000'000, parsed ) )
        return false;

    aValue = parsed;
    return true;
}


bool compileLayers( const DOCUMENT& aDocument, size_t aNode, JSON& aLayers )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    static const std::set<std::string> allowed = {
        "all_copper", "all_mask", "F.Cu", "B.Cu", "F.Mask", "B.Mask",
        "F.Paste", "B.Paste", "F.Adhes", "B.Adhes"
    };
    std::set<std::string> unique;

    if( node.children.size() < 2 || node.children.size() > 17 )
        return false;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        std::string layer;

        if( !scalar( aDocument, node.children[index], layer ) || !allowed.contains( layer )
            || !unique.emplace( layer ).second )
        {
            return false;
        }

        aLayers.push_back( layer );
    }

    if( unique.contains( "all_copper" )
        && ( unique.contains( "F.Cu" ) || unique.contains( "B.Cu" ) ) )
    {
        return false;
    }

    if( unique.contains( "all_mask" )
        && ( unique.contains( "F.Mask" ) || unique.contains( "B.Mask" ) ) )
    {
        return false;
    }

    return true;
}


bool compileDrill( const DOCUMENT& aDocument, size_t aNode, JSON& aDrill )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string shape;
    std::string xText;
    std::string yText;
    int64_t x = 0;
    int64_t y = 0;

    if( node.children.size() < 3 || node.children.size() > 4
        || !scalar( aDocument, node.children[1], shape )
        || !scalar( aDocument, node.children[2], xText ) || !distance( xText, x )
        || x <= 0 || x > MAX_PAD_DIMENSION_NM )
    {
        return false;
    }

    if( shape == "round" && node.children.size() == 3 )
    {
        y = x;
    }
    else if( shape == "oval" && node.children.size() == 4
             && scalar( aDocument, node.children[3], yText ) && distance( yText, y )
             && y > 0 && y <= MAX_PAD_DIMENSION_NM )
    {
        // Parsed above.
    }
    else
    {
        return false;
    }

    aDrill = { { "shape", shape }, { "xNm", x }, { "yNm", y } };
    return true;
}


bool compileChamfers( const DOCUMENT& aDocument, size_t aNode, JSON& aCorners )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    static const std::set<std::string> allowed = {
        "top_left", "top_right", "bottom_left", "bottom_right"
    };
    std::set<std::string> unique;

    if( node.children.size() < 2 || node.children.size() > 5 )
        return false;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        std::string corner;

        if( !scalar( aDocument, node.children[index], corner ) || !allowed.contains( corner )
            || !unique.emplace( corner ).second )
        {
            return false;
        }

        aCorners.push_back( corner );
    }

    return true;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_FOOTPRINT_PAD_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_PAD_COMPILER::Compile( const LOSSLESS_SEXPR_DOCUMENT& aDocument,
                                               size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( result, "invalid_authored_footprint_pad_id",
                    "footprint pad requires one unique bounded logical ID" );
        return result;
    }

    result.pad = {
        { "id", id }, { "number", "" }, { "type", "" }, { "shape", "" },
        { "rotationTenths", 0 }, { "layers", JSON::array() }, { "drill", nullptr },
        { "shapeOffset", { { "xNm", 0 }, { "yNm", 0 } } },
        { "trapezoidDelta", { { "xNm", 0 }, { "yNm", 0 } } },
        { "roundrectRadiusNm", nullptr }, { "chamferRatioPpm", nullptr },
        { "chamferCorners", JSON::array() }, { "custom", nullptr }, { "padstack", nullptr },
        { "backdrills", nullptr }, { "holeTreatment", nullptr }, { "teardrop", nullptr },
        { "property", "none" },
        { "pinFunction", "" }, { "pinType", "" }, { "dieLengthNm", 0 },
        { "solderMaskMarginNm", nullptr }, { "solderPasteMarginNm", nullptr },
        { "solderPasteMarginPpm", nullptr }, { "clearanceNm", nullptr },
        { "zoneConnection", "inherit" }, { "thermalSpokeWidthNm", nullptr },
        { "thermalSpokeAngleTenths", nullptr }, { "thermalGapNm", nullptr },
        { "removeUnusedLayers", false }, { "keepEndLayers", false }
    };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_footprint_pad_field",
                        "footprint pad " + id + " field " + head + " occurs more than once" );
            continue;
        }

        if( head == "at" || head == "size" || head == "shape_offset"
            || head == "trapezoid_delta" )
        {
            JSON parsed;

            if( !point( aDocument, child, parsed ) )
            {
                diagnostic( result, "invalid_authored_footprint_pad_geometry",
                            "footprint pad " + id + " " + head
                                    + " requires two bounded physical coordinates" );
            }
            else
            {
                const std::string key = head == "shape_offset" ? "shapeOffset"
                                        : head == "trapezoid_delta" ? "trapezoidDelta"
                                                                      : head;
                result.pad[key] = std::move( parsed );
            }

            continue;
        }

        if( head == "layers" )
        {
            if( !compileLayers( aDocument, child, result.pad["layers"] ) )
                diagnostic( result, "invalid_authored_footprint_pad_layers",
                            "pad layers require unique supported physical or all_copper/all_mask "
                            "tokens" );

            continue;
        }

        if( head == "drill" )
        {
            if( !compileDrill( aDocument, child, result.pad["drill"] ) )
                diagnostic( result, "invalid_authored_footprint_pad_drill",
                            "drill requires round DIAMETER or oval WIDTH HEIGHT" );

            continue;
        }

        if( head == "custom" )
        {
            CUSTOM_COMPILER::RESULT custom = CUSTOM_COMPILER::Compile( aDocument, child );

            for( JSON& entry : custom.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            result.pad["custom"] = std::move( custom.custom );
            continue;
        }

        if( head == "padstack" )
        {
            PADSTACK_COMPILER::RESULT padstack = PADSTACK_COMPILER::Compile( aDocument, child );

            for( JSON& entry : padstack.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            result.pad["padstack"] = std::move( padstack.padstack );
            continue;
        }

        if( head == "hole_treatment" )
        {
            HOLE_TREATMENT_COMPILER::RESULT treatment =
                    HOLE_TREATMENT_COMPILER::Compile( aDocument, child );

            for( JSON& entry : treatment.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            result.pad["holeTreatment"] = std::move( treatment.treatment );
            continue;
        }

        if( head == "backdrills" )
        {
            BACKDRILL_COMPILER::RESULT backdrills =
                    BACKDRILL_COMPILER::Compile( aDocument, child );

            for( JSON& entry : backdrills.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            result.pad["backdrills"] = std::move( backdrills.backdrills );
            continue;
        }

        if( head == "teardrop" )
        {
            TEARDROP_COMPILER::RESULT teardrop = TEARDROP_COMPILER::Compile( aDocument, child );

            for( JSON& entry : teardrop.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            result.pad["teardrop"] = std::move( teardrop.teardrop );
            continue;
        }

        if( head == "chamfer" )
        {
            if( !compileChamfers( aDocument, child, result.pad["chamferCorners"] ) )
                diagnostic( result, "invalid_authored_footprint_pad_chamfers",
                            "chamfer requires one through four unique corner names" );

            continue;
        }

        if( head == "solder_mask_margin" || head == "solder_paste_margin"
            || head == "clearance" || head == "thermal_spoke_width" || head == "thermal_gap" )
        {
            const bool nonNegative = head == "clearance" || head == "thermal_spoke_width"
                                     || head == "thermal_gap";
            JSON parsed;

            if( !nullableDistance( aDocument, child, parsed, nonNegative ) )
                diagnostic( result, "invalid_authored_footprint_pad_override",
                            head + " requires inherit or a bounded physical distance" );
            else
                result.pad[head == "solder_mask_margin" ? "solderMaskMarginNm"
                           : head == "solder_paste_margin" ? "solderPasteMarginNm"
                           : head == "clearance" ? "clearanceNm"
                           : head == "thermal_spoke_width" ? "thermalSpokeWidthNm"
                                                             : "thermalGapNm"] = parsed;

            continue;
        }

        if( head == "solder_paste_margin_ratio" )
        {
            JSON parsed;

            if( !nullableDecimal( aDocument, child, parsed ) )
                diagnostic( result, "invalid_authored_footprint_pad_paste_ratio",
                            "solder_paste_margin_ratio requires inherit or -1 through 1" );
            else
                result.pad["solderPasteMarginPpm"] = parsed;

            continue;
        }

        std::string value;

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( result, "invalid_authored_footprint_pad_field",
                        "footprint pad " + id + " field " + head + " requires one value" );
            continue;
        }

        if( head == "number" )
        {
            if( value.size() > 64 || value.find( '\0' ) != std::string::npos )
                diagnostic( result, "invalid_authored_footprint_pad_number",
                            "pad number must contain at most 64 UTF-8 bytes" );
            else
                result.pad["number"] = value;
        }
        else if( head == "type" )
        {
            static const std::set<std::string> types = {
                "smd", "thru_hole", "connect", "np_thru_hole"
            };

            if( !types.contains( value ) )
                diagnostic( result, "invalid_authored_footprint_pad_type",
                            "pad type must be smd, thru_hole, connect, or np_thru_hole" );
            else
                result.pad["type"] = value;
        }
        else if( head == "shape" )
        {
            static const std::set<std::string> shapes = {
                "circle", "rect", "oval", "trapezoid", "roundrect",
                "chamfered_rect", "custom"
            };

            if( !shapes.contains( value ) )
                diagnostic( result, "invalid_authored_footprint_pad_shape",
                            "pad shape must be circle, rect, oval, trapezoid, roundrect, "
                            "chamfered_rect, or custom" );
            else
                result.pad["shape"] = value;
        }
        else if( head == "rotation" )
        {
            int64_t parsed = 0;

            if( !angle( value, parsed ) )
                diagnostic( result, "invalid_authored_footprint_pad_rotation",
                            "pad rotation requires exact 0.1-degree units within +/-360 degrees" );
            else
                result.pad["rotationTenths"] = parsed;
        }
        else if( head == "roundrect_radius" )
        {
            int64_t parsed = 0;

            if( !distance( value, parsed ) || parsed < 0
                || parsed > MAX_PAD_DIMENSION_NM )
            {
                diagnostic( result, "invalid_authored_footprint_pad_roundrect_radius",
                            "roundrect_radius requires a non-negative bounded distance" );
            }
            else
            {
                result.pad["roundrectRadiusNm"] = parsed;
            }
        }
        else if( head == "chamfer_ratio" )
        {
            int64_t parsed = 0;

            if( !decimalPpm( value, 1, 500'000, parsed ) )
                diagnostic( result, "invalid_authored_footprint_pad_chamfer_ratio",
                            "chamfer_ratio must be greater than zero and no greater than 0.5" );
            else
                result.pad["chamferRatioPpm"] = parsed;
        }
        else if( head == "property" )
        {
            static const std::set<std::string> properties = {
                "none", "bga", "global_fiducial", "local_fiducial", "testpoint",
                "heatsink", "castellated", "mechanical", "pressfit"
            };

            if( !properties.contains( value ) )
                diagnostic( result, "invalid_authored_footprint_pad_property",
                            "pad property is not a supported KiCad 10 property" );
            else
                result.pad["property"] = value;
        }
        else if( head == "pin_function" || head == "pin_type" )
        {
            if( value.size() > 256 || value.find( '\0' ) != std::string::npos )
                diagnostic( result, "invalid_authored_footprint_pad_pin_metadata",
                            head + " must contain at most 256 UTF-8 bytes" );
            else
                result.pad[head == "pin_function" ? "pinFunction" : "pinType"] = value;
        }
        else if( head == "die_length" )
        {
            int64_t parsed = 0;

            if( !distance( value, parsed ) || parsed < 0 || parsed > MAX_PAD_DIMENSION_NM )
                diagnostic( result, "invalid_authored_footprint_pad_die_length",
                            "die_length requires a non-negative bounded distance" );
            else
                result.pad["dieLengthNm"] = parsed;
        }
        else if( head == "zone_connection" )
        {
            static const std::set<std::string> connections = {
                "inherit", "none", "thermal", "solid", "tht_thermal"
            };

            if( !connections.contains( value ) )
                diagnostic( result, "invalid_authored_footprint_pad_zone_connection",
                            "zone_connection must be inherit, none, thermal, solid, or "
                            "tht_thermal" );
            else
                result.pad["zoneConnection"] = value;
        }
        else if( head == "thermal_spoke_angle" )
        {
            if( value == "inherit" )
            {
                result.pad["thermalSpokeAngleTenths"] = nullptr;
            }
            else
            {
                int64_t parsed = 0;

                if( !angle( value, parsed ) )
                    diagnostic( result, "invalid_authored_footprint_pad_thermal_angle",
                                "thermal_spoke_angle requires inherit or exact 0.1-degree units" );
                else
                    result.pad["thermalSpokeAngleTenths"] = parsed;
            }
        }
        else if( head == "remove_unused_layers" || head == "keep_end_layers" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( result, "invalid_authored_footprint_pad_boolean",
                            head + " must be true or false" );
            else
                result.pad[head == "remove_unused_layers" ? "removeUnusedLayers"
                                                             : "keepEndLayers"] = parsed;
        }
        else
        {
            diagnostic( result, "unknown_authored_footprint_pad_field",
                        "unsupported footprint pad field " + head );
        }
    }

    const std::string type = result.pad["type"].get<std::string>();
    const std::string shape = result.pad["shape"].get<std::string>();

    if( type.empty() || shape.empty() || !result.pad.contains( "at" )
        || !result.pad.contains( "size" ) || result.pad["layers"].empty() )
    {
        diagnostic( result, "incomplete_authored_footprint_pad",
                    "pad " + id + " requires type, shape, at, size, and layers" );
    }

    if( result.pad.contains( "size" ) )
    {
        const int64_t width = result.pad["size"]["xNm"].get<int64_t>();
        const int64_t height = result.pad["size"]["yNm"].get<int64_t>();

        if( width <= 0 || height <= 0 || width > MAX_PAD_DIMENSION_NM
            || height > MAX_PAD_DIMENSION_NM )
        {
            diagnostic( result, "invalid_authored_footprint_pad_size",
                        "pad " + id + " size must be positive and no greater than 1 metre" );
        }

        if( shape == "circle" && width != height )
        {
            diagnostic( result, "invalid_authored_footprint_pad_circle_size",
                        "circle pads require equal width and height" );
        }

        if( shape == "roundrect" || shape == "chamfered_rect" )
        {
            if( result.pad["roundrectRadiusNm"].is_null()
                || ( shape == "roundrect"
                     && result.pad["roundrectRadiusNm"].get<int64_t>() <= 0 )
                || result.pad["roundrectRadiusNm"].get<int64_t>() * 2
                           > std::min( width, height ) )
            {
                diagnostic( result, "invalid_authored_footprint_pad_roundrect_radius",
                            "rounded/chamfered radius must be valid for the shorter pad side" );
            }
        }
        else if( !result.pad["roundrectRadiusNm"].is_null() )
        {
            diagnostic( result, "unexpected_authored_footprint_pad_roundrect_radius",
                        "roundrect_radius is only valid for roundrect and chamfered_rect pads" );
        }

        if( shape == "chamfered_rect" )
        {
            if( result.pad["chamferRatioPpm"].is_null()
                || result.pad["chamferCorners"].empty() )
            {
                diagnostic( result, "incomplete_authored_footprint_pad_chamfer",
                            "chamfered_rect requires chamfer_ratio and at least one corner" );
            }
        }
        else if( !result.pad["chamferRatioPpm"].is_null()
                 || !result.pad["chamferCorners"].empty() )
        {
            diagnostic( result, "unexpected_authored_footprint_pad_chamfer",
                        "chamfer fields are only valid for chamfered_rect pads" );
        }

        if( shape == "custom" )
        {
            if( !result.pad["custom"].is_object() )
                diagnostic( result, "incomplete_authored_custom_pad",
                            "custom shape requires one complete custom geometry form" );
        }
        else if( !result.pad["custom"].is_null() )
        {
            diagnostic( result, "unexpected_authored_custom_pad",
                        "custom geometry is only valid for custom pads" );
        }

        const int64_t deltaX = result.pad["trapezoidDelta"]["xNm"].get<int64_t>();
        const int64_t deltaY = result.pad["trapezoidDelta"]["yNm"].get<int64_t>();

        if( shape != "trapezoid" && ( deltaX != 0 || deltaY != 0 ) )
            diagnostic( result, "unexpected_authored_footprint_pad_trapezoid_delta",
                        "trapezoid_delta is only valid for trapezoid pads" );
        else if( shape == "trapezoid"
                 && ( std::abs( deltaX ) > height || std::abs( deltaY ) > width ) )
            diagnostic( result, "invalid_authored_footprint_pad_trapezoid_delta",
                        "trapezoid_delta cannot invert the pad" );
    }

    const bool drilled = type == "thru_hole" || type == "np_thru_hole";

    if( !result.pad["padstack"].is_null() && type != "thru_hole" )
        diagnostic( result, "invalid_authored_footprint_padstack_type",
                    "per-layer copper padstacks require a plated through-hole pad" );

    if( drilled != result.pad["drill"].is_object() )
        diagnostic( result, "invalid_authored_footprint_pad_drill_semantics",
                    drilled ? "through-hole pads require a drill"
                            : "smd and connect pads cannot declare a drill" );

    if( result.pad["teardrop"].is_object() && type == "np_thru_hole" )
        diagnostic( result, "invalid_authored_footprint_pad_teardrop_type",
                    "teardrops require an electrically connectable copper pad" );

    if( drilled && result.pad["drill"].is_object() && result.pad.contains( "size" )
        && ( result.pad["drill"]["xNm"].get<int64_t>()
                     > result.pad["size"]["xNm"].get<int64_t>()
             || result.pad["drill"]["yNm"].get<int64_t>()
                        > result.pad["size"]["yNm"].get<int64_t>() ) )
    {
        diagnostic( result, "invalid_authored_footprint_pad_drill_size",
                    "drill dimensions cannot exceed the pad dimensions" );
    }

    if( result.pad["backdrills"].is_object() )
    {
        if( type != "thru_hole" || !result.pad["drill"].is_object() )
        {
            diagnostic( result, "invalid_authored_footprint_pad_backdrill_type",
                        "top/bottom backdrilling requires a plated through-hole pad" );
        }
        else
        {
            const int64_t primaryDrill = std::max(
                    result.pad["drill"]["xNm"].get<int64_t>(),
                    result.pad["drill"]["yNm"].get<int64_t>() );
            int topStop = -1;
            int bottomStop = -1;

            for( const char* side : { "top", "bottom" } )
            {
                const JSON& operation = result.pad["backdrills"][side];

                if( !operation.is_object() )
                    continue;

                if( operation.value( "diameterNm", int64_t{ 0 } ) <= primaryDrill )
                    diagnostic( result, "invalid_authored_footprint_pad_backdrill_diameter",
                                "backdrill diameter must exceed the primary pad drill" );

                const std::string stopLayer = operation.value( "stopLayer", "" );
                int stopIndex = -1;

                if( stopLayer.starts_with( "In" ) && stopLayer.ends_with( ".Cu" ) )
                {
                    const std::string_view number(
                            stopLayer.data() + 2, stopLayer.size() - 5 );
                    const std::from_chars_result parsed = std::from_chars(
                            number.data(), number.data() + number.size(), stopIndex );

                    if( parsed.ec != std::errc()
                        || parsed.ptr != number.data() + number.size()
                        || stopIndex < 1 || stopIndex > 30 )
                    {
                        stopIndex = -1;
                    }
                }

                if( stopIndex < 1 )
                    diagnostic( result, "invalid_authored_footprint_pad_backdrill_stop_layer",
                                "backdrill stop_layer must be In1.Cu through In30.Cu" );

                if( side[0] == 't' )
                    topStop = stopIndex;
                else
                    bottomStop = stopIndex;
            }

            if( topStop > 0 && bottomStop > 0 && topStop >= bottomStop )
                diagnostic( result, "overlapping_authored_footprint_pad_backdrills",
                            "top and bottom backdrills must leave a plated layer span" );
        }
    }

    if( result.pad["holeTreatment"].is_object() )
    {
        const JSON& treatment = result.pad["holeTreatment"];

        for( const char* field : { "frontPostMachining", "backPostMachining" } )
        {
            if( !treatment[field].is_object() )
                continue;

            if( !drilled || !result.pad["drill"].is_object() )
            {
                diagnostic( result, "invalid_authored_footprint_pad_post_machining_type",
                            "post-machining requires a drilled footprint pad" );
                continue;
            }

            const int64_t diameter = treatment[field]["diameterNm"].is_number_integer()
                                             ? treatment[field]["diameterNm"].get<int64_t>()
                                             : 0;
            const int64_t drillDiameter = std::max(
                    result.pad["drill"]["xNm"].get<int64_t>(),
                    result.pad["drill"]["yNm"].get<int64_t>() );

            if( diameter <= drillDiameter )
                diagnostic( result, "invalid_authored_footprint_pad_post_machining_diameter",
                            "post-machining diameter must exceed the primary drill diameter" );
        }
    }

    if( type == "np_thru_hole" && !result.pad["number"].get_ref<const std::string&>().empty() )
        diagnostic( result, "numbered_authored_footprint_npth_pad",
                    "non-plated through-hole pads cannot have an electrical pad number" );

    std::set<std::string> layers;

    for( const JSON& layer : result.pad["layers"] )
        layers.emplace( layer.get<std::string>() );

    if( drilled && !layers.contains( "all_copper" ) )
        diagnostic( result, "invalid_authored_footprint_through_hole_layers",
                    "through-hole pads require all_copper in their layer set" );

    const bool frontCopper = layers.contains( "F.Cu" );
    const bool backCopper = layers.contains( "B.Cu" );
    const bool aperturePad = type == "smd" && !frontCopper && !backCopper;

    if( aperturePad )
    {
        const bool frontAperture = layers.contains( "F.Paste" )
                                   || layers.contains( "F.Mask" )
                                   || layers.contains( "F.Adhes" );
        const bool backAperture = layers.contains( "B.Paste" )
                                  || layers.contains( "B.Mask" )
                                  || layers.contains( "B.Adhes" );
        const bool onlyApertureLayers =
                std::all_of( layers.begin(), layers.end(),
                             []( const std::string& aLayer )
                             {
                                 return aLayer == "F.Paste" || aLayer == "F.Mask"
                                        || aLayer == "F.Adhes" || aLayer == "B.Paste"
                                        || aLayer == "B.Mask" || aLayer == "B.Adhes";
                             } );

        if( !result.pad["number"].get_ref<const std::string&>().empty() )
        {
            diagnostic( result, "numbered_authored_footprint_aperture_pad",
                        "non-copper aperture pads must have an empty pad number" );
        }

        if( !onlyApertureLayers || frontAperture == backAperture )
        {
            diagnostic( result, "invalid_authored_footprint_aperture_pad_layers",
                        "non-copper aperture pads require front-only or back-only paste, mask, "
                        "or adhesive layers" );
        }

        const bool electricalMetadata = result.pad["teardrop"].is_object()
                                        || result.pad["property"] != "none"
                                        || !result.pad["pinFunction"].get_ref<
                                                const std::string&>().empty()
                                        || !result.pad["pinType"].get_ref<
                                                const std::string&>().empty()
                                        || result.pad["dieLengthNm"] != 0
                                        || !result.pad["clearanceNm"].is_null()
                                        || result.pad["zoneConnection"] != "inherit"
                                        || !result.pad["thermalSpokeWidthNm"].is_null()
                                        || !result.pad["thermalSpokeAngleTenths"].is_null()
                                        || !result.pad["thermalGapNm"].is_null()
                                        || result.pad["removeUnusedLayers"] == true
                                        || result.pad["keepEndLayers"] == true;

        if( electricalMetadata )
        {
            diagnostic( result,
                        "invalid_authored_footprint_aperture_pad_electrical_metadata",
                        "non-copper aperture pads are stencil/mask/adhesive geometry and "
                        "cannot declare electrical pad properties" );
        }

        if( !result.pad["holeTreatment"].is_null()
            || !result.pad["backdrills"].is_null()
            || !result.pad["padstack"].is_null() )
        {
            diagnostic( result,
                        "invalid_authored_footprint_aperture_pad_manufacturing_metadata",
                        "non-copper aperture pads cannot declare drilled-hole or copper "
                        "padstack manufacturing properties" );
        }

        if( !result.pad["solderMaskMarginNm"].is_null()
            && !layers.contains( "F.Mask" ) && !layers.contains( "B.Mask" ) )
        {
            diagnostic( result, "invalid_authored_footprint_aperture_pad_mask_margin",
                        "solder_mask_margin requires a mask layer on the aperture pad" );
        }

        if( ( !result.pad["solderPasteMarginNm"].is_null()
              || !result.pad["solderPasteMarginPpm"].is_null() )
            && !layers.contains( "F.Paste" ) && !layers.contains( "B.Paste" ) )
        {
            diagnostic( result, "invalid_authored_footprint_aperture_pad_paste_margin",
                        "solder-paste margins require a paste layer on the aperture pad" );
        }
    }
    else if( ( type == "smd" || type == "connect" ) && !frontCopper && !backCopper )
    {
        diagnostic( result, "invalid_authored_footprint_surface_pad_layers",
                    "connect pads and electrical SMD pads require F.Cu or B.Cu" );
    }

    if( result.pad["keepEndLayers"].get<bool>()
        && !result.pad["removeUnusedLayers"].get<bool>() )
    {
        diagnostic( result, "invalid_authored_footprint_pad_keep_end_layers",
                    "keep_end_layers requires remove_unused_layers true" );
    }

    if( result.pad["removeUnusedLayers"].get<bool>() && type != "thru_hole" )
        diagnostic( result, "invalid_authored_footprint_pad_remove_unused_layers",
                    "remove_unused_layers applies only to plated through-hole pads" );

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
