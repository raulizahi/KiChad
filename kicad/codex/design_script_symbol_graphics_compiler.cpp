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

#include "design_script_symbol_graphics_compiler.h"
#include "kichad_wide_int.h"
#include "kichad_from_chars.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_SYMBOL_GRAPHICS_COMPILER::RESULT;

constexpr int64_t MAX_COORDINATE_NM = 2'000'000'000LL;
constexpr size_t MAX_POLYLINE_POINTS = 16'384;


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


bool integer( const std::string& aText, int64_t aMinimum, int64_t aMaximum, int64_t& aValue )
{
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = std::from_chars( begin, end, aValue );
    return converted.ec == std::errc() && converted.ptr == end
           && aValue >= aMinimum && aValue <= aMaximum;
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


bool color( const DOCUMENT& aDocument, size_t aNode, JSON& aColor )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string redText;
    std::string greenText;
    std::string blueText;
    std::string alphaText;
    int64_t red = 0;
    int64_t green = 0;
    int64_t blue = 0;
    long double alpha = 0.0L;

    if( node.kind != DOCUMENT::NODE_KIND::LIST || node.children.size() != 5
        || !scalar( aDocument, node.children[1], redText )
        || !scalar( aDocument, node.children[2], greenText )
        || !scalar( aDocument, node.children[3], blueText )
        || !scalar( aDocument, node.children[4], alphaText )
        || !integer( redText, 0, 255, red ) || !integer( greenText, 0, 255, green )
        || !integer( blueText, 0, 255, blue ) )
    {
        return false;
    }

    const char* begin = alphaText.data();
    const char* end = begin + alphaText.size();
    const std::from_chars_result converted = KICHAD::FromChars( begin, end, alpha );

    if( converted.ec != std::errc() || converted.ptr != end || !std::isfinite( alpha )
        || alpha < 0.0L || alpha > 1.0L )
    {
        return false;
    }

    aColor = { { "red", red }, { "green", green }, { "blue", blue },
               { "alphaPpm", static_cast<int64_t>( std::round( alpha * 1'000'000.0L ) ) } };
    return true;
}


bool compileStroke( const DOCUMENT& aDocument, size_t aNode, JSON& aStroke )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string widthText;
    std::string style;
    int64_t width = 0;
    static const std::set<std::string> styles = {
        "default", "solid", "dash", "dot", "dash_dot", "dash_dot_dot"
    };

    if( node.children.size() < 3 || node.children.size() > 4
        || !scalar( aDocument, node.children[1], widthText )
        || !scalar( aDocument, node.children[2], style ) || !distance( widthText, width )
        || width < 0 || width > 10'000'000 || !styles.contains( style ) )
    {
        return false;
    }

    aStroke = { { "widthNm", width }, { "style", style } };

    if( node.children.size() == 4 )
    {
        JSON parsedColor;

        if( aDocument.ListHead( node.children[3] ) != "color"
            || !color( aDocument, node.children[3], parsedColor ) )
        {
            return false;
        }

        aStroke["color"] = std::move( parsedColor );
    }

    return true;
}


bool compileFill( const DOCUMENT& aDocument, size_t aNode, JSON& aFill )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string type;
    static const std::set<std::string> types = {
        "none", "outline", "background", "color", "hatch", "reverse_hatch", "cross_hatch"
    };

    if( node.children.size() < 2 || node.children.size() > 3
        || !scalar( aDocument, node.children[1], type ) || !types.contains( type ) )
    {
        return false;
    }

    aFill = { { "type", type } };

    if( node.children.size() == 3 )
    {
        JSON parsedColor;

        if( aDocument.ListHead( node.children[2] ) != "color"
            || !color( aDocument, node.children[2], parsedColor ) )
        {
            return false;
        }

        aFill["color"] = std::move( parsedColor );
    }

    if( type == "color" && !aFill.contains( "color" ) )
        return false;

    return true;
}


