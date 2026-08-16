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

#include "design_script_footprint_compiler.h"
#include "kichad_from_chars.h"

#include "design_script_footprint_component_classes_compiler.h"
#include "design_script_footprint_graphic_compiler.h"
#include "design_script_footprint_group_compiler.h"
#include "design_script_footprint_pad_compiler.h"
#include "design_script_footprint_property_compiler.h"
#include "design_script_footprint_settings_compiler.h"
#include "design_script_footprint_text_compiler.h"
#include "design_script_footprint_variant_compiler.h"
#include "design_script_footprint_zone_compiler.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_COMPILER::RESULT;
using COMPONENT_CLASSES_COMPILER =
        KICHAD::DESIGN_SCRIPT_FOOTPRINT_COMPONENT_CLASSES_COMPILER;
using GRAPHIC_COMPILER = KICHAD::DESIGN_SCRIPT_FOOTPRINT_GRAPHIC_COMPILER;
using GROUP_COMPILER = KICHAD::DESIGN_SCRIPT_FOOTPRINT_GROUP_COMPILER;
using PAD_COMPILER = KICHAD::DESIGN_SCRIPT_FOOTPRINT_PAD_COMPILER;
using PROPERTY_COMPILER = KICHAD::DESIGN_SCRIPT_FOOTPRINT_PROPERTY_COMPILER;
using SETTINGS_COMPILER = KICHAD::DESIGN_SCRIPT_FOOTPRINT_SETTINGS_COMPILER;
using TEXT_COMPILER = KICHAD::DESIGN_SCRIPT_FOOTPRINT_TEXT_COMPILER;
using VARIANT_COMPILER = KICHAD::DESIGN_SCRIPT_FOOTPRINT_VARIANT_COMPILER;
using ZONE_COMPILER = KICHAD::DESIGN_SCRIPT_FOOTPRINT_ZONE_COMPILER;

constexpr size_t MAX_FOOTPRINT_PADS = 4096;
constexpr size_t MAX_FOOTPRINT_GRAPHICS = 8192;
constexpr size_t MAX_FOOTPRINT_TEXT_ITEMS = 4096;
constexpr size_t MAX_FOOTPRINT_MODELS = 64;
constexpr size_t MAX_FOOTPRINT_ZONES = 512;
constexpr size_t MAX_FOOTPRINT_GROUPS = 1024;
constexpr size_t MAX_FOOTPRINT_VARIANTS = 256;
constexpr size_t MAX_FOOTPRINT_PROPERTIES = 1024;
constexpr int64_t MAX_MODEL_OFFSET_NM = 2'000'000'000LL;


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


bool libraryId( const std::string& aValue, std::string& aLibrary, std::string& aName )
{
    const size_t separator = aValue.find( ':' );

    if( separator == std::string::npos || separator == 0 || separator + 1 >= aValue.size()
        || aValue.find( ':', separator + 1 ) != std::string::npos )
    {
        return false;
    }

    aLibrary = aValue.substr( 0, separator );
    aName = aValue.substr( separator + 1 );
    return identifier( aLibrary ) && identifier( aName )
           && aLibrary != "." && aLibrary != ".." && aName != "." && aName != "..";
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

    if( !std::isfinite( rounded ) || rounded < -MAX_MODEL_OFFSET_NM
        || rounded > MAX_MODEL_OFFSET_NM )
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


bool xyz( const DOCUMENT& aDocument, size_t aNode, bool aAngles, bool aScale, JSON& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );

    if( node.children.size() != 4 )
        return false;

    static const char* keys[] = { "x", "y", "z" };

    for( size_t index = 0; index < 3; ++index )
    {
        std::string text;
        int64_t parsed = 0;

        if( !scalar( aDocument, node.children[index + 1], text )
            || ( aAngles && !angle( text, parsed ) )
            || ( aScale && !decimalPpm( text, 1'000, 1'000'000'000, parsed ) )
            || ( !aAngles && !aScale && !distance( text, parsed ) ) )
        {
            return false;
        }

        aValue[keys[index]] = parsed;
    }

    return true;
}


