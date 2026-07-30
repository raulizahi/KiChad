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

#include "design_script_schematic_planner.h"

#include "design_script_pcb_planner.h"
#include "lossless_sexpr_document.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>


namespace
{

using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT;

constexpr int SCHEMATIC_FILE_VERSION = 20260306;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


std::string quoted( const std::string& aText )
{
    std::string result = "\"";

    for( char character : aText )
    {
        switch( character )
        {
        case '\\': result += "\\\\"; break;
        case '"':  result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:   result.push_back( character ); break;
        }
    }

    result.push_back( '"' );
    return result;
}


std::string titleBlockExpression( const JSON& aTitleBlock )
{
    std::ostringstream output;
    output << "(title_block";
    const std::array<std::pair<const char*, const char*>, 4> fields = {
        std::pair{ "title", "title" }, std::pair{ "date", "date" },
        std::pair{ "revision", "rev" }, std::pair{ "company", "company" }
    };

    for( const auto& [irName, nativeName] : fields )
    {
        const std::string value = aTitleBlock.at( irName ).get<std::string>();

        if( !value.empty() )
            output << "\n    (" << nativeName << ' ' << quoted( value ) << ')';
    }

    const JSON& comments = aTitleBlock.at( "comments" );

    for( size_t i = 0; i < comments.size(); ++i )
    {
        const std::string value = comments.at( i ).get<std::string>();

        if( !value.empty() )
            output << "\n    (comment " << i + 1 << ' ' << quoted( value ) << ')';
    }

    output << "\n  )";
    return output.str();
}


std::string millimetres( int64_t aNanometres )
{
    const bool negative = aNanometres < 0;
    uint64_t magnitude = negative ? static_cast<uint64_t>( -( aNanometres + 1 ) ) + 1
                                  : static_cast<uint64_t>( aNanometres );
    const uint64_t whole = magnitude / 1'000'000;
    uint64_t fraction = magnitude % 1'000'000;
    std::ostringstream output;

    if( negative )
        output << '-';

    output << whole;

    if( fraction != 0 )
    {
        std::string digits = std::to_string( fraction + 1'000'000 ).substr( 1 );

        while( !digits.empty() && digits.back() == '0' )
            digits.pop_back();

        output << '.' << digits;
    }

    return output.str();
}


std::string finiteDecimal( double aValue )
{
    std::ostringstream output;
    output << std::setprecision( 12 ) << aValue;
    return output.str();
}


std::string stableUuid( const std::string& aProject, const std::string& aKind,
                        const std::string& aLogicalId )
{
    return KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid( aProject, aKind, aLogicalId );
}


std::string effects( const std::string& aJustify )
{
    std::string result =
            "    (effects\n"
            "      (font (size 1.27 1.27))";

    if( !aJustify.empty() )
        result += "\n      (justify " + aJustify + ")";

    return result + "\n    )\n";
}


int sideAngle( const std::string& aSide )
{
    if( aSide == "left" )
        return 180;

    if( aSide == "top" )
        return 90;

    if( aSide == "bottom" )
        return 270;

    return 0;
}


std::string sheetPin( const JSON& aPin, const std::string& aUuid )
{
    std::ostringstream output;
    output << "    (pin " << quoted( aPin.at( "name" ).get<std::string>() ) << ' '
           << aPin.at( "direction" ).get<std::string>() << "\n"
           << "      (at " << millimetres( aPin.at( "position" ).at( "xNm" ).get<int64_t>() )
           << ' ' << millimetres( aPin.at( "position" ).at( "yNm" ).get<int64_t>() )
           << ' ' << sideAngle( aPin.at( "side" ).get<std::string>() ) << ")\n"
           << "      (uuid " << quoted( aUuid ) << ")\n"
           << effects( "left" )
           << "    )\n";
    return output.str();
}


std::string sheetExpression( const JSON& aSheet, const std::string& aProject,
                             const std::string& aParentPath, int aPage )
{
    const std::string id = aSheet.at( "id" ).get<std::string>();
    const int64_t x = aSheet.at( "position" ).at( "xNm" ).get<int64_t>();
    const int64_t y = aSheet.at( "position" ).at( "yNm" ).get<int64_t>();
    const int64_t width = aSheet.at( "size" ).at( "xNm" ).get<int64_t>();
    const int64_t height = aSheet.at( "size" ).at( "yNm" ).get<int64_t>();
    const std::string uuid = stableUuid( aProject, "schematic_sheet", id );
    std::ostringstream output;
    output << "(sheet\n"
           << "    (at " << millimetres( x ) << ' ' << millimetres( y ) << ")\n"
           << "    (size " << millimetres( width ) << ' ' << millimetres( height ) << ")\n"
           << "    (exclude_from_sim no)\n"
           << "    (in_bom yes)\n"
           << "    (on_board yes)\n"
           << "    (dnp no)\n"
           << "    (stroke (width 0.1524) (type solid))\n"
           << "    (fill (color 0 0 0 0))\n"
           << "    (uuid " << quoted( uuid ) << ")\n"
           << "    (property \"Sheetname\" "
           << quoted( aSheet.at( "title" ).get<std::string>() ) << "\n"
           << "      (at " << millimetres( x ) << ' ' << millimetres( y - 711'600 )
           << " 0)\n"
           << "      (show_name no)\n"
           << "      (do_not_autoplace no)\n"
           << effects( "left bottom" )
           << "    )\n"
           << "    (property \"Sheetfile\" "
           << quoted( aSheet.at( "file" ).get<std::string>() ) << "\n"
           << "      (at " << millimetres( x ) << ' '
           << millimetres( y + height + 1'384'600 ) << " 0)\n"
           << "      (show_name no)\n"
           << "      (do_not_autoplace no)\n"
           << effects( "left top" )
           << "    )\n";

    std::vector<JSON> pins = aSheet.at( "pins" ).get<std::vector<JSON>>();
    std::sort( pins.begin(), pins.end(), []( const JSON& aLeft, const JSON& aRight )
               {
                   return aLeft.at( "name" ).get<std::string>()
                          < aRight.at( "name" ).get<std::string>();
               } );

    for( const JSON& pin : pins )
    {
        output << sheetPin( pin, stableUuid( aProject, "schematic_sheet_pin",
                                             id + "/" + pin.at( "name" ).get<std::string>() ) );
    }

    output << "    (instances\n"
           << "      (project " << quoted( aProject ) << "\n"
           << "        (path " << quoted( aParentPath ) << " (page "
           << quoted( std::to_string( aPage ) ) << "))\n"
           << "      )\n"
           << "    )\n"
           << "  )";
    return output.str();
}


std::string hierarchicalLabel( const JSON& aSheet, const JSON& aPin,
                               const std::string& aProject, size_t aIndex )
{
    const std::string id = aSheet.at( "id" ).get<std::string>();
    const int64_t y = 20'000'000 + static_cast<int64_t>( aIndex ) * 2'540'000;
    std::ostringstream output;
    output << "(hierarchical_label " << quoted( aPin.at( "name" ).get<std::string>() )
           << "\n"
           << "    (shape " << aPin.at( "direction" ).get<std::string>() << ")\n"
           << "    (at 20 " << millimetres( y ) << " 180)\n"
           << effects( "right" )
           << "    (uuid "
           << quoted( stableUuid( aProject, "schematic_hier_label",
                                  id + "/" + aPin.at( "name" ).get<std::string>() ) )
           << ")\n"
           << "  )";
    return output.str();
}


std::pair<int64_t, int64_t> transformPoint( int64_t aX, int64_t aY, int aRotation,
                                            const std::string& aMirror )
{
    // Library coordinates use an upward Y axis; schematic coordinates use a downward Y axis.
    aY = -aY;
    int x1 = 1;
    int y1 = 0;
    int x2 = 0;
    int y2 = 1;

    if( aRotation == 90 )
    {
        x1 = 0;
        y1 = 1;
        x2 = -1;
        y2 = 0;
    }
    else if( aRotation == 180 )
    {
        x1 = -1;
        y1 = 0;
        x2 = 0;
        y2 = -1;
    }
    else if( aRotation == 270 )
    {
        x1 = 0;
        y1 = -1;
        x2 = 1;
        y2 = 0;
    }

    const auto compose = [&]( int aTempX1, int aTempY1, int aTempX2, int aTempY2 )
    {
        const int newX1 = x1 * aTempX1 + x2 * aTempY1;
        const int newY1 = y1 * aTempX1 + y2 * aTempY1;
        const int newX2 = x1 * aTempX2 + x2 * aTempY2;
        const int newY2 = y1 * aTempX2 + y2 * aTempY2;
        x1 = newX1;
        y1 = newY1;
        x2 = newX2;
        y2 = newY2;
    };

    if( aMirror.find( 'x' ) != std::string::npos )
        compose( 1, 0, 0, -1 );

    if( aMirror.find( 'y' ) != std::string::npos )
        compose( -1, 0, 0, 1 );

    return { x1 * aX + y1 * aY, x2 * aX + y2 * aY };
}


std::string alphaChannel( int aAlpha );


JSON defaultComponentFieldLayout( const std::string& aName, int64_t aX, int64_t aY,
                                  int aAngle, bool aHidden, const std::string& aHorizontal )
{
    return { { "name", aName },
             { "position", { { "xNm", aX }, { "yNm", aY } } },
             { "rotationDegrees", aAngle },
             { "visible", !aHidden },
             { "showName", false },
             { "autoplace", true },
             { "size", { { "xNm", 1'270'000 }, { "yNm", 1'270'000 } } },
             { "font", "stroke" },
             { "lineSpacing", 1.0 },
             { "thicknessNm", 0 },
             { "color", nullptr },
             { "justify", { { "horizontal", aHorizontal }, { "vertical", "center" } } },
             { "mirror", false },
             { "bold", false },
             { "italic", false },
             { "hyperlink", "" },
             { "private", false } };
}


std::string propertyExpression( const std::string& aName, const std::string& aValue,
                                const JSON& aLayout )
{
    const JSON& position = aLayout.at( "position" );
    const JSON& size = aLayout.at( "size" );
    const JSON& justify = aLayout.at( "justify" );
    const std::string horizontal = justify.at( "horizontal" ).get<std::string>();
    const std::string vertical = justify.at( "vertical" ).get<std::string>();
    const int64_t thickness = aLayout.at( "thicknessNm" ).get<int64_t>();
    std::ostringstream output;
    output << "    (property ";

    if( aLayout.at( "private" ).get<bool>() )
        output << "private ";

    output << quoted( aName ) << ' ' << quoted( aValue ) << "\n"
           << "      (at " << millimetres( position.at( "xNm" ).get<int64_t>() ) << ' '
           << millimetres( position.at( "yNm" ).get<int64_t>() ) << ' '
           << finiteDecimal( aLayout.at( "rotationDegrees" ).get<double>() ) << ")\n";

    if( !aLayout.at( "visible" ).get<bool>() )
        output << "      (hide yes)\n";

    output << "      (show_name " << ( aLayout.at( "showName" ).get<bool>() ? "yes" : "no" )
           << ")\n"
           << "      (do_not_autoplace "
           << ( aLayout.at( "autoplace" ).get<bool>() ? "no" : "yes" ) << ")\n"
           << "      (effects\n"
           << "        (font\n";

    if( aLayout.at( "font" ).get<std::string>() != "stroke" )
        output << "          (face " << quoted( aLayout.at( "font" ).get<std::string>() )
               << ")\n";

    output << "          (size " << millimetres( size.at( "yNm" ).get<int64_t>() ) << ' '
           << millimetres( size.at( "xNm" ).get<int64_t>() ) << ")\n"
           << "          (line_spacing "
           << finiteDecimal( aLayout.at( "lineSpacing" ).get<double>() ) << ")\n";

    if( thickness != 0 )
        output << "          (thickness " << millimetres( thickness ) << ")\n";

    if( aLayout.at( "bold" ).get<bool>() )
        output << "          (bold yes)\n";

    if( aLayout.at( "italic" ).get<bool>() )
        output << "          (italic yes)\n";

    if( !aLayout.at( "color" ).is_null() )
    {
        const JSON& color = aLayout.at( "color" );
        output << "          (color " << color.at( "r" ).get<int>() << ' '
               << color.at( "g" ).get<int>() << ' ' << color.at( "b" ).get<int>() << ' '
               << alphaChannel( color.at( "a" ).get<int>() ) << ")\n";
    }

    output << "        )\n";

    if( horizontal != "center" || vertical != "center" )
    {
        output << "        (justify";

        if( horizontal != "center" )
            output << ' ' << horizontal;

        if( vertical != "center" )
            output << ' ' << vertical;

        output << ")\n";
    }

    if( !aLayout.at( "hyperlink" ).get<std::string>().empty() )
    {
        output << "        (href "
               << quoted( aLayout.at( "hyperlink" ).get<std::string>() ) << ")\n";
    }

    output << "      )\n"
           << "    )\n";
    return output.str();
}


std::string componentExpression( const JSON& aComponent, const JSON& aUnit,
                                 const JSON& aResolved, const std::string& aProject,
                                 const std::string& aInstancePath )
{
    const std::string reference = aComponent.at( "reference" ).get<std::string>();
    const std::string libraryId = aComponent.at( "symbol" ).get<std::string>();
    const int unit = aUnit.at( "number" ).get<int>();
    const int rotation = aUnit.at( "rotationDegrees" ).get<int>();
    const std::string mirror = aUnit.at( "mirror" ).get<std::string>();
    const int nativeRotation = mirror == "xy" ? ( rotation + 180 ) % 360 : rotation;
    const std::string nativeMirror = mirror == "xy" ? "none" : mirror;
    const int64_t x = aUnit.at( "position" ).at( "xNm" ).get<int64_t>();
    const int64_t y = aUnit.at( "position" ).at( "yNm" ).get<int64_t>();
    const std::string logicalId = reference + "/" + std::to_string( unit );
    const std::string uuid = stableUuid( aProject, "schematic_symbol", logicalId );
    const std::string unitKey = std::to_string( unit );
    const JSON& pins = aResolved.at( "units" ).at( unitKey );
    const JSON& nativeProperties = aResolved.at( "properties" );
    const JSON nativePropertyLayouts =
            aResolved.value( "propertyLayouts", JSON::object() );
    const JSON& nativeFlags = aResolved.at( "flags" );
    const std::string footprint =
            aComponent.at( "footprint" ).is_null()
                    ? std::string()
                    : aComponent.at( "footprint" ).get<std::string>();
    std::map<std::string, JSON> fieldLayouts;

    for( const JSON& layout : aUnit.at( "fields" ) )
        fieldLayouts.emplace( layout.at( "name" ).get<std::string>(), layout );

    const auto fieldLayout = [&]( const std::string& aName, int64_t aX, int64_t aY,
                                  int aAngle, bool aHidden,
                                  const std::string& aHorizontal ) -> JSON
    {
        auto layout = fieldLayouts.find( aName );

        if( layout != fieldLayouts.end() )
            return layout->second;

        auto nativeLayout = nativePropertyLayouts.find( aName );

        if( nativeLayout != nativePropertyLayouts.end()
            && nativeLayout->contains( "position" ) )
        {
            const JSON& relativePosition = nativeLayout->at( "position" );
            const auto [ relativeX, relativeY ] = transformPoint(
                    relativePosition.at( "xNm" ).get<int64_t>(),
                    relativePosition.at( "yNm" ).get<int64_t>(), rotation, mirror );
            JSON inherited = defaultComponentFieldLayout(
                    aName, x + relativeX, y + relativeY, nativeRotation,
                    !nativeLayout->value( "visible", true ), "center" );
            inherited["rotationDegrees"] =
                    std::fmod( nativeLayout->value( "rotationDegrees", 0.0 )
                                       + nativeRotation,
                               360.0 );
            inherited["showName"] = nativeLayout->value( "showName", false );
            inherited["autoplace"] = nativeLayout->value( "autoplace", true );
            return inherited;
        }

        return defaultComponentFieldLayout(
                aName, aX, aY, aAngle, aHidden, aHorizontal );
    };

    std::ostringstream output;
    output << "(symbol\n"
           << "    (lib_id " << quoted( libraryId ) << ")\n"
           << "    (at " << millimetres( x ) << ' ' << millimetres( y ) << ' '
           << nativeRotation << ")\n";

    if( nativeMirror != "none" )
    {
        output << "    (mirror";

        if( nativeMirror == "x" )
            output << " x";

        if( nativeMirror == "y" )
            output << " y";

        output << ")\n";
    }

    output << "    (unit " << unit << ")\n"
           << "    (body_style 1)\n"
           << "    (exclude_from_sim "
           << ( nativeFlags.at( "excludeFromSim" ).get<bool>() ? "yes" : "no" ) << ")\n"
           << "    (in_bom " << ( nativeFlags.at( "inBom" ).get<bool>() ? "yes" : "no" )
           << ")\n"
           << "    (on_board "
           << ( nativeFlags.at( "onBoard" ).get<bool>() ? "yes" : "no" ) << ")\n"
           << "    (in_pos_files "
           << ( nativeFlags.at( "inPosFiles" ).get<bool>() ? "yes" : "no" ) << ")\n"
           << "    (dnp " << ( aComponent.value( "dnp", false ) ? "yes" : "no" ) << ")\n";

    if( aUnit.at( "fieldsAutoplaced" ).get<bool>() )
        output << "    (fields_autoplaced yes)\n";

    const std::string datasheet = aComponent.contains( "datasheet" )
                                          ? aComponent.at( "datasheet" ).get<std::string>()
                                          : nativeProperties.value( "Datasheet", "" );
    const std::string description = aComponent.contains( "description" )
                                            ? aComponent.at( "description" ).get<std::string>()
                                            : nativeProperties.value( "Description", "" );

    output << "    (uuid " << quoted( uuid ) << ")\n"
           << propertyExpression(
                      "Reference", reference,
                      fieldLayout( "Reference", x + 2'540'000, y - 1'270'000,
                                   nativeRotation, false, "left" ) )
           << propertyExpression(
                      "Value", aComponent.at( "value" ).get<std::string>(),
                      fieldLayout( "Value", x + 2'540'000, y + 1'270'000,
                                   nativeRotation, false, "left" ) )
           << propertyExpression(
                      "Footprint", footprint,
                      fieldLayout( "Footprint", x - 1'778'000, y,
                                   ( nativeRotation + 90 ) % 360, true, "center" ) )
           << propertyExpression(
                      "Datasheet", datasheet,
                      fieldLayout( "Datasheet", x, y, nativeRotation, true, "center" ) )
           << propertyExpression(
                      "Description", description,
                      fieldLayout( "Description", x, y, nativeRotation, true, "center" ) );

    std::vector<std::pair<std::string, std::string>> customProperties;

    for( auto property = aComponent.at( "properties" ).begin();
         property != aComponent.at( "properties" ).end(); ++property )
    {
        customProperties.emplace_back( property.key(), property.value().get<std::string>() );
    }

    std::sort( customProperties.begin(), customProperties.end() );

    for( const auto& [ name, value ] : customProperties )
    {
        output << propertyExpression(
                name, value,
                fieldLayout( name, x, y, nativeRotation, true, "center" ) );
    }

    for( size_t pinIndex = 0; pinIndex < pins.size(); ++pinIndex )
    {
        const std::string number = pins[pinIndex].at( "number" ).get<std::string>();
        output << "    (pin " << quoted( number ) << "\n"
               << "      (uuid "
               << quoted( stableUuid( aProject, "schematic_symbol_pin",
                                      logicalId + "/" + number + "/"
                                              + std::to_string( pinIndex ) ) )
               << ")\n"
               << "    )\n";
    }

    output << "    (instances\n"
           << "      (project " << quoted( aProject ) << "\n"
           << "        (path " << quoted( aInstancePath ) << "\n"
           << "          (reference " << quoted( reference ) << ")\n"
           << "          (unit " << unit << ")\n"
           << "        )\n"
           << "      )\n"
           << "    )\n"
           << "  )";
    return output.str();
}


std::string globalLabelExpression( const std::string& aName, int64_t aX, int64_t aY,
                                   int aRotation, const std::string& aUuid )
{
    const std::string justification = aRotation == 0 || aRotation == 90 ? "left" : "right";
    std::ostringstream output;
    output << "(global_label " << quoted( aName ) << "\n"
           << "    (shape passive)\n"
           << "    (at " << millimetres( aX ) << ' ' << millimetres( aY ) << ' '
           << aRotation << ")\n"
           << "    (fields_autoplaced yes)\n"
           << effects( justification )
           << "    (uuid " << quoted( aUuid ) << ")\n"
           << "  )";
    return output.str();
}


std::string noConnectExpression( int64_t aX, int64_t aY, const std::string& aUuid )
{
    std::ostringstream output;
    output << "(no_connect\n"
           << "    (at " << millimetres( aX ) << ' ' << millimetres( aY ) << ")\n"
           << "    (uuid " << quoted( aUuid ) << ")\n"
           << "  )";
    return output.str();
}


std::string strokeExpression( const JSON& aStroke )
{
    std::ostringstream output;
    output << "    (stroke\n"
           << "      (width "
           << millimetres( aStroke.at( "widthNm" ).get<int64_t>() ) << ")\n"
           << "      (type " << aStroke.at( "lineStyle" ).get<std::string>() << ")\n"
           << "    )\n";
    return output.str();
}


std::string schematicLineExpression( const JSON& aDrawing, const std::string& aUuid )
{
    const JSON& from = aDrawing.at( "from" );
    const JSON& to = aDrawing.at( "to" );
    const std::string kind = aDrawing.at( "kind" ).get<std::string>();
    std::ostringstream output;

    if( kind == "bus_entry" )
    {
        const int64_t dx = to.at( "xNm" ).get<int64_t>()
                           - from.at( "xNm" ).get<int64_t>();
        const int64_t dy = to.at( "yNm" ).get<int64_t>()
                           - from.at( "yNm" ).get<int64_t>();
        output << "(bus_entry\n"
               << "    (at " << millimetres( from.at( "xNm" ).get<int64_t>() ) << ' '
               << millimetres( from.at( "yNm" ).get<int64_t>() ) << ")\n"
               << "    (size " << millimetres( dx ) << ' ' << millimetres( dy ) << ")\n";
    }
    else
    {
        output << '(' << kind << "\n"
               << "    (pts\n"
               << "      (xy " << millimetres( from.at( "xNm" ).get<int64_t>() ) << ' '
               << millimetres( from.at( "yNm" ).get<int64_t>() ) << ")\n"
               << "      (xy " << millimetres( to.at( "xNm" ).get<int64_t>() ) << ' '
               << millimetres( to.at( "yNm" ).get<int64_t>() ) << ")\n"
               << "    )\n";
    }

    output << strokeExpression( aDrawing.at( "stroke" ) )
           << "    (uuid " << quoted( aUuid ) << ")\n"
           << "  )";
    return output.str();
}


std::string alphaChannel( int aAlpha )
{
    if( aAlpha == 0 )
        return "0";

    if( aAlpha == 255 )
        return "1";

    std::ostringstream output;
    output << std::fixed << std::setprecision( 8 )
           << static_cast<double>( aAlpha ) / 255.0;
    std::string result = output.str();

    while( !result.empty() && result.back() == '0' )
        result.pop_back();

    return result;
}


std::string junctionExpression( const JSON& aDrawing, const std::string& aUuid )
{
    const JSON& position = aDrawing.at( "position" );
    int red = 0;
    int green = 0;
    int blue = 0;
    int alpha = 0;

    if( !aDrawing.at( "color" ).is_null() )
    {
        red = aDrawing.at( "color" ).at( "r" ).get<int>();
        green = aDrawing.at( "color" ).at( "g" ).get<int>();
        blue = aDrawing.at( "color" ).at( "b" ).get<int>();
        alpha = aDrawing.at( "color" ).at( "a" ).get<int>();
    }

    std::ostringstream output;
    output << "(junction\n"
           << "    (at " << millimetres( position.at( "xNm" ).get<int64_t>() ) << ' '
           << millimetres( position.at( "yNm" ).get<int64_t>() ) << ")\n"
           << "    (diameter " << millimetres( aDrawing.at( "diameterNm" ).get<int64_t>() )
           << ")\n"
           << "    (color " << red << ' ' << green << ' ' << blue << ' '
           << alphaChannel( alpha ) << ")\n"
           << "    (uuid " << quoted( aUuid ) << ")\n"
           << "  )";
    return output.str();
}


std::string labelExpression( const JSON& aDrawing, const std::string& aNativeKind,
                             const std::string& aUuid )
{
    const JSON& position = aDrawing.at( "position" );
    const JSON& size = aDrawing.at( "size" );
    const JSON& justify = aDrawing.at( "justify" );
    const int64_t thickness = aDrawing.at( "thicknessNm" ).get<int64_t>();
    const std::string horizontal = justify.at( "horizontal" ).get<std::string>();
    const std::string vertical = justify.at( "vertical" ).get<std::string>();
    std::ostringstream output;
    output << '(' << aNativeKind << ' ' << quoted( aDrawing.at( "net" ).get<std::string>() )
           << "\n";

    if( aNativeKind == "global_label" )
        output << "    (shape " << aDrawing.at( "shape" ).get<std::string>() << ")\n";

    output << "    (at " << millimetres( position.at( "xNm" ).get<int64_t>() ) << ' '
           << millimetres( position.at( "yNm" ).get<int64_t>() ) << ' '
           << aDrawing.at( "rotationDegrees" ).get<int>() << ")\n"
           << "    (effects\n"
           << "      (font\n"
           << "        (size " << millimetres( size.at( "yNm" ).get<int64_t>() ) << ' '
           << millimetres( size.at( "xNm" ).get<int64_t>() ) << ")\n";

    if( thickness != 0 )
        output << "        (thickness " << millimetres( thickness ) << ")\n";

    if( aDrawing.at( "bold" ).get<bool>() )
        output << "        (bold yes)\n";

    if( aDrawing.at( "italic" ).get<bool>() )
        output << "        (italic yes)\n";

    output << "      )\n";

    if( horizontal != "center" || vertical != "center" )
    {
        output << "      (justify";

        if( horizontal != "center" )
            output << ' ' << horizontal;

        if( vertical != "center" )
            output << ' ' << vertical;

        output << ")\n";
    }

    output << "    )\n"
           << "    (uuid " << quoted( aUuid ) << ")\n"
           << "  )";
    return output.str();
}


void appendSchematicColor( std::ostringstream& aOutput, const JSON& aColor,
                           const std::string& aIndent )
{
    aOutput << aIndent << "(color " << aColor.at( "r" ).get<int>() << ' '
            << aColor.at( "g" ).get<int>() << ' ' << aColor.at( "b" ).get<int>() << ' '
            << alphaChannel( aColor.at( "a" ).get<int>() ) << ")\n";
}


std::string ruleAreaExpression( const JSON& aDrawing, const std::string& aUuid )
{
    const JSON& stroke = aDrawing.at( "stroke" );
    const JSON& fill = aDrawing.at( "fill" );
    std::ostringstream output;
    output << "(rule_area\n"
           << "    (exclude_from_sim "
           << ( aDrawing.at( "exclude_from_sim" ).get<bool>() ? "yes" : "no" ) << ")\n"
           << "    (in_bom "
           << ( aDrawing.at( "exclude_from_bom" ).get<bool>() ? "no" : "yes" ) << ")\n"
           << "    (on_board "
           << ( aDrawing.at( "exclude_from_board" ).get<bool>() ? "no" : "yes" ) << ")\n"
           << "    (dnp " << ( aDrawing.at( "dnp" ).get<bool>() ? "yes" : "no" ) << ")\n"
           << "    (polyline\n"
           << "      (pts\n";

    for( const JSON& point : aDrawing.at( "polygon" ) )
    {
        output << "        (xy " << millimetres( point.at( "xNm" ).get<int64_t>() ) << ' '
               << millimetres( point.at( "yNm" ).get<int64_t>() ) << ")\n";
    }

    output << "      )\n"
           << "      (stroke\n"
           << "        (width " << millimetres( stroke.at( "widthNm" ).get<int64_t>() )
           << ")\n"
           << "        (type " << stroke.at( "lineStyle" ).get<std::string>() << ")\n";

    if( !stroke.at( "color" ).is_null() )
        appendSchematicColor( output, stroke.at( "color" ), "        " );

    output << "      )\n"
           << "      (fill\n"
           << "        (type " << fill.at( "type" ).get<std::string>() << ")\n";

    if( !fill.at( "color" ).is_null() )
        appendSchematicColor( output, fill.at( "color" ), "        " );

    output << "      )\n"
           << "      (uuid " << aUuid << ")\n"
           << "    )\n"
           << "  )";
    return output.str();
}


void appendSchematicTextEffects( std::ostringstream& aOutput, const JSON& aDrawing,
                                 const std::string& aIndent )
{
    const JSON& size = aDrawing.at( "size" );
    const JSON& justify = aDrawing.at( "justify" );
    const std::string horizontal = justify.at( "horizontal" ).get<std::string>();
    const std::string vertical = justify.at( "vertical" ).get<std::string>();
    const int64_t thickness = aDrawing.at( "thicknessNm" ).get<int64_t>();
    aOutput << aIndent << "(effects\n"
            << aIndent << "  (font\n";

    if( aDrawing.at( "font" ).get<std::string>() != "stroke" )
    {
        aOutput << aIndent << "    (face "
                << quoted( aDrawing.at( "font" ).get<std::string>() ) << ")\n";
    }

    aOutput << aIndent << "    (size " << millimetres( size.at( "yNm" ).get<int64_t>() )
            << ' ' << millimetres( size.at( "xNm" ).get<int64_t>() ) << ")\n"
            << aIndent << "    (line_spacing "
            << finiteDecimal( aDrawing.at( "lineSpacing" ).get<double>() ) << ")\n";

    if( thickness != 0 )
        aOutput << aIndent << "    (thickness " << millimetres( thickness ) << ")\n";

    if( aDrawing.at( "bold" ).get<bool>() )
        aOutput << aIndent << "    (bold yes)\n";

    if( aDrawing.at( "italic" ).get<bool>() )
        aOutput << aIndent << "    (italic yes)\n";

    if( !aDrawing.at( "color" ).is_null() )
        appendSchematicColor( aOutput, aDrawing.at( "color" ), aIndent + "    " );

    aOutput << aIndent << "  )\n";

    if( horizontal != "center" || vertical != "center"
        || aDrawing.at( "mirror" ).get<bool>() )
    {
        aOutput << aIndent << "  (justify";

        if( horizontal != "center" )
            aOutput << ' ' << horizontal;

        if( vertical != "center" )
            aOutput << ' ' << vertical;

        if( aDrawing.at( "mirror" ).get<bool>() )
            aOutput << " mirror";

        aOutput << ")\n";
    }

    if( !aDrawing.at( "hyperlink" ).get<std::string>().empty() )
    {
        aOutput << aIndent << "  (href "
                << quoted( aDrawing.at( "hyperlink" ).get<std::string>() ) << ")\n";
    }

    aOutput << aIndent << ")\n";
}


std::string textExpression( const JSON& aDrawing, const std::string& aUuid )
{
    const JSON& position = aDrawing.at( "position" );
    std::ostringstream output;
    output << "(text " << quoted( aDrawing.at( "content" ).get<std::string>() ) << "\n"
           << "    (exclude_from_sim "
           << ( aDrawing.at( "exclude_from_sim" ).get<bool>() ? "yes" : "no" ) << ")\n"
           << "    (at " << millimetres( position.at( "xNm" ).get<int64_t>() ) << ' '
           << millimetres( position.at( "yNm" ).get<int64_t>() ) << ' '
           << finiteDecimal( aDrawing.at( "rotationDegrees" ).get<double>() ) << ")\n";
    appendSchematicTextEffects( output, aDrawing, "    " );
    output << "    (uuid " << quoted( aUuid ) << ")\n"
           << "  )";
    return output.str();
}


std::string textBoxExpression( const JSON& aDrawing, const std::string& aUuid )
{
    const JSON& position = aDrawing.at( "position" );
    const JSON& boxSize = aDrawing.at( "boxSize" );
    const JSON& margins = aDrawing.at( "margins" );
    const JSON& stroke = aDrawing.at( "stroke" );
    const JSON& fill = aDrawing.at( "fill" );
    std::ostringstream output;
    output << "(text_box " << quoted( aDrawing.at( "content" ).get<std::string>() ) << "\n"
           << "    (exclude_from_sim "
           << ( aDrawing.at( "exclude_from_sim" ).get<bool>() ? "yes" : "no" ) << ")\n"
           << "    (at " << millimetres( position.at( "xNm" ).get<int64_t>() ) << ' '
           << millimetres( position.at( "yNm" ).get<int64_t>() ) << ' '
           << finiteDecimal( aDrawing.at( "rotationDegrees" ).get<double>() ) << ")\n"
           << "    (size " << millimetres( boxSize.at( "xNm" ).get<int64_t>() ) << ' '
           << millimetres( boxSize.at( "yNm" ).get<int64_t>() ) << ")\n"
           << "    (margins " << millimetres( margins.at( "leftNm" ).get<int64_t>() )
           << ' ' << millimetres( margins.at( "topNm" ).get<int64_t>() ) << ' '
           << millimetres( margins.at( "rightNm" ).get<int64_t>() ) << ' '
           << millimetres( margins.at( "bottomNm" ).get<int64_t>() ) << ")\n"
           << "    (stroke\n"
           << "      (width " << millimetres( stroke.at( "widthNm" ).get<int64_t>() )
           << ")\n"
           << "      (type " << stroke.at( "lineStyle" ).get<std::string>() << ")\n";

    if( !stroke.at( "color" ).is_null() )
        appendSchematicColor( output, stroke.at( "color" ), "      " );

    output << "    )\n"
           << "    (fill\n"
           << "      (type " << fill.at( "type" ).get<std::string>() << ")\n";

    if( !fill.at( "color" ).is_null() )
        appendSchematicColor( output, fill.at( "color" ), "      " );

    output << "    )\n";
    appendSchematicTextEffects( output, aDrawing, "    " );
    output << "    (uuid " << quoted( aUuid ) << ")\n"
           << "  )";
    return output.str();
}


std::string schematicGraphicExpression( const JSON& aDrawing, const std::string& aUuid )
{
    const std::string kind = aDrawing.at( "kind" ).get<std::string>();
    const JSON& stroke = aDrawing.at( "stroke" );
    const JSON& fill = aDrawing.at( "fill" );
    std::ostringstream output;
    output << '(' << kind << '\n';

    const auto appendPoint = [&]( const std::string& aName, const JSON& aPoint )
    {
        output << "    (" << aName << ' '
               << millimetres( aPoint.at( "xNm" ).get<int64_t>() ) << ' '
               << millimetres( aPoint.at( "yNm" ).get<int64_t>() ) << ")\n";
    };

    if( kind == "polyline" || kind == "bezier" )
    {
        output << "    (pts\n";

        for( const JSON& point : aDrawing.at( "points" ) )
            appendPoint( "xy", point );

        output << "    )\n";
    }
    else if( kind == "rectangle" )
    {
        appendPoint( "start", aDrawing.at( "start" ) );
        appendPoint( "end", aDrawing.at( "end" ) );

        if( aDrawing.at( "cornerRadiusNm" ).get<int64_t>() != 0 )
        {
            output << "    (radius "
                   << millimetres( aDrawing.at( "cornerRadiusNm" ).get<int64_t>() )
                   << ")\n";
        }
    }
    else if( kind == "circle" )
    {
        appendPoint( "center", aDrawing.at( "center" ) );
        output << "    (radius " << millimetres( aDrawing.at( "radiusNm" ).get<int64_t>() )
               << ")\n";
    }
    else
    {
        appendPoint( "start", aDrawing.at( "start" ) );
        appendPoint( "mid", aDrawing.at( "mid" ) );
        appendPoint( "end", aDrawing.at( "end" ) );
    }

    output << "    (stroke\n"
           << "      (width " << millimetres( stroke.at( "widthNm" ).get<int64_t>() )
           << ")\n"
           << "      (type " << stroke.at( "lineStyle" ).get<std::string>() << ")\n";

    if( !stroke.at( "color" ).is_null() )
        appendSchematicColor( output, stroke.at( "color" ), "      " );

    output << "    )\n"
           << "    (fill\n"
           << "      (type " << fill.at( "type" ).get<std::string>() << ")\n";

    if( !fill.at( "color" ).is_null() )
        appendSchematicColor( output, fill.at( "color" ), "      " );

    output << "    )\n"
           << "    (uuid " << quoted( aUuid ) << ")\n"
           << "  )";
    return output.str();
}


std::string schematicImageExpression( const JSON& aDrawing, const std::string& aUuid )
{
    const JSON& position = aDrawing.at( "position" );
    const std::string data = aDrawing.at( "dataBase64" ).get<std::string>();
    constexpr size_t BASE64_LINE_LENGTH = 76;
    std::ostringstream output;
    output << "(image\n"
           << "    (at " << millimetres( position.at( "xNm" ).get<int64_t>() ) << ' '
           << millimetres( position.at( "yNm" ).get<int64_t>() ) << ")\n"
           << "    (scale " << finiteDecimal( aDrawing.at( "scale" ).get<double>() ) << ")\n"
           << "    (uuid " << quoted( aUuid ) << ")\n"
           << "    (data";

    for( size_t offset = 0; offset < data.size(); offset += BASE64_LINE_LENGTH )
    {
        output << "\n      "
               << quoted( data.substr( offset, BASE64_LINE_LENGTH ) );
    }

    output << "\n    )\n"
           << "  )";
    return output.str();
}


void appendSchematicTableStroke( std::ostringstream& aOutput, const JSON& aStroke,
                                 const std::string& aIndent )
{
    aOutput << aIndent << "(stroke\n"
            << aIndent << "  (width "
            << millimetres( aStroke.at( "widthNm" ).get<int64_t>() ) << ")\n"
            << aIndent << "  (type " << aStroke.at( "lineStyle" ).get<std::string>()
            << ")\n";

    if( !aStroke.at( "color" ).is_null() )
        appendSchematicColor( aOutput, aStroke.at( "color" ), aIndent + "  " );

    aOutput << aIndent << ")\n";
}


std::string schematicTableExpression( const JSON& aDrawing, const std::string& aProject,
                                      const std::string& aUuid )
{
    const JSON& position = aDrawing.at( "position" );
    const JSON& columnWidths = aDrawing.at( "columnWidthsNm" );
    const JSON& rowHeights = aDrawing.at( "rowHeightsNm" );
    const JSON& border = aDrawing.at( "border" );
    const JSON& separators = aDrawing.at( "separators" );
    const size_t columnCount = columnWidths.size();
    const size_t rowCount = rowHeights.size();
    const int rotation = aDrawing.at( "rotationDegrees" ).get<int>();
    std::vector<int64_t> columnOffsets( columnCount, 0 );
    std::vector<int64_t> rowOffsets( rowCount, 0 );

    for( size_t column = 1; column < columnCount; ++column )
    {
        columnOffsets[column] = columnOffsets[column - 1]
                                + columnWidths[column - 1].get<int64_t>();
    }

    for( size_t row = 1; row < rowCount; ++row )
        rowOffsets[row] = rowOffsets[row - 1] + rowHeights[row - 1].get<int64_t>();

    std::vector<std::pair<int, int>> spans( rowCount * columnCount, { 1, 1 } );

    for( const JSON& merge : aDrawing.at( "merges" ) )
    {
        const int firstRow = merge.at( "firstRow" ).get<int>();
        const int firstColumn = merge.at( "firstColumn" ).get<int>();
        const int lastRow = merge.at( "lastRow" ).get<int>();
        const int lastColumn = merge.at( "lastColumn" ).get<int>();
        spans[static_cast<size_t>( firstRow ) * columnCount
              + static_cast<size_t>( firstColumn )] = {
            lastColumn - firstColumn + 1, lastRow - firstRow + 1
        };

        for( int row = firstRow; row <= lastRow; ++row )
        {
            for( int column = firstColumn; column <= lastColumn; ++column )
            {
                if( row != firstRow || column != firstColumn )
                {
                    spans[static_cast<size_t>( row ) * columnCount
                          + static_cast<size_t>( column )] = { 0, 0 };
                }
            }
        }
    }

    std::map<std::pair<int, int>, const JSON*> cells;

    for( const JSON& cell : aDrawing.at( "cells" ) )
    {
        cells[{ cell.at( "row" ).get<int>(), cell.at( "column" ).get<int>() }] = &cell;
    }

    std::ostringstream output;
    output << "(table\n"
           << "    (column_count " << columnCount << ")\n"
           << "    (border\n"
           << "      (external " << ( border.at( "external" ).get<bool>() ? "yes" : "no" )
           << ")\n"
           << "      (header " << ( border.at( "header" ).get<bool>() ? "yes" : "no" )
           << ")\n";
    appendSchematicTableStroke( output, border.at( "stroke" ), "      " );
    output << "    )\n"
           << "    (separators\n"
           << "      (rows " << ( separators.at( "rows" ).get<bool>() ? "yes" : "no" )
           << ")\n"
           << "      (cols " << ( separators.at( "columns" ).get<bool>() ? "yes" : "no" )
           << ")\n";
    appendSchematicTableStroke( output, separators.at( "stroke" ), "      " );
    output << "    )\n"
           << "    (column_widths";

    for( const JSON& width : columnWidths )
        output << ' ' << millimetres( width.get<int64_t>() );

    output << ")\n"
           << "    (row_heights";

    for( const JSON& height : rowHeights )
        output << ' ' << millimetres( height.get<int64_t>() );

    output << ")\n"
           << "    (uuid " << quoted( aUuid ) << ")\n"
           << "    (cells\n";

    const int64_t originX = position.at( "xNm" ).get<int64_t>();
    const int64_t originY = position.at( "yNm" ).get<int64_t>();
    const std::string tableId = aDrawing.at( "id" ).get<std::string>();

    for( size_t row = 0; row < rowCount; ++row )
    {
        for( size_t column = 0; column < columnCount; ++column )
        {
            const JSON& cell = *cells.at( { static_cast<int>( row ),
                                            static_cast<int>( column ) } );
            const auto [ columnSpan, rowSpan ] = spans[row * columnCount + column];
            int64_t cellWidth = columnWidths[column].get<int64_t>();
            int64_t cellHeight = rowHeights[row].get<int64_t>();

            if( columnSpan > 1 )
            {
                cellWidth = 0;

                for( int offset = 0; offset < columnSpan; ++offset )
                    cellWidth += columnWidths[column + static_cast<size_t>( offset )].get<int64_t>();
            }

            if( rowSpan > 1 )
            {
                cellHeight = 0;

                for( int offset = 0; offset < rowSpan; ++offset )
                    cellHeight += rowHeights[row + static_cast<size_t>( offset )].get<int64_t>();
            }

            int64_t x = originX + columnOffsets[column];
            int64_t y = originY + rowOffsets[row];
            int64_t sizeX = cellWidth;
            int64_t sizeY = cellHeight;

            if( rotation == 90 )
            {
                x = originX + rowOffsets[row];
                y = originY - columnOffsets[column];
                sizeX = cellHeight;
                sizeY = -cellWidth;
            }

            const JSON& margins = cell.at( "margins" );
            const JSON& fill = cell.at( "fill" );
            const std::string cellUuid = stableUuid(
                    aProject, "schematic_table_cell",
                    tableId + "/" + std::to_string( row + 1 ) + "/"
                            + std::to_string( column + 1 ) );
            output << "      (table_cell " << quoted( cell.at( "content" ).get<std::string>() )
                   << "\n"
                   << "        (exclude_from_sim "
                   << ( cell.at( "exclude_from_sim" ).get<bool>() ? "yes" : "no" ) << ")\n"
                   << "        (at " << millimetres( x ) << ' ' << millimetres( y ) << ' '
                   << rotation << ")\n"
                   << "        (size " << millimetres( sizeX ) << ' ' << millimetres( sizeY )
                   << ")\n"
                   << "        (margins "
                   << millimetres( margins.at( "leftNm" ).get<int64_t>() ) << ' '
                   << millimetres( margins.at( "topNm" ).get<int64_t>() ) << ' '
                   << millimetres( margins.at( "rightNm" ).get<int64_t>() ) << ' '
                   << millimetres( margins.at( "bottomNm" ).get<int64_t>() ) << ")\n"
                   << "        (span " << columnSpan << ' ' << rowSpan << ")\n"
                   << "        (fill\n"
                   << "          (type " << fill.at( "type" ).get<std::string>() << ")\n";

            if( !fill.at( "color" ).is_null() )
                appendSchematicColor( output, fill.at( "color" ), "          " );

            output << "        )\n";
            appendSchematicTextEffects( output, cell, "        " );
            output << "        (uuid " << quoted( cellUuid ) << ")\n"
                   << "      )\n";
        }
    }

    output << "    )\n"
           << "  )";
    return output.str();
}


std::string directivePropertyExpression( const JSON& aProperty )
{
    const JSON& position = aProperty.at( "position" );
    const JSON& size = aProperty.at( "size" );
    const JSON& justify = aProperty.at( "justify" );
    const int64_t thickness = aProperty.at( "thicknessNm" ).get<int64_t>();
    const std::string horizontal = justify.at( "horizontal" ).get<std::string>();
    const std::string vertical = justify.at( "vertical" ).get<std::string>();
    std::ostringstream output;
    output << "    (property " << quoted( aProperty.at( "name" ).get<std::string>() ) << ' '
           << quoted( aProperty.at( "value" ).get<std::string>() ) << "\n"
           << "      (at " << millimetres( position.at( "xNm" ).get<int64_t>() ) << ' '
           << millimetres( position.at( "yNm" ).get<int64_t>() ) << ' '
           << aProperty.at( "rotationDegrees" ).get<int>() << ")\n";

    if( !aProperty.at( "visible" ).get<bool>() )
        output << "      (hide yes)\n";

    output << "      (effects\n"
           << "        (font\n"
           << "          (size " << millimetres( size.at( "yNm" ).get<int64_t>() ) << ' '
           << millimetres( size.at( "xNm" ).get<int64_t>() ) << ")\n";

    if( thickness != 0 )
        output << "          (thickness " << millimetres( thickness ) << ")\n";

    if( aProperty.at( "bold" ).get<bool>() )
        output << "          (bold yes)\n";

    if( aProperty.at( "italic" ).get<bool>() )
        output << "          (italic yes)\n";

    output << "        )\n";

    if( horizontal != "center" || vertical != "center" )
    {
        output << "        (justify";

        if( horizontal != "center" )
            output << ' ' << horizontal;

        if( vertical != "center" )
            output << ' ' << vertical;

        output << ")\n";
    }

    output << "      )\n"
           << "    )";
    return output.str();
}


std::string directiveExpression( const JSON& aDrawing, const std::string& aUuid )
{
    const JSON& position = aDrawing.at( "position" );
    std::ostringstream output;
    output << "(netclass_flag \"\"\n"
           << "    (length " << millimetres( aDrawing.at( "lengthNm" ).get<int64_t>() )
           << ")\n"
           << "    (shape " << aDrawing.at( "shape" ).get<std::string>() << ")\n"
           << "    (at " << millimetres( position.at( "xNm" ).get<int64_t>() ) << ' '
           << millimetres( position.at( "yNm" ).get<int64_t>() ) << ' '
           << aDrawing.at( "rotationDegrees" ).get<int>() << ")\n"
           << "    (effects\n"
           << "      (font\n"
           << "        (size 1.27 1.27)\n"
           << "      )\n"
           << "      (justify left bottom)\n"
           << "    )\n"
           << "    (uuid " << quoted( aUuid ) << ")\n";

    for( size_t index = 0; index < aDrawing.at( "properties" ).size(); ++index )
    {
        if( index != 0 )
            output << '\n';

        output << directivePropertyExpression( aDrawing.at( "properties" ).at( index ) ) << '\n';
    }

    output << "  )";
    return output.str();
}


std::string busAliasExpression( const JSON& aAlias )
{
    std::ostringstream output;
    output << "(bus_alias " << quoted( aAlias.at( "name" ).get<std::string>() ) << "\n"
           << "    (members";

    for( const JSON& member : aAlias.at( "members" ) )
        output << ' ' << quoted( member.get<std::string>() );

    output << ")\n"
           << "  )";
    return output.str();
}


std::string schematicGroupExpression( const JSON& aGroup, const std::string& aUuid,
                                      std::vector<std::string> aMemberUuids )
{
    std::sort( aMemberUuids.begin(), aMemberUuids.end() );
    std::ostringstream output;
    output << "(group " << quoted( aGroup.at( "name" ).get<std::string>() ) << "\n"
           << "    (uuid " << quoted( aUuid ) << ")\n";

    if( aGroup.at( "locked" ).get<bool>() )
        output << "    (locked yes)\n";

    output << "    (members";

    for( const std::string& memberUuid : aMemberUuids )
        output << ' ' << quoted( memberUuid );

    output << ")\n"
           << "  )";
    return output.str();
}


bool validSheetShape( const JSON& aSheet )
{
    if( !aSheet.is_object() || !aSheet.contains( "id" ) || !aSheet["id"].is_string()
        || !aSheet.contains( "parent" )
        || !( aSheet["parent"].is_null() || aSheet["parent"].is_string() )
        || !aSheet.contains( "file" ) || !aSheet["file"].is_string()
        || !aSheet.contains( "title" ) || !aSheet["title"].is_string()
        || !aSheet.contains( "pins" ) || !aSheet["pins"].is_array() )
    {
        return false;
    }

    if( !aSheet["parent"].is_null()
        && ( !aSheet.contains( "position" ) || !aSheet["position"].is_object()
             || !aSheet["position"].contains( "xNm" )
             || !aSheet["position"]["xNm"].is_number_integer()
             || !aSheet["position"].contains( "yNm" )
             || !aSheet["position"]["yNm"].is_number_integer()
             || !aSheet.contains( "size" ) || !aSheet["size"].is_object()
             || !aSheet["size"].contains( "xNm" )
             || !aSheet["size"]["xNm"].is_number_integer()
             || !aSheet["size"].contains( "yNm" )
             || !aSheet["size"]["yNm"].is_number_integer() ) )
    {
        return false;
    }

    for( const JSON& pin : aSheet["pins"] )
    {
        if( !pin.is_object() || !pin.contains( "name" ) || !pin["name"].is_string()
            || !pin.contains( "direction" ) || !pin["direction"].is_string()
            || !pin.contains( "side" ) || !pin["side"].is_string()
            || !pin.contains( "position" ) || !pin["position"].is_object()
            || !pin["position"].contains( "xNm" )
            || !pin["position"]["xNm"].is_number_integer()
            || !pin["position"].contains( "yNm" )
            || !pin["position"]["yNm"].is_number_integer() )
        {
            return false;
        }
    }

    return true;
}


bool validBusAliasShape( const JSON& aAlias )
{
    if( !aAlias.is_object() || !aAlias.contains( "name" ) || !aAlias["name"].is_string()
        || aAlias["name"].get<std::string>().empty() || !aAlias.contains( "sheet" )
        || !aAlias["sheet"].is_string() || !aAlias.contains( "members" )
        || !aAlias["members"].is_array() || aAlias["members"].empty()
        || aAlias["members"].size() > 256 )
    {
        return false;
    }

    return std::all_of( aAlias["members"].begin(), aAlias["members"].end(),
                        []( const JSON& aMember )
                        {
                            return aMember.is_string()
                                   && !aMember.get<std::string>().empty();
                        } );
}


bool validGroupShape( const JSON& aGroup )
{
    if( !aGroup.is_object() || !aGroup.contains( "id" ) || !aGroup["id"].is_string()
        || aGroup["id"].get<std::string>().empty()
        || aGroup["id"].get<std::string>().size() > 128 || !aGroup.contains( "sheet" )
        || !aGroup["sheet"].is_string() || aGroup["sheet"].get<std::string>().empty()
        || aGroup["sheet"].get<std::string>().size() > 128 || !aGroup.contains( "name" )
        || !aGroup["name"].is_string() || aGroup["name"].get<std::string>().empty()
        || aGroup["name"].get<std::string>().size() > 4096
        || !aGroup.contains( "locked" ) || !aGroup["locked"].is_boolean()
        || !aGroup.contains( "members" ) || !aGroup["members"].is_array()
        || aGroup["members"].empty() || aGroup["members"].size() > 4096 )
    {
        return false;
    }

    for( const JSON& member : aGroup["members"] )
    {
        if( !member.is_object() || !member.contains( "kind" )
            || !member["kind"].is_string() )
        {
            return false;
        }

        const std::string kind = member["kind"].get<std::string>();

        if( kind == "drawing" || kind == "sheet" || kind == "group" )
        {
            if( !member.contains( "id" ) || !member["id"].is_string()
                || member["id"].get<std::string>().empty()
                || member["id"].get<std::string>().size() > 128 )
            {
                return false;
            }
        }
        else if( kind == "component" )
        {
            if( !member.contains( "reference" ) || !member["reference"].is_string()
                || member["reference"].get<std::string>().empty()
                || !member.contains( "unit" ) || !member["unit"].is_number_integer()
                || member["unit"].get<int64_t>() < 1
                || member["unit"].get<int64_t>() > 256 )
            {
                return false;
            }
        }
        else if( kind == "hierarchical_label" )
        {
            if( !member.contains( "sheet" ) || !member["sheet"].is_string()
                || member["sheet"].get<std::string>().empty()
                || !member.contains( "pin" ) || !member["pin"].is_string()
                || member["pin"].get<std::string>().empty() )
            {
                return false;
            }
        }
        else if( kind == "net_label" || kind == "no_connect" )
        {
            if( !member.contains( "reference" ) || !member["reference"].is_string()
                || member["reference"].get<std::string>().empty()
                || !member.contains( "unit" ) || !member["unit"].is_number_integer()
                || member["unit"].get<int64_t>() < 1
                || member["unit"].get<int64_t>() > 256
                || !member.contains( "pin" ) || !member["pin"].is_string()
                || member["pin"].get<std::string>().empty()
                || !member.contains( "occurrence" )
                || !member["occurrence"].is_number_integer()
                || member["occurrence"].get<int64_t>() < 1
                || member["occurrence"].get<int64_t>() > 256
                || ( kind == "net_label"
                     && ( !member.contains( "net" ) || !member["net"].is_string()
                          || member["net"].get<std::string>().empty() ) ) )
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }

    return true;
}


std::string groupMemberKey( const JSON& aMember )
{
    const std::string kind = aMember.at( "kind" ).get<std::string>();

    if( kind == "drawing" || kind == "sheet" || kind == "group" )
        return kind + "/" + aMember.at( "id" ).get<std::string>();

    if( kind == "component" )
    {
        return kind + "/" + aMember.at( "reference" ).get<std::string>() + "/"
               + std::to_string( aMember.at( "unit" ).get<int64_t>() );
    }

    if( kind == "hierarchical_label" )
    {
        return kind + "/" + aMember.at( "sheet" ).get<std::string>() + "/"
               + aMember.at( "pin" ).get<std::string>();
    }

    std::string key = kind + "/";

    if( kind == "net_label" )
        key += aMember.at( "net" ).get<std::string>() + "/";

    return key + aMember.at( "reference" ).get<std::string>() + "/"
           + std::to_string( aMember.at( "unit" ).get<int64_t>() ) + "/"
           + aMember.at( "pin" ).get<std::string>() + "/"
           + std::to_string( aMember.at( "occurrence" ).get<int64_t>() );
}


bool validSchematicColorShape( const JSON& aColor )
{
    if( aColor.is_null() )
        return true;

    if( !aColor.is_object() )
        return false;

    for( const char* channel : { "r", "g", "b", "a" } )
    {
        if( !aColor.contains( channel ) || !aColor[channel].is_number_integer()
            || aColor[channel].get<int>() < 0 || aColor[channel].get<int>() > 255 )
        {
            return false;
        }
    }

    return true;
}


bool validSchematicTextEffectsShape( const JSON& aDrawing )
{
    return aDrawing.contains( "size" ) && aDrawing["size"].is_object()
           && aDrawing["size"].contains( "xNm" )
           && aDrawing["size"]["xNm"].is_number_integer()
           && aDrawing["size"].contains( "yNm" )
           && aDrawing["size"]["yNm"].is_number_integer()
           && aDrawing.contains( "font" ) && aDrawing["font"].is_string()
           && aDrawing.contains( "lineSpacing" ) && aDrawing["lineSpacing"].is_number()
           && aDrawing.contains( "thicknessNm" )
           && aDrawing["thicknessNm"].is_number_integer()
           && aDrawing.contains( "color" )
           && validSchematicColorShape( aDrawing["color"] )
           && aDrawing.contains( "justify" ) && aDrawing["justify"].is_object()
           && aDrawing["justify"].contains( "horizontal" )
           && aDrawing["justify"]["horizontal"].is_string()
           && aDrawing["justify"].contains( "vertical" )
           && aDrawing["justify"]["vertical"].is_string()
           && aDrawing.contains( "mirror" ) && aDrawing["mirror"].is_boolean()
           && aDrawing.contains( "bold" ) && aDrawing["bold"].is_boolean()
           && aDrawing.contains( "italic" ) && aDrawing["italic"].is_boolean()
           && aDrawing.contains( "hyperlink" ) && aDrawing["hyperlink"].is_string();
}


bool validSchematicPointShape( const JSON& aPoint )
{
    return aPoint.is_object() && aPoint.contains( "xNm" )
           && aPoint["xNm"].is_number_integer() && aPoint.contains( "yNm" )
           && aPoint["yNm"].is_number_integer();
}


bool validSchematicStrokeShape( const JSON& aStroke )
{
    static const std::set<std::string> LINE_STYLES = {
        "default", "solid", "dash", "dot", "dash_dot", "dash_dot_dot"
    };
    return aStroke.is_object() && aStroke.contains( "widthNm" )
           && aStroke["widthNm"].is_number_integer() && aStroke.contains( "lineStyle" )
           && aStroke["lineStyle"].is_string()
           && LINE_STYLES.contains( aStroke["lineStyle"].get<std::string>() )
           && aStroke.contains( "color" ) && validSchematicColorShape( aStroke["color"] );
}


bool validSchematicFillShape( const JSON& aFill )
{
    static const std::set<std::string> FILL_TYPES = {
        "none", "outline", "background", "color", "hatch", "reverse_hatch",
        "cross_hatch"
    };
    return aFill.is_object() && aFill.contains( "type" ) && aFill["type"].is_string()
           && FILL_TYPES.contains( aFill["type"].get<std::string>() )
           && aFill.contains( "color" ) && validSchematicColorShape( aFill["color"] );
}


bool validDrawingShape( const JSON& aDrawing )
{
    if( !aDrawing.is_object() || !aDrawing.contains( "kind" )
        || !aDrawing["kind"].is_string() || !aDrawing.contains( "id" )
        || !aDrawing["id"].is_string() || aDrawing["id"].get<std::string>().empty()
        || !aDrawing.contains( "sheet" ) || !aDrawing["sheet"].is_string() )
    {
        return false;
    }

    const std::string kind = aDrawing["kind"].get<std::string>();

    if( kind == "label" )
    {
        if( !aDrawing.contains( "scope" ) || !aDrawing["scope"].is_string()
            || ( aDrawing["scope"] != "local" && aDrawing["scope"] != "global" )
            || !aDrawing.contains( "net" ) || !aDrawing["net"].is_string()
            || !aDrawing.contains( "position" ) || !aDrawing["position"].is_object()
            || !aDrawing["position"].contains( "xNm" )
            || !aDrawing["position"]["xNm"].is_number_integer()
            || !aDrawing["position"].contains( "yNm" )
            || !aDrawing["position"]["yNm"].is_number_integer()
            || !aDrawing.contains( "rotationDegrees" )
            || !aDrawing["rotationDegrees"].is_number_integer()
            || !aDrawing.contains( "shape" ) || !aDrawing["shape"].is_string()
            || !aDrawing.contains( "size" ) || !aDrawing["size"].is_object()
            || !aDrawing["size"].contains( "xNm" )
            || !aDrawing["size"]["xNm"].is_number_integer()
            || !aDrawing["size"].contains( "yNm" )
            || !aDrawing["size"]["yNm"].is_number_integer()
            || !aDrawing.contains( "thicknessNm" )
            || !aDrawing["thicknessNm"].is_number_integer()
            || !aDrawing.contains( "justify" ) || !aDrawing["justify"].is_object()
            || !aDrawing["justify"].contains( "horizontal" )
            || !aDrawing["justify"]["horizontal"].is_string()
            || !aDrawing["justify"].contains( "vertical" )
            || !aDrawing["justify"]["vertical"].is_string()
            || !aDrawing.contains( "bold" ) || !aDrawing["bold"].is_boolean()
            || !aDrawing.contains( "italic" ) || !aDrawing["italic"].is_boolean() )
        {
            return false;
        }

        return true;
    }

    if( kind == "text" )
    {
        if( !aDrawing.contains( "content" ) || !aDrawing["content"].is_string()
            || !aDrawing.contains( "position" ) || !aDrawing["position"].is_object()
            || !aDrawing["position"].contains( "xNm" )
            || !aDrawing["position"]["xNm"].is_number_integer()
            || !aDrawing["position"].contains( "yNm" )
            || !aDrawing["position"]["yNm"].is_number_integer()
            || !aDrawing.contains( "rotationDegrees" )
            || !aDrawing["rotationDegrees"].is_number()
            || !aDrawing.contains( "exclude_from_sim" )
            || !aDrawing["exclude_from_sim"].is_boolean()
            || !aDrawing.contains( "size" ) || !aDrawing["size"].is_object()
            || !aDrawing["size"].contains( "xNm" )
            || !aDrawing["size"]["xNm"].is_number_integer()
            || !aDrawing["size"].contains( "yNm" )
            || !aDrawing["size"]["yNm"].is_number_integer()
            || !aDrawing.contains( "font" ) || !aDrawing["font"].is_string()
            || !aDrawing.contains( "lineSpacing" ) || !aDrawing["lineSpacing"].is_number()
            || !aDrawing.contains( "thicknessNm" )
            || !aDrawing["thicknessNm"].is_number_integer()
            || !aDrawing.contains( "color" )
            || !( aDrawing["color"].is_null() || aDrawing["color"].is_object() )
            || !aDrawing.contains( "justify" ) || !aDrawing["justify"].is_object()
            || !aDrawing["justify"].contains( "horizontal" )
            || !aDrawing["justify"]["horizontal"].is_string()
            || !aDrawing["justify"].contains( "vertical" )
            || !aDrawing["justify"]["vertical"].is_string()
            || !aDrawing.contains( "mirror" ) || !aDrawing["mirror"].is_boolean()
            || !aDrawing.contains( "bold" ) || !aDrawing["bold"].is_boolean()
            || !aDrawing.contains( "italic" ) || !aDrawing["italic"].is_boolean()
            || !aDrawing.contains( "hyperlink" ) || !aDrawing["hyperlink"].is_string() )
        {
            return false;
        }

        if( aDrawing["color"].is_object() )
        {
            for( const char* channel : { "r", "g", "b", "a" } )
            {
                if( !aDrawing["color"].contains( channel )
                    || !aDrawing["color"][channel].is_number_integer() )
                {
                    return false;
                }
            }
        }

        return true;
    }

    if( kind == "text_box" )
    {
        static const std::set<std::string> LINE_STYLES = {
            "default", "solid", "dash", "dot", "dash_dot", "dash_dot_dot"
        };
        static const std::set<std::string> FILL_TYPES = {
            "none", "outline", "background", "color", "hatch", "reverse_hatch",
            "cross_hatch"
        };

        if( !aDrawing.contains( "content" ) || !aDrawing["content"].is_string()
            || !aDrawing.contains( "position" ) || !aDrawing["position"].is_object()
            || !aDrawing["position"].contains( "xNm" )
            || !aDrawing["position"]["xNm"].is_number_integer()
            || !aDrawing["position"].contains( "yNm" )
            || !aDrawing["position"]["yNm"].is_number_integer()
            || !aDrawing.contains( "rotationDegrees" )
            || !aDrawing["rotationDegrees"].is_number()
            || !aDrawing.contains( "exclude_from_sim" )
            || !aDrawing["exclude_from_sim"].is_boolean()
            || !aDrawing.contains( "boxSize" ) || !aDrawing["boxSize"].is_object()
            || !aDrawing["boxSize"].contains( "xNm" )
            || !aDrawing["boxSize"]["xNm"].is_number_integer()
            || !aDrawing["boxSize"].contains( "yNm" )
            || !aDrawing["boxSize"]["yNm"].is_number_integer()
            || !aDrawing.contains( "margins" ) || !aDrawing["margins"].is_object()
            || !aDrawing["margins"].contains( "leftNm" )
            || !aDrawing["margins"]["leftNm"].is_number_integer()
            || !aDrawing["margins"].contains( "topNm" )
            || !aDrawing["margins"]["topNm"].is_number_integer()
            || !aDrawing["margins"].contains( "rightNm" )
            || !aDrawing["margins"]["rightNm"].is_number_integer()
            || !aDrawing["margins"].contains( "bottomNm" )
            || !aDrawing["margins"]["bottomNm"].is_number_integer()
            || !aDrawing.contains( "stroke" ) || !aDrawing["stroke"].is_object()
            || !aDrawing["stroke"].contains( "widthNm" )
            || !aDrawing["stroke"]["widthNm"].is_number_integer()
            || !aDrawing["stroke"].contains( "lineStyle" )
            || !aDrawing["stroke"]["lineStyle"].is_string()
            || !LINE_STYLES.contains( aDrawing["stroke"]["lineStyle"].get<std::string>() )
            || !aDrawing["stroke"].contains( "color" )
            || !validSchematicColorShape( aDrawing["stroke"]["color"] )
            || !aDrawing.contains( "fill" ) || !aDrawing["fill"].is_object()
            || !aDrawing["fill"].contains( "type" )
            || !aDrawing["fill"]["type"].is_string()
            || !FILL_TYPES.contains( aDrawing["fill"]["type"].get<std::string>() )
            || !aDrawing["fill"].contains( "color" )
            || !validSchematicColorShape( aDrawing["fill"]["color"] )
            || !validSchematicTextEffectsShape( aDrawing ) )
        {
            return false;
        }

        return true;
    }

    if( kind == "polyline" || kind == "rectangle" || kind == "circle" || kind == "arc"
        || kind == "bezier" )
    {
        static const std::set<std::string> LINE_STYLES = {
            "default", "solid", "dash", "dot", "dash_dot", "dash_dot_dot"
        };
        static const std::set<std::string> FILL_TYPES = {
            "none", "outline", "background", "color", "hatch", "reverse_hatch",
            "cross_hatch"
        };

        if( !aDrawing.contains( "stroke" ) || !aDrawing["stroke"].is_object()
            || !aDrawing["stroke"].contains( "widthNm" )
            || !aDrawing["stroke"]["widthNm"].is_number_integer()
            || !aDrawing["stroke"].contains( "lineStyle" )
            || !aDrawing["stroke"]["lineStyle"].is_string()
            || !LINE_STYLES.contains( aDrawing["stroke"]["lineStyle"].get<std::string>() )
            || !aDrawing["stroke"].contains( "color" )
            || !validSchematicColorShape( aDrawing["stroke"]["color"] )
            || !aDrawing.contains( "fill" ) || !aDrawing["fill"].is_object()
            || !aDrawing["fill"].contains( "type" )
            || !aDrawing["fill"]["type"].is_string()
            || !FILL_TYPES.contains( aDrawing["fill"]["type"].get<std::string>() )
            || !aDrawing["fill"].contains( "color" )
            || !validSchematicColorShape( aDrawing["fill"]["color"] ) )
        {
            return false;
        }

        if( kind == "polyline" || kind == "bezier" )
        {
            if( !aDrawing.contains( "points" ) || !aDrawing["points"].is_array()
                || ( kind == "polyline"
                             ? aDrawing["points"].size() < 2
                                       || aDrawing["points"].size() > 1024
                             : aDrawing["points"].size() != 4 ) )
            {
                return false;
            }

            return std::all_of( aDrawing["points"].begin(), aDrawing["points"].end(),
                                []( const JSON& aPoint )
                                {
                                    return validSchematicPointShape( aPoint );
                                } );
        }

        if( kind == "rectangle" )
        {
            return aDrawing.contains( "start" )
                   && validSchematicPointShape( aDrawing["start"] )
                   && aDrawing.contains( "end" )
                   && validSchematicPointShape( aDrawing["end"] )
                   && aDrawing.contains( "cornerRadiusNm" )
                   && aDrawing["cornerRadiusNm"].is_number_integer();
        }

        if( kind == "circle" )
        {
            return aDrawing.contains( "center" )
                   && validSchematicPointShape( aDrawing["center"] )
                   && aDrawing.contains( "radiusNm" )
                   && aDrawing["radiusNm"].is_number_integer();
        }

        return aDrawing.contains( "start" )
               && validSchematicPointShape( aDrawing["start"] )
               && aDrawing.contains( "mid" )
               && validSchematicPointShape( aDrawing["mid"] )
               && aDrawing.contains( "end" )
               && validSchematicPointShape( aDrawing["end"] );
    }

    if( kind == "image" )
    {
        return aDrawing.contains( "position" )
               && validSchematicPointShape( aDrawing["position"] )
               && aDrawing.contains( "scale" ) && aDrawing["scale"].is_number()
               && aDrawing.contains( "mediaType" ) && aDrawing["mediaType"].is_string()
               && aDrawing.contains( "sha256" ) && aDrawing["sha256"].is_string()
               && aDrawing.contains( "description" ) && aDrawing["description"].is_string()
               && aDrawing.contains( "dataBase64" ) && aDrawing["dataBase64"].is_string()
               && aDrawing.contains( "byteCount" )
               && aDrawing["byteCount"].is_number_unsigned();
    }

    if( kind == "table" )
    {
        if( !aDrawing.contains( "position" )
            || !validSchematicPointShape( aDrawing["position"] )
            || !aDrawing.contains( "rotationDegrees" )
            || !aDrawing["rotationDegrees"].is_number_integer()
            || ( aDrawing["rotationDegrees"] != 0 && aDrawing["rotationDegrees"] != 90 )
            || !aDrawing.contains( "columnWidthsNm" )
            || !aDrawing["columnWidthsNm"].is_array()
            || aDrawing["columnWidthsNm"].empty()
            || aDrawing["columnWidthsNm"].size() > 256
            || !aDrawing.contains( "rowHeightsNm" )
            || !aDrawing["rowHeightsNm"].is_array()
            || aDrawing["rowHeightsNm"].empty() || aDrawing["rowHeightsNm"].size() > 256
            || !aDrawing.contains( "border" ) || !aDrawing["border"].is_object()
            || !aDrawing["border"].contains( "external" )
            || !aDrawing["border"]["external"].is_boolean()
            || !aDrawing["border"].contains( "header" )
            || !aDrawing["border"]["header"].is_boolean()
            || !aDrawing["border"].contains( "stroke" )
            || !validSchematicStrokeShape( aDrawing["border"]["stroke"] )
            || !aDrawing.contains( "separators" ) || !aDrawing["separators"].is_object()
            || !aDrawing["separators"].contains( "rows" )
            || !aDrawing["separators"]["rows"].is_boolean()
            || !aDrawing["separators"].contains( "columns" )
            || !aDrawing["separators"]["columns"].is_boolean()
            || !aDrawing["separators"].contains( "stroke" )
            || !validSchematicStrokeShape( aDrawing["separators"]["stroke"] )
            || !aDrawing.contains( "cells" ) || !aDrawing["cells"].is_array()
            || !aDrawing.contains( "merges" ) || !aDrawing["merges"].is_array() )
        {
            return false;
        }

        if( !std::all_of( aDrawing["columnWidthsNm"].begin(),
                          aDrawing["columnWidthsNm"].end(),
                          []( const JSON& aValue ) { return aValue.is_number_integer(); } )
            || !std::all_of( aDrawing["rowHeightsNm"].begin(),
                             aDrawing["rowHeightsNm"].end(),
                             []( const JSON& aValue ) { return aValue.is_number_integer(); } ) )
        {
            return false;
        }

        const size_t columnCount = aDrawing["columnWidthsNm"].size();
        const size_t rowCount = aDrawing["rowHeightsNm"].size();

        if( rowCount > 65536 / columnCount
            || aDrawing["cells"].size() != rowCount * columnCount )
        {
            return false;
        }

        std::set<std::pair<int, int>> coordinates;

        for( const JSON& cell : aDrawing["cells"] )
        {
            if( !cell.is_object() || !cell.contains( "row" )
                || !cell["row"].is_number_integer() || !cell.contains( "column" )
                || !cell["column"].is_number_integer() || !cell.contains( "content" )
                || !cell["content"].is_string() || !cell.contains( "margins" )
                || !cell["margins"].is_object() || !cell.contains( "exclude_from_sim" )
                || !cell["exclude_from_sim"].is_boolean() || !cell.contains( "fill" )
                || !validSchematicFillShape( cell["fill"] )
                || !validSchematicTextEffectsShape( cell ) )
            {
                return false;
            }

            for( const char* margin : { "leftNm", "topNm", "rightNm", "bottomNm" } )
            {
                if( !cell["margins"].contains( margin )
                    || !cell["margins"][margin].is_number_integer() )
                {
                    return false;
                }
            }

            const int row = cell["row"].get<int>();
            const int column = cell["column"].get<int>();

            if( row < 0 || column < 0 || static_cast<size_t>( row ) >= rowCount
                || static_cast<size_t>( column ) >= columnCount
                || !coordinates.emplace( row, column ).second )
            {
                return false;
            }
        }

        std::vector<bool> merged( rowCount * columnCount, false );

        for( const JSON& merge : aDrawing["merges"] )
        {
            if( !merge.is_object() || !merge.contains( "firstRow" )
                || !merge["firstRow"].is_number_integer() || !merge.contains( "firstColumn" )
                || !merge["firstColumn"].is_number_integer() || !merge.contains( "lastRow" )
                || !merge["lastRow"].is_number_integer() || !merge.contains( "lastColumn" )
                || !merge["lastColumn"].is_number_integer() )
            {
                return false;
            }

            const int firstRow = merge["firstRow"].get<int>();
            const int firstColumn = merge["firstColumn"].get<int>();
            const int lastRow = merge["lastRow"].get<int>();
            const int lastColumn = merge["lastColumn"].get<int>();

            if( firstRow < 0 || firstColumn < 0 || lastRow < firstRow
                || lastColumn < firstColumn || static_cast<size_t>( lastRow ) >= rowCount
                || static_cast<size_t>( lastColumn ) >= columnCount
                || ( firstRow == lastRow && firstColumn == lastColumn ) )
            {
                return false;
            }

            for( int row = firstRow; row <= lastRow; ++row )
            {
                for( int column = firstColumn; column <= lastColumn; ++column )
                {
                    const size_t offset = static_cast<size_t>( row ) * columnCount
                                          + static_cast<size_t>( column );

                    if( merged[offset] )
                        return false;

                    merged[offset] = true;
                }
            }
        }

        return true;
    }

    if( kind == "rule_area" )
    {
        if( !aDrawing.contains( "polygon" ) || !aDrawing["polygon"].is_array()
            || aDrawing["polygon"].size() < 3 || aDrawing["polygon"].size() > 1024
            || !aDrawing.contains( "stroke" ) || !aDrawing["stroke"].is_object()
            || !aDrawing["stroke"].contains( "widthNm" )
            || !aDrawing["stroke"]["widthNm"].is_number_integer()
            || !aDrawing["stroke"].contains( "lineStyle" )
            || !aDrawing["stroke"]["lineStyle"].is_string()
            || !aDrawing["stroke"].contains( "color" )
            || !( aDrawing["stroke"]["color"].is_null()
                  || aDrawing["stroke"]["color"].is_object() )
            || !aDrawing.contains( "fill" ) || !aDrawing["fill"].is_object()
            || !aDrawing["fill"].contains( "type" )
            || !aDrawing["fill"]["type"].is_string()
            || !aDrawing["fill"].contains( "color" )
            || !( aDrawing["fill"]["color"].is_null()
                  || aDrawing["fill"]["color"].is_object() )
            || !aDrawing.contains( "exclude_from_sim" )
            || !aDrawing["exclude_from_sim"].is_boolean()
            || !aDrawing.contains( "exclude_from_bom" )
            || !aDrawing["exclude_from_bom"].is_boolean()
            || !aDrawing.contains( "exclude_from_board" )
            || !aDrawing["exclude_from_board"].is_boolean()
            || !aDrawing.contains( "dnp" ) || !aDrawing["dnp"].is_boolean() )
        {
            return false;
        }

        for( const JSON& point : aDrawing["polygon"] )
        {
            if( !point.is_object() || !point.contains( "xNm" )
                || !point["xNm"].is_number_integer() || !point.contains( "yNm" )
                || !point["yNm"].is_number_integer() )
            {
                return false;
            }
        }

        for( const JSON* color : { &aDrawing["stroke"]["color"],
                                   &aDrawing["fill"]["color"] } )
        {
            if( color->is_null() )
                continue;

            for( const char* channel : { "r", "g", "b", "a" } )
            {
                if( !color->contains( channel ) || !( *color )[channel].is_number_integer() )
                    return false;
            }
        }

        return true;
    }

    if( kind == "directive" )
    {
        if( !aDrawing.contains( "target" ) || !aDrawing["target"].is_object()
            || ( aDrawing["target"].value( "kind", "" ) != "net"
                 && aDrawing["target"].value( "kind", "" ) != "rule_area" )
            || !aDrawing["target"].contains( "name" )
            || !aDrawing["target"]["name"].is_string()
            || !aDrawing.contains( "position" ) || !aDrawing["position"].is_object()
            || !aDrawing["position"].contains( "xNm" )
            || !aDrawing["position"]["xNm"].is_number_integer()
            || !aDrawing["position"].contains( "yNm" )
            || !aDrawing["position"]["yNm"].is_number_integer()
            || !aDrawing.contains( "rotationDegrees" )
            || !aDrawing["rotationDegrees"].is_number_integer()
            || !aDrawing.contains( "shape" ) || !aDrawing["shape"].is_string()
            || !aDrawing.contains( "lengthNm" )
            || !aDrawing["lengthNm"].is_number_integer()
            || !aDrawing.contains( "properties" ) || !aDrawing["properties"].is_array()
            || aDrawing["properties"].empty() || aDrawing["properties"].size() > 64 )
        {
            return false;
        }

        for( const JSON& property : aDrawing["properties"] )
        {
            if( !property.is_object() || !property.contains( "name" )
                || !property["name"].is_string() || property["name"].get<std::string>().empty()
                || !property.contains( "value" ) || !property["value"].is_string()
                || !property.contains( "position" ) || !property["position"].is_object()
                || !property["position"].contains( "xNm" )
                || !property["position"]["xNm"].is_number_integer()
                || !property["position"].contains( "yNm" )
                || !property["position"]["yNm"].is_number_integer()
                || !property.contains( "rotationDegrees" )
                || !property["rotationDegrees"].is_number_integer()
                || !property.contains( "size" ) || !property["size"].is_object()
                || !property["size"].contains( "xNm" )
                || !property["size"]["xNm"].is_number_integer()
                || !property["size"].contains( "yNm" )
                || !property["size"]["yNm"].is_number_integer()
                || !property.contains( "thicknessNm" )
                || !property["thicknessNm"].is_number_integer()
                || !property.contains( "justify" ) || !property["justify"].is_object()
                || !property["justify"].contains( "horizontal" )
                || !property["justify"]["horizontal"].is_string()
                || !property["justify"].contains( "vertical" )
                || !property["justify"]["vertical"].is_string()
                || !property.contains( "bold" ) || !property["bold"].is_boolean()
                || !property.contains( "italic" ) || !property["italic"].is_boolean()
                || !property.contains( "visible" ) || !property["visible"].is_boolean() )
            {
                return false;
            }
        }

        return true;
    }

    if( kind == "junction" )
    {
        if( !aDrawing.contains( "position" ) || !aDrawing["position"].is_object()
            || !aDrawing["position"].contains( "xNm" )
            || !aDrawing["position"]["xNm"].is_number_integer()
            || !aDrawing["position"].contains( "yNm" )
            || !aDrawing["position"]["yNm"].is_number_integer()
            || !aDrawing.contains( "diameterNm" )
            || !aDrawing["diameterNm"].is_number_integer()
            || !aDrawing.contains( "color" )
            || !( aDrawing["color"].is_null() || aDrawing["color"].is_object() ) )
        {
            return false;
        }

        if( aDrawing["color"].is_object() )
        {
            for( const char* channel : { "r", "g", "b", "a" } )
            {
                if( !aDrawing["color"].contains( channel )
                    || !aDrawing["color"][channel].is_number_integer() )
                {
                    return false;
                }
            }
        }

        return true;
    }

    if( kind != "wire" && kind != "bus" && kind != "bus_entry" )
        return false;

    for( const char* point : { "from", "to" } )
    {
        if( !aDrawing.contains( point ) || !aDrawing[point].is_object()
            || !aDrawing[point].contains( "xNm" )
            || !aDrawing[point]["xNm"].is_number_integer()
            || !aDrawing[point].contains( "yNm" )
            || !aDrawing[point]["yNm"].is_number_integer() )
        {
            return false;
        }
    }

    return aDrawing.contains( "stroke" ) && aDrawing["stroke"].is_object()
           && aDrawing["stroke"].contains( "widthNm" )
           && aDrawing["stroke"]["widthNm"].is_number_integer()
           && aDrawing["stroke"].contains( "lineStyle" )
           && aDrawing["stroke"]["lineStyle"].is_string();
}


bool validComponentShape( const JSON& aComponent )
{
    if( !aComponent.is_object() || !aComponent.contains( "reference" )
        || !aComponent["reference"].is_string() || !aComponent.contains( "symbol" )
        || !aComponent["symbol"].is_string() || !aComponent.contains( "value" )
        || !aComponent["value"].is_string() || !aComponent.contains( "footprint" )
        || ( !aComponent["footprint"].is_string() && !aComponent["footprint"].is_null() )
        || !aComponent.contains( "properties" )
        || !aComponent["properties"].is_object() || aComponent["properties"].size() > 1024
        || !aComponent.contains( "units" )
        || !aComponent["units"].is_array() || aComponent["units"].empty()
        || ( aComponent.contains( "datasheet" ) && !aComponent["datasheet"].is_string() )
        || ( aComponent.contains( "description" )
             && !aComponent["description"].is_string() )
        || ( aComponent.contains( "datasheet" )
             && aComponent["datasheet"].get<std::string>().size() > 4096 )
        || ( aComponent.contains( "description" )
             && aComponent["description"].get<std::string>().size() > 4096 ) )
    {
        return false;
    }

    std::set<std::string> propertyNames;

    for( auto property = aComponent["properties"].begin();
         property != aComponent["properties"].end(); ++property )
    {
        if( property.key().empty() || property.key().size() > 128
            || !property.value().is_string()
            || property.value().get<std::string>().size() > 4096 )
        {
            return false;
        }

        propertyNames.emplace( property.key() );
    }

    static const std::set<std::string> MANDATORY_FIELD_NAMES = {
        "Reference", "Value", "Footprint", "Datasheet", "Description"
    };

    for( const JSON& unit : aComponent["units"] )
    {
        if( !unit.is_object() || !unit.contains( "number" )
            || !unit["number"].is_number_integer() || !unit.contains( "sheet" )
            || !unit["sheet"].is_string() || !unit.contains( "position" )
            || !unit["position"].is_object() || !unit["position"].contains( "xNm" )
            || !unit["position"]["xNm"].is_number_integer()
            || !unit["position"].contains( "yNm" )
            || !unit["position"]["yNm"].is_number_integer()
            || !unit.contains( "rotationDegrees" )
            || !unit["rotationDegrees"].is_number_integer() || !unit.contains( "mirror" )
            || !unit["mirror"].is_string() || !unit.contains( "fieldsAutoplaced" )
            || !unit["fieldsAutoplaced"].is_boolean() || !unit.contains( "fields" )
            || !unit["fields"].is_array() || unit["fields"].size() > 1024 )
        {
            return false;
        }

        std::set<std::string> fieldNames;

        for( const JSON& field : unit["fields"] )
        {
            if( !field.is_object() || !field.contains( "name" ) || !field["name"].is_string()
                || field["name"].get<std::string>().empty()
                || field["name"].get<std::string>().size() > 128
                || !field.contains( "position" )
                || !validSchematicPointShape( field["position"] )
                || !field.contains( "rotationDegrees" )
                || !field["rotationDegrees"].is_number()
                || field["rotationDegrees"].get<double>() < 0.0
                || field["rotationDegrees"].get<double>() >= 360.0
                || !field.contains( "visible" ) || !field["visible"].is_boolean()
                || !field.contains( "showName" ) || !field["showName"].is_boolean()
                || !field.contains( "autoplace" ) || !field["autoplace"].is_boolean()
                || !field.contains( "private" ) || !field["private"].is_boolean()
                || !validSchematicTextEffectsShape( field )
                || field["mirror"].get<bool>() )
            {
                return false;
            }

            const std::string name = field["name"].get<std::string>();
            const int64_t fieldX = field["position"]["xNm"].get<int64_t>();
            const int64_t fieldY = field["position"]["yNm"].get<int64_t>();
            const int64_t width = field["size"]["xNm"].get<int64_t>();
            const int64_t height = field["size"]["yNm"].get<int64_t>();
            const int64_t thickness = field["thicknessNm"].get<int64_t>();
            const double lineSpacing = field["lineSpacing"].get<double>();
            const std::string font = field["font"].get<std::string>();
            const std::string horizontal = field["justify"]["horizontal"].get<std::string>();
            const std::string vertical = field["justify"]["vertical"].get<std::string>();
            const std::string hyperlink = field["hyperlink"].get<std::string>();

            if( !fieldNames.emplace( name ).second
                || ( !MANDATORY_FIELD_NAMES.contains( name )
                     && !propertyNames.contains( name ) )
                || ( MANDATORY_FIELD_NAMES.contains( name )
                     && field["private"].get<bool>() )
                || fieldX < 0 || fieldY < 0 || fieldX > 2'000'000'000
                || fieldY > 2'000'000'000 || width < 100'000 || height < 100'000
                || width > 50'000'000 || height > 50'000'000 || font.empty()
                || font.size() > 256 || !std::isfinite( lineSpacing )
                || lineSpacing < 0.5 || lineSpacing > 5.0 || thickness < 0
                || thickness > 10'000'000
                || ( horizontal != "left" && horizontal != "center"
                     && horizontal != "right" )
                || ( vertical != "top" && vertical != "center" && vertical != "bottom" )
                || hyperlink.size() > 2048 || hyperlink.find_first_of( "\r\n" )
                                                   != std::string::npos
                || hyperlink.find( '\0' ) != std::string::npos )
            {
                return false;
            }
        }
    }

    return true;
}


bool validUuid( const std::string& aUuid )
{
    if( aUuid.size() != 36 )
        return false;

    for( size_t index = 0; index < aUuid.size(); ++index )
    {
        if( index == 8 || index == 13 || index == 18 || index == 23 )
        {
            if( aUuid[index] != '-' )
                return false;
        }
        else if( !std::isxdigit( static_cast<unsigned char>( aUuid[index] ) ) )
        {
            return false;
        }
    }

    return true;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT
DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( const JSON& aCompilerIr,
                                      const JSON& aExistingScreenUuids,
                                      const JSON& aResolvedSymbols )
{
    RESULT result;
    result.counts = { { "files", 0 }, { "sheets", 0 }, { "pins", 0 },
                      { "components", 0 }, { "netEndpoints", 0 },
                      { "noConnects", 0 }, { "drawings", 0 }, { "busAliases", 0 },
                      { "generatedWires", 0 }, { "generatedJunctions", 0 },
                      { "groups", 0 },
                      { "librarySymbols", 0 },
                      { "managedItems", 0 } };

    if( !aCompilerIr.is_object() || aCompilerIr.value( "language", "" ) != "kichad-design"
        || aCompilerIr.value( "version", 0 ) != 1 || !aCompilerIr.contains( "project" )
        || !aCompilerIr["project"].is_object()
        || !aCompilerIr["project"].contains( "name" )
        || !aCompilerIr["project"]["name"].is_string()
        || !aCompilerIr.contains( "schematic" ) || !aCompilerIr["schematic"].is_object()
        || !aCompilerIr["schematic"].contains( "sheets" )
        || !aCompilerIr["schematic"]["sheets"].is_array()
        || !aCompilerIr["schematic"].contains( "components" )
        || !aCompilerIr["schematic"]["components"].is_array()
        || !aCompilerIr["schematic"].contains( "nets" )
        || !aCompilerIr["schematic"]["nets"].is_array()
        || !aCompilerIr["schematic"].contains( "noConnects" )
        || !aCompilerIr["schematic"]["noConnects"].is_array()
        || !aCompilerIr["schematic"].contains( "drawings" )
        || !aCompilerIr["schematic"]["drawings"].is_array()
        || !aCompilerIr["schematic"].contains( "busAliases" )
        || !aCompilerIr["schematic"]["busAliases"].is_array()
        || !aCompilerIr["schematic"].contains( "groups" )
        || !aCompilerIr["schematic"]["groups"].is_array()
        || !aExistingScreenUuids.is_object() || !aResolvedSymbols.is_object() )
    {
        diagnostic( result, "invalid_compiler_ir",
                    "schematic planning requires valid KiChad Design Script version 1 IR" );
        return result;
    }

    const std::string project = aCompilerIr["project"]["name"].get<std::string>();
    const bool ownsTitleBlock = aCompilerIr["project"].contains( "titleBlock" );

    if( ownsTitleBlock )
    {
        const JSON& titleBlock = aCompilerIr["project"]["titleBlock"];

        if( !titleBlock.is_object() || !titleBlock.contains( "title" )
            || !titleBlock["title"].is_string() || !titleBlock.contains( "date" )
            || !titleBlock["date"].is_string() || !titleBlock.contains( "revision" )
            || !titleBlock["revision"].is_string() || !titleBlock.contains( "company" )
            || !titleBlock["company"].is_string() || !titleBlock.contains( "comments" )
            || !titleBlock["comments"].is_array() || titleBlock["comments"].size() != 9
            || !std::all_of( titleBlock["comments"].begin(), titleBlock["comments"].end(),
                             []( const JSON& aComment ) { return aComment.is_string(); } ) )
        {
            diagnostic( result, "invalid_title_block_ir",
                        "schematic title block IR must contain four strings and nine comments" );
            return result;
        }
    }
    const JSON& sourceSheets = aCompilerIr["schematic"]["sheets"];
    const JSON& sourceComponents = aCompilerIr["schematic"]["components"];
    const JSON& sourceNets = aCompilerIr["schematic"]["nets"];
    const JSON& sourceNoConnects = aCompilerIr["schematic"]["noConnects"];
    const JSON& sourceDrawings = aCompilerIr["schematic"]["drawings"];
    const JSON& sourceBusAliases = aCompilerIr["schematic"]["busAliases"];
    const JSON& sourceGroups = aCompilerIr["schematic"]["groups"];

    if( sourceSheets.empty() )
    {
        if( !sourceDrawings.empty() || !sourceBusAliases.empty() || !sourceGroups.empty() )
        {
            diagnostic( result, "invalid_schematic_ir",
                        "schematic drawings, aliases, and groups require a declared hierarchy" );
            return result;
        }

        result.fullyLowered = true;
        return result;
    }

    std::map<std::string, JSON> sheets;
    std::map<std::string, std::string> fileOwners;
    std::string rootId;

    for( const JSON& sheet : sourceSheets )
    {
        if( !validSheetShape( sheet ) )
        {
            diagnostic( result, "invalid_schematic_ir",
                        "schematic IR contains a malformed sheet or pin" );
            continue;
        }

        const std::string id = sheet["id"].get<std::string>();
        const std::string file = sheet["file"].get<std::string>();

        if( id.empty() || !sheets.emplace( id, sheet ).second )
            diagnostic( result, "invalid_schematic_ir", "schematic sheet IDs must be unique" );

        auto [fileEntry, fileInserted] = fileOwners.emplace( file, id );

        if( !fileInserted )
        {
            diagnostic( result, "shared_sheet_file_not_supported",
                        "sheet file " + file + " is referenced by both " + fileEntry->second
                                + " and " + id
                                + "; shared-screen instance lowering is not yet supported" );
        }

        if( sheet["parent"].is_null() )
        {
            if( !rootId.empty() )
                diagnostic( result, "invalid_schematic_ir", "schematic IR has multiple roots" );

            rootId = id;
        }
    }

    if( rootId.empty() )
        diagnostic( result, "invalid_schematic_ir", "schematic IR has no root sheet" );

    std::map<std::string, std::vector<std::string>> children;

    for( const auto& [ id, sheet ] : sheets )
    {
        if( sheet["parent"].is_null() )
            continue;

        const std::string parent = sheet["parent"].get<std::string>();

        if( !sheets.contains( parent ) )
            diagnostic( result, "invalid_schematic_ir", "sheet " + id + " has no parent " + parent );
        else
            children[parent].push_back( id );
    }

    for( auto& [ parent, childIds ] : children )
        std::sort( childIds.begin(), childIds.end() );

    std::set<std::string> visiting;
    std::set<std::string> visited;
    std::vector<std::string> orderedIds;
    std::function<void( const std::string& )> visit = [&]( const std::string& aId )
    {
        if( visiting.contains( aId ) )
        {
            diagnostic( result, "invalid_schematic_ir", "schematic IR contains a parent cycle" );
            return;
        }

        if( visited.contains( aId ) )
            return;

        visiting.insert( aId );
        visited.insert( aId );
        orderedIds.push_back( aId );

        for( const std::string& child : children[aId] )
            visit( child );

        visiting.erase( aId );
    };

    if( !rootId.empty() )
        visit( rootId );

    if( visited.size() != sheets.size() )
        diagnostic( result, "invalid_schematic_ir", "schematic IR contains unreachable sheets" );

    if( !result.diagnostics.empty() )
        return result;

    std::map<std::string, int> pages;

    for( size_t index = 0; index < orderedIds.size(); ++index )
        pages[orderedIds[index]] = static_cast<int>( index + 1 );

    std::map<std::string, std::string> screenUuids;

    for( const auto& [ id, sheet ] : sheets )
    {
        const std::string file = sheet["file"].get<std::string>();
        std::string screenUuid = stableUuid( project, "schematic_file", file );

        if( aExistingScreenUuids.contains( file ) )
        {
            if( !aExistingScreenUuids[file].is_string()
                || !validUuid( aExistingScreenUuids[file].get<std::string>() ) )
            {
                diagnostic( result, "invalid_screen_identity",
                            "existing screen UUID for " + file + " is invalid" );
                return result;
            }

            screenUuid = aExistingScreenUuids[file].get<std::string>();
        }

        screenUuids.emplace( file, std::move( screenUuid ) );
    }

    const std::string rootScreenUuid =
            screenUuids.at( sheets.at( rootId )["file"].get<std::string>() );
    std::map<std::string, std::string> instancePaths;

    for( const std::string& id : orderedIds )
    {
        std::vector<std::string> ancestors;
        std::string cursor = id;

        while( cursor != rootId )
        {
            ancestors.push_back( cursor );
            cursor = sheets.at( cursor )["parent"].get<std::string>();
        }

        std::reverse( ancestors.begin(), ancestors.end() );
        std::string path = "/" + rootScreenUuid;

        for( const std::string& ancestor : ancestors )
            path += "/" + stableUuid( project, "schematic_sheet", ancestor );

        instancePaths[id] = std::move( path );
    }

    struct PIN_POINT
    {
        std::string sheet;
        int64_t     x;
        int64_t     y;
        int         labelRotation;
    };

    std::map<std::string, std::vector<JSON>> placementsBySheet;
    std::map<std::string, std::map<std::string, JSON>> librariesBySheet;
    std::map<std::string, std::vector<PIN_POINT>> endpointPositions;
    std::map<std::string, JSON> groupTargets;
    std::set<std::string> componentReferences;

    try
    {
        for( const JSON& component : sourceComponents )
        {
            if( !validComponentShape( component ) )
            {
                diagnostic( result, "invalid_schematic_ir",
                            "schematic IR contains a malformed component placement" );
                continue;
            }

            const std::string reference = component["reference"].get<std::string>();
            const std::string libraryId = component["symbol"].get<std::string>();

            if( !componentReferences.emplace( reference ).second )
            {
                diagnostic( result, "invalid_schematic_ir",
                            "schematic component references must be unique" );
                continue;
            }

            if( !aResolvedSymbols.contains( libraryId )
                || !aResolvedSymbols[libraryId].is_object()
                || !aResolvedSymbols[libraryId].contains( "cacheSource" )
                || !aResolvedSymbols[libraryId]["cacheSource"].is_string()
                || !aResolvedSymbols[libraryId].contains( "properties" )
                || !aResolvedSymbols[libraryId]["properties"].is_object()
                || !aResolvedSymbols[libraryId].contains( "flags" )
                || !aResolvedSymbols[libraryId]["flags"].is_object()
                || !aResolvedSymbols[libraryId].contains( "units" )
                || !aResolvedSymbols[libraryId]["units"].is_object() )
            {
                diagnostic( result, "unresolved_schematic_symbol",
                            "component " + reference + " symbol " + libraryId
                                    + " has no exact resolved project-library source" );
                continue;
            }

            const JSON& resolved = aResolvedSymbols[libraryId];
            const JSON& flags = resolved["flags"];

            if( !flags.contains( "excludeFromSim" ) || !flags["excludeFromSim"].is_boolean()
                || !flags.contains( "inBom" ) || !flags["inBom"].is_boolean()
                || !flags.contains( "onBoard" ) || !flags["onBoard"].is_boolean()
                || !flags.contains( "inPosFiles" ) || !flags["inPosFiles"].is_boolean() )
            {
                diagnostic( result, "invalid_resolved_symbol_flags",
                            "resolved symbol " + libraryId
                                    + " has malformed native inclusion flags" );
                continue;
            }

            for( const JSON& unit : component["units"] )
            {
                const int unitNumber = unit["number"].get<int>();
                const std::string unitKey = std::to_string( unitNumber );
                const std::string sheetId = unit["sheet"].get<std::string>();

                if( !sheets.contains( sheetId ) || !resolved["units"].contains( unitKey )
                    || !resolved["units"][unitKey].is_array() )
                {
                    diagnostic( result, "unresolved_schematic_symbol_unit",
                                "component " + reference + " unit " + unitKey
                                        + " has no resolved sheet or symbol unit" );
                    continue;
                }

                placementsBySheet[sheetId].push_back(
                        { { "component", component }, { "unit", unit },
                          { "resolved", resolved } } );
                const std::string logicalId = reference + "/" + unitKey;
                groupTargets["component/" + logicalId] = {
                    { "sheet", sheetId },
                    { "uuid", stableUuid( project, "schematic_symbol", logicalId ) }
                };
                librariesBySheet[sheetId][libraryId] = resolved;
                const int64_t originX = unit["position"]["xNm"].get<int64_t>();
                const int64_t originY = unit["position"]["yNm"].get<int64_t>();
                const int rotation = unit["rotationDegrees"].get<int>();
                const std::string mirror = unit["mirror"].get<std::string>();

                for( const JSON& pin : resolved["units"][unitKey] )
                {
                    if( !pin.is_object() || !pin.contains( "number" )
                        || !pin["number"].is_string() || !pin.contains( "xNm" )
                        || !pin["xNm"].is_number_integer() || !pin.contains( "yNm" )
                        || !pin["yNm"].is_number_integer()
                        || !pin.contains( "rotationDegrees" )
                        || !pin["rotationDegrees"].is_number_integer() )
                    {
                        diagnostic( result, "invalid_resolved_symbol_pin",
                                    "resolved symbol " + libraryId + " has malformed pin metadata" );
                        continue;
                    }

                    const auto [ pinX, pinY ] = transformPoint(
                            pin["xNm"].get<int64_t>(), pin["yNm"].get<int64_t>(),
                            rotation, mirror );
                    const int pinRotation = pin["rotationDegrees"].get<int>();
                    const int directionX = pinRotation == 0 ? 1 : pinRotation == 180 ? -1 : 0;
                    const int directionY = pinRotation == 90 ? 1 : pinRotation == 270 ? -1 : 0;
                    const auto [ transformedDirectionX, transformedDirectionY ] =
                            transformPoint( directionX, directionY, rotation, mirror );
                    const int outwardX = -static_cast<int>( transformedDirectionX );
                    const int outwardY = -static_cast<int>( transformedDirectionY );
                    const int labelRotation = outwardX > 0 ? 0 : outwardX < 0 ? 180
                                                       : outwardY > 0 ? 90 : 270;
                    const std::string endpoint = reference + "/" + unitKey + "/"
                                                 + pin["number"].get<std::string>();
                    endpointPositions[endpoint].push_back(
                            { sheetId, originX + pinX, originY + pinY, labelRotation } );
                }
            }
        }
    }
    catch( const JSON::exception& error )
    {
        diagnostic( result, "invalid_schematic_ir", error.what() );
    }

    std::map<std::string, std::vector<JSON>> connectivityBySheet;
    std::map<std::string, std::vector<JSON>> drawingsBySheet;
    std::map<std::string, std::vector<JSON>> busAliasesBySheet;

    for( const JSON& alias : sourceBusAliases )
    {
        if( !validBusAliasShape( alias ) || !sheets.contains( alias.value( "sheet", "" ) ) )
        {
            diagnostic( result, "invalid_schematic_ir",
                        "schematic IR contains a malformed bus alias or sheet reference" );
            continue;
        }

        const std::string sheet = alias["sheet"].get<std::string>();
        const std::string name = alias["name"].get<std::string>();
        const std::string logicalId = sheet + "/" + name;
        busAliasesBySheet[sheet].push_back(
                { { "kind", "bus_alias" },
                  { "logicalId", logicalId },
                  { "name", name },
                  { "uuid", stableUuid( project, "schematic_bus_alias", logicalId ) },
                  { "source", busAliasExpression( alias ) } } );
    }

    for( const JSON& drawing : sourceDrawings )
    {
        if( !validDrawingShape( drawing )
            || !sheets.contains( drawing.value( "sheet", "" ) ) )
        {
            diagnostic( result, "invalid_schematic_ir",
                        "schematic IR contains a malformed drawing or sheet reference" );
            continue;
        }

        const std::string kind = drawing["kind"].get<std::string>();
        const std::string id = drawing["id"].get<std::string>();
        const std::string nativeKind = kind == "label"
                                               ? ( drawing["scope"] == "local" ? "label"
                                                                                : "global_label" )
                                       : kind == "directive" ? "netclass_flag"
                                               : kind;
        const std::string uuid = stableUuid( project, "schematic_" + nativeKind, id );
        const std::string source = kind == "junction"
                                           ? junctionExpression( drawing, uuid )
                                   : kind == "label"
                                           ? labelExpression( drawing, nativeKind, uuid )
                                   : kind == "directive"
                                           ? directiveExpression( drawing, uuid )
                                   : kind == "text"
                                           ? textExpression( drawing, uuid )
                                   : kind == "text_box"
                                           ? textBoxExpression( drawing, uuid )
                                   : kind == "image"
                                           ? schematicImageExpression( drawing, uuid )
                                   : kind == "table"
                                           ? schematicTableExpression( drawing, project, uuid )
                                   : kind == "polyline" || kind == "rectangle"
                                                     || kind == "circle" || kind == "arc"
                                                     || kind == "bezier"
                                           ? schematicGraphicExpression( drawing, uuid )
                                   : kind == "rule_area"
                                           ? ruleAreaExpression( drawing, uuid )
                                           : schematicLineExpression( drawing, uuid );
        JSON planned = { { "kind", nativeKind },
                         { "logicalId", id },
                         { "uuid", uuid },
                         { "source", source } };
        const std::string drawingSheet = drawing["sheet"].get<std::string>();
        groupTargets["drawing/" + id] = { { "sheet", drawingSheet }, { "uuid", uuid } };
        drawingsBySheet[drawingSheet].push_back( std::move( planned ) );
    }

    const auto addEndpoint = [&]( const JSON& aEndpoint, const std::string& aNetName,
                                  bool aNoConnect )
    {
        if( !aEndpoint.is_object() || !aEndpoint.contains( "component" )
            || !aEndpoint["component"].is_string() || !aEndpoint.contains( "unit" )
            || !aEndpoint["unit"].is_number_integer() || !aEndpoint.contains( "number" )
            || !aEndpoint["number"].is_string() )
        {
            diagnostic( result, "invalid_schematic_ir", "schematic endpoint IR is malformed" );
            return;
        }

        const std::string reference = aEndpoint["component"].get<std::string>();
        const std::string unit = std::to_string( aEndpoint["unit"].get<int>() );
        const std::string number = aEndpoint["number"].get<std::string>();
        const std::string endpoint = reference + "/" + unit + "/" + number;
        auto positions = endpointPositions.find( endpoint );

        if( positions == endpointPositions.end() || positions->second.empty() )
        {
            diagnostic( result, "unresolved_schematic_pin",
                        "schematic endpoint " + reference + ":" + unit + ":" + number
                                + " does not resolve to a native symbol pin" );
            return;
        }

        for( size_t index = 0; index < positions->second.size(); ++index )
        {
            const PIN_POINT& point = positions->second[index];
            const std::string prefix = aNoConnect ? "no_connect" : "net";
            const std::string logicalId = prefix + "/" + aNetName + "/" + endpoint + "/"
                                          + std::to_string( index );
            const std::string kind = aNoConnect ? "no_connect" : "global_label";
            const std::string uuid = stableUuid( project, "schematic_" + kind, logicalId );
            // Anchor the label exactly on the pin endpoint. An offset anchor needs a stub
            // wire to bridge the gap, and generated stub geometry can cross other pins or
            // stubs and silently merge unrelated nets; a label with no stub cannot touch
            // anything but its own pin.
            const std::string source = aNoConnect
                                               ? noConnectExpression( point.x, point.y, uuid )
                                               : globalLabelExpression( aNetName, point.x,
                                                                        point.y,
                                                                        point.labelRotation,
                                                                        uuid );

            connectivityBySheet[point.sheet].push_back(
                    { { "kind", kind }, { "logicalId", logicalId }, { "uuid", uuid },
                      { "source", source } } );
            const std::string groupKey = aNoConnect
                                                 ? "no_connect/" + endpoint + "/"
                                                           + std::to_string( index + 1 )
                                                 : "net_label/" + aNetName + "/" + endpoint
                                                           + "/" + std::to_string( index + 1 );
            groupTargets[groupKey] = { { "sheet", point.sheet }, { "uuid", uuid } };
        }
    };

    struct AXIS_SEGMENT
    {
        int64_t x1;
        int64_t y1;
        int64_t x2;
        int64_t y2;
    };

    std::map<std::string, std::set<int64_t>> reservedVerticalLanes;
    std::map<std::string, std::set<int64_t>> reservedHorizontalLanes;

    const auto addWiredNet = [&]( const JSON& aNet )
    {
        const std::string netName = aNet["name"].get<std::string>();
        std::vector<PIN_POINT> points;
        std::string sheet;

        for( const JSON& endpointJson : aNet["pins"] )
        {
            const std::string endpoint = endpointJson["component"].get<std::string>() + "/"
                                         + std::to_string( endpointJson["unit"].get<int>() )
                                         + "/" + endpointJson["number"].get<std::string>();
            auto positions = endpointPositions.find( endpoint );

            if( positions == endpointPositions.end() || positions->second.empty() )
            {
                diagnostic( result, "unresolved_schematic_pin",
                            "schematic endpoint " + endpoint
                                    + " does not resolve to a native symbol pin" );
                continue;
            }

            for( const PIN_POINT& point : positions->second )
            {
                if( sheet.empty() )
                    sheet = point.sheet;
                else if( sheet != point.sheet )
                {
                    diagnostic( result, "wired_net_spans_sheets",
                                "net " + netName
                                        + " requests wired presentation across multiple sheets; "
                                          "use labels or split the circuit into local wired nets "
                                          "through hierarchical pins" );
                    return;
                }

                points.push_back( point );
            }
        }

        if( points.size() < 2 || sheet.empty() )
            return;

        int64_t minX = points.front().x;
        int64_t maxX = points.front().x;
        int64_t minY = points.front().y;
        int64_t maxY = points.front().y;
        size_t horizontalPins = 0;
        size_t verticalPins = 0;

        for( const PIN_POINT& point : points )
        {
            minX = std::min( minX, point.x );
            maxX = std::max( maxX, point.x );
            minY = std::min( minY, point.y );
            maxY = std::max( maxY, point.y );

            if( point.labelRotation == 0 || point.labelRotation == 180 )
                ++horizontalPins;
            else
                ++verticalPins;
        }

        const bool verticalTrunk = horizontalPins != verticalPins
                                           ? horizontalPins > verticalPins
                                           : maxX - minX >= maxY - minY;
        constexpr int64_t STUB_LENGTH_NM = 5'080'000;
        constexpr int64_t DETOUR_STEP_NM = 2'540'000;
        constexpr int64_t LABEL_INSET_MAX_NM = 10'160'000;
        constexpr int64_t CONNECTION_GRID_NM = 1'270'000;

        const auto chooseTrunk = [&]( bool aVertical )
        {
            const int64_t minimum = aVertical ? minX : minY;
            const int64_t maximum = aVertical ? maxX : maxY;
            std::vector<int64_t> candidates = { minimum + ( maximum - minimum ) / 2,
                                                minimum, maximum };

            if( minimum >= STUB_LENGTH_NM )
                candidates.push_back( minimum - STUB_LENGTH_NM );

            if( maximum <= 2'000'000'000 - STUB_LENGTH_NM )
                candidates.push_back( maximum + STUB_LENGTH_NM );

            for( const PIN_POINT& point : points )
                candidates.push_back( aVertical ? point.x : point.y );

            std::sort( candidates.begin(), candidates.end() );
            candidates.erase( std::unique( candidates.begin(), candidates.end() ),
                              candidates.end() );
            int64_t best = candidates.front();
            __int128 bestScore = -1;

            for( int64_t candidate : candidates )
            {
                __int128 score = 0;

                for( const PIN_POINT& point : points )
                {
                    const int64_t coordinate = aVertical ? point.x : point.y;
                    score += std::abs( candidate - coordinate );
                    bool opposes = false;

                    if( aVertical && point.labelRotation == 0 )
                        opposes = candidate <= point.x;
                    else if( aVertical && point.labelRotation == 180 )
                        opposes = candidate >= point.x;
                    else if( !aVertical && point.labelRotation == 90 )
                        opposes = candidate <= point.y;
                    else if( !aVertical && point.labelRotation == 270 )
                        opposes = candidate >= point.y;

                    if( opposes )
                        score += static_cast<__int128>( 4'000'000'000LL );
                }

                if( bestScore < 0 || score < bestScore || ( score == bestScore && candidate < best ) )
                {
                    bestScore = score;
                    best = candidate;
                }
            }

            return best;
        };

        const int64_t trunk = chooseTrunk( verticalTrunk );
        std::vector<AXIS_SEGMENT> rawSegments;
        std::vector<int64_t> attachments;

        const auto appendSegment = [&]( int64_t aX1, int64_t aY1,
                                        int64_t aX2, int64_t aY2 )
        {
            if( aX1 == aX2 && aY1 == aY2 )
                return;

            if( aX1 != aX2 && aY1 != aY2 )
            {
                diagnostic( result, "invalid_generated_wire",
                            "internal schematic router generated a diagonal segment for net "
                                    + netName );
                return;
            }

            rawSegments.push_back( { aX1, aY1, aX2, aY2 } );
        };

        if( points.size() == 2 )
        {
            const PIN_POINT& first = points[0];
            const PIN_POINT& second = points[1];

            if( first.x == second.x || first.y == second.y )
            {
                appendSegment( first.x, first.y, second.x, second.y );
            }
            else
            {
                const bool useVerticalLane =
                        ( first.labelRotation == 0 || first.labelRotation == 180 )
                        && ( second.labelRotation == 0 || second.labelRotation == 180 );
                auto& reserved = useVerticalLane ? reservedVerticalLanes[sheet]
                                                 : reservedHorizontalLanes[sheet];
                const int64_t firstCoordinate = useVerticalLane ? first.x : first.y;
                const int64_t secondCoordinate = useVerticalLane ? second.x : second.y;
                const int64_t base = firstCoordinate
                                     + ( secondCoordinate - firstCoordinate ) / 2;
                int64_t lane = base;

                for( size_t attempt = 0; reserved.contains( lane ); ++attempt )
                {
                    const int64_t magnitude = static_cast<int64_t>( attempt / 2 + 1 )
                                              * DETOUR_STEP_NM;
                    const int64_t candidate = base + ( attempt % 2 == 0 ? magnitude
                                                                        : -magnitude );

                    if( candidate >= 0 && candidate <= 2'000'000'000 )
                        lane = candidate;
                }

                reserved.insert( lane );

                if( useVerticalLane )
                {
                    appendSegment( first.x, first.y, lane, first.y );
                    appendSegment( lane, first.y, lane, second.y );
                    appendSegment( lane, second.y, second.x, second.y );
                }
                else
                {
                    appendSegment( first.x, first.y, first.x, lane );
                    appendSegment( first.x, lane, second.x, lane );
                    appendSegment( second.x, lane, second.x, second.y );
                }
            }
        }
        else
        {
            for( size_t index = 0; index < points.size(); ++index )
            {
                const PIN_POINT& point = points[index];

                if( verticalTrunk )
                {
                    int64_t attachY = point.y;

                    if( point.labelRotation == 90 || point.labelRotation == 270 )
                    {
                        const int64_t direction = point.labelRotation == 90 ? 1 : -1;
                        attachY = std::clamp( point.y + direction * STUB_LENGTH_NM,
                                              int64_t( 0 ), int64_t( 2'000'000'000 ) );
                        appendSegment( point.x, point.y, point.x, attachY );
                        appendSegment( point.x, attachY, trunk, attachY );
                    }
                    else
                    {
                        const bool follows = trunk == point.x
                                             || ( point.labelRotation == 0 && trunk > point.x )
                                             || ( point.labelRotation == 180 && trunk < point.x );

                        if( follows )
                        {
                            appendSegment( point.x, point.y, trunk, point.y );
                        }
                        else
                        {
                            const int64_t direction = point.labelRotation == 0 ? 1 : -1;
                            const int64_t stubX = std::clamp(
                                    point.x + direction * STUB_LENGTH_NM,
                                    int64_t( 0 ), int64_t( 2'000'000'000 ) );
                            const int64_t detourDirection = index % 2 == 0 ? 1 : -1;
                            attachY = std::clamp(
                                    point.y + detourDirection
                                                      * static_cast<int64_t>( index + 1 )
                                                      * DETOUR_STEP_NM,
                                    int64_t( 0 ), int64_t( 2'000'000'000 ) );
                            appendSegment( point.x, point.y, stubX, point.y );
                            appendSegment( stubX, point.y, stubX, attachY );
                            appendSegment( stubX, attachY, trunk, attachY );
                        }
                    }

                    attachments.push_back( attachY );
                }
                else
                {
                    int64_t attachX = point.x;

                    if( point.labelRotation == 0 || point.labelRotation == 180 )
                    {
                        const int64_t direction = point.labelRotation == 0 ? 1 : -1;
                        attachX = std::clamp( point.x + direction * STUB_LENGTH_NM,
                                              int64_t( 0 ), int64_t( 2'000'000'000 ) );
                        appendSegment( point.x, point.y, attachX, point.y );
                        appendSegment( attachX, point.y, attachX, trunk );
                    }
                    else
                    {
                        const bool follows = trunk == point.y
                                             || ( point.labelRotation == 90 && trunk > point.y )
                                             || ( point.labelRotation == 270 && trunk < point.y );

                        if( follows )
                        {
                            appendSegment( point.x, point.y, point.x, trunk );
                        }
                        else
                        {
                            const int64_t direction = point.labelRotation == 90 ? 1 : -1;
                            const int64_t stubY = std::clamp(
                                    point.y + direction * STUB_LENGTH_NM,
                                    int64_t( 0 ), int64_t( 2'000'000'000 ) );
                            const int64_t detourDirection = index % 2 == 0 ? 1 : -1;
                            attachX = std::clamp(
                                    point.x + detourDirection
                                                      * static_cast<int64_t>( index + 1 )
                                                      * DETOUR_STEP_NM,
                                    int64_t( 0 ), int64_t( 2'000'000'000 ) );
                            appendSegment( point.x, point.y, point.x, stubY );
                            appendSegment( point.x, stubY, attachX, stubY );
                            appendSegment( attachX, stubY, attachX, trunk );
                        }
                    }

                    attachments.push_back( attachX );
                }
            }

            const auto [ attachmentMin, attachmentMax ] =
                    std::minmax_element( attachments.begin(), attachments.end() );

            if( verticalTrunk )
                appendSegment( trunk, *attachmentMin, trunk, *attachmentMax );
            else
                appendSegment( *attachmentMin, trunk, *attachmentMax, trunk );
        }

        using AXIS_KEY = std::pair<char, int64_t>;
        std::map<AXIS_KEY, std::vector<std::pair<int64_t, int64_t>>> intervals;
        std::set<std::pair<int64_t, int64_t>> protectedNodes;

        for( const AXIS_SEGMENT& segment : rawSegments )
        {
            protectedNodes.emplace( segment.x1, segment.y1 );
            protectedNodes.emplace( segment.x2, segment.y2 );

            if( segment.y1 == segment.y2 )
            {
                intervals[{ 'h', segment.y1 }].push_back(
                        std::minmax( segment.x1, segment.x2 ) );
            }
            else
            {
                intervals[{ 'v', segment.x1 }].push_back(
                        std::minmax( segment.y1, segment.y2 ) );
            }
        }

        std::array<PIN_POINT, 2> nameAnchors = { points.front(), points.back() };

        for( PIN_POINT& anchor : nameAnchors )
        {
            for( const AXIS_SEGMENT& segment : rawSegments )
            {
                int64_t otherX = 0;
                int64_t otherY = 0;

                if( segment.x1 == anchor.x && segment.y1 == anchor.y )
                {
                    otherX = segment.x2;
                    otherY = segment.y2;
                }
                else if( segment.x2 == anchor.x && segment.y2 == anchor.y )
                {
                    otherX = segment.x1;
                    otherY = segment.y1;
                }
                else
                {
                    continue;
                }

                const int64_t length = std::abs( otherX - anchor.x )
                                       + std::abs( otherY - anchor.y );
                const int64_t desiredInset = std::min( LABEL_INSET_MAX_NM, length / 3 );
                const int64_t inset = desiredInset / CONNECTION_GRID_NM
                                      * CONNECTION_GRID_NM;

                if( inset == 0 )
                    break;

                if( otherX != anchor.x )
                    anchor.x += otherX > anchor.x ? inset : -inset;
                else
                    anchor.y += otherY > anchor.y ? inset : -inset;

                protectedNodes.emplace( anchor.x, anchor.y );
                break;
            }
        }

        size_t wireIndex = 0;

        for( auto& [ axis, ranges ] : intervals )
        {
            std::sort( ranges.begin(), ranges.end() );
            std::vector<std::pair<int64_t, int64_t>> merged;

            for( const auto& range : ranges )
            {
                if( merged.empty() || range.first > merged.back().second )
                    merged.push_back( range );
                else
                    merged.back().second = std::max( merged.back().second, range.second );
            }

            for( const auto& range : merged )
            {
                const bool horizontal = axis.first == 'h';
                std::vector<int64_t> cuts = { range.first, range.second };

                for( const auto& [ nodeX, nodeY ] : protectedNodes )
                {
                    const bool onAxis = horizontal ? nodeY == axis.second
                                                   : nodeX == axis.second;
                    const int64_t coordinate = horizontal ? nodeX : nodeY;

                    if( onAxis && coordinate > range.first && coordinate < range.second )
                        cuts.push_back( coordinate );
                }

                std::sort( cuts.begin(), cuts.end() );
                cuts.erase( std::unique( cuts.begin(), cuts.end() ), cuts.end() );

                for( size_t cut = 1; cut < cuts.size(); ++cut )
                {
                    const JSON wire = {
                        { "kind", "wire" },
                        { "from", { { "xNm", horizontal ? cuts[cut - 1] : axis.second },
                                      { "yNm", horizontal ? axis.second : cuts[cut - 1] } } },
                        { "to", { { "xNm", horizontal ? cuts[cut] : axis.second },
                                    { "yNm", horizontal ? axis.second : cuts[cut] } } },
                        { "stroke", { { "widthNm", 0 }, { "lineStyle", "default" } } }
                    };
                    const std::string logicalId = "net_wire/" + netName + "/"
                                                  + std::to_string( wireIndex++ );
                    const std::string uuid =
                            stableUuid( project, "schematic_wire", logicalId );
                    connectivityBySheet[sheet].push_back(
                            { { "kind", "wire" }, { "logicalId", logicalId },
                              { "uuid", uuid },
                              { "source", schematicLineExpression( wire, uuid ) } } );
                    result.counts["generatedWires"] =
                            result.counts["generatedWires"].get<size_t>() + 1;
                }
            }
        }

        if( points.size() > 2 && !attachments.empty() )
        {
            const auto [ attachmentMin, attachmentMax ] =
                    std::minmax_element( attachments.begin(), attachments.end() );

            if( *attachmentMin != *attachmentMax )
            {
                std::set<int64_t> uniqueAttachments( attachments.begin(), attachments.end() );

                for( int64_t attachment : uniqueAttachments )
                {
                    if( attachment == *attachmentMin || attachment == *attachmentMax )
                        continue;

                    const int64_t x = verticalTrunk ? trunk : attachment;
                    const int64_t y = verticalTrunk ? attachment : trunk;
                    const std::string logicalId = "net_junction/" + netName + "/"
                                                  + std::to_string( x ) + "/"
                                                  + std::to_string( y );
                    const std::string uuid =
                            stableUuid( project, "schematic_junction", logicalId );
                    const JSON junction = {
                        { "position", { { "xNm", x }, { "yNm", y } } },
                        { "diameterNm", 0 }, { "color", nullptr }
                    };
                    connectivityBySheet[sheet].push_back(
                            { { "kind", "junction" }, { "logicalId", logicalId },
                              { "uuid", uuid },
                              { "source", junctionExpression( junction, uuid ) } } );
                    result.counts["generatedJunctions"] =
                            result.counts["generatedJunctions"].get<size_t>() + 1;
                }
            }
        }

        for( size_t anchorNumber = 0; anchorNumber < nameAnchors.size(); ++anchorNumber )
        {
            const PIN_POINT& nameAnchor = nameAnchors[anchorNumber];
            const std::string labelLogicalId = "net_name/" + netName + "/"
                                               + std::to_string( anchorNumber );
            const std::string labelUuid =
                    stableUuid( project, "schematic_global_label", labelLogicalId );
            connectivityBySheet[sheet].push_back(
                    { { "kind", "global_label" }, { "logicalId", labelLogicalId },
                      { "uuid", labelUuid },
                      { "source", globalLabelExpression(
                                          netName, nameAnchor.x, nameAnchor.y,
                                          nameAnchor.labelRotation, labelUuid ) } } );
        }
    };

    try
    {
        for( const JSON& net : sourceNets )
        {
            if( !net.is_object() || !net.contains( "name" ) || !net["name"].is_string()
                || !net.contains( "pins" ) || !net["pins"].is_array()
                || !net.contains( "presentation" ) || !net["presentation"].is_string()
                || ( net["presentation"] != "wired" && net["presentation"] != "labels" ) )
            {
                diagnostic( result, "invalid_schematic_ir", "schematic net IR is malformed" );
                continue;
            }

            const std::string name = net["name"].get<std::string>();

            if( net["presentation"] == "wired" )
            {
                addWiredNet( net );
            }
            else
            {
                for( const JSON& endpoint : net["pins"] )
                    addEndpoint( endpoint, name, false );
            }
        }

        for( const JSON& endpoint : sourceNoConnects )
            addEndpoint( endpoint, "", true );
    }
    catch( const JSON::exception& error )
    {
        diagnostic( result, "invalid_schematic_ir", error.what() );
    }

    if( !result.diagnostics.empty() )
        return result;

    for( const std::string& id : orderedIds )
    {
        const JSON& sheet = sheets.at( id );

        if( id != rootId )
        {
            for( const JSON& pin : sheet["pins"] )
            {
                const std::string logicalId = id + "/" + pin["name"].get<std::string>();
                groupTargets["hierarchical_label/" + logicalId] = {
                    { "sheet", id },
                    { "uuid", stableUuid( project, "schematic_hier_label", logicalId ) }
                };
            }
        }

        for( const std::string& childId : children[id] )
        {
            groupTargets["sheet/" + childId] = {
                { "sheet", id },
                { "uuid", stableUuid( project, "schematic_sheet", childId ) }
            };
        }
    }

    std::map<std::string, std::vector<JSON>> groupsBySheet;
    std::set<std::string> groupIds;

    for( const JSON& group : sourceGroups )
    {
        if( !validGroupShape( group ) || !sheets.contains( group.value( "sheet", "" ) )
            || !groupIds.emplace( group.value( "id", "" ) ).second )
        {
            diagnostic( result, "invalid_schematic_ir",
                        "schematic IR contains a malformed or duplicate group" );
            continue;
        }

        const std::string id = group["id"].get<std::string>();
        const std::string sheet = group["sheet"].get<std::string>();
        groupTargets["group/" + id] = {
            { "sheet", sheet },
            { "uuid", stableUuid( project, "schematic_group", id ) }
        };
    }

    std::set<std::string> groupedTargets;
    std::map<std::string, std::vector<std::string>> nestedGroups;

    for( const JSON& group : sourceGroups )
    {
        if( !validGroupShape( group ) || !groupIds.contains( group.value( "id", "" ) ) )
            continue;

        const std::string id = group["id"].get<std::string>();
        const std::string sheet = group["sheet"].get<std::string>();
        std::vector<std::string> memberUuids;

        for( const JSON& member : group["members"] )
        {
            const std::string key = groupMemberKey( member );

            if( member["kind"] == "group" )
                nestedGroups[id].push_back( member["id"].get<std::string>() );

            auto target = groupTargets.find( key );

            if( target == groupTargets.end() )
            {
                diagnostic( result, "unresolved_schematic_group_member",
                            "group " + id + " member " + key
                                    + " does not resolve to a generated native item" );
                continue;
            }

            if( target->second.value( "sheet", "" ) != sheet )
            {
                diagnostic( result, "mismatched_schematic_group_member_sheet",
                            "group " + id + " member " + key
                                    + " is on a different native schematic screen" );
                continue;
            }

            if( !groupedTargets.emplace( key ).second )
            {
                diagnostic( result, "multiply_grouped_schematic_item",
                            "native schematic item " + key
                                    + " is a direct member of more than one group" );
                continue;
            }

            memberUuids.push_back( target->second["uuid"].get<std::string>() );
        }

        if( memberUuids.size() != group["members"].size() )
            continue;

        const std::string uuid = stableUuid( project, "schematic_group", id );
        groupsBySheet[sheet].push_back(
                { { "kind", "group" },
                  { "logicalId", id },
                  { "uuid", uuid },
                  { "source", schematicGroupExpression( group, uuid, memberUuids ) } } );
    }

    std::map<std::string, int> groupVisitState;
    std::function<void( const std::string& )> visitGroup = [&]( const std::string& aId )
    {
        if( groupVisitState[aId] == 2 )
            return;

        if( groupVisitState[aId] == 1 )
        {
            diagnostic( result, "recursive_schematic_group",
                        "schematic group IR contains a nesting cycle at " + aId );
            return;
        }

        groupVisitState[aId] = 1;

        for( const std::string& child : nestedGroups[aId] )
        {
            if( groupIds.contains( child ) )
                visitGroup( child );
        }

        groupVisitState[aId] = 2;
    };

    for( const std::string& id : groupIds )
        visitGroup( id );

    if( !result.diagnostics.empty() )
        return result;

    JSON files = JSON::array();
    JSON managedItems = JSON::array();

    try
    {
        for( const std::string& id : orderedIds )
        {
            const JSON& sheet = sheets.at( id );
            const std::string file = sheet["file"].get<std::string>();
            const std::string screenUuid = screenUuids.at( file );
            JSON items = JSON::array();
            JSON libSymbols = JSON::array();
            JSON busAliases = JSON::array();

            for( JSON alias : busAliasesBySheet[id] )
            {
                alias["file"] = file;
                busAliases.push_back( alias );
                JSON ownership = std::move( alias );
                ownership.erase( "source" );
                managedItems.push_back( std::move( ownership ) );
            }

            for( const auto& [ libraryId, resolved ] : librariesBySheet[id] )
            {
                const std::string ownershipUuid =
                        stableUuid( project, "schematic_lib_symbol", id + "/" + libraryId );
                libSymbols.push_back( { { "kind", "lib_symbol" },
                                        { "file", file },
                                        { "logicalId", id + "/" + libraryId },
                                        { "libraryId", libraryId },
                                        { "uuid", ownershipUuid },
                                        { "source", resolved["cacheSource"] } } );
                managedItems.push_back( { { "file", file },
                                          { "kind", "lib_symbol" },
                                          { "logicalId", id + "/" + libraryId },
                                          { "libraryId", libraryId },
                                          { "uuid", ownershipUuid } } );
            }

            for( const JSON& placement : placementsBySheet[id] )
            {
                const JSON& component = placement["component"];
                const JSON& unit = placement["unit"];
                const std::string reference = component["reference"].get<std::string>();
                const std::string logicalId = reference + "/"
                                              + std::to_string( unit["number"].get<int>() );
                const std::string uuid = stableUuid( project, "schematic_symbol", logicalId );
                const std::string source = componentExpression(
                        component, unit, placement["resolved"], project, instancePaths.at( id ) );
                items.push_back( { { "kind", "symbol" },
                                   { "file", file },
                                   { "logicalId", logicalId },
                                   { "uuid", uuid },
                                   { "source", source } } );
                managedItems.push_back( { { "file", file },
                                          { "kind", "symbol" },
                                          { "logicalId", logicalId },
                                          { "uuid", uuid } } );
            }

            for( JSON connectivity : connectivityBySheet[id] )
            {
                connectivity["file"] = file;
                items.push_back( connectivity );
                JSON ownership = connectivity;
                ownership.erase( "source" );
                managedItems.push_back( std::move( ownership ) );
            }

            for( JSON drawing : drawingsBySheet[id] )
            {
                drawing["file"] = file;
                items.push_back( drawing );
                JSON ownership = std::move( drawing );
                ownership.erase( "source" );
                managedItems.push_back( std::move( ownership ) );
            }

            if( id != rootId )
            {
                std::vector<JSON> pins = sheet["pins"].get<std::vector<JSON>>();
                std::sort( pins.begin(), pins.end(), []( const JSON& aLeft, const JSON& aRight )
                           {
                               return aLeft.at( "name" ).get<std::string>()
                                      < aRight.at( "name" ).get<std::string>();
                           } );

                for( size_t pinIndex = 0; pinIndex < pins.size(); ++pinIndex )
                {
                    const JSON& pin = pins[pinIndex];
                    const std::string logicalId = id + "/" + pin["name"].get<std::string>();
                    const std::string uuid =
                            stableUuid( project, "schematic_hier_label", logicalId );
                    const std::string source =
                            hierarchicalLabel( sheet, pin, project, pinIndex );
                    items.push_back( { { "kind", "hierarchical_label" },
                                       { "file", file },
                                       { "logicalId", logicalId },
                                       { "uuid", uuid },
                                       { "source", source } } );
                    managedItems.push_back( { { "file", file },
                                              { "kind", "hierarchical_label" },
                                              { "logicalId", logicalId },
                                              { "uuid", uuid } } );
                }
            }

            for( const std::string& childId : children[id] )
            {
                const JSON& child = sheets.at( childId );
                const std::string uuid = stableUuid( project, "schematic_sheet", childId );
                const std::string source =
                        sheetExpression( child, project, instancePaths.at( id ),
                                         pages.at( childId ) );
                items.push_back( { { "kind", "sheet" },
                                   { "file", file },
                                   { "logicalId", childId },
                                   { "uuid", uuid },
                                   { "source", source } } );
                managedItems.push_back( { { "file", file },
                                          { "kind", "sheet" },
                                          { "logicalId", childId },
                                          { "uuid", uuid } } );
            }

            for( JSON group : groupsBySheet[id] )
            {
                group["file"] = file;
                items.push_back( group );
                JSON ownership = std::move( group );
                ownership.erase( "source" );
                managedItems.push_back( std::move( ownership ) );
            }

            std::ostringstream document;
            document << "(kicad_sch\n"
                     << "  (version " << SCHEMATIC_FILE_VERSION << ")\n"
                     << "  (generator \"eeschema\")\n"
                     << "  (generator_version \"10.0\")\n"
                     << "  (uuid " << quoted( screenUuid ) << ")\n"
                     << "  (paper \"A4\")\n";

            if( id == rootId && ownsTitleBlock )
            {
                document << "  "
                         << titleBlockExpression( aCompilerIr["project"]["titleBlock"] )
                         << "\n";
            }
            else if( id == rootId )
            {
                document << "  (title_block\n"
                         << "    (title " << quoted( sheet["title"].get<std::string>() ) << ")\n"
                         << "  )\n";
            }

            document << "  (lib_symbols\n";

            for( const JSON& libSymbol : libSymbols )
                document << "    " << libSymbol["source"].get<std::string>() << "\n";

            document << "  )\n";

            for( const JSON& alias : busAliases )
                document << "  " << alias["source"].get<std::string>() << "\n";

            for( const JSON& item : items )
                document << "  " << item["source"].get<std::string>() << "\n";

            if( id == rootId )
            {
                document << "  (sheet_instances\n"
                         << "    (path \"/\" (page \"1\"))\n"
                         << "  )\n"
                         << "  (embedded_fonts no)\n";
            }

            document << ")\n";
            std::string parseError;

            if( !LOSSLESS_SEXPR_DOCUMENT::Parse( document.str(), &parseError ) )
            {
                diagnostic( result, "invalid_generated_schematic",
                            "generated " + file + " is not a valid s-expression: " + parseError );
                return result;
            }

            files.push_back( { { "path", file },
                               { "sheetId", id },
                               { "screenUuid", screenUuid },
                               { "page", pages.at( id ) },
                               { "root", id == rootId },
                               { "title", sheet["title"] },
                               { "rootTitleBlockOwned", id == rootId && ownsTitleBlock },
                               { "rootTitleBlockSource",
                                 id == rootId && ownsTitleBlock
                                         ? JSON( titleBlockExpression(
                                                   aCompilerIr["project"]["titleBlock"] ) )
                                         : JSON( nullptr ) },
                               { "rootTitleSource",
                                 id == rootId
                                         ? JSON( "(title "
                                                 + quoted( sheet["title"].get<std::string>() )
                                                 + ")" )
                                         : JSON( nullptr ) },
                               { "rootInstancesSource",
                                 id == rootId
                                         ? JSON( "(sheet_instances\n"
                                                 "    (path \"/\" (page \"1\"))\n"
                                                 "  )" )
                                         : JSON( nullptr ) },
                               { "libSymbols", std::move( libSymbols ) },
                               { "busAliases", std::move( busAliases ) },
                               { "items", std::move( items ) },
                               { "newDocumentSource", document.str() } } );
            result.counts["pins"] = result.counts["pins"].get<size_t>()
                                    + sheet["pins"].size();
        }
    }
    catch( const JSON::exception& error )
    {
        diagnostic( result, "invalid_schematic_ir", error.what() );
        return result;
    }

    result.operations.push_back( { { "action", "reconcile_schematic_hierarchy" },
                                   { "project", project },
                                   { "rootFile", sheets.at( rootId )["file"] },
                                   { "files", std::move( files ) },
                                   { "managedItems", std::move( managedItems ) } } );
    result.counts["files"] = orderedIds.size();
    result.counts["sheets"] = orderedIds.size();
    result.counts["components"] = sourceComponents.size();
    result.counts["netEndpoints"] = 0;

    for( const JSON& net : sourceNets )
        result.counts["netEndpoints"] = result.counts["netEndpoints"].get<size_t>()
                                        + net.value( "pins", JSON::array() ).size();

    result.counts["noConnects"] = sourceNoConnects.size();
    result.counts["drawings"] = sourceDrawings.size();
    result.counts["busAliases"] = sourceBusAliases.size();
    result.counts["groups"] = sourceGroups.size();

    for( const auto& [ sheet, symbols ] : librariesBySheet )
        result.counts["librarySymbols"] = result.counts["librarySymbols"].get<size_t>()
                                          + symbols.size();

    result.counts["managedItems"] = result.operations[0]["managedItems"].size();
    result.fullyLowered = true;
    return result;
}

} // namespace KICHAD