bool samePoint( const JSON& aLeft, const JSON& aRight )
{
    return aLeft["xNm"] == aRight["xNm"] && aLeft["yNm"] == aRight["yNm"];
}


bool nonCollinear( const JSON& aStart, const JSON& aMid, const JSON& aEnd )
{
    const WIDE_INT ax = aMid["xNm"].get<int64_t>() - aStart["xNm"].get<int64_t>();
    const WIDE_INT ay = aMid["yNm"].get<int64_t>() - aStart["yNm"].get<int64_t>();
    const WIDE_INT bx = aEnd["xNm"].get<int64_t>() - aStart["xNm"].get<int64_t>();
    const WIDE_INT by = aEnd["yNm"].get<int64_t>() - aStart["yNm"].get<int64_t>();
    return ax * by != ay * bx;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_SYMBOL_GRAPHICS_COMPILER::IsGraphic( const std::string& aHead )
{
    static const std::set<std::string> heads = {
        "arc", "bezier", "circle", "polyline", "rectangle"
    };
    return heads.contains( aHead );
}


DESIGN_SCRIPT_SYMBOL_GRAPHICS_COMPILER::RESULT
DESIGN_SCRIPT_SYMBOL_GRAPHICS_COMPILER::Compile( const LOSSLESS_SEXPR_DOCUMENT& aDocument,
                                                 size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    const std::string kind = aDocument.ListHead( aNode );
    result.recognized = IsGraphic( kind );

    if( !result.recognized )
        return result;

    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( result, "invalid_authored_symbol_graphic",
                    "symbol " + kind + " requires a bounded logical ID" );
        return result;
    }

    result.item = {
        { "kind", kind }, { "id", id }, { "private", false },
        { "stroke", { { "widthNm", 0 }, { "style", "default" } } },
        { "fill", { { "type", "none" } } }
    };
    std::set<std::string> singletonFields;
    std::vector<JSON> points;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "point" && kind == "polyline" )
        {
            JSON parsedPoint;

            if( points.size() >= MAX_POLYLINE_POINTS || !point( aDocument, child, parsedPoint ) )
            {
                diagnostic( result, "invalid_authored_symbol_polyline_point",
                            "polyline " + id + " has an invalid point or exceeds 16384 points" );
            }
            else
            {
                points.push_back( std::move( parsedPoint ) );
            }

            continue;
        }

        if( !singletonFields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_symbol_graphic_field",
                        kind + " " + id + " field " + head + " occurs more than once" );
            continue;
        }

        if( head == "stroke" )
        {
            JSON stroke;

            if( !compileStroke( aDocument, child, stroke ) )
                diagnostic( result, "invalid_authored_symbol_graphic_stroke",
                            kind + " " + id + " has an invalid bounded stroke" );
            else
                result.item["stroke"] = std::move( stroke );
        }
        else if( head == "fill" )
        {
            JSON fill;

            if( !compileFill( aDocument, child, fill ) )
                diagnostic( result, "invalid_authored_symbol_graphic_fill",
                            kind + " " + id + " has an invalid fill" );
            else
                result.item["fill"] = std::move( fill );
        }
        else if( head == "private" )
        {
            const DOCUMENT::NODE& field = aDocument.Nodes().at( child );
            std::string value;
            bool parsed = false;

            if( field.children.size() != 2 || !scalar( aDocument, field.children[1], value )
                || !boolean( value, parsed ) )
            {
                diagnostic( result, "invalid_authored_symbol_graphic_private",
                            kind + " " + id + " private must be true or false" );
            }
            else
            {
                result.item["private"] = parsed;
            }
        }
        else if( head == "radius" )
        {
            const DOCUMENT::NODE& field = aDocument.Nodes().at( child );
            std::string value;
            int64_t parsed = 0;

            if( field.children.size() != 2 || !scalar( aDocument, field.children[1], value )
                || !distance( value, parsed ) || parsed < 0 )
            {
                diagnostic( result, "invalid_authored_symbol_graphic_radius",
                            kind + " " + id + " radius must be a nonnegative bounded distance" );
            }
            else
            {
                result.item["radiusNm"] = parsed;
            }
        }
        else
        {
            static const std::set<std::string> pointFields = {
                "from", "to", "center", "start", "mid", "control1", "control2", "end"
            };
            JSON parsedPoint;

            if( pointFields.contains( head ) && point( aDocument, child, parsedPoint ) )
                result.item[head] = std::move( parsedPoint );
            else
                diagnostic( result, "unknown_authored_symbol_graphic_field",
                            kind + " " + id + " has unsupported or invalid field " + head );
        }
    }

    if( kind == "rectangle" )
    {
        if( !result.item.contains( "from" ) || !result.item.contains( "to" ) )
            diagnostic( result, "missing_authored_symbol_rectangle_geometry",
                        "rectangle " + id + " requires from and to" );
        else
        {
            const int64_t width = std::abs( result.item["to"]["xNm"].get<int64_t>()
                                            - result.item["from"]["xNm"].get<int64_t>() );
            const int64_t height = std::abs( result.item["to"]["yNm"].get<int64_t>()
                                             - result.item["from"]["yNm"].get<int64_t>() );

            if( width == 0 || height == 0 )
                diagnostic( result, "degenerate_authored_symbol_rectangle",
                            "rectangle " + id + " must have nonzero width and height" );

            if( result.item.contains( "radiusNm" )
                && result.item["radiusNm"].get<int64_t>() > std::min( width, height ) / 2 )
            {
                diagnostic( result, "invalid_authored_symbol_rectangle_radius",
                            "rectangle " + id + " radius cannot exceed half its shortest side" );
            }
        }
    }
    else if( kind == "circle" )
    {
        if( !result.item.contains( "center" ) || !result.item.contains( "radiusNm" )
            || result.item.value( "radiusNm", 0 ) <= 0 )
        {
            diagnostic( result, "missing_authored_symbol_circle_geometry",
                        "circle " + id + " requires center and a positive radius" );
        }
    }
    else if( kind == "arc" )
    {
        if( !result.item.contains( "start" ) || !result.item.contains( "mid" )
            || !result.item.contains( "end" ) )
        {
            diagnostic( result, "missing_authored_symbol_arc_geometry",
                        "arc " + id + " requires start, mid, and end" );
        }
        else if( !nonCollinear( result.item["start"], result.item["mid"], result.item["end"] ) )
        {
            diagnostic( result, "degenerate_authored_symbol_arc",
                        "arc " + id + " points must define a non-collinear arc" );
        }
    }
    else if( kind == "bezier" )
    {
        static const std::vector<std::string> fields = { "start", "control1", "control2", "end" };
        bool complete = true;

        for( const std::string& field : fields )
            complete = complete && result.item.contains( field );

        if( !complete )
            diagnostic( result, "missing_authored_symbol_bezier_geometry",
                        "bezier " + id + " requires start, control1, control2, and end" );
        else if( samePoint( result.item["start"], result.item["control1"] )
                 && samePoint( result.item["start"], result.item["control2"] )
                 && samePoint( result.item["start"], result.item["end"] ) )
            diagnostic( result, "degenerate_authored_symbol_bezier",
                        "bezier " + id + " must contain at least two distinct points" );
    }
    else if( kind == "polyline" )
    {
        result.item["points"] = std::move( points );

        if( result.item["points"].size() < 2 )
            diagnostic( result, "missing_authored_symbol_polyline_geometry",
                        "polyline " + id + " requires at least two point fields" );
        else
        {
            bool distinct = false;

            for( size_t index = 1; index < result.item["points"].size(); ++index )
                distinct = distinct || !samePoint( result.item["points"][0],
                                                   result.item["points"][index] );

            if( !distinct )
                diagnostic( result, "degenerate_authored_symbol_polyline",
                            "polyline " + id + " requires at least two distinct points" );
        }
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