JSON compileAttributes( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    JSON attributes = {
        { "smd", false }, { "throughHole", false }, { "boardOnly", false },
        { "excludeFromPosition", false }, { "excludeFromBom", false },
        { "allowMissingCourtyard", false }, { "dnp", false },
        { "allowSoldermaskBridges", false }
    };
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::set<std::string> fields;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        std::string value;
        bool parsed = false;
        static const std::set<std::string> supported = {
            "smd", "through_hole", "board_only", "exclude_from_position",
            "exclude_from_bom", "allow_missing_courtyard", "dnp",
            "allow_soldermask_bridges"
        };

        if( !supported.contains( head ) || !fields.emplace( head ).second
            || !oneValue( aDocument, child, value ) || !boolean( value, parsed ) )
        {
            diagnostic( aResult, "invalid_authored_footprint_attributes",
                        "footprint attributes require unique supported true/false fields" );
            continue;
        }

        const std::string key = head == "through_hole" ? "throughHole"
                              : head == "board_only" ? "boardOnly"
                              : head == "exclude_from_position" ? "excludeFromPosition"
                              : head == "exclude_from_bom" ? "excludeFromBom"
                              : head == "allow_missing_courtyard" ? "allowMissingCourtyard"
                              : head == "allow_soldermask_bridges" ? "allowSoldermaskBridges"
                                                                    : head;
        attributes[key] = parsed;
    }

    return attributes;
}


JSON compilePadGroup( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                      const std::string& aKind )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    JSON numbers = JSON::array();
    std::set<std::string> unique;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        std::string number;

        if( !scalar( aDocument, node.children[index], number ) || number.empty()
            || number.size() > 64 || number.find( '\0' ) != std::string::npos
            || !unique.emplace( number ).second )
        {
            diagnostic( aResult, "invalid_authored_footprint_" + aKind,
                        aKind + " requires at least two unique bounded pad numbers" );
            return JSON::array();
        }

        numbers.push_back( number );
    }

    if( numbers.size() < 2 )
        diagnostic( aResult, "invalid_authored_footprint_" + aKind,
                    aKind + " requires at least two unique bounded pad numbers" );

    return numbers;
}


JSON compileModel( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string path;
    const auto trustedRoot = []( const std::string& aPath )
    {
        return aPath.starts_with( "${KIPRJMOD}/" )
               || aPath.starts_with( "${KICAD10_3DMODEL_DIR}/" );
    };

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], path )
        || !trustedRoot( path ) || path.size() > 4096
        || path.find( '\0' ) != std::string::npos || path.find_first_of( "\r\n" ) != std::string::npos
        || path.find( '\\' ) != std::string::npos || path.find( "/../" ) != std::string::npos
        || !( path.ends_with( ".step" ) || path.ends_with( ".stp" )
              || path.ends_with( ".wrl" ) ) )
    {
        diagnostic( aResult, "invalid_authored_footprint_model_path",
                    "footprint model requires a confined project or KiCad 10 stock-library "
                    "STEP, STP, or WRL path" );
        return JSON::object();
    }

    JSON model = {
        { "path", path }, { "visible", true }, { "opacityPpm", 1'000'000 },
        { "offsetNm", { { "x", 0 }, { "y", 0 }, { "z", 0 } } },
        { "scalePpm", { { "x", 1'000'000 }, { "y", 1'000'000 }, { "z", 1'000'000 } } },
        { "rotationTenths", { { "x", 0 }, { "y", 0 }, { "z", 0 } } }
    };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_authored_footprint_model_field",
                        "footprint model field " + head + " occurs more than once" );
            continue;
        }

        if( head == "offset" || head == "scale" || head == "rotation" )
        {
            JSON parsed = JSON::object();

            if( !xyz( aDocument, child, head == "rotation", head == "scale", parsed ) )
                diagnostic( aResult, "invalid_authored_footprint_model_transform",
                            "model " + head + " requires three bounded values" );
            else
                model[head == "offset" ? "offsetNm"
                      : head == "scale" ? "scalePpm"
                                          : "rotationTenths"] = std::move( parsed );

            continue;
        }

        std::string value;

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( aResult, "invalid_authored_footprint_model_field",
                        "footprint model field " + head + " requires one value" );
            continue;
        }

        if( head == "visible" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( aResult, "invalid_authored_footprint_model_visibility",
                            "model visible must be true or false" );
            else
                model["visible"] = parsed;
        }
        else if( head == "opacity" )
        {
            int64_t parsed = 0;

            if( !decimalPpm( value, 0, 1'000'000, parsed ) )
                diagnostic( aResult, "invalid_authored_footprint_model_opacity",
                            "model opacity must be from zero through one" );
            else
                model["opacityPpm"] = parsed;
        }
        else
        {
            diagnostic( aResult, "unknown_authored_footprint_model_field",
                        "model supports visible, opacity, offset, scale, and rotation" );
        }
    }

    return model;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_FOOTPRINT_COMPILER::RESULT DESIGN_SCRIPT_FOOTPRINT_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;
    std::string library;
    std::string name;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !libraryId( id, library, name ) )
    {
        diagnostic( result, "invalid_authored_footprint_id",
                    "footprint requires one bounded LIBRARY:NAME identifier" );
        return result;
    }

    result.footprint = {
        { "id", id }, { "library", library }, { "name", name },
        { "reference", "REF**" }, { "value", name }, { "datasheet", "" },
        { "description", "" }, { "keywords", "" },
        { "attributes",
          { { "smd", false }, { "throughHole", false }, { "boardOnly", false },
            { "excludeFromPosition", false }, { "excludeFromBom", false },
            { "allowMissingCourtyard", false }, { "dnp", false },
            { "allowSoldermaskBridges", false } } },
        { "duplicatePadNumbersAreJumpers", false }, { "jumperGroups", JSON::array() },
        { "netTieGroups", JSON::array() }, { "pads", JSON::array() },
        { "graphics", JSON::array() }, { "texts", JSON::array() },
        { "textBoxes", JSON::array() }, { "zones", JSON::array() },
        { "groups", JSON::array() }, { "variants", JSON::array() },
        { "properties", JSON::array() }, { "componentClasses", JSON::array() },
        { "rules",
          { { "clearanceNm", nullptr }, { "solderMaskMarginNm", nullptr },
            { "solderPasteMarginNm", nullptr }, { "solderPasteMarginPpm", nullptr },
            { "zoneConnection", "inherit" } } },
        { "stackup", nullptr }, { "privateLayers", JSON::array() },
        { "models", JSON::array() }
    };
    std::set<std::string> fields;
    std::set<std::string> padIds;
    std::set<std::string> objectIds;
    std::set<std::string> graphicIds;
    std::set<std::string> textIds;
    std::set<std::string> textBoxIds;
    std::set<std::string> zoneIds;
    std::set<std::string> groupIds;
    std::set<std::string> variantIds;
    std::set<std::string> propertyIds;
    std::set<std::string> propertyNames = {
        "reference", "value", "datasheet", "description"
    };
    std::set<std::string> settingsForms;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "pad" )
        {
            if( result.footprint["pads"].size() >= MAX_FOOTPRINT_PADS )
            {
                diagnostic( result, "too_many_authored_footprint_pads",
                            "a footprint may contain at most 4096 pads" );
                continue;
            }

            PAD_COMPILER::RESULT pad = PAD_COMPILER::Compile( aDocument, child );

            for( JSON& entry : pad.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            const std::string padId = pad.pad.value( "id", "" );

            if( !padId.empty() && !padIds.emplace( padId ).second )
                diagnostic( result, "duplicate_authored_footprint_pad_id",
                            "footprint pad logical ID " + padId + " occurs more than once" );

            result.footprint["pads"].push_back( std::move( pad.pad ) );
            continue;
        }

        if( GRAPHIC_COMPILER::IsGraphicHead( head ) )
        {
            if( result.footprint["graphics"].size() >= MAX_FOOTPRINT_GRAPHICS )
            {
                diagnostic( result, "too_many_authored_footprint_graphics",
                            "a footprint may contain at most 8192 graphic primitives" );
                continue;
            }

            GRAPHIC_COMPILER::RESULT graphic = GRAPHIC_COMPILER::Compile( aDocument, child );

            for( JSON& entry : graphic.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            const std::string objectId = graphic.graphic.value( "id", "" );

            if( !graphic.graphic.value( "net", "" ).empty() )
                diagnostic( result, "invalid_authored_footprint_graphic_net",
                            "library footprint graphics cannot own a board net" );

            if( !objectId.empty() && !objectIds.emplace( objectId ).second )
                diagnostic( result, "duplicate_authored_footprint_object_id",
                            "footprint graphic/text logical ID " + objectId
                                    + " occurs more than once" );

            if( !objectId.empty() )
                graphicIds.emplace( objectId );

            result.footprint["graphics"].push_back( std::move( graphic.graphic ) );
            continue;
        }

        if( head == "text" || head == "text_box" )
        {
            const size_t textCount = result.footprint["texts"].size()
                                     + result.footprint["textBoxes"].size();

            if( textCount >= MAX_FOOTPRINT_TEXT_ITEMS )
            {
                diagnostic( result, "too_many_authored_footprint_text_items",
                            "a footprint may contain at most 4096 text and text-box items" );
                continue;
            }

            TEXT_COMPILER::RESULT text = TEXT_COMPILER::Compile( aDocument, child );

            for( JSON& entry : text.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            const std::string objectId = text.text.value( "id", "" );

            if( !objectId.empty() && !objectIds.emplace( objectId ).second )
                diagnostic( result, "duplicate_authored_footprint_object_id",
                            "footprint graphic/text logical ID " + objectId
                                    + " occurs more than once" );

            if( !objectId.empty() )
                ( head == "text" ? textIds : textBoxIds ).emplace( objectId );

            result.footprint[head == "text" ? "texts" : "textBoxes"]
                    .push_back( std::move( text.text ) );
            continue;
        }

        if( head == "property" )
        {
            if( result.footprint["properties"].size() >= MAX_FOOTPRINT_PROPERTIES )
            {
                diagnostic( result, "too_many_authored_footprint_properties",
                            "a footprint may contain at most 1024 custom properties" );
                continue;
            }

            PROPERTY_COMPILER::RESULT property = PROPERTY_COMPILER::Compile( aDocument, child );

            for( JSON& entry : property.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            const std::string propertyId = property.property.value( "id", "" );
            const std::string propertyName = property.property.value( "name", "" );
            std::string foldedName = propertyName;
            std::transform( foldedName.begin(), foldedName.end(), foldedName.begin(),
                            []( unsigned char aCharacter )
                            {
                                return static_cast<char>( std::tolower( aCharacter ) );
                            } );

            if( !propertyId.empty() && !propertyIds.emplace( propertyId ).second )
                diagnostic( result, "duplicate_authored_footprint_property_id",
                            "footprint property logical ID " + propertyId
                                    + " occurs more than once" );

            if( !propertyName.empty() && !propertyNames.emplace( foldedName ).second )
                diagnostic( result, "duplicate_authored_footprint_property_name",
                            "footprint property name " + propertyName
                                    + " collides case-insensitively with another field" );

            result.footprint["properties"].push_back( std::move( property.property ) );
            continue;
        }

        if( head == "model" )
        {
            if( result.footprint["models"].size() >= MAX_FOOTPRINT_MODELS )
                diagnostic( result, "too_many_authored_footprint_models",
                            "a footprint may reference at most 64 3D models" );
            else
                result.footprint["models"].push_back(
                        compileModel( aDocument, child, result ) );

            continue;
        }

        if( head == "zone" )
        {
            if( result.footprint["zones"].size() >= MAX_FOOTPRINT_ZONES )
            {
                diagnostic( result, "too_many_authored_footprint_zones",
                            "a footprint may contain at most 512 zones" );
                continue;
            }

            ZONE_COMPILER::RESULT zone = ZONE_COMPILER::Compile( aDocument, child );

            for( JSON& entry : zone.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            const std::string zoneId = zone.zone.value( "id", "" );

            if( !zoneId.empty() && !zoneIds.emplace( zoneId ).second )
                diagnostic( result, "duplicate_authored_footprint_zone_id",
                            "footprint zone logical ID " + zoneId + " occurs more than once" );

            result.footprint["zones"].push_back( std::move( zone.zone ) );
            continue;
        }

        if( head == "group" )
        {
            if( result.footprint["groups"].size() >= MAX_FOOTPRINT_GROUPS )
            {
                diagnostic( result, "too_many_authored_footprint_groups",
                            "a footprint may contain at most 1024 groups" );
                continue;
            }

            GROUP_COMPILER::RESULT group = GROUP_COMPILER::Compile( aDocument, child );

            for( JSON& entry : group.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            const std::string groupId = group.group.value( "id", "" );

            if( !groupId.empty() && !groupIds.emplace( groupId ).second )
                diagnostic( result, "duplicate_authored_footprint_group_id",
                            "footprint group logical ID " + groupId + " occurs more than once" );

            result.footprint["groups"].push_back( std::move( group.group ) );
            continue;
        }

        if( head == "variant" )
        {
            if( result.footprint["variants"].size() >= MAX_FOOTPRINT_VARIANTS )
            {
                diagnostic( result, "too_many_authored_footprint_variants",
                            "a footprint may contain at most 256 variants" );
                continue;
            }

            VARIANT_COMPILER::RESULT variant = VARIANT_COMPILER::Compile( aDocument, child );

            for( JSON& entry : variant.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            const std::string variantId = variant.variant.value( "id", "" );
            std::string folded = variantId;
            std::transform( folded.begin(), folded.end(), folded.begin(), []( unsigned char character )
            {
                return static_cast<char>( std::tolower( character ) );
            } );

            if( !variantId.empty() && !variantIds.emplace( folded ).second )
                diagnostic( result, "duplicate_authored_footprint_variant_id",
                            "footprint variant " + variantId + " occurs more than once" );

            result.footprint["variants"].push_back( std::move( variant.variant ) );
            continue;
        }

        if( head == "rules" || head == "stackup" || head == "private_layers" )
        {
            if( !settingsForms.emplace( head ).second )
            {
                diagnostic( result, "duplicate_authored_footprint_settings",
                            "footprint settings form " + head + " occurs more than once" );
                continue;
            }

            SETTINGS_COMPILER::RESULT settings =
                    head == "rules" ? SETTINGS_COMPILER::CompileRules( aDocument, child )
                    : head == "stackup" ? SETTINGS_COMPILER::CompileStackup( aDocument, child )
                                          : SETTINGS_COMPILER::CompilePrivateLayers( aDocument, child );

            for( JSON& entry : settings.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            result.footprint[head == "rules" ? "rules"
                             : head == "stackup" ? "stackup" : "privateLayers"] =
                    std::move( settings.value );
            continue;
        }

        if( head == "component_classes" )
        {
            if( !settingsForms.emplace( head ).second )
            {
                diagnostic( result, "duplicate_authored_footprint_component_classes",
                            "footprint component_classes occurs more than once" );
                continue;
            }

            COMPONENT_CLASSES_COMPILER::RESULT classes =
                    COMPONENT_CLASSES_COMPILER::Compile( aDocument, child );

            for( JSON& entry : classes.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            result.footprint["componentClasses"] = std::move( classes.classes );
            continue;
        }

        if( head == "jumper_group" || head == "net_tie_group" )
        {
            JSON group = compilePadGroup( aDocument, child, result, head );

            if( group.size() >= 2 )
                result.footprint[head == "jumper_group" ? "jumperGroups"
                                                          : "netTieGroups"]
                        .push_back( std::move( group ) );

            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_footprint_field",
                        "footprint " + id + " field " + head + " occurs more than once" );
            continue;
        }

        if( head == "attributes" )
        {
            result.footprint["attributes"] = compileAttributes( aDocument, child, result );
            continue;
        }

        std::string value;

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( result, "invalid_authored_footprint_field",
                        "footprint " + id + " field " + head + " requires one value" );
            continue;
        }

        if( head == "reference" || head == "value" || head == "datasheet"
            || head == "description" || head == "keywords" )
        {
            const size_t maximum = head == "reference" ? 64 : 4096;

            if( value.size() > maximum || value.find( '\0' ) != std::string::npos )
                diagnostic( result, "invalid_authored_footprint_text_field",
                            "footprint " + head + " exceeds its bounded UTF-8 size" );
            else
                result.footprint[head] = value;
        }
        else if( head == "duplicate_pad_numbers_are_jumpers" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( result, "invalid_authored_footprint_jumper_flag",
                            "duplicate_pad_numbers_are_jumpers must be true or false" );
            else
                result.footprint["duplicatePadNumbersAreJumpers"] = parsed;
        }
        else
        {
            diagnostic( result, "unknown_authored_footprint_field",
                        "unsupported footprint field " + head );
        }
    }

    std::set<std::string> availablePadNumbers;

    for( const JSON& pad : result.footprint["pads"] )
    {
        const std::string number = pad.value( "number", "" );

        if( !number.empty() )
            availablePadNumbers.emplace( number );
    }

    std::set<std::string> jumperPads;

    for( const JSON& group : result.footprint["jumperGroups"] )
    {
        for( const JSON& number : group )
        {
            const std::string value = number.get<std::string>();

            if( !availablePadNumbers.contains( value ) )
                diagnostic( result, "unknown_authored_footprint_jumper_pad",
                            "jumper group references absent pad number " + value );
            else if( !jumperPads.emplace( value ).second )
                diagnostic( result, "duplicate_authored_footprint_jumper_pad",
                            "pad number " + value + " occurs in multiple jumper groups" );
        }
    }

    std::set<std::string> netTiePads;

    for( const JSON& group : result.footprint["netTieGroups"] )
    {
        for( const JSON& number : group )
        {
            const std::string value = number.get<std::string>();

            if( !availablePadNumbers.contains( value ) )
                diagnostic( result, "unknown_authored_footprint_net_tie_pad",
                            "net-tie group references absent pad number " + value );
            else if( !netTiePads.emplace( value ).second )
                diagnostic( result, "duplicate_authored_footprint_net_tie_pad",
                            "pad number " + value + " occurs in multiple net-tie groups" );
        }
    }

    const JSON& attributes = result.footprint["attributes"];

    if( attributes.value( "boardOnly", false )
        && ( attributes.value( "smd", false ) || attributes.value( "throughHole", false ) ) )
    {
        diagnostic( result, "invalid_authored_footprint_mounting_attributes",
                    "board_only footprints cannot also be classified as smd or through_hole" );
    }

    const std::map<std::string, const std::set<std::string>*> memberSets = {
        { "pad", &padIds }, { "graphic", &graphicIds }, { "text", &textIds },
        { "text_box", &textBoxIds }, { "property", &propertyIds },
        { "zone", &zoneIds }, { "group", &groupIds }
    };
    std::map<std::pair<std::string, std::string>, std::string> memberOwners;
    std::map<std::string, std::vector<std::string>> groupEdges;

    for( const JSON& group : result.footprint["groups"] )
    {
        if( !group.is_object() || !group.contains( "id" ) || !group["id"].is_string()
            || !group.contains( "members" ) || !group["members"].is_array() )
        {
            continue;
        }

        const std::string groupId = group["id"].get<std::string>();

        for( const JSON& member : group["members"] )
        {
            if( !member.is_object() || !member.contains( "type" ) || !member["type"].is_string()
                || !member.contains( "id" ) || !member["id"].is_string() )
            {
                continue;
            }

            const std::string type = member["type"].get<std::string>();
            const std::string memberId = member["id"].get<std::string>();
            const auto available = memberSets.find( type );

            if( available == memberSets.end() || !available->second->contains( memberId ) )
            {
                diagnostic( result, "unknown_authored_footprint_group_member",
                            "group " + groupId + " references absent " + type + " " + memberId );
                continue;
            }

            const std::pair<std::string, std::string> key = { type, memberId };
            const auto owner = memberOwners.emplace( key, groupId );

            if( !owner.second )
                diagnostic( result, "multiply_owned_authored_footprint_group_member",
                            type + " " + memberId + " belongs to both " + owner.first->second
                                    + " and " + groupId );

            if( type == "group" )
                groupEdges[groupId].push_back( memberId );
        }
    }

    std::map<std::string, int> groupVisit;
    std::function<void( const std::string& )> visitGroup = [&]( const std::string& aGroup )
    {
        if( groupVisit[aGroup] == 1 )
        {
            diagnostic( result, "cyclic_authored_footprint_groups",
                        "footprint groups contain a membership cycle through " + aGroup );
            return;
        }

        if( groupVisit[aGroup] == 2 )
            return;

        groupVisit[aGroup] = 1;

        for( const std::string& child : groupEdges[aGroup] )
            visitGroup( child );

        groupVisit[aGroup] = 2;
    };

    for( const std::string& groupId : groupIds )
        visitGroup( groupId );

    for( const JSON& variant : result.footprint["variants"] )
    {
        if( !variant.is_object() || !variant.contains( "fields" )
            || !variant["fields"].is_array() )
        {
            continue;
        }

        for( const JSON& field : variant["fields"] )
        {
            std::string variantFieldName = field.value( "name", "" );
            std::transform( variantFieldName.begin(), variantFieldName.end(),
                            variantFieldName.begin(), []( unsigned char aCharacter )
            {
                return static_cast<char>( std::tolower( aCharacter ) );
            } );

            if( !variantFieldName.empty() && !propertyNames.contains( variantFieldName ) )
                diagnostic( result, "unknown_authored_footprint_variant_field",
                            "variant " + variant.value( "id", "" )
                                    + " overrides absent footprint field "
                                    + field.value( "name", "" ) );
        }
    }

    if( result.footprint["stackup"].is_array() )
    {
        std::set<std::string> stackupLayers;
        std::map<std::string, size_t> stackupPositions;

        for( const JSON& layer : result.footprint["stackup"] )
        {
            if( layer.is_string() )
            {
                const std::string layerName = layer.get<std::string>();
                stackupLayers.emplace( layerName );
                stackupPositions.emplace( layerName, stackupPositions.size() );
            }
        }

        for( const JSON& zone : result.footprint["zones"] )
        {
            if( !zone.is_object() || !zone.contains( "layers" ) || !zone["layers"].is_array() )
                continue;

            for( const JSON& layer : zone["layers"] )
            {
                if( !layer.is_string() )
                    continue;

                const std::string zoneLayerName = layer.get<std::string>();

                if( ( zoneLayerName == "F.Cu" || zoneLayerName == "B.Cu"
                      || zoneLayerName.starts_with( "In" ) )
                    && !stackupLayers.contains( zoneLayerName ) )
                {
                    diagnostic( result, "authored_footprint_zone_outside_custom_stackup",
                                "footprint zone references " + zoneLayerName
                                        + " outside its custom stackup" );
                }
            }
        }

        for( const JSON& layer : result.footprint["privateLayers"] )
        {
            if( !layer.is_string() )
                continue;

            const std::string privateLayerName = layer.get<std::string>();

            if( ( privateLayerName == "F.Cu" || privateLayerName == "B.Cu"
                  || ( privateLayerName.starts_with( "In" )
                       && privateLayerName.ends_with( ".Cu" ) ) )
                && !stackupLayers.contains( privateLayerName ) )
            {
                diagnostic( result, "authored_footprint_private_layer_outside_custom_stackup",
                            "footprint private layer " + privateLayerName
                                    + " is outside its custom stackup" );
            }
        }

        for( const JSON& pad : result.footprint["pads"] )
        {
            if( !pad.is_object() || !pad.contains( "padstack" )
                || !pad["padstack"].is_object() )
            {
                continue;
            }

            const JSON& padstack = pad["padstack"];
            const std::string padId = pad.value( "id", "" );

            if( padstack.value( "mode", "" ) == "front_inner_back" )
            {
                diagnostic( result, "ambiguous_authored_footprint_padstack_for_custom_stackup",
                            "pad " + padId + " uses front_inner_back with a custom stackup; "
                                    "use a custom padstack with named copper layers" );
                continue;
            }

            if( !padstack.contains( "layers" ) || !padstack["layers"].is_array() )
                continue;

            for( const JSON& layer : padstack["layers"] )
            {
                const std::string padstackLayerName = layer.value( "name", "" );

                if( !padstackLayerName.empty()
                    && !stackupLayers.contains( padstackLayerName ) )
                {
                    diagnostic( result, "authored_footprint_padstack_outside_custom_stackup",
                                "pad " + padId + " padstack references " + padstackLayerName
                                        + " outside the footprint custom stackup" );
                }
            }
        }

        for( const JSON& pad : result.footprint["pads"] )
        {
            if( !pad.is_object() || !pad.contains( "backdrills" )
                || !pad["backdrills"].is_object() )
            {
                continue;
            }

            const JSON& backdrills = pad["backdrills"];
            const std::string padId = pad.value( "id", "" );
            int topStop = -1;
            int bottomStop = -1;

            for( const char* side : { "top", "bottom" } )
            {
                if( !backdrills[side].is_object() )
                    continue;

                const std::string stopLayer = backdrills[side].value( "stopLayer", "" );
                const auto position = stackupPositions.find( stopLayer );

                if( position == stackupPositions.end() )
                {
                    diagnostic( result,
                                "authored_footprint_backdrill_outside_custom_stackup",
                                "pad " + padId + " backdrill stops on " + stopLayer
                                        + " outside the footprint custom stackup" );
                    continue;
                }

                if( side[0] == 't' )
                    topStop = static_cast<int>( position->second );
                else
                    bottomStop = static_cast<int>( position->second );
            }

            if( topStop >= 0 && bottomStop >= 0 && topStop >= bottomStop )
                diagnostic( result, "overlapping_authored_footprint_pad_backdrills",
                            "pad " + padId
                                    + " top and bottom backdrills must leave a plated layer span" );
        }
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
