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

#include "design_script_compiler.h"
#include "kichad_from_chars.h"

#include "design_script_capabilities.h"

#include "design_script_board_compiler.h"
#include "design_script_electrical_compiler.h"
#include "design_script_footprint_compiler.h"
#include "design_script_production_compiler.h"
#include "design_script_symbol_compiler.h"
#include "lossless_sexpr_document.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <limits>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/multiprecision/cpp_int.hpp>
#include <picosha2.h>
#include <wx/base64.h>
#include <wx/string.h>


namespace
{

using JSON = nlohmann::json;
using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;

constexpr size_t MAX_SCRIPT_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_TOP_LEVEL_FORMS = 20000;
constexpr size_t MAX_IDENTIFIER_BYTES = 128;
constexpr size_t MAX_TITLE_BLOCK_TEXT_BYTES = 4096;
constexpr size_t MAX_SCHEMATIC_PROPERTY_BYTES = 4096;
constexpr size_t MAX_SCHEMATIC_TEXT_BYTES = 64 * 1024;
constexpr size_t MAX_FONT_NAME_BYTES = 256;
constexpr size_t MAX_HYPERLINK_BYTES = 2048;
constexpr size_t MAX_SCHEMATIC_IMAGE_BYTES = 8 * 1024 * 1024;
constexpr size_t MAX_SCHEMATIC_TABLE_AXIS = 256;
constexpr size_t MAX_SCHEMATIC_TABLE_CELLS = 65536;
constexpr size_t MAX_SCHEMATIC_GROUP_MEMBERS = 4096;
constexpr size_t MAX_LIBRARIES = 512;
constexpr size_t MAX_PROJECT_LIBRARIES_PER_TABLE = 256;
constexpr size_t MAX_PROJECT_TEXT_VARIABLES = 1024;
constexpr size_t MAX_PROJECT_FIELD_TEMPLATES = 1024;


void diagnostic( KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult, const std::string& aSeverity,
                 const std::string& aCode, const std::string& aMessage,
                 const JSON& aDetails = JSON::object() )
{
    JSON entry = { { "severity", aSeverity }, { "code", aCode }, { "message", aMessage } };

    for( const auto& [key, value] : aDetails.items() )
        entry[key] = value;

    aResult.diagnostics.push_back( std::move( entry ) );
}


JSON sourceLocation( const std::string& aSource, size_t aByteOffset )
{
    size_t line = 1;
    size_t column = 1;

    for( size_t i = 0; i < std::min( aByteOffset, aSource.size() ); ++i )
    {
        if( aSource[i] == '\n' )
        {
            ++line;
            column = 1;
        }
        else
        {
            const unsigned char byte = static_cast<unsigned char>( aSource[i] );

            if( ( byte & 0xC0 ) != 0x80 )
                ++column;
        }
    }

    return { { "byteOffset", aByteOffset }, { "line", line }, { "column", column } };
}


size_t firstInvalidUtf8Byte( const std::string& aSource )
{
    const auto continuation = [&]( size_t aIndex )
    {
        return aIndex < aSource.size()
               && ( static_cast<unsigned char>( aSource[aIndex] ) & 0xC0 ) == 0x80;
    };

    for( size_t i = 0; i < aSource.size(); )
    {
        const unsigned char first = static_cast<unsigned char>( aSource[i] );

        if( first <= 0x7F )
        {
            ++i;
            continue;
        }

        if( first >= 0xC2 && first <= 0xDF )
        {
            if( !continuation( i + 1 ) )
                return i;

            i += 2;
            continue;
        }

        if( first >= 0xE0 && first <= 0xEF )
        {
            if( i + 2 >= aSource.size() || !continuation( i + 1 )
                || !continuation( i + 2 ) )
            {
                return i;
            }

            const unsigned char second = static_cast<unsigned char>( aSource[i + 1] );

            if( ( first == 0xE0 && second < 0xA0 ) || ( first == 0xED && second > 0x9F ) )
                return i;

            i += 3;
            continue;
        }

        if( first >= 0xF0 && first <= 0xF4 )
        {
            if( i + 3 >= aSource.size() || !continuation( i + 1 )
                || !continuation( i + 2 ) || !continuation( i + 3 ) )
            {
                return i;
            }

            const unsigned char second = static_cast<unsigned char>( aSource[i + 1] );

            if( ( first == 0xF0 && second < 0x90 ) || ( first == 0xF4 && second > 0x8F ) )
                return i;

            i += 4;
            continue;
        }

        return i;
    }

    return std::string::npos;
}


bool isScalar( const DOCUMENT& aDocument, size_t aNode )
{
    return aNode < aDocument.Nodes().size()
           && aDocument.Nodes()[aNode].kind != DOCUMENT::NODE_KIND::LIST;
}


bool scalarText( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    if( !isScalar( aDocument, aNode ) )
        return false;

    aValue = aDocument.AtomText( aNode );
    return true;
}


bool validIdentifier( const std::string& aValue )
{
    if( aValue.empty() || aValue.size() > MAX_IDENTIFIER_BYTES )
        return false;

    return std::all_of( aValue.begin(), aValue.end(),
                        []( unsigned char aCharacter )
                        {
                            return std::isalnum( aCharacter ) || aCharacter == '_'
                                   || aCharacter == '-' || aCharacter == '+'
                                   || aCharacter == '.' || aCharacter == '/'
                                   || aCharacter == '#';
                        } );
}


bool validTextVariableName( const std::string& aValue )
{
    static constexpr std::string_view excluded = "{}[]()%~<>\"='`;:.,&?/\\|$";

    if( aValue.empty() || aValue.size() > MAX_IDENTIFIER_BYTES
        || std::isspace( static_cast<unsigned char>( aValue.front() ) )
        || std::isspace( static_cast<unsigned char>( aValue.back() ) ) )
    {
        return false;
    }

    return std::none_of( aValue.begin(), aValue.end(),
                         []( unsigned char aCharacter )
                         {
                             return aCharacter == '\0' || std::iscntrl( aCharacter )
                                    || excluded.find( static_cast<char>( aCharacter ) )
                                               != std::string_view::npos;
                         } );
}


bool validNativeRuleKey( const std::string& aValue )
{
    return !aValue.empty() && aValue.size() <= 128
           && std::all_of( aValue.begin(), aValue.end(), []( unsigned char aCharacter )
               {
                   return std::islower( aCharacter ) || std::isdigit( aCharacter )
                          || aCharacter == '_' || aCharacter == '-';
               } );
}


std::string asciiLower( std::string aValue )
{
    std::transform( aValue.begin(), aValue.end(), aValue.begin(),
                    []( unsigned char aCharacter )
                    {
                        return static_cast<char>( std::tolower( aCharacter ) );
                    } );
    return aValue;
}


bool libraryIdNickname( const std::string& aValue, std::string& aNickname )
{
    const size_t separator = aValue.find( ':' );

    if( separator == std::string::npos || separator == 0 || separator + 1 == aValue.size()
        || aValue.find( ':', separator + 1 ) != std::string::npos || aValue.size() > 512 )
    {
        return false;
    }

    aNickname = aValue.substr( 0, separator );
    return validIdentifier( aNickname );
}


JSON scalarValue( const DOCUMENT& aDocument, size_t aNode )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    const std::string     value = aDocument.AtomText( aNode );

    if( node.kind == DOCUMENT::NODE_KIND::STRING )
        return value;

    if( value == "true" )
        return true;

    if( value == "false" )
        return false;

    int64_t integer = 0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    std::from_chars_result converted = std::from_chars( begin, end, integer );

    if( converted.ec == std::errc() && converted.ptr == end )
        return integer;

    return value;
}


JSON expressionToIr( const DOCUMENT& aDocument, size_t aNode )
{
    if( isScalar( aDocument, aNode ) )
        return scalarValue( aDocument, aNode );

    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    JSON                  arguments = JSON::array();

    for( size_t i = 1; i < node.children.size(); ++i )
        arguments.emplace_back( expressionToIr( aDocument, node.children[i] ) );

    return { { "op", aDocument.ListHead( aNode ) }, { "args", std::move( arguments ) } };
}


bool parseSingleValueForm( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalarText( aDocument, node.children[1], aValue );
}


bool parseDistance( const std::string& aText, int64_t& aNanometers )
{
    long double value = 0.0L;
    std::from_chars_result converted =
            KICHAD::FromChars( aText.data(), aText.data() + aText.size(), value );

    if( converted.ec != std::errc() || converted.ptr == aText.data()
        || !std::isfinite( value ) )
    {
        return false;
    }

    const std::string_view unit( converted.ptr,
                                 static_cast<size_t>( aText.data() + aText.size()
                                                      - converted.ptr ) );
    long double scale = 0.0L;

    if( unit == "mm" )
        scale = 1000000.0L;
    else if( unit == "mil" )
        scale = 25400.0L;
    else if( unit == "um" )
        scale = 1000.0L;
    else if( unit == "nm" )
        scale = 1.0L;
    else if( unit == "in" )
        scale = 25400000.0L;
    else
        return false;

    const long double rounded = std::round( value * scale );

    if( !std::isfinite( rounded ) || rounded < -9223372036854775808.0L
        || rounded >= 9223372036854775808.0L )
    {
        return false;
    }

    aNanometers = static_cast<int64_t>( rounded );
    return true;
}


bool parseTime( const std::string& aText, int64_t& aFemtoseconds )
{
    long double value = 0.0L;
    std::from_chars_result converted =
            KICHAD::FromChars( aText.data(), aText.data() + aText.size(), value );

    if( converted.ec != std::errc() || converted.ptr == aText.data()
        || !std::isfinite( value ) )
    {
        return false;
    }

    const std::string_view unit( converted.ptr,
                                 static_cast<size_t>( aText.data() + aText.size()
                                                      - converted.ptr ) );
    long double scale = 0.0L;

    if( unit == "fs" )
        scale = 1.0L;
    else if( unit == "ps" )
        scale = 1000.0L;
    else
        return false;

    const long double rounded = std::round( value * scale );

    if( !std::isfinite( rounded ) || rounded < -9223372036854775808.0L
        || rounded >= 9223372036854775808.0L )
    {
        return false;
    }

    aFemtoseconds = static_cast<int64_t>( rounded );
    return true;
}


bool parseFiniteDecimal( const std::string& aText, double& aValue,
                         const std::string_view aRequiredSuffix = {} )
{
    std::from_chars_result converted =
            KICHAD::FromChars( aText.data(), aText.data() + aText.size(), aValue );

    if( converted.ec != std::errc() || converted.ptr == aText.data()
        || !std::isfinite( aValue ) )
    {
        return false;
    }

    return std::string_view( converted.ptr,
                             static_cast<size_t>( aText.data() + aText.size()
                                                  - converted.ptr ) ) == aRequiredSuffix;
}


bool parseBoundedInteger( const std::string& aText, int64_t aMinimum, int64_t aMaximum,
                          int64_t& aValue )
{
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    std::from_chars_result converted = std::from_chars( begin, end, aValue );
    return converted.ec == std::errc() && converted.ptr == end
           && aValue >= aMinimum && aValue <= aMaximum;
}


bool parseHexColor( const std::string& aText, JSON& aColor )
{
    if( aText.size() != 7 && aText.size() != 9 )
        return false;

    if( aText.front() != '#' )
        return false;

    int channels[4] = { 0, 0, 0, 255 };

    for( size_t channel = 0; channel < ( aText.size() - 1 ) / 2; ++channel )
    {
        const char* begin = aText.data() + 1 + channel * 2;
        const char* end = begin + 2;
        std::from_chars_result converted = std::from_chars( begin, end, channels[channel], 16 );

        if( converted.ec != std::errc() || converted.ptr != end )
            return false;
    }

    aColor = { { "r", channels[0] / 255.0 },
               { "g", channels[1] / 255.0 },
               { "b", channels[2] / 255.0 },
               { "a", channels[3] / 255.0 } };
    return true;
}


bool boundedBusRanges( const std::string& aPattern )
{
    size_t range = 0;

    while( ( range = aPattern.find( "..", range ) ) != std::string::npos )
    {
        const size_t open = aPattern.rfind( '[', range );
        const size_t close = aPattern.find( ']', range + 2 );

        if( open != std::string::npos && close != std::string::npos )
        {
            int64_t first = 0;
            int64_t last = 0;
            const char* firstBegin = aPattern.data() + open + 1;
            const char* firstEnd = aPattern.data() + range;
            const char* lastBegin = aPattern.data() + range + 2;
            const char* lastEnd = aPattern.data() + close;
            std::from_chars_result firstResult =
                    std::from_chars( firstBegin, firstEnd, first );
            std::from_chars_result lastResult = std::from_chars( lastBegin, lastEnd, last );

            if( firstResult.ec == std::errc() && firstResult.ptr == firstEnd
                && lastResult.ec == std::errc() && lastResult.ptr == lastEnd
                && std::fabs( static_cast<long double>( first )
                              - static_cast<long double>( last ) ) > 255.0L )
            {
                return false;
            }
        }

        range += 2;
    }

    return true;
}


JSON compileProject( const DOCUMENT& aDocument, size_t aNode,
                     KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           name;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], name )
        || name.empty() || name.size() > MAX_IDENTIFIER_BYTES )
    {
        diagnostic( aResult, "error", "invalid_project",
                    "project requires a non-empty name of at most 128 bytes" );
        return JSON::object();
    }

    JSON project = { { "name", name } };
    JSON titleBlock = { { "title", "" }, { "company", "" }, { "revision", "" },
                        { "date", "" }, { "comments", JSON::array() } };

    for( int i = 0; i < 9; ++i )
        titleBlock["comments"].emplace_back( "" );

    const std::set<std::string> allowed = { "title", "company", "revision", "date" };
    std::set<std::string>       fields;
    std::set<int>               comments;
    bool                        hasTitleBlock = false;
    bool                        hasTextVariables = false;
    bool                        hasFieldTemplates = false;

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        std::string       value;

        if( head == "text_variables" )
        {
            const DOCUMENT::NODE& variables = aDocument.Nodes()[child];

            if( hasTextVariables )
            {
                diagnostic( aResult, "error", "duplicate_project_text_variables",
                            "project text_variables occurs more than once" );
                continue;
            }

            hasTextVariables = true;
            JSON desired = JSON::object();

            if( variables.children.size() - 1 > MAX_PROJECT_TEXT_VARIABLES )
            {
                diagnostic( aResult, "error", "too_many_project_text_variables",
                            "project text_variables supports at most 1024 variables" );
            }

            for( size_t variableIndex = 1; variableIndex < variables.children.size();
                 ++variableIndex )
            {
                const size_t variableNode = variables.children[variableIndex];
                const DOCUMENT::NODE& variable = aDocument.Nodes()[variableNode];
                std::string variableName;
                std::string variableValue;

                if( aDocument.ListHead( variableNode ) != "variable"
                    || variable.children.size() != 3
                    || !scalarText( aDocument, variable.children[1], variableName )
                    || !scalarText( aDocument, variable.children[2], variableValue )
                    || !validTextVariableName( variableName )
                    || variableValue.size() > MAX_TITLE_BLOCK_TEXT_BYTES
                    || variableValue.find( '\0' ) != std::string::npos )
                {
                    diagnostic( aResult, "error", "invalid_project_text_variable",
                                "text_variables entries require (variable NAME VALUE); names "
                                "must be bounded native KiCad text-variable names and values at "
                                "most 4096 bytes" );
                    continue;
                }

                if( desired.contains( variableName ) )
                {
                    diagnostic( aResult, "error", "duplicate_project_text_variable",
                                "project text variable '" + variableName
                                        + "' occurs more than once" );
                    continue;
                }

                desired[variableName] = variableValue;
            }

            project["textVariables"] = std::move( desired );
            continue;
        }

        if( head == "field_templates" )
        {
            const DOCUMENT::NODE& templates = aDocument.Nodes()[child];

            if( hasFieldTemplates )
            {
                diagnostic( aResult, "error", "duplicate_project_field_templates",
                            "project field_templates occurs more than once" );
                continue;
            }

            hasFieldTemplates = true;
            JSON desired = JSON::array();
            std::set<std::string> names;
            const std::set<std::string> mandatory = {
                "reference", "value", "footprint", "datasheet", "description"
            };

            if( templates.children.size() - 1 > MAX_PROJECT_FIELD_TEMPLATES )
            {
                diagnostic( aResult, "error", "too_many_project_field_templates",
                            "project field_templates supports at most 1024 templates" );
            }

            for( size_t templateIndex = 1; templateIndex < templates.children.size();
                 ++templateIndex )
            {
                const size_t templateNode = templates.children[templateIndex];
                const DOCUMENT::NODE& field = aDocument.Nodes()[templateNode];
                std::string fieldName;

                if( aDocument.ListHead( templateNode ) != "field"
                    || field.children.size() != 4
                    || !scalarText( aDocument, field.children[1], fieldName )
                    || fieldName.empty() || fieldName.size() > MAX_IDENTIFIER_BYTES
                    || fieldName.find( '\0' ) != std::string::npos
                    || std::any_of( fieldName.begin(), fieldName.end(),
                                    []( unsigned char aCharacter )
                                    {
                                        return std::iscntrl( aCharacter );
                                    } )
                    || std::isspace( static_cast<unsigned char>( fieldName.front() ) )
                    || std::isspace( static_cast<unsigned char>( fieldName.back() ) )
                    || mandatory.contains( asciiLower( fieldName ) ) )
                {
                    diagnostic( aResult, "error", "invalid_project_field_template",
                                "field_templates entries require a non-mandatory bounded name "
                                "and exactly one visible and url boolean" );
                    continue;
                }

                JSON parsed = { { "name", fieldName } };
                std::set<std::string> attributes;

                for( size_t attributeIndex = 2; attributeIndex < field.children.size();
                     ++attributeIndex )
                {
                    const size_t attributeNode = field.children[attributeIndex];
                    const std::string attribute = aDocument.ListHead( attributeNode );
                    std::string boolean;

                    if( ( attribute != "visible" && attribute != "url" )
                        || !parseSingleValueForm( aDocument, attributeNode, boolean )
                        || ( boolean != "true" && boolean != "false" )
                        || !attributes.emplace( attribute ).second )
                    {
                        diagnostic( aResult, "error", "invalid_project_field_template",
                                    "field template attributes must be unique visible and url "
                                    "booleans" );
                        continue;
                    }

                    parsed[attribute] = boolean == "true";
                }

                const std::string foldedName = asciiLower( fieldName );

                if( attributes != std::set<std::string>{ "url", "visible" }
                    || parsed.size() != 3 )
                {
                    diagnostic( aResult, "error", "incomplete_project_field_template",
                                "each project field template requires visible and url" );
                    continue;
                }

                if( !names.emplace( foldedName ).second )
                {
                    diagnostic( aResult, "error", "duplicate_project_field_template",
                                "project field template '" + fieldName
                                        + "' conflicts with another template name" );
                    continue;
                }

                desired.emplace_back( std::move( parsed ) );
            }

            project["fieldTemplates"] = std::move( desired );
            continue;
        }

        if( head == "comment" )
        {
            const DOCUMENT::NODE& comment = aDocument.Nodes()[child];
            std::string indexText;
            int         index = 0;

            if( comment.children.size() != 3
                || !scalarText( aDocument, comment.children[1], indexText )
                || !scalarText( aDocument, comment.children[2], value ) )
            {
                diagnostic( aResult, "error", "invalid_project_comment",
                            "project comment requires an index from 1 through 9 and one scalar "
                            "value" );
                continue;
            }

            const char* begin = indexText.data();
            const char* end = begin + indexText.size();
            std::from_chars_result converted = std::from_chars( begin, end, index );

            if( converted.ec != std::errc() || converted.ptr != end || index < 1 || index > 9
                || value.size() > MAX_TITLE_BLOCK_TEXT_BYTES )
            {
                diagnostic( aResult, "error", "invalid_project_comment",
                            "project comment index must be 1 through 9 and its value at most "
                            "4096 bytes" );
                continue;
            }

            if( !comments.emplace( index ).second )
            {
                diagnostic( aResult, "error", "duplicate_project_comment",
                            "project comment index " + std::to_string( index )
                                    + " occurs more than once" );
                continue;
            }

            titleBlock["comments"][index - 1] = value;
            hasTitleBlock = true;
            continue;
        }

        if( !allowed.contains( head ) || !parseSingleValueForm( aDocument, child, value )
            || value.size() > MAX_TITLE_BLOCK_TEXT_BYTES )
        {
            diagnostic( aResult, "error", "invalid_project_field",
                        "project fields must be title, company, revision, or date with one "
                        "scalar value of at most 4096 bytes; comments use (comment INDEX VALUE)" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_project_field",
                        "project field '" + head + "' occurs more than once" );
            continue;
        }

        titleBlock[head] = value;
        hasTitleBlock = true;
    }

    if( hasTitleBlock )
        project["titleBlock"] = std::move( titleBlock );

    return project;
}


JSON compileLibrary( const DOCUMENT& aDocument, size_t aNode,
                     KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           kind;
    std::string           id;

    if( node.children.size() < 3 || !scalarText( aDocument, node.children[1], kind )
        || !scalarText( aDocument, node.children[2], id )
        || ( kind != "symbol" && kind != "footprint" && kind != "model" )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_library",
                    "library requires kind symbol, footprint, or model and a bounded identifier" );
        return JSON::object();
    }

    JSON library = { { "kind", kind }, { "id", id }, { "uri", nullptr },
                     { "managed", false } };
    std::set<std::string> fields;

    for( size_t i = 3; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        std::string       value;

        if( ( head != "uri" && head != "table" && head != "managed" )
            || !parseSingleValueForm( aDocument, child, value ) )
        {
            diagnostic( aResult, "error", "invalid_library_field",
                        "library fields must be uri, table, or managed with one scalar value" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_library_field",
                        "library field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "managed" )
        {
            if( value != "true" && value != "false" )
            {
                diagnostic( aResult, "error", "invalid_library_management",
                            "library managed must be true or false" );
            }
            else
            {
                library["managed"] = value == "true";
            }
        }
        else
        {
            library[head] = value;
        }
    }

    const std::string table = library.value( "table", "" );

    if( table != "global" && table != "project" )
    {
        diagnostic( aResult, "error", "invalid_library_table",
                    "library requires exactly one (table global|project)" );
    }

    if( table == "global" && !library["uri"].is_null() )
    {
        diagnostic( aResult, "error", "redundant_global_library_uri",
                    "global library dependencies use their installed nickname and cannot set uri" );
    }

    if( library.value( "managed", false )
        && ( table != "project" || ( kind != "symbol" && kind != "footprint" ) ) )
    {
        diagnostic( aResult, "error", "unsupported_managed_library",
                    "KDS version 1 managed libraries require a project symbol or footprint "
                    "library" );
    }

    if( table == "project" )
    {
        const std::string uri = library["uri"].is_string()
                                      ? library["uri"].get<std::string>()
                                      : std::string();
        const bool safe = uri.starts_with( "${KIPRJMOD}/" ) && uri.size() <= 4096
                          && uri.size() > std::strlen( "${KIPRJMOD}/" )
                          && uri.find( '\0' ) == std::string::npos
                          && uri.find_first_of( "\r\n\"\\" ) == std::string::npos
                          && uri.find( "/../" ) == std::string::npos
                          && !uri.ends_with( "/.." );
        const bool nativeSuffix = kind == "symbol" ? uri.ends_with( ".kicad_sym" )
                                : kind == "footprint" ? uri.ends_with( ".pretty" )
                                                      : true;

        if( !safe || !nativeSuffix )
        {
            diagnostic( aResult, "error", "invalid_project_library_uri",
                        "project library uri must be a safe ${KIPRJMOD}/ path ending in "
                        ".kicad_sym for symbols or .pretty for footprints" );
        }
    }

    return library;
}


JSON compileComponentField( const DOCUMENT& aDocument, size_t aNode,
                            KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult,
                            const std::string& aReference, int64_t aUnit )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string name;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], name )
        || name.empty() || name.size() > MAX_IDENTIFIER_BYTES
        || name.find( '\0' ) != std::string::npos )
    {
        diagnostic( aResult, "error", "invalid_component_unit_field_layout",
                    "component unit field requires a name from 1 through 128 UTF-8 bytes" );
        return JSON::object();
    }

    JSON layout = { { "name", name }, { "mirror", false } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_component_unit_field_layout_field",
                        "component " + aReference + " unit " + std::to_string( aUnit )
                                + " field " + name + " setting '" + head
                                + "' occurs more than once" );
            continue;
        }

        if( head == "at" || head == "size" )
        {
            const DOCUMENT::NODE& point = aDocument.Nodes()[child];
            std::string xText;
            std::string yText;
            int64_t x = 0;
            int64_t y = 0;
            const bool isPosition = head == "at";
            const bool valid = point.children.size() == 3
                               && scalarText( aDocument, point.children[1], xText )
                               && scalarText( aDocument, point.children[2], yText )
                               && parseDistance( xText, x ) && parseDistance( yText, y )
                               && ( isPosition
                                            ? x >= 0 && y >= 0 && x <= 2'000'000'000
                                                      && y <= 2'000'000'000
                                            : x >= 100'000 && y >= 100'000
                                                      && x <= 50'000'000 && y <= 50'000'000 );

            if( !valid )
            {
                diagnostic( aResult, "error", "invalid_component_unit_field_layout_" + head,
                            isPosition
                                    ? "component unit field at requires two distances from 0 to 2 m"
                                    : "component unit field size requires two dimensions from "
                                      "0.1 mm through 50 mm" );
            }
            else
            {
                layout[isPosition ? "position" : "size"] = {
                    { "xNm", x }, { "yNm", y }
                };
            }

            continue;
        }

        if( head == "rotation" )
        {
            std::string value;
            double degrees = 0.0;

            if( !parseSingleValueForm( aDocument, child, value )
                || !parseFiniteDecimal( value, degrees, "deg" )
                || degrees < 0.0 || degrees >= 360.0 )
            {
                diagnostic( aResult, "error", "invalid_component_unit_field_layout_rotation",
                            "component unit field rotation requires an angle from 0deg through "
                            "less than 360deg" );
            }
            else
            {
                layout["rotationDegrees"] = degrees;
            }

            continue;
        }

        if( head == "font" )
        {
            std::string font;

            if( !parseSingleValueForm( aDocument, child, font ) || font.empty()
                || font.size() > MAX_FONT_NAME_BYTES
                || font.find( '\0' ) != std::string::npos )
            {
                diagnostic( aResult, "error", "invalid_component_unit_field_layout_font",
                            "component unit field font requires stroke or a name up to 256 "
                            "UTF-8 bytes" );
            }
            else
            {
                layout["font"] = font;
            }

            continue;
        }

        if( head == "line_spacing" )
        {
            std::string value;
            double spacing = 0.0;

            if( !parseSingleValueForm( aDocument, child, value )
                || !parseFiniteDecimal( value, spacing, "" )
                || spacing < 0.5 || spacing > 5.0 )
            {
                diagnostic( aResult, "error",
                            "invalid_component_unit_field_layout_line_spacing",
                            "component unit field line_spacing requires a finite value from "
                            "0.5 through 5" );
            }
            else
            {
                layout["lineSpacing"] = spacing;
            }

            continue;
        }

        if( head == "thickness" )
        {
            std::string value;
            int64_t thickness = 0;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "auto"
                     && ( !parseDistance( value, thickness ) || thickness < 10'000
                          || thickness > 10'000'000 ) ) )
            {
                diagnostic( aResult, "error",
                            "invalid_component_unit_field_layout_thickness",
                            "component unit field thickness requires auto or 0.01 mm through "
                            "10 mm" );
            }
            else
            {
                layout["thicknessNm"] = value == "auto" ? 0 : thickness;
            }

            continue;
        }

        if( head == "color" )
        {
            std::string value;
            JSON color;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "default" && !parseHexColor( value, color ) ) )
            {
                diagnostic( aResult, "error", "invalid_component_unit_field_layout_color",
                            "component unit field color requires default or #RRGGBB[AA]" );
            }
            else if( value == "default" )
            {
                layout["color"] = nullptr;
            }
            else
            {
                layout["color"] = {
                    { "r", std::lround( color["r"].get<double>() * 255.0 ) },
                    { "g", std::lround( color["g"].get<double>() * 255.0 ) },
                    { "b", std::lround( color["b"].get<double>() * 255.0 ) },
                    { "a", std::lround( color["a"].get<double>() * 255.0 ) }
                };
            }

            continue;
        }

        if( head == "justify" )
        {
            const DOCUMENT::NODE& justify = aDocument.Nodes()[child];
            std::string horizontal;
            std::string vertical;

            if( justify.children.size() != 3
                || !scalarText( aDocument, justify.children[1], horizontal )
                || !scalarText( aDocument, justify.children[2], vertical )
                || ( horizontal != "left" && horizontal != "center"
                     && horizontal != "right" )
                || ( vertical != "top" && vertical != "center" && vertical != "bottom" ) )
            {
                diagnostic( aResult, "error", "invalid_component_unit_field_layout_justify",
                            "component unit field justify requires left|center|right and "
                            "top|center|bottom" );
            }
            else
            {
                layout["justify"] = {
                    { "horizontal", horizontal }, { "vertical", vertical }
                };
            }

            continue;
        }

        if( head == "hyperlink" )
        {
            std::string value;

            if( !parseSingleValueForm( aDocument, child, value )
                || value.size() > MAX_HYPERLINK_BYTES
                || value.find( '\0' ) != std::string::npos
                || value.find( '\r' ) != std::string::npos
                || value.find( '\n' ) != std::string::npos )
            {
                diagnostic( aResult, "error", "invalid_component_unit_field_layout_hyperlink",
                            "component unit field hyperlink requires none or one line up to "
                            "2048 UTF-8 bytes" );
            }
            else
            {
                layout["hyperlink"] = value == "none" ? "" : value;
            }

            continue;
        }

        if( head == "visible" || head == "show_name" || head == "autoplace"
            || head == "bold" || head == "italic" || head == "private" )
        {
            std::string value;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "true" && value != "false" ) )
            {
                diagnostic( aResult, "error",
                            "invalid_component_unit_field_layout_" + head,
                            "component unit field " + head + " must be true or false" );
            }
            else
            {
                const std::string key = head == "show_name" ? "showName" : head;
                layout[key] = value == "true";
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_component_unit_field_layout_field",
                    "component unit field supports at, rotation, visible, show_name, autoplace, "
                    "size, font, line_spacing, thickness, color, justify, bold, italic, "
                    "hyperlink, and private" );
    }

    for( const char* required : { "position", "rotationDegrees", "visible", "showName",
                                  "autoplace", "size", "font", "lineSpacing", "thicknessNm",
                                  "color", "justify", "bold", "italic", "hyperlink",
                                  "private" } )
    {
        if( !layout.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_component_unit_field_layout_field",
                        "component " + aReference + " unit " + std::to_string( aUnit )
                                + " field " + name + " is missing " + required );
        }
    }

    return layout;
}


JSON compileComponent( const DOCUMENT& aDocument, size_t aNode,
                       KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           reference;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], reference )
        || !validIdentifier( reference ) )
    {
        diagnostic( aResult, "error", "invalid_component",
                    "component requires a bounded logical reference" );
        return JSON::object();
    }

    JSON component = { { "reference", reference },
                       { "properties", JSON::object() },
                       { "units", JSON::array() } };
    std::set<std::string> singletonFields;
    std::set<std::string> propertyNames;
    std::set<int64_t>     unitNumbers;

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        const DOCUMENT::NODE& field = aDocument.Nodes()[child];

        if( head == "unit" )
        {
            std::string numberText;
            int64_t     number = 0;

            if( field.children.size() < 2
                || !scalarText( aDocument, field.children[1], numberText )
                || !parseBoundedInteger( numberText, 1, 256, number ) )
            {
                diagnostic( aResult, "error", "invalid_component_unit",
                            "component unit requires a number from 1 through 256" );
                continue;
            }

            if( !unitNumbers.emplace( number ).second )
            {
                diagnostic( aResult, "error", "duplicate_component_unit",
                            "component " + reference + " unit " + numberText
                                    + " occurs more than once" );
                continue;
            }

            JSON unit = { { "number", number }, { "fieldsAutoplaced", true },
                          { "fields", JSON::array() } };
            std::set<std::string> unitFields;
            std::set<std::string> unitFieldNames;

            for( size_t unitIndex = 2; unitIndex < field.children.size(); ++unitIndex )
            {
                const size_t unitChild = field.children[unitIndex];
                const std::string unitHead = aDocument.ListHead( unitChild );

                if( unitHead == "field" )
                {
                    JSON layout = compileComponentField( aDocument, unitChild, aResult,
                                                         reference, number );
                    const std::string name = layout.value( "name", "" );

                    if( !name.empty() && !unitFieldNames.emplace( name ).second )
                    {
                        diagnostic( aResult, "error", "duplicate_component_unit_field_layout",
                                    "component " + reference + " unit " + numberText
                                            + " field " + name + " occurs more than once" );
                    }

                    unit["fields"].emplace_back( std::move( layout ) );
                    continue;
                }

                if( !unitFields.emplace( unitHead ).second )
                {
                    diagnostic( aResult, "error", "duplicate_component_unit_field",
                                "component unit field '" + unitHead
                                        + "' occurs more than once" );
                    continue;
                }

                if( unitHead == "fields_autoplaced" )
                {
                    std::string value;

                    if( !parseSingleValueForm( aDocument, unitChild, value )
                        || ( value != "true" && value != "false" ) )
                    {
                        diagnostic( aResult, "error",
                                    "invalid_component_unit_fields_autoplaced",
                                    "component unit fields_autoplaced must be true or false" );
                    }
                    else
                    {
                        unit["fieldsAutoplaced"] = value == "true";
                    }

                    continue;
                }

                if( unitHead == "sheet" || unitHead == "mirror" )
                {
                    std::string value;

                    if( !parseSingleValueForm( aDocument, unitChild, value )
                        || ( unitHead == "sheet" && !validIdentifier( value ) )
                        || ( unitHead == "mirror" && value != "none" && value != "x"
                             && value != "y" && value != "xy" ) )
                    {
                        diagnostic( aResult, "error", "invalid_component_unit_" + unitHead,
                                    unitHead == "sheet"
                                            ? "component unit sheet must be a bounded sheet ID"
                                            : "component unit mirror must be none, x, y, or xy" );
                    }
                    else
                    {
                        unit[unitHead] = value;
                    }

                    continue;
                }

                if( unitHead == "at" )
                {
                    const DOCUMENT::NODE& position = aDocument.Nodes()[unitChild];
                    std::string xText;
                    std::string yText;
                    int64_t x = 0;
                    int64_t y = 0;

                    if( position.children.size() != 3
                        || !scalarText( aDocument, position.children[1], xText )
                        || !scalarText( aDocument, position.children[2], yText )
                        || !parseDistance( xText, x ) || !parseDistance( yText, y )
                        || x < 0 || y < 0 || x > 2'000'000'000 || y > 2'000'000'000 )
                    {
                        diagnostic( aResult, "error", "invalid_component_unit_position",
                                    "component unit at requires two distances from 0 to 2 m" );
                    }
                    else
                    {
                        unit["position"] = { { "xNm", x }, { "yNm", y } };
                    }

                    continue;
                }

                if( unitHead == "rotation" )
                {
                    std::string value;
                    double degrees = 0.0;

                    if( !parseSingleValueForm( aDocument, unitChild, value )
                        || !parseFiniteDecimal( value, degrees, "deg" )
                        || ( degrees != 0.0 && degrees != 90.0 && degrees != 180.0
                             && degrees != 270.0 ) )
                    {
                        diagnostic( aResult, "error", "invalid_component_unit_rotation",
                                    "component unit rotation must be 0deg, 90deg, 180deg, or "
                                    "270deg" );
                    }
                    else
                    {
                        unit["rotationDegrees"] = static_cast<int>( degrees );
                    }

                    continue;
                }

                diagnostic( aResult, "error", "unknown_component_unit_field",
                            "component unit supports sheet, at, rotation, mirror, "
                            "fields_autoplaced, and field" );
            }

            for( const char* required : { "sheet", "position", "rotationDegrees", "mirror" } )
            {
                if( !unit.contains( required ) )
                {
                    diagnostic( aResult, "error", "missing_component_unit_field",
                                "component " + reference + " unit " + numberText
                                        + " is missing " + required );
                }
            }

            component["units"].emplace_back( std::move( unit ) );
            continue;
        }

        if( head == "property" )
        {
            std::string key;
            std::string value;

            if( field.children.size() != 3
                || !scalarText( aDocument, field.children[1], key )
                || !scalarText( aDocument, field.children[2], value ) || key.empty()
                || key.size() > MAX_IDENTIFIER_BYTES
                || value.size() > MAX_SCHEMATIC_PROPERTY_BYTES
                || key.find( '\0' ) != std::string::npos
                || value.find( '\0' ) != std::string::npos )
            {
                diagnostic( aResult, "error", "invalid_component_property",
                            "component property requires a 1..128-byte name and a value up to "
                            "4096 UTF-8 bytes" );
                continue;
            }

            if( !propertyNames.emplace( key ).second )
            {
                diagnostic( aResult, "error", "duplicate_component_property",
                            "component property '" + key + "' occurs more than once" );
                continue;
            }

            std::string foldedKey = key;
            std::transform( foldedKey.begin(), foldedKey.end(), foldedKey.begin(),
                            []( unsigned char aCharacter )
                            {
                                return static_cast<char>( std::tolower( aCharacter ) );
                            } );
            static const std::set<std::string> RESERVED_PROPERTIES = {
                "reference", "value", "footprint", "datasheet", "description"
            };

            if( RESERVED_PROPERTIES.contains( foldedKey ) )
            {
                diagnostic( aResult, "error", "reserved_component_property",
                            "component property '" + key
                                    + "' is a native field controlled by KDS" );
                continue;
            }

            component["properties"][key] = value;
            continue;
        }

        if( head == "dnp" )
        {
            std::string value;

            if( !singletonFields.emplace( head ).second )
            {
                diagnostic( aResult, "error", "duplicate_component_field",
                            "component field 'dnp' occurs more than once" );
            }
            else if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "true" && value != "false" ) )
            {
                diagnostic( aResult, "error", "invalid_component_dnp",
                            "component dnp must be true or false" );
            }
            else
            {
                component["dnp"] = value == "true";
            }

            continue;
        }

        if( head == "datasheet" || head == "description" )
        {
            std::string value;

            if( !singletonFields.emplace( head ).second )
            {
                diagnostic( aResult, "error", "duplicate_component_field",
                            "component field '" + head + "' occurs more than once" );
            }
            else if( !parseSingleValueForm( aDocument, child, value )
                     || value.size() > MAX_SCHEMATIC_PROPERTY_BYTES
                     || value.find( '\0' ) != std::string::npos )
            {
                diagnostic( aResult, "error", "invalid_component_field",
                            "component " + head + " requires one value up to 4096 UTF-8 bytes" );
            }
            else
            {
                component[head] = value;
            }

            continue;
        }

        if( head != "symbol" && head != "value" && head != "footprint" )
        {
            diagnostic( aResult, "error", "unknown_component_field",
                        "component supports symbol, value, footprint, datasheet, description, "
                        "property, dnp, and unit" );
            continue;
        }

        std::string value;

        if( !parseSingleValueForm( aDocument, child, value ) || value.empty() )
        {
            diagnostic( aResult, "error", "invalid_component_field",
                        "component symbol and value require one non-empty value; footprint "
                        "requires LIBRARY:ITEM or none" );
            continue;
        }

        if( !singletonFields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_component_field",
                        "component field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "footprint" && value == "none" )
        {
            component[head] = nullptr;
            continue;
        }

        if( head == "symbol" || head == "footprint" )
        {
            std::string nickname;

            if( !libraryIdNickname( value, nickname ) )
            {
                diagnostic( aResult, "error", "invalid_component_library_id",
                            "component " + head + " must use bounded LIBRARY:ITEM syntax" );
            }
        }

        component[head] = value;
    }

    for( const char* required : { "symbol", "value", "footprint" } )
    {
        if( !component.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_component_field",
                        "component " + reference + " is missing " + required );
        }
    }

    if( component["properties"].size() > 1024 )
    {
        diagnostic( aResult, "error", "too_many_component_properties",
                    "component " + reference + " may declare at most 1024 custom properties" );
    }

    static const std::set<std::string> MANDATORY_FIELD_NAMES = {
        "Reference", "Value", "Footprint", "Datasheet", "Description"
    };

    for( const JSON& unit : component["units"] )
    {
        if( !unit.is_object() || !unit.contains( "fields" ) || !unit["fields"].is_array() )
            continue;

        if( unit["fields"].size() > 1024 )
        {
            diagnostic( aResult, "error", "too_many_component_unit_field_layouts",
                        "component " + reference + " unit "
                                + std::to_string( unit.value( "number", 0 ) )
                                + " may declare at most 1024 field layouts" );
        }

        for( const JSON& layout : unit["fields"] )
        {
            const std::string name = layout.value( "name", "" );

            if( name.empty() )
                continue;

            const bool mandatory = MANDATORY_FIELD_NAMES.contains( name );

            if( !mandatory && !component["properties"].contains( name ) )
            {
                diagnostic( aResult, "error", "unresolved_component_unit_field_layout",
                            "component " + reference + " unit field " + name
                                    + " has no matching component property" );
            }

            if( mandatory && layout.value( "private", false ) )
            {
                diagnostic( aResult, "error", "private_mandatory_component_unit_field",
                            "component mandatory field " + name + " cannot be private" );
            }
        }
    }

    return component;
}


JSON compileNet( const DOCUMENT& aDocument, size_t aNode,
                 KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult,
                 std::vector<std::string>& aReferencedComponents,
                 std::set<std::string>& aConnectedPins, size_t& aPinConnections )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           name;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], name )
        || name.empty() || name.size() > MAX_IDENTIFIER_BYTES )
    {
        diagnostic( aResult, "error", "invalid_net", "net requires a bounded name" );
        return JSON::object();
    }

    JSON net = { { "name", name },
                 { "presentation", "labels" },
                 { "presentationExplicit", false },
                 { "pins", JSON::array() } };
    bool sawPresentation = false;

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t child = node.children[i];
        const std::string head = aDocument.ListHead( child );

        if( head == "presentation" )
        {
            std::string presentation;

            if( sawPresentation || !parseSingleValueForm( aDocument, child, presentation )
                || ( presentation != "wired" && presentation != "labels" ) )
            {
                diagnostic( aResult, "error", "invalid_net_presentation",
                            "net " + name
                                    + " presentation may occur once and must be wired or labels" );
            }
            else
            {
                net["presentation"] = presentation;
                net["presentationExplicit"] = true;
            }

            sawPresentation = true;
            continue;
        }

        const DOCUMENT::NODE& pin = aDocument.Nodes()[child];
        std::string reference;
        std::string unitText;
        std::string number;
        int64_t     unit = 0;

        if( head != "pin" || pin.children.size() != 4
            || !scalarText( aDocument, pin.children[1], reference )
            || !scalarText( aDocument, pin.children[2], unitText )
            || !scalarText( aDocument, pin.children[3], number )
            || !parseBoundedInteger( unitText, 1, 256, unit )
            || !validIdentifier( reference ) || number.empty() || number.size() > 64 )
        {
            diagnostic( aResult, "error", "invalid_net_field",
                        "net " + name
                                + " accepts one (presentation wired|labels) field and endpoints "
                                  "using (pin COMPONENT UNIT PIN_NUMBER)" );
            diagnostic( aResult, "error", "invalid_pin",
                        "net " + name
                                + " pin endpoints require component, unit 1 through 256, and a bounded pin number" );
            continue;
        }

        net["pins"].push_back( { { "component", reference },
                                 { "unit", unit },
                                 { "number", number } } );
        aReferencedComponents.emplace_back( reference );

        const std::string endpoint = reference + ":" + unitText + ":" + number;

        if( !aConnectedPins.emplace( endpoint ).second )
        {
            diagnostic( aResult, "error", "duplicate_pin_connection",
                        "pin " + endpoint + " is assigned to more than one net endpoint" );
        }

        ++aPinConnections;
    }

    if( net["pins"].size() < 2 )
    {
        diagnostic( aResult, "error", "underspecified_net",
                    "net " + name + " must connect at least two pins" );
    }

    return net;
}


JSON compileNoConnect( const DOCUMENT& aDocument, size_t aNode,
                       KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult,
                       std::vector<std::string>& aReferencedComponents,
                       std::set<std::string>& aConnectedPins, size_t& aPinConnections )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           reference;
    std::string           unitText;
    std::string           number;
    int64_t               unit = 0;

    if( node.children.size() != 4
        || !scalarText( aDocument, node.children[1], reference )
        || !scalarText( aDocument, node.children[2], unitText )
        || !scalarText( aDocument, node.children[3], number )
        || !validIdentifier( reference )
        || !parseBoundedInteger( unitText, 1, 256, unit )
        || number.empty() || number.size() > 64 )
    {
        diagnostic( aResult, "error", "invalid_no_connect",
                    "no_connect must use (no_connect COMPONENT UNIT PIN_NUMBER)" );
        return JSON::object();
    }

    const std::string endpoint = reference + ":" + unitText + ":" + number;

    if( !aConnectedPins.emplace( endpoint ).second )
    {
        diagnostic( aResult, "error", "duplicate_pin_connection",
                    "pin " + endpoint + " is assigned more than once" );
    }

    aReferencedComponents.emplace_back( reference );
    ++aPinConnections;
    return { { "component", reference }, { "unit", unit }, { "number", number } };
}


bool parseSchematicPoint( const DOCUMENT& aDocument, size_t aNode, JSON& aPoint )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string xText;
    std::string yText;
    int64_t x = 0;
    int64_t y = 0;

    if( node.children.size() != 3
        || !scalarText( aDocument, node.children[1], xText )
        || !scalarText( aDocument, node.children[2], yText )
        || !parseDistance( xText, x ) || !parseDistance( yText, y )
        || x < 0 || y < 0 || x > 2'000'000'000 || y > 2'000'000'000 )
    {
        return false;
    }

    aPoint = { { "xNm", x }, { "yNm", y } };
    return true;
}


bool parseSchematicStroke( const DOCUMENT& aDocument, size_t aNode, JSON& aStroke )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string widthText;
    std::string lineStyle;
    int64_t width = 0;
    static const std::set<std::string> STYLES = {
        "default", "solid", "dash", "dot", "dash_dot", "dash_dot_dot"
    };

    if( node.children.size() != 3
        || !scalarText( aDocument, node.children[1], widthText )
        || !scalarText( aDocument, node.children[2], lineStyle )
        || !STYLES.contains( lineStyle )
        || ( widthText != "default"
             && ( !parseDistance( widthText, width ) || width < 10'000
                  || width > 10'000'000 ) ) )
    {
        return false;
    }

    aStroke = { { "widthNm", widthText == "default" ? 0 : width },
                { "lineStyle", lineStyle } };
    return true;
}


JSON schematicNativeColor( const JSON& aColor )
{
    return {
        { "r", std::lround( aColor["r"].get<double>() * 255.0 ) },
        { "g", std::lround( aColor["g"].get<double>() * 255.0 ) },
        { "b", std::lround( aColor["b"].get<double>() * 255.0 ) },
        { "a", std::lround( aColor["a"].get<double>() * 255.0 ) }
    };
}


bool parseCompleteSchematicStroke( const DOCUMENT& aDocument, size_t aNode,
                                   bool aAllowHidden, JSON& aStroke )
{
    const DOCUMENT::NODE& stroke = aDocument.Nodes()[aNode];
    std::string widthText;
    std::string lineStyle;
    std::string colorText;
    int64_t width = 0;
    JSON color;
    static const std::set<std::string> STYLES = {
        "default", "solid", "dash", "dot", "dash_dot", "dash_dot_dot"
    };

    if( stroke.children.size() != 4
        || !scalarText( aDocument, stroke.children[1], widthText )
        || !scalarText( aDocument, stroke.children[2], lineStyle )
        || !scalarText( aDocument, stroke.children[3], colorText )
        || !STYLES.contains( lineStyle )
        || ( widthText == "none" && !aAllowHidden )
        || ( widthText != "none" && widthText != "default"
             && ( !parseDistance( widthText, width ) || width < 10'000
                  || width > 10'000'000 ) )
        || ( colorText != "default" && !parseHexColor( colorText, color ) ) )
    {
        return false;
    }

    JSON nativeColor = colorText == "default" ? JSON( nullptr )
                                                : schematicNativeColor( color );
    aStroke = {
        { "widthNm", widthText == "none" ? -1 : widthText == "default" ? 0 : width },
        { "lineStyle", lineStyle },
        { "color", std::move( nativeColor ) }
    };
    return true;
}


bool parseCompleteSchematicFill( const DOCUMENT& aDocument, size_t aNode, JSON& aFill )
{
    const DOCUMENT::NODE& fill = aDocument.Nodes()[aNode];
    std::string fillType;
    std::string colorText;
    JSON color;
    static const std::set<std::string> TYPES = {
        "none", "outline", "background", "color", "hatch", "reverse_hatch",
        "cross_hatch"
    };

    if( fill.children.size() != 3
        || !scalarText( aDocument, fill.children[1], fillType )
        || !scalarText( aDocument, fill.children[2], colorText )
        || !TYPES.contains( fillType )
        || ( ( fillType == "color" || fillType == "hatch"
               || fillType == "reverse_hatch" || fillType == "cross_hatch" )
                     ? !parseHexColor( colorText, color )
                     : colorText != "default" ) )
    {
        return false;
    }

    JSON nativeColor = colorText == "default" ? JSON( nullptr )
                                                : schematicNativeColor( color );
    aFill = { { "type", fillType }, { "color", std::move( nativeColor ) } };
    return true;
}


using SCHEMATIC_POINT = std::pair<int64_t, int64_t>;
using WIDE_SCHEMATIC_INTEGER = boost::multiprecision::int128_t;


WIDE_SCHEMATIC_INTEGER schematicOrientation( const SCHEMATIC_POINT& aStart,
                                             const SCHEMATIC_POINT& aEnd,
                                             const SCHEMATIC_POINT& aPoint )
{
    return static_cast<WIDE_SCHEMATIC_INTEGER>( aEnd.first - aStart.first )
                   * ( aPoint.second - aStart.second )
           - static_cast<WIDE_SCHEMATIC_INTEGER>( aEnd.second - aStart.second )
                     * ( aPoint.first - aStart.first );
}


bool schematicPointOnSegment( const SCHEMATIC_POINT& aPoint,
                              const SCHEMATIC_POINT& aStart,
                              const SCHEMATIC_POINT& aEnd )
{
    return schematicOrientation( aStart, aEnd, aPoint ) == 0
           && aPoint.first >= std::min( aStart.first, aEnd.first )
           && aPoint.first <= std::max( aStart.first, aEnd.first )
           && aPoint.second >= std::min( aStart.second, aEnd.second )
           && aPoint.second <= std::max( aStart.second, aEnd.second );
}


bool schematicSegmentsIntersect( const SCHEMATIC_POINT& aStart,
                                 const SCHEMATIC_POINT& aEnd,
                                 const SCHEMATIC_POINT& bStart,
                                 const SCHEMATIC_POINT& bEnd )
{
    const WIDE_SCHEMATIC_INTEGER a = schematicOrientation( aStart, aEnd, bStart );
    const WIDE_SCHEMATIC_INTEGER b = schematicOrientation( aStart, aEnd, bEnd );
    const WIDE_SCHEMATIC_INTEGER c = schematicOrientation( bStart, bEnd, aStart );
    const WIDE_SCHEMATIC_INTEGER d = schematicOrientation( bStart, bEnd, aEnd );

    if( ( ( a > 0 && b < 0 ) || ( a < 0 && b > 0 ) )
        && ( ( c > 0 && d < 0 ) || ( c < 0 && d > 0 ) ) )
    {
        return true;
    }

    return ( a == 0 && schematicPointOnSegment( bStart, aStart, aEnd ) )
           || ( b == 0 && schematicPointOnSegment( bEnd, aStart, aEnd ) )
           || ( c == 0 && schematicPointOnSegment( aStart, bStart, bEnd ) )
           || ( d == 0 && schematicPointOnSegment( aEnd, bStart, bEnd ) );
}


bool schematicPolygonIsSimple( const std::vector<SCHEMATIC_POINT>& aPoints )
{
    if( aPoints.size() < 3 )
        return false;

    WIDE_SCHEMATIC_INTEGER twiceArea = 0;

    for( size_t index = 0; index < aPoints.size(); ++index )
    {
        const SCHEMATIC_POINT& point = aPoints[index];
        const SCHEMATIC_POINT& next = aPoints[( index + 1 ) % aPoints.size()];
        twiceArea += static_cast<WIDE_SCHEMATIC_INTEGER>( point.first ) * next.second
                     - static_cast<WIDE_SCHEMATIC_INTEGER>( point.second ) * next.first;

        if( point == next )
            return false;
    }

    if( twiceArea == 0 )
        return false;

    for( size_t first = 0; first < aPoints.size(); ++first )
    {
        const size_t firstEnd = ( first + 1 ) % aPoints.size();

        for( size_t second = first + 1; second < aPoints.size(); ++second )
        {
            const size_t secondEnd = ( second + 1 ) % aPoints.size();

            if( first == second || firstEnd == second || secondEnd == first )
                continue;

            if( schematicSegmentsIntersect( aPoints[first], aPoints[firstEnd],
                                             aPoints[second], aPoints[secondEnd] ) )
            {
                return false;
            }
        }
    }

    return true;
}


bool schematicPointOnPolygonBoundary( const JSON& aPoint, const JSON& aPolygon )
{
    if( !aPoint.is_object() || !aPolygon.is_array() || aPolygon.size() < 3 )
        return false;

    const SCHEMATIC_POINT point = { aPoint.at( "xNm" ).get<int64_t>(),
                                    aPoint.at( "yNm" ).get<int64_t>() };

    for( size_t index = 0; index < aPolygon.size(); ++index )
    {
        const JSON& startJson = aPolygon[index];
        const JSON& endJson = aPolygon[( index + 1 ) % aPolygon.size()];
        const SCHEMATIC_POINT start = { startJson.at( "xNm" ).get<int64_t>(),
                                        startJson.at( "yNm" ).get<int64_t>() };
        const SCHEMATIC_POINT end = { endJson.at( "xNm" ).get<int64_t>(),
                                      endJson.at( "yNm" ).get<int64_t>() };

        if( schematicPointOnSegment( point, start, end ) )
            return true;
    }

    return false;
}


JSON compileSchematicLine( const DOCUMENT& aDocument, size_t aNode,
                           const std::string& aKind,
                           KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_" + aKind,
                    aKind + " requires a bounded stable ID" );
        return JSON::object();
    }

    JSON line = { { "kind", aKind }, { "id", id } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_" + aKind + "_field",
                        aKind + " field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" )
        {
            std::string sheet;

            if( !parseSingleValueForm( aDocument, child, sheet )
                || !validIdentifier( sheet ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_" + aKind + "_sheet",
                            aKind + " sheet must be a bounded sheet ID" );
            }
            else
            {
                line["sheet"] = sheet;
            }
        }
        else if( head == "from" || head == "to" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_" + aKind + "_point",
                            aKind + " " + head + " requires two distances from 0 to 2 m" );
            }
            else
            {
                line[head] = std::move( point );
            }
        }
        else if( head == "stroke" )
        {
            JSON stroke;

            if( !parseSchematicStroke( aDocument, child, stroke ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_" + aKind + "_stroke",
                            aKind + " stroke requires default or 0.01 mm through 10 mm and "
                                    "a native line style" );
            }
            else
            {
                line["stroke"] = std::move( stroke );
            }
        }
        else
        {
            diagnostic( aResult, "error", "unknown_schematic_" + aKind + "_field",
                        aKind + " supports sheet, from, to, and stroke" );
        }
    }

    for( const char* required : { "sheet", "from", "to", "stroke" } )
    {
        if( !line.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_" + aKind + "_field",
                        aKind + " " + id + " is missing " + required );
        }
    }

    if( line.contains( "from" ) && line.contains( "to" ) && line["from"] == line["to"] )
    {
        diagnostic( aResult, "error", "zero_length_schematic_" + aKind,
                    aKind + " " + id + " must have distinct endpoints" );
    }

    if( aKind == "bus_entry" && line.contains( "from" ) && line.contains( "to" ) )
    {
        const int64_t dx = line["to"]["xNm"].get<int64_t>()
                           - line["from"]["xNm"].get<int64_t>();
        const int64_t dy = line["to"]["yNm"].get<int64_t>()
                           - line["from"]["yNm"].get<int64_t>();

        if( dx == 0 || dy == 0 || std::abs( dx ) > 10'000'000
            || std::abs( dy ) > 10'000'000 )
        {
            diagnostic( aResult, "error", "invalid_schematic_bus_entry_geometry",
                        "bus_entry must be diagonal and at most 10 mm on each axis" );
        }
    }

    return line;
}


JSON compileSchematicRuleArea( const DOCUMENT& aDocument, size_t aNode,
                               KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_rule_area",
                    "rule_area requires a bounded stable ID" );
        return JSON::object();
    }

    JSON ruleArea = { { "kind", "rule_area" }, { "id", id } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_rule_area_field",
                        "rule_area field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" )
        {
            std::string sheet;

            if( !parseSingleValueForm( aDocument, child, sheet )
                || !validIdentifier( sheet ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_rule_area_sheet",
                            "rule_area sheet must be a bounded sheet ID" );
            }
            else
            {
                ruleArea["sheet"] = sheet;
            }

            continue;
        }

        if( head == "polygon" )
        {
            const DOCUMENT::NODE& polygon = aDocument.Nodes()[child];
            JSON points = JSON::array();
            std::vector<SCHEMATIC_POINT> geometry;
            std::set<SCHEMATIC_POINT> uniquePoints;
            bool valid = polygon.children.size() >= 4 && polygon.children.size() <= 1025;

            for( size_t pointIndex = 1; pointIndex < polygon.children.size(); ++pointIndex )
            {
                const size_t pointNode = polygon.children[pointIndex];
                JSON point;

                if( aDocument.ListHead( pointNode ) != "point"
                    || !parseSchematicPoint( aDocument, pointNode, point ) )
                {
                    valid = false;
                    continue;
                }

                const SCHEMATIC_POINT coordinate = {
                    point["xNm"].get<int64_t>(), point["yNm"].get<int64_t>()
                };
                valid = uniquePoints.emplace( coordinate ).second && valid;
                geometry.emplace_back( coordinate );
                points.emplace_back( std::move( point ) );
            }

            if( !valid || !schematicPolygonIsSimple( geometry ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_rule_area_polygon",
                            "rule_area polygon requires 3 through 1024 unique points forming "
                            "one non-self-intersecting, non-zero-area boundary" );
            }
            else
            {
                ruleArea["polygon"] = std::move( points );
            }

            continue;
        }

        if( head == "stroke" )
        {
            const DOCUMENT::NODE& stroke = aDocument.Nodes()[child];
            std::string widthText;
            std::string lineStyle;
            std::string colorText;
            int64_t width = 0;
            JSON color;
            static const std::set<std::string> STYLES = {
                "default", "solid", "dash", "dot", "dash_dot", "dash_dot_dot"
            };

            if( stroke.children.size() != 4
                || !scalarText( aDocument, stroke.children[1], widthText )
                || !scalarText( aDocument, stroke.children[2], lineStyle )
                || !scalarText( aDocument, stroke.children[3], colorText )
                || !STYLES.contains( lineStyle )
                || ( widthText != "default"
                     && ( !parseDistance( widthText, width ) || width < 10'000
                          || width > 10'000'000 ) )
                || ( colorText != "default" && !parseHexColor( colorText, color ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_rule_area_stroke",
                            "rule_area stroke requires default or 0.01 mm through 10 mm, a "
                            "native line style, and default or #RRGGBB[AA] color" );
            }
            else
            {
                JSON nativeColor = nullptr;

                if( colorText != "default" )
                {
                    nativeColor = {
                        { "r", std::lround( color["r"].get<double>() * 255.0 ) },
                        { "g", std::lround( color["g"].get<double>() * 255.0 ) },
                        { "b", std::lround( color["b"].get<double>() * 255.0 ) },
                        { "a", std::lround( color["a"].get<double>() * 255.0 ) }
                    };
                }

                ruleArea["stroke"] = { { "widthNm", widthText == "default" ? 0 : width },
                                         { "lineStyle", lineStyle },
                                         { "color", std::move( nativeColor ) } };
            }

            continue;
        }

        if( head == "fill" )
        {
            const DOCUMENT::NODE& fill = aDocument.Nodes()[child];
            std::string fillType;
            std::string colorText;
            JSON color;
            static const std::set<std::string> TYPES = {
                "none", "outline", "background", "color", "hatch", "reverse_hatch",
                "cross_hatch"
            };

            if( fill.children.size() != 3
                || !scalarText( aDocument, fill.children[1], fillType )
                || !scalarText( aDocument, fill.children[2], colorText )
                || !TYPES.contains( fillType )
                || ( ( fillType == "color" || fillType == "hatch"
                       || fillType == "reverse_hatch" || fillType == "cross_hatch" )
                             ? !parseHexColor( colorText, color )
                             : colorText != "default" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_rule_area_fill",
                            "rule_area fill requires none|outline|background with default, or "
                            "color|hatch|reverse_hatch|cross_hatch with #RRGGBB[AA]" );
            }
            else
            {
                JSON nativeColor = nullptr;

                if( colorText != "default" )
                {
                    nativeColor = {
                        { "r", std::lround( color["r"].get<double>() * 255.0 ) },
                        { "g", std::lround( color["g"].get<double>() * 255.0 ) },
                        { "b", std::lround( color["b"].get<double>() * 255.0 ) },
                        { "a", std::lround( color["a"].get<double>() * 255.0 ) }
                    };
                }

                ruleArea["fill"] = { { "type", fillType },
                                       { "color", std::move( nativeColor ) } };
            }

            continue;
        }

        if( head == "exclude_from_sim" || head == "exclude_from_bom"
            || head == "exclude_from_board" || head == "dnp" )
        {
            std::string boolean;

            if( !parseSingleValueForm( aDocument, child, boolean )
                || ( boolean != "true" && boolean != "false" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_rule_area_" + head,
                            "rule_area " + head + " must be true or false" );
            }
            else
            {
                ruleArea[head] = boolean == "true";
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_rule_area_field",
                    "rule_area supports sheet, polygon, stroke, fill, exclude_from_sim, "
                    "exclude_from_bom, exclude_from_board, and dnp" );
    }

    for( const char* required : { "sheet", "polygon", "stroke", "fill", "exclude_from_sim",
                                  "exclude_from_bom", "exclude_from_board", "dnp" } )
    {
        if( !ruleArea.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_rule_area_field",
                        "rule_area " + id + " is missing " + required );
        }
    }

    return ruleArea;
}


JSON compileSchematicJunction( const DOCUMENT& aDocument, size_t aNode,
                               KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_junction",
                    "junction requires a bounded stable ID" );
        return JSON::object();
    }

    JSON junction = { { "kind", "junction" }, { "id", id } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_junction_field",
                        "junction field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" )
        {
            std::string sheet;

            if( !parseSingleValueForm( aDocument, child, sheet )
                || !validIdentifier( sheet ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_junction_sheet",
                            "junction sheet must be a bounded sheet ID" );
            }
            else
            {
                junction["sheet"] = sheet;
            }
        }
        else if( head == "at" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_junction_position",
                            "junction at requires two distances from 0 to 2 m" );
            }
            else
            {
                junction["position"] = std::move( point );
            }
        }
        else if( head == "diameter" )
        {
            std::string value;
            int64_t diameter = 0;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "auto"
                     && ( !parseDistance( value, diameter ) || diameter < 10'000
                          || diameter > 10'000'000 ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_junction_diameter",
                            "junction diameter requires auto or 0.01 mm through 10 mm" );
            }
            else
            {
                junction["diameterNm"] = value == "auto" ? 0 : diameter;
            }
        }
        else if( head == "color" )
        {
            std::string value;
            JSON color;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "default" && !parseHexColor( value, color ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_junction_color",
                            "junction color requires default or #RRGGBB[AA]" );
            }
            else if( value == "default" )
            {
                junction["color"] = nullptr;
            }
            else
            {
                junction["color"] = {
                    { "r", std::lround( color["r"].get<double>() * 255.0 ) },
                    { "g", std::lround( color["g"].get<double>() * 255.0 ) },
                    { "b", std::lround( color["b"].get<double>() * 255.0 ) },
                    { "a", std::lround( color["a"].get<double>() * 255.0 ) }
                };
            }
        }
        else
        {
            diagnostic( aResult, "error", "unknown_schematic_junction_field",
                        "junction supports sheet, at, diameter, and color" );
        }
    }

    for( const char* required : { "sheet", "position", "diameterNm", "color" } )
    {
        if( !junction.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_junction_field",
                        "junction " + id + " is missing " + required );
        }
    }

    return junction;
}


JSON compileSchematicLabel( const DOCUMENT& aDocument, size_t aNode,
                            KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_label",
                    "label requires a bounded stable ID" );
        return JSON::object();
    }

    JSON label = { { "kind", "label" }, { "id", id } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_label_field",
                        "label field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" || head == "net" || head == "scope" || head == "shape" )
        {
            std::string value;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( head == "sheet" && !validIdentifier( value ) )
                || ( head == "net"
                     && ( value.empty() || value.size() > MAX_IDENTIFIER_BYTES ) )
                || ( head == "scope" && value != "local" && value != "global" )
                || ( head == "shape" && value != "none" && value != "input"
                     && value != "output" && value != "bidirectional"
                     && value != "tri_state" && value != "passive" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_label_" + head,
                            "label " + head + " has an invalid semantic value" );
            }
            else
            {
                label[head] = value;
            }

            continue;
        }

        if( head == "at" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_label_position",
                            "label at requires two distances from 0 to 2 m" );
            }
            else
            {
                label["position"] = std::move( point );
            }

            continue;
        }

        if( head == "rotation" )
        {
            std::string value;
            double degrees = 0.0;

            if( !parseSingleValueForm( aDocument, child, value )
                || !parseFiniteDecimal( value, degrees, "deg" )
                || ( degrees != 0.0 && degrees != 90.0 && degrees != 180.0
                     && degrees != 270.0 ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_label_rotation",
                            "label rotation must be 0deg, 90deg, 180deg, or 270deg" );
            }
            else
            {
                label["rotationDegrees"] = static_cast<int>( degrees );
            }

            continue;
        }

        if( head == "size" )
        {
            JSON size;

            if( !parseSchematicPoint( aDocument, child, size )
                || size["xNm"].get<int64_t>() < 100'000
                || size["yNm"].get<int64_t>() < 100'000
                || size["xNm"].get<int64_t>() > 50'000'000
                || size["yNm"].get<int64_t>() > 50'000'000 )
            {
                diagnostic( aResult, "error", "invalid_schematic_label_size",
                            "label size requires two dimensions from 0.1 mm through 50 mm" );
            }
            else
            {
                label["size"] = std::move( size );
            }

            continue;
        }

        if( head == "thickness" )
        {
            std::string value;
            int64_t thickness = 0;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "auto"
                     && ( !parseDistance( value, thickness ) || thickness < 10'000
                          || thickness > 10'000'000 ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_label_thickness",
                            "label thickness requires auto or 0.01 mm through 10 mm" );
            }
            else
            {
                label["thicknessNm"] = value == "auto" ? 0 : thickness;
            }

            continue;
        }

        if( head == "justify" )
        {
            const DOCUMENT::NODE& justify = aDocument.Nodes()[child];
            std::string horizontal;
            std::string vertical;

            if( justify.children.size() != 3
                || !scalarText( aDocument, justify.children[1], horizontal )
                || !scalarText( aDocument, justify.children[2], vertical )
                || ( horizontal != "left" && horizontal != "center"
                     && horizontal != "right" )
                || ( vertical != "top" && vertical != "center" && vertical != "bottom" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_label_justify",
                            "label justify requires left|center|right and top|center|bottom" );
            }
            else
            {
                label["justify"] = { { "horizontal", horizontal },
                                     { "vertical", vertical } };
            }

            continue;
        }

        if( head == "bold" || head == "italic" )
        {
            std::string value;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "true" && value != "false" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_label_" + head,
                            "label " + head + " must be true or false" );
            }
            else
            {
                label[head] = value == "true";
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_label_field",
                    "label supports sheet, scope, net, at, rotation, shape, size, thickness, "
                    "justify, bold, and italic" );
    }

    for( const char* required : { "sheet", "scope", "net", "position", "rotationDegrees",
                                  "shape", "size", "thicknessNm", "justify", "bold",
                                  "italic" } )
    {
        if( !label.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_label_field",
                        "label " + id + " is missing " + required );
        }
    }

    if( label.contains( "scope" ) && label.contains( "shape" )
        && ( ( label["scope"] == "local" && label["shape"] != "none" )
             || ( label["scope"] == "global" && label["shape"] == "none" ) ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_label_scope_shape",
                    "local labels require shape none; global labels require an electrical shape" );
    }

    return label;
}


JSON compileSchematicDirectiveProperty(
        const DOCUMENT& aDocument, size_t aNode,
        KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult,
        const std::string& aDirectiveId )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string name;
    std::string value;

    if( node.children.size() < 3 || !scalarText( aDocument, node.children[1], name )
        || !scalarText( aDocument, node.children[2], value ) || name.empty()
        || name.size() > MAX_IDENTIFIER_BYTES
        || value.size() > MAX_SCHEMATIC_PROPERTY_BYTES )
    {
        diagnostic( aResult, "error", "invalid_schematic_directive_property",
                    "directive " + aDirectiveId
                            + " property requires a 1..128-byte name and a value up to 4096 bytes" );
        return JSON::object();
    }

    JSON property = { { "name", name }, { "value", value } };
    std::set<std::string> fields;

    for( size_t index = 3; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_directive_property_field",
                        "directive " + aDirectiveId + " property " + name + " field '"
                                + head + "' occurs more than once" );
            continue;
        }

        if( head == "at" || head == "size" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point )
                || ( head == "size"
                     && ( point["xNm"].get<int64_t>() < 100'000
                          || point["yNm"].get<int64_t>() < 100'000
                          || point["xNm"].get<int64_t>() > 50'000'000
                          || point["yNm"].get<int64_t>() > 50'000'000 ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_property_" + head,
                            head == "at"
                                    ? "directive property at requires two distances from 0 to 2 m"
                                    : "directive property size requires two dimensions from 0.1 mm through 50 mm" );
            }
            else
            {
                property[head == "at" ? "position" : "size"] = std::move( point );
            }

            continue;
        }

        if( head == "rotation" )
        {
            std::string rotation;
            double degrees = 0.0;

            if( !parseSingleValueForm( aDocument, child, rotation )
                || !parseFiniteDecimal( rotation, degrees, "deg" )
                || ( degrees != 0.0 && degrees != 90.0 && degrees != 180.0
                     && degrees != 270.0 ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_property_rotation",
                            "directive property rotation must be 0deg, 90deg, 180deg, or 270deg" );
            }
            else
            {
                property["rotationDegrees"] = static_cast<int>( degrees );
            }

            continue;
        }

        if( head == "thickness" )
        {
            std::string thicknessText;
            int64_t thickness = 0;

            if( !parseSingleValueForm( aDocument, child, thicknessText )
                || ( thicknessText != "auto"
                     && ( !parseDistance( thicknessText, thickness ) || thickness < 10'000
                          || thickness > 10'000'000 ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_property_thickness",
                            "directive property thickness requires auto or 0.01 mm through 10 mm" );
            }
            else
            {
                property["thicknessNm"] = thicknessText == "auto" ? 0 : thickness;
            }

            continue;
        }

        if( head == "justify" )
        {
            const DOCUMENT::NODE& justify = aDocument.Nodes()[child];
            std::string horizontal;
            std::string vertical;

            if( justify.children.size() != 3
                || !scalarText( aDocument, justify.children[1], horizontal )
                || !scalarText( aDocument, justify.children[2], vertical )
                || ( horizontal != "left" && horizontal != "center"
                     && horizontal != "right" )
                || ( vertical != "top" && vertical != "center" && vertical != "bottom" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_property_justify",
                            "directive property justify requires left|center|right and top|center|bottom" );
            }
            else
            {
                property["justify"] = { { "horizontal", horizontal },
                                         { "vertical", vertical } };
            }

            continue;
        }

        if( head == "bold" || head == "italic" || head == "visible" )
        {
            std::string boolean;

            if( !parseSingleValueForm( aDocument, child, boolean )
                || ( boolean != "true" && boolean != "false" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_property_" + head,
                            "directive property " + head + " must be true or false" );
            }
            else
            {
                property[head] = boolean == "true";
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_directive_property_field",
                    "directive property supports at, rotation, size, thickness, justify, bold, "
                    "italic, and visible" );
    }

    for( const char* required : { "position", "rotationDegrees", "size", "thicknessNm",
                                  "justify", "bold", "italic", "visible" } )
    {
        if( !property.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_directive_property_field",
                        "directive " + aDirectiveId + " property " + name + " is missing "
                                + required );
        }
    }

    return property;
}


JSON compileSchematicDirective( const DOCUMENT& aDocument, size_t aNode,
                                KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_directive",
                    "directive requires a bounded stable ID" );
        return JSON::object();
    }

    JSON directive = { { "kind", "directive" }, { "id", id },
                       { "properties", JSON::array() } };
    std::set<std::string> fields;
    std::set<std::string> propertyNames;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "property" )
        {
            JSON property = compileSchematicDirectiveProperty( aDocument, child, aResult, id );
            const std::string name = property.value( "name", "" );

            if( !name.empty() && !propertyNames.emplace( name ).second )
            {
                diagnostic( aResult, "error", "duplicate_schematic_directive_property",
                            "directive " + id + " property " + name
                                    + " occurs more than once" );
            }

            directive["properties"].emplace_back( std::move( property ) );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_directive_field",
                        "directive field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" || head == "shape" )
        {
            std::string value;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( head == "sheet" && !validIdentifier( value ) )
                || ( head == "shape" && value != "dot" && value != "round"
                     && value != "diamond" && value != "rectangle" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_" + head,
                            "directive " + head + " has an invalid semantic value" );
            }
            else
            {
                directive[head] = value;
            }

            continue;
        }

        if( head == "target" )
        {
            const DOCUMENT::NODE& target = aDocument.Nodes()[child];
            std::string kind;
            std::string name;

            if( target.children.size() != 3
                || !scalarText( aDocument, target.children[1], kind )
                || !scalarText( aDocument, target.children[2], name )
                || ( kind != "net" && kind != "rule_area" )
                || name.empty() || name.size() > MAX_IDENTIFIER_BYTES )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_target",
                            "directive target requires net|rule_area and a bounded declared name" );
            }
            else
            {
                directive["target"] = { { "kind", kind }, { "name", name } };
            }

            continue;
        }

        if( head == "at" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_position",
                            "directive at requires two distances from 0 to 2 m" );
            }
            else
            {
                directive["position"] = std::move( point );
            }

            continue;
        }

        if( head == "rotation" )
        {
            std::string rotation;
            double degrees = 0.0;

            if( !parseSingleValueForm( aDocument, child, rotation )
                || !parseFiniteDecimal( rotation, degrees, "deg" )
                || ( degrees != 0.0 && degrees != 90.0 && degrees != 180.0
                     && degrees != 270.0 ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_rotation",
                            "directive rotation must be 0deg, 90deg, 180deg, or 270deg" );
            }
            else
            {
                directive["rotationDegrees"] = static_cast<int>( degrees );
            }

            continue;
        }

        if( head == "length" )
        {
            std::string lengthText;
            int64_t length = 0;

            if( !parseSingleValueForm( aDocument, child, lengthText )
                || !parseDistance( lengthText, length ) || length < 100'000
                || length > 50'000'000 )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_length",
                            "directive length requires 0.1 mm through 50 mm" );
            }
            else
            {
                directive["lengthNm"] = length;
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_directive_field",
                    "directive supports sheet, target, at, rotation, shape, length, and property" );
    }

    for( const char* required : { "sheet", "target", "position", "rotationDegrees", "shape",
                                  "lengthNm" } )
    {
        if( !directive.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_directive_field",
                        "directive " + id + " is missing " + required );
        }
    }

    if( directive["properties"].empty() )
    {
        diagnostic( aResult, "error", "missing_schematic_directive_property",
                    "directive " + id + " requires at least one property" );
    }
    else if( directive["properties"].size() > 64 )
    {
        diagnostic( aResult, "error", "too_many_schematic_directive_properties",
                    "directive " + id + " supports at most 64 properties" );
    }

    return directive;
}


JSON compileSchematicText( const DOCUMENT& aDocument, size_t aNode,
                           KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string content;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], content )
        || content.size() > MAX_SCHEMATIC_TEXT_BYTES )
    {
        diagnostic( aResult, "error", "invalid_schematic_text",
                    "text requires content up to 65536 UTF-8 bytes" );
        return JSON::object();
    }

    JSON text = { { "kind", "text" }, { "content", content } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_text_field",
                        "text field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "id" || head == "sheet" )
        {
            std::string value;

            if( !parseSingleValueForm( aDocument, child, value )
                || !validIdentifier( value ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_" + head,
                            "text " + head + " must be a bounded identifier" );
            }
            else
            {
                text[head] = value;
            }

            continue;
        }

        if( head == "at" || head == "size" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point )
                || ( head == "size"
                     && ( point["xNm"].get<int64_t>() < 100'000
                          || point["yNm"].get<int64_t>() < 100'000
                          || point["xNm"].get<int64_t>() > 50'000'000
                          || point["yNm"].get<int64_t>() > 50'000'000 ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_" + head,
                            head == "at"
                                    ? "text at requires two distances from 0 to 2 m"
                                    : "text size requires two dimensions from 0.1 mm through 50 mm" );
            }
            else
            {
                text[head == "at" ? "position" : "size"] = std::move( point );
            }

            continue;
        }

        if( head == "rotation" )
        {
            std::string rotation;
            double degrees = 0.0;

            if( !parseSingleValueForm( aDocument, child, rotation )
                || !parseFiniteDecimal( rotation, degrees, "deg" )
                || degrees < 0.0 || degrees >= 360.0 )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_rotation",
                            "text rotation requires an angle from 0deg through less than 360deg" );
            }
            else
            {
                text["rotationDegrees"] = degrees;
            }

            continue;
        }

        if( head == "font" )
        {
            std::string font;

            if( !parseSingleValueForm( aDocument, child, font ) || font.empty()
                || font.size() > MAX_FONT_NAME_BYTES
                || font.find( '\0' ) != std::string::npos )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_font",
                            "text font requires stroke or a name up to 256 UTF-8 bytes" );
            }
            else
            {
                text["font"] = font;
            }

            continue;
        }

        if( head == "line_spacing" )
        {
            std::string spacingText;
            double spacing = 0.0;

            if( !parseSingleValueForm( aDocument, child, spacingText )
                || !parseFiniteDecimal( spacingText, spacing, "" )
                || spacing < 0.5 || spacing > 5.0 )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_line_spacing",
                            "text line_spacing requires a finite value from 0.5 through 5" );
            }
            else
            {
                text["lineSpacing"] = spacing;
            }

            continue;
        }

        if( head == "thickness" )
        {
            std::string thicknessText;
            int64_t thickness = 0;

            if( !parseSingleValueForm( aDocument, child, thicknessText )
                || ( thicknessText != "auto"
                     && ( !parseDistance( thicknessText, thickness ) || thickness < 10'000
                          || thickness > 10'000'000 ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_thickness",
                            "text thickness requires auto or 0.01 mm through 10 mm" );
            }
            else
            {
                text["thicknessNm"] = thicknessText == "auto" ? 0 : thickness;
            }

            continue;
        }

        if( head == "color" )
        {
            std::string colorText;
            JSON color;

            if( !parseSingleValueForm( aDocument, child, colorText )
                || ( colorText != "default" && !parseHexColor( colorText, color ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_color",
                            "text color requires default or #RRGGBB[AA]" );
            }
            else if( colorText == "default" )
            {
                text["color"] = nullptr;
            }
            else
            {
                text["color"] = {
                    { "r", std::lround( color["r"].get<double>() * 255.0 ) },
                    { "g", std::lround( color["g"].get<double>() * 255.0 ) },
                    { "b", std::lround( color["b"].get<double>() * 255.0 ) },
                    { "a", std::lround( color["a"].get<double>() * 255.0 ) }
                };
            }

            continue;
        }

        if( head == "justify" )
        {
            const DOCUMENT::NODE& justify = aDocument.Nodes()[child];
            std::string horizontal;
            std::string vertical;

            if( justify.children.size() != 3
                || !scalarText( aDocument, justify.children[1], horizontal )
                || !scalarText( aDocument, justify.children[2], vertical )
                || ( horizontal != "left" && horizontal != "center"
                     && horizontal != "right" )
                || ( vertical != "top" && vertical != "center" && vertical != "bottom" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_justify",
                            "text justify requires left|center|right and top|center|bottom" );
            }
            else
            {
                text["justify"] = { { "horizontal", horizontal },
                                     { "vertical", vertical } };
            }

            continue;
        }

        if( head == "hyperlink" )
        {
            std::string hyperlink;

            if( !parseSingleValueForm( aDocument, child, hyperlink )
                || hyperlink.size() > MAX_HYPERLINK_BYTES
                || hyperlink.find( '\0' ) != std::string::npos
                || hyperlink.find( '\r' ) != std::string::npos
                || hyperlink.find( '\n' ) != std::string::npos )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_hyperlink",
                            "text hyperlink requires none or one line up to 2048 UTF-8 bytes" );
            }
            else
            {
                text["hyperlink"] = hyperlink == "none" ? "" : hyperlink;
            }

            continue;
        }

        if( head == "exclude_from_sim" || head == "mirror" || head == "bold"
            || head == "italic" )
        {
            std::string boolean;

            if( !parseSingleValueForm( aDocument, child, boolean )
                || ( boolean != "true" && boolean != "false" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_" + head,
                            "text " + head + " must be true or false" );
            }
            else
            {
                text[head] = boolean == "true";
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_text_field",
                    "text supports id, sheet, at, rotation, exclude_from_sim, size, font, "
                    "line_spacing, thickness, color, justify, mirror, bold, italic, and hyperlink" );
    }

    for( const char* required : { "id", "sheet", "position", "rotationDegrees",
                                  "exclude_from_sim", "size", "font", "lineSpacing",
                                  "thicknessNm", "color", "justify", "mirror", "bold",
                                  "italic", "hyperlink" } )
    {
        if( !text.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_text_field",
                        "text is missing " + std::string( required ) );
        }
    }

    return text;
}


JSON compileSchematicTextBox( const DOCUMENT& aDocument, size_t aNode,
                              KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string content;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], content )
        || content.size() > MAX_SCHEMATIC_TEXT_BYTES )
    {
        diagnostic( aResult, "error", "invalid_schematic_text_box",
                    "text_box requires content up to 65536 UTF-8 bytes" );
        return JSON::object();
    }

    JSON textBox = { { "kind", "text_box" }, { "content", content } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_text_box_field",
                        "text_box field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "id" || head == "sheet" )
        {
            std::string value;

            if( !parseSingleValueForm( aDocument, child, value )
                || !validIdentifier( value ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_box_" + head,
                            "text_box " + head + " must be a bounded identifier" );
            }
            else
            {
                textBox[head] = value;
            }

            continue;
        }

        if( head == "at" || head == "box_size" || head == "text_size" )
        {
            JSON point;
            const bool isPosition = head == "at";
            constexpr int64_t minimum = 100'000;
            const int64_t maximum = head == "box_size" ? 2'000'000'000 : 50'000'000;

            if( !parseSchematicPoint( aDocument, child, point )
                || ( !isPosition
                     && ( point["xNm"].get<int64_t>() < minimum
                          || point["yNm"].get<int64_t>() < minimum
                          || point["xNm"].get<int64_t>() > maximum
                          || point["yNm"].get<int64_t>() > maximum ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_box_" + head,
                            isPosition
                                    ? "text_box at requires two distances from 0 to 2 m"
                            : head == "box_size"
                                    ? "text_box box_size requires two dimensions from 0.1 mm through 2 m"
                                    : "text_box text_size requires two dimensions from 0.1 mm through 50 mm" );
            }
            else
            {
                textBox[isPosition ? "position" : head == "box_size" ? "boxSize" : "size"] =
                        std::move( point );
            }

            continue;
        }

        if( head == "margins" )
        {
            const DOCUMENT::NODE& margins = aDocument.Nodes()[child];
            JSON values = JSON::array();
            bool valid = margins.children.size() == 5;

            for( size_t marginIndex = 1; marginIndex < margins.children.size(); ++marginIndex )
            {
                std::string valueText;
                int64_t value = 0;

                if( !scalarText( aDocument, margins.children[marginIndex], valueText )
                    || !parseDistance( valueText, value ) || value < 0
                    || value > 2'000'000'000 )
                {
                    valid = false;
                }

                values.push_back( value );
            }

            if( !valid )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_box_margins",
                            "text_box margins requires left, top, right, and bottom distances from 0 to 2 m" );
            }
            else
            {
                textBox["margins"] = { { "leftNm", values[0] }, { "topNm", values[1] },
                                         { "rightNm", values[2] },
                                         { "bottomNm", values[3] } };
            }

            continue;
        }

        if( head == "rotation" )
        {
            std::string rotation;
            double degrees = 0.0;

            if( !parseSingleValueForm( aDocument, child, rotation )
                || !parseFiniteDecimal( rotation, degrees, "deg" )
                || degrees < 0.0 || degrees >= 360.0 )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_box_rotation",
                            "text_box rotation requires an angle from 0deg through less than 360deg" );
            }
            else
            {
                textBox["rotationDegrees"] = degrees;
            }

            continue;
        }

        if( head == "stroke" )
        {
            const DOCUMENT::NODE& stroke = aDocument.Nodes()[child];
            std::string widthText;
            std::string lineStyle;
            std::string colorText;
            int64_t width = 0;
            JSON color;
            static const std::set<std::string> STYLES = {
                "default", "solid", "dash", "dot", "dash_dot", "dash_dot_dot"
            };

            if( stroke.children.size() != 4
                || !scalarText( aDocument, stroke.children[1], widthText )
                || !scalarText( aDocument, stroke.children[2], lineStyle )
                || !scalarText( aDocument, stroke.children[3], colorText )
                || !STYLES.contains( lineStyle )
                || ( widthText != "none" && widthText != "default"
                     && ( !parseDistance( widthText, width ) || width < 10'000
                          || width > 10'000'000 ) )
                || ( colorText != "default" && !parseHexColor( colorText, color ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_box_stroke",
                            "text_box stroke requires none, default, or 0.01 mm through 10 mm; a native line style; and default or #RRGGBB[AA] color" );
            }
            else
            {
                JSON nativeColor = nullptr;

                if( colorText != "default" )
                {
                    nativeColor = {
                        { "r", std::lround( color["r"].get<double>() * 255.0 ) },
                        { "g", std::lround( color["g"].get<double>() * 255.0 ) },
                        { "b", std::lround( color["b"].get<double>() * 255.0 ) },
                        { "a", std::lround( color["a"].get<double>() * 255.0 ) }
                    };
                }

                textBox["stroke"] = {
                    { "widthNm", widthText == "none" ? -1
                                                       : widthText == "default" ? 0 : width },
                    { "lineStyle", lineStyle },
                    { "color", std::move( nativeColor ) }
                };
            }

            continue;
        }

        if( head == "fill" )
        {
            const DOCUMENT::NODE& fill = aDocument.Nodes()[child];
            std::string fillType;
            std::string colorText;
            JSON color;
            static const std::set<std::string> TYPES = {
                "none", "outline", "background", "color", "hatch", "reverse_hatch",
                "cross_hatch"
            };

            if( fill.children.size() != 3
                || !scalarText( aDocument, fill.children[1], fillType )
                || !scalarText( aDocument, fill.children[2], colorText )
                || !TYPES.contains( fillType )
                || ( ( fillType == "color" || fillType == "hatch"
                       || fillType == "reverse_hatch" || fillType == "cross_hatch" )
                             ? !parseHexColor( colorText, color )
                             : colorText != "default" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_box_fill",
                            "text_box fill requires none|outline|background with default, or color|hatch|reverse_hatch|cross_hatch with #RRGGBB[AA]" );
            }
            else
            {
                JSON nativeColor = nullptr;

                if( colorText != "default" )
                {
                    nativeColor = {
                        { "r", std::lround( color["r"].get<double>() * 255.0 ) },
                        { "g", std::lround( color["g"].get<double>() * 255.0 ) },
                        { "b", std::lround( color["b"].get<double>() * 255.0 ) },
                        { "a", std::lround( color["a"].get<double>() * 255.0 ) }
                    };
                }

                textBox["fill"] = { { "type", fillType },
                                      { "color", std::move( nativeColor ) } };
            }

            continue;
        }

        if( head == "font" )
        {
            std::string font;

            if( !parseSingleValueForm( aDocument, child, font ) || font.empty()
                || font.size() > MAX_FONT_NAME_BYTES
                || font.find( '\0' ) != std::string::npos )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_box_font",
                            "text_box font requires stroke or a name up to 256 UTF-8 bytes" );
            }
            else
            {
                textBox["font"] = font;
            }

            continue;
        }

        if( head == "line_spacing" )
        {
            std::string spacingText;
            double spacing = 0.0;

            if( !parseSingleValueForm( aDocument, child, spacingText )
                || !parseFiniteDecimal( spacingText, spacing, "" )
                || spacing < 0.5 || spacing > 5.0 )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_box_line_spacing",
                            "text_box line_spacing requires a finite value from 0.5 through 5" );
            }
            else
            {
                textBox["lineSpacing"] = spacing;
            }

            continue;
        }

        if( head == "thickness" )
        {
            std::string thicknessText;
            int64_t thickness = 0;

            if( !parseSingleValueForm( aDocument, child, thicknessText )
                || ( thicknessText != "auto"
                     && ( !parseDistance( thicknessText, thickness ) || thickness < 10'000
                          || thickness > 10'000'000 ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_box_thickness",
                            "text_box thickness requires auto or 0.01 mm through 10 mm" );
            }
            else
            {
                textBox["thicknessNm"] = thicknessText == "auto" ? 0 : thickness;
            }

            continue;
        }

        if( head == "color" )
        {
            std::string colorText;
            JSON color;

            if( !parseSingleValueForm( aDocument, child, colorText )
                || ( colorText != "default" && !parseHexColor( colorText, color ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_box_color",
                            "text_box color requires default or #RRGGBB[AA]" );
            }
            else if( colorText == "default" )
            {
                textBox["color"] = nullptr;
            }
            else
            {
                textBox["color"] = {
                    { "r", std::lround( color["r"].get<double>() * 255.0 ) },
                    { "g", std::lround( color["g"].get<double>() * 255.0 ) },
                    { "b", std::lround( color["b"].get<double>() * 255.0 ) },
                    { "a", std::lround( color["a"].get<double>() * 255.0 ) }
                };
            }

            continue;
        }

        if( head == "justify" )
        {
            const DOCUMENT::NODE& justify = aDocument.Nodes()[child];
            std::string horizontal;
            std::string vertical;

            if( justify.children.size() != 3
                || !scalarText( aDocument, justify.children[1], horizontal )
                || !scalarText( aDocument, justify.children[2], vertical )
                || ( horizontal != "left" && horizontal != "center"
                     && horizontal != "right" )
                || ( vertical != "top" && vertical != "center" && vertical != "bottom" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_box_justify",
                            "text_box justify requires left|center|right and top|center|bottom" );
            }
            else
            {
                textBox["justify"] = { { "horizontal", horizontal },
                                        { "vertical", vertical } };
            }

            continue;
        }

        if( head == "hyperlink" )
        {
            std::string hyperlink;

            if( !parseSingleValueForm( aDocument, child, hyperlink )
                || hyperlink.size() > MAX_HYPERLINK_BYTES
                || hyperlink.find( '\0' ) != std::string::npos
                || hyperlink.find( '\r' ) != std::string::npos
                || hyperlink.find( '\n' ) != std::string::npos )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_box_hyperlink",
                            "text_box hyperlink requires none or one line up to 2048 UTF-8 bytes" );
            }
            else
            {
                textBox["hyperlink"] = hyperlink == "none" ? "" : hyperlink;
            }

            continue;
        }

        if( head == "exclude_from_sim" || head == "mirror" || head == "bold"
            || head == "italic" )
        {
            std::string boolean;

            if( !parseSingleValueForm( aDocument, child, boolean )
                || ( boolean != "true" && boolean != "false" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_text_box_" + head,
                            "text_box " + head + " must be true or false" );
            }
            else
            {
                textBox[head] = boolean == "true";
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_text_box_field",
                    "text_box supports id, sheet, at, rotation, box_size, margins, exclude_from_sim, stroke, fill, text_size, font, line_spacing, thickness, color, justify, mirror, bold, italic, and hyperlink" );
    }

    for( const char* required : { "id", "sheet", "position", "rotationDegrees", "boxSize",
                                  "margins", "exclude_from_sim", "stroke", "fill", "size",
                                  "font", "lineSpacing", "thicknessNm", "color", "justify",
                                  "mirror", "bold", "italic", "hyperlink" } )
    {
        if( !textBox.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_text_box_field",
                        "text_box is missing " + std::string( required ) );
        }
    }

    if( textBox.contains( "position" ) && textBox.contains( "boxSize" ) )
    {
        const int64_t right = textBox["position"]["xNm"].get<int64_t>()
                              + textBox["boxSize"]["xNm"].get<int64_t>();
        const int64_t bottom = textBox["position"]["yNm"].get<int64_t>()
                               + textBox["boxSize"]["yNm"].get<int64_t>();

        if( right > 2'000'000'000 || bottom > 2'000'000'000 )
        {
            diagnostic( aResult, "error", "invalid_schematic_text_box_extent",
                        "text_box at plus box_size must remain within the 2 m schematic coordinate range" );
        }
    }

    return textBox;
}


JSON compileSchematicGraphic( const DOCUMENT& aDocument, size_t aNode,
                              const std::string& aKind,
                              KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_" + aKind,
                    aKind + " requires a bounded stable ID" );
        return JSON::object();
    }

    JSON graphic = { { "kind", aKind }, { "id", id } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_graphic_field",
                        aKind + " field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" )
        {
            std::string sheet;

            if( !parseSingleValueForm( aDocument, child, sheet )
                || !validIdentifier( sheet ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_graphic_sheet",
                            aKind + " sheet must be a bounded sheet ID" );
            }
            else
            {
                graphic["sheet"] = sheet;
            }

            continue;
        }

        if( head == "stroke" )
        {
            JSON stroke;

            if( !parseCompleteSchematicStroke( aDocument, child, true, stroke ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_graphic_stroke",
                            aKind + " stroke requires none, default, or 0.01 mm through 10 mm; a native line style; and default or #RRGGBB[AA] color" );
            }
            else
            {
                graphic["stroke"] = std::move( stroke );
            }

            continue;
        }

        if( head == "fill" )
        {
            JSON fill;

            if( !parseCompleteSchematicFill( aDocument, child, fill ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_graphic_fill",
                            aKind + " fill requires none|outline|background with default, or color|hatch|reverse_hatch|cross_hatch with #RRGGBB[AA]" );
            }
            else
            {
                graphic["fill"] = std::move( fill );
            }

            continue;
        }

        if( head == "points" && ( aKind == "polyline" || aKind == "bezier" ) )
        {
            const DOCUMENT::NODE& pointsNode = aDocument.Nodes()[child];
            const size_t minimum = aKind == "polyline" ? 2 : 4;
            const size_t maximum = aKind == "polyline" ? 1024 : 4;
            JSON points = JSON::array();
            std::vector<SCHEMATIC_POINT> geometry;
            bool valid = pointsNode.children.size() >= minimum + 1
                         && pointsNode.children.size() <= maximum + 1;

            for( size_t pointIndex = 1; pointIndex < pointsNode.children.size(); ++pointIndex )
            {
                JSON point;
                const size_t pointNode = pointsNode.children[pointIndex];

                if( aDocument.ListHead( pointNode ) != "point"
                    || !parseSchematicPoint( aDocument, pointNode, point ) )
                {
                    valid = false;
                    continue;
                }

                const SCHEMATIC_POINT coordinate = {
                    point["xNm"].get<int64_t>(), point["yNm"].get<int64_t>()
                };

                if( !geometry.empty() && geometry.back() == coordinate )
                    valid = false;

                geometry.emplace_back( coordinate );
                points.emplace_back( std::move( point ) );
            }

            const bool nonDegenerateBezier =
                    aKind != "bezier"
                    || ( geometry.size() == 4
                         && std::any_of(
                                 geometry.begin() + 1, geometry.end(),
                                 [&]( const SCHEMATIC_POINT& aPoint )
                                 {
                                     return aPoint != geometry.front();
                                 } ) );

            if( !valid || !nonDegenerateBezier )
            {
                diagnostic( aResult, "error", "invalid_schematic_graphic_points",
                            aKind == "polyline"
                                    ? "polyline points requires 2 through 1024 bounded points without zero-length consecutive segments"
                                    : "bezier points requires exactly four bounded cubic control points that are not all identical" );
            }
            else
            {
                graphic["points"] = std::move( points );
            }

            continue;
        }

        if( ( head == "from" || head == "to" ) && aKind == "rectangle" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_rectangle_" + head,
                            "rectangle " + head + " requires two distances from 0 to 2 m" );
            }
            else
            {
                graphic[head == "from" ? "start" : "end"] = std::move( point );
            }

            continue;
        }

        if( ( head == "start" || head == "mid" || head == "end" ) && aKind == "arc" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_arc_" + head,
                            "arc " + head + " requires two distances from 0 to 2 m" );
            }
            else
            {
                graphic[head] = std::move( point );
            }

            continue;
        }

        if( head == "center" && aKind == "circle" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_circle_center",
                            "circle center requires two distances from 0 to 2 m" );
            }
            else
            {
                graphic["center"] = std::move( point );
            }

            continue;
        }

        if( head == "radius" && ( aKind == "circle" || aKind == "rectangle" ) )
        {
            std::string radiusText;
            int64_t radius = 0;
            const int64_t minimum = aKind == "circle" ? 100'000 : 0;

            if( !parseSingleValueForm( aDocument, child, radiusText )
                || !parseDistance( radiusText, radius ) || radius < minimum
                || radius > 1'000'000'000 )
            {
                diagnostic( aResult, "error", "invalid_schematic_" + aKind + "_radius",
                            aKind + " radius is outside its native coordinate range" );
            }
            else
            {
                graphic[aKind == "circle" ? "radiusNm" : "cornerRadiusNm"] = radius;
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_graphic_field",
                    aKind + " contains a field that is not valid for its geometry" );
    }

    for( const char* required : { "sheet", "stroke", "fill" } )
    {
        if( !graphic.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_graphic_field",
                        aKind + " " + id + " is missing " + required );
        }
    }

    if( aKind == "polyline" || aKind == "bezier" )
    {
        if( !graphic.contains( "points" ) )
        {
            diagnostic( aResult, "error", "missing_schematic_graphic_field",
                        aKind + " " + id + " is missing points" );
        }
        else if( graphic.contains( "fill" ) && graphic["fill"]["type"] != "none"
                 && aKind == "polyline" && graphic["points"].size() < 3 )
        {
            diagnostic( aResult, "error", "invalid_schematic_polyline_fill",
                        "a filled polyline requires at least three points" );
        }
    }
    else if( aKind == "rectangle" )
    {
        for( const char* required : { "start", "end", "cornerRadiusNm" } )
        {
            if( !graphic.contains( required ) )
            {
                diagnostic( aResult, "error", "missing_schematic_graphic_field",
                            "rectangle " + id + " is missing " + required );
            }
        }

        if( graphic.contains( "start" ) && graphic.contains( "end" )
            && graphic.contains( "cornerRadiusNm" ) )
        {
            const int64_t width = graphic["end"]["xNm"].get<int64_t>()
                                  - graphic["start"]["xNm"].get<int64_t>();
            const int64_t height = graphic["end"]["yNm"].get<int64_t>()
                                   - graphic["start"]["yNm"].get<int64_t>();
            const int64_t radius = graphic["cornerRadiusNm"].get<int64_t>();

            if( width <= 0 || height <= 0 || radius > std::min( width, height ) / 2 )
            {
                diagnostic( aResult, "error", "invalid_schematic_rectangle_geometry",
                            "rectangle requires normalized non-zero from/to corners and radius no greater than half its smaller dimension" );
            }
        }
    }
    else if( aKind == "circle" )
    {
        for( const char* required : { "center", "radiusNm" } )
        {
            if( !graphic.contains( required ) )
            {
                diagnostic( aResult, "error", "missing_schematic_graphic_field",
                            "circle " + id + " is missing " + required );
            }
        }

        if( graphic.contains( "center" ) && graphic.contains( "radiusNm" ) )
        {
            const int64_t x = graphic["center"]["xNm"].get<int64_t>();
            const int64_t y = graphic["center"]["yNm"].get<int64_t>();
            const int64_t radius = graphic["radiusNm"].get<int64_t>();

            if( x < radius || y < radius || x + radius > 2'000'000'000
                || y + radius > 2'000'000'000 )
            {
                diagnostic( aResult, "error", "invalid_schematic_circle_extent",
                            "circle must remain within the 0 through 2 m schematic coordinate range" );
            }
        }
    }
    else if( aKind == "arc" )
    {
        for( const char* required : { "start", "mid", "end" } )
        {
            if( !graphic.contains( required ) )
            {
                diagnostic( aResult, "error", "missing_schematic_graphic_field",
                            "arc " + id + " is missing " + required );
            }
        }

        if( graphic.contains( "start" ) && graphic.contains( "mid" )
            && graphic.contains( "end" ) )
        {
            const SCHEMATIC_POINT start = { graphic["start"]["xNm"].get<int64_t>(),
                                            graphic["start"]["yNm"].get<int64_t>() };
            const SCHEMATIC_POINT mid = { graphic["mid"]["xNm"].get<int64_t>(),
                                          graphic["mid"]["yNm"].get<int64_t>() };
            const SCHEMATIC_POINT end = { graphic["end"]["xNm"].get<int64_t>(),
                                          graphic["end"]["yNm"].get<int64_t>() };

            if( start == mid || mid == end || start == end
                || schematicOrientation( start, mid, end ) == 0 )
            {
                diagnostic( aResult, "error", "invalid_schematic_arc_geometry",
                            "arc requires three distinct non-collinear start, mid, and end points" );
            }
        }
    }

    return graphic;
}


std::string schematicImageMediaType( const std::vector<char>& aBytes )
{
    const auto byte = [&]( size_t aIndex )
    {
        return static_cast<unsigned char>( aBytes[aIndex] );
    };

    if( aBytes.size() >= 8 && byte( 0 ) == 0x89 && aBytes[1] == 'P'
        && aBytes[2] == 'N' && aBytes[3] == 'G' && byte( 4 ) == 0x0d
        && byte( 5 ) == 0x0a && byte( 6 ) == 0x1a && byte( 7 ) == 0x0a )
    {
        return "image/png";
    }

    if( aBytes.size() >= 3 && byte( 0 ) == 0xff && byte( 1 ) == 0xd8
        && byte( 2 ) == 0xff )
    {
        return "image/jpeg";
    }

    if( aBytes.size() >= 6
        && ( std::equal( aBytes.begin(), aBytes.begin() + 6, "GIF87a" )
             || std::equal( aBytes.begin(), aBytes.begin() + 6, "GIF89a" ) ) )
    {
        return "image/gif";
    }

    if( aBytes.size() >= 2 && aBytes[0] == 'B' && aBytes[1] == 'M' )
        return "image/bmp";

    if( aBytes.size() >= 12 && std::equal( aBytes.begin(), aBytes.begin() + 4, "RIFF" )
        && std::equal( aBytes.begin() + 8, aBytes.begin() + 12, "WEBP" ) )
    {
        return "image/webp";
    }

    return {};
}


JSON compileSchematicImage( const DOCUMENT& aDocument, size_t aNode,
                            KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_image",
                    "image requires a bounded stable ID" );
        return JSON::object();
    }

    JSON image = { { "kind", "image" }, { "id", id } };
    std::set<std::string> fields;
    std::vector<char> decoded;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_image_field",
                        "image field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" )
        {
            std::string sheet;

            if( !parseSingleValueForm( aDocument, child, sheet )
                || !validIdentifier( sheet ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_image_sheet",
                            "image sheet must be a bounded sheet ID" );
            }
            else
            {
                image["sheet"] = sheet;
            }

            continue;
        }

        if( head == "at" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_image_at",
                            "image at requires two distances from 0 to 2 m" );
            }
            else
            {
                image["position"] = std::move( point );
            }

            continue;
        }

        if( head == "scale" )
        {
            std::string scaleText;
            double scale = 0.0;

            if( !parseSingleValueForm( aDocument, child, scaleText )
                || !parseFiniteDecimal( scaleText, scale, "" ) || scale < 0.001
                || scale > 1000.0 )
            {
                diagnostic( aResult, "error", "invalid_schematic_image_scale",
                            "image scale requires a finite factor from 0.001 through 1000" );
            }
            else
            {
                image["scale"] = scale;
            }

            continue;
        }

        if( head == "media_type" )
        {
            std::string mediaType;
            static const std::set<std::string> TYPES = {
                "image/png", "image/jpeg", "image/gif", "image/bmp", "image/webp"
            };

            if( !parseSingleValueForm( aDocument, child, mediaType )
                || !TYPES.contains( mediaType ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_image_media_type",
                            "image media_type must be image/png|jpeg|gif|bmp|webp" );
            }
            else
            {
                image["mediaType"] = mediaType;
            }

            continue;
        }

        if( head == "sha256" )
        {
            std::string digest;

            if( !parseSingleValueForm( aDocument, child, digest ) || digest.size() != 64
                || !std::all_of( digest.begin(), digest.end(),
                                 []( unsigned char aCharacter )
                                 {
                                     return std::isdigit( aCharacter )
                                            || ( aCharacter >= 'a' && aCharacter <= 'f' );
                                 } ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_image_sha256",
                            "image sha256 requires one lowercase 64-digit digest" );
            }
            else
            {
                image["sha256"] = digest;
            }

            continue;
        }

        if( head == "description" )
        {
            std::string description;

            if( !parseSingleValueForm( aDocument, child, description )
                || description.empty() || description.size() > MAX_SCHEMATIC_PROPERTY_BYTES
                || description.find( '\0' ) != std::string::npos )
            {
                diagnostic( aResult, "error", "invalid_schematic_image_description",
                            "image description requires 1 through 4096 UTF-8 bytes" );
            }
            else
            {
                image["description"] = description;
            }

            continue;
        }

        if( head == "data_base64" )
        {
            std::string data;
            size_t errorPosition = 0;

            if( !parseSingleValueForm( aDocument, child, data ) || data.empty()
                || data.size() > ( MAX_SCHEMATIC_IMAGE_BYTES * 4 / 3 + 4 ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_image_data",
                            "image data_base64 must encode 1 byte through 8 MiB" );
                continue;
            }

            wxMemoryBuffer buffer = wxBase64Decode( data.data(), data.size(),
                                                     wxBase64DecodeMode_Strict,
                                                     &errorPosition );

            if( buffer.GetDataLen() == 0 || buffer.GetDataLen() > MAX_SCHEMATIC_IMAGE_BYTES )
            {
                diagnostic( aResult, "error", "invalid_schematic_image_data",
                            "image data_base64 is malformed or exceeds 8 MiB decoded" );
            }
            else
            {
                const char* bytes = static_cast<const char*>( buffer.GetData() );
                decoded.assign( bytes, bytes + buffer.GetDataLen() );
                image["dataBase64"] = std::move( data );
                image["byteCount"] = decoded.size();
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_image_field",
                    "image supports sheet, at, scale, media_type, sha256, description, and data_base64" );
    }

    for( const char* required : { "sheet", "position", "scale", "mediaType", "sha256",
                                  "description", "dataBase64", "byteCount" } )
    {
        if( !image.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_image_field",
                        "image " + id + " is missing " + required );
        }
    }

    if( !decoded.empty() && image.contains( "mediaType" ) && image.contains( "sha256" ) )
    {
        const std::string detected = schematicImageMediaType( decoded );
        std::string digest;
        picosha2::hash256_hex_string( decoded, digest );

        if( detected.empty() || detected != image["mediaType"].get<std::string>() )
        {
            diagnostic( aResult, "error", "schematic_image_media_mismatch",
                        "image media_type does not match the decoded file signature" );
        }

        if( digest != image["sha256"].get<std::string>() )
        {
            diagnostic( aResult, "error", "schematic_image_digest_mismatch",
                        "image sha256 does not match decoded data_base64" );
        }

        if( detected == "image/png" && decoded.size() >= 24 )
        {
            const auto bigEndian32 = [&]( size_t aOffset )
            {
                return ( static_cast<uint32_t>( static_cast<unsigned char>( decoded[aOffset] ) )
                         << 24 )
                       | ( static_cast<uint32_t>(
                                   static_cast<unsigned char>( decoded[aOffset + 1] ) )
                           << 16 )
                       | ( static_cast<uint32_t>(
                                   static_cast<unsigned char>( decoded[aOffset + 2] ) )
                           << 8 )
                       | static_cast<uint32_t>(
                                 static_cast<unsigned char>( decoded[aOffset + 3] ) );
            };
            const uint32_t width = bigEndian32( 16 );
            const uint32_t height = bigEndian32( 20 );

            if( width == 0 || height == 0 || width > 100'000 || height > 100'000 )
            {
                diagnostic( aResult, "error", "invalid_schematic_image_dimensions",
                            "PNG image dimensions must be 1 through 100000 pixels" );
            }
            else
            {
                image["pixels"] = { { "width", width }, { "height", height } };
            }
        }
    }

    return image;
}


bool compileSchematicTableLines( const DOCUMENT& aDocument, size_t aNode, bool aBorder,
                                 JSON& aLines,
                                 KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    const std::string name = aBorder ? "border" : "separators";
    const std::set<std::string> booleanFields = aBorder
                                                        ? std::set<std::string>{ "external", "header" }
                                                        : std::set<std::string>{ "rows", "columns" };
    std::set<std::string> fields;
    bool valid = true;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_table_" + name + "_field",
                        "table " + name + " field '" + head + "' occurs more than once" );
            valid = false;
            continue;
        }

        if( booleanFields.contains( head ) )
        {
            std::string boolean;

            if( !parseSingleValueForm( aDocument, child, boolean )
                || ( boolean != "true" && boolean != "false" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_" + name + "_" + head,
                            "table " + name + " " + head + " must be true or false" );
                valid = false;
            }
            else
            {
                aLines[head] = boolean == "true";
            }

            continue;
        }

        if( head == "stroke" )
        {
            JSON stroke;

            if( !parseCompleteSchematicStroke( aDocument, child, false, stroke ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_" + name + "_stroke",
                            "table " + name + " stroke requires default or 0.01 mm through 10 mm, a native line style, and default or #RRGGBB[AA] color" );
                valid = false;
            }
            else
            {
                aLines["stroke"] = std::move( stroke );
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_table_" + name + "_field",
                    aBorder ? "table border supports external, header, and stroke"
                            : "table separators supports rows, columns, and stroke" );
        valid = false;
    }

    for( const std::string& required : booleanFields )
    {
        if( !aLines.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_table_" + name + "_field",
                        "table " + name + " is missing " + required );
            valid = false;
        }
    }

    if( !aLines.contains( "stroke" ) )
    {
        diagnostic( aResult, "error", "missing_schematic_table_" + name + "_field",
                    "table " + name + " is missing stroke" );
        valid = false;
    }

    return valid;
}


JSON compileSchematicTableCell( const DOCUMENT& aDocument, size_t aNode,
                                size_t aRowCount, size_t aColumnCount,
                                KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string rowText;
    std::string columnText;
    std::string content;
    int64_t row = 0;
    int64_t column = 0;

    if( node.children.size() < 4
        || !scalarText( aDocument, node.children[1], rowText )
        || !scalarText( aDocument, node.children[2], columnText )
        || !scalarText( aDocument, node.children[3], content )
        || !parseBoundedInteger( rowText, 1, static_cast<int64_t>( aRowCount ), row )
        || !parseBoundedInteger( columnText, 1, static_cast<int64_t>( aColumnCount ), column )
        || content.size() > MAX_SCHEMATIC_TEXT_BYTES
        || content.find( '\0' ) != std::string::npos )
    {
        diagnostic( aResult, "error", "invalid_schematic_table_cell",
                    "cell requires a valid 1-based row and column plus content up to 65536 UTF-8 bytes" );
        return JSON::object();
    }

    JSON cell = { { "row", row - 1 }, { "column", column - 1 }, { "content", content } };
    std::set<std::string> fields;

    for( size_t index = 4; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_table_cell_field",
                        "cell " + rowText + "," + columnText + " field '" + head
                                + "' occurs more than once" );
            continue;
        }

        if( head == "margins" )
        {
            const DOCUMENT::NODE& margins = aDocument.Nodes()[child];
            JSON values = JSON::array();
            bool valid = margins.children.size() == 5;

            for( size_t marginIndex = 1; marginIndex < margins.children.size(); ++marginIndex )
            {
                std::string valueText;
                int64_t value = 0;

                if( !scalarText( aDocument, margins.children[marginIndex], valueText )
                    || !parseDistance( valueText, value ) || value < 0
                    || value > 2'000'000'000 )
                {
                    valid = false;
                }

                values.push_back( value );
            }

            if( !valid )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_cell_margins",
                            "cell margins requires left, top, right, and bottom distances from 0 to 2 m" );
            }
            else
            {
                cell["margins"] = { { "leftNm", values[0] }, { "topNm", values[1] },
                                      { "rightNm", values[2] }, { "bottomNm", values[3] } };
            }

            continue;
        }

        if( head == "fill" )
        {
            JSON fill;

            if( !parseCompleteSchematicFill( aDocument, child, fill ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_cell_fill",
                            "cell fill requires none|outline|background with default, or color|hatch|reverse_hatch|cross_hatch with #RRGGBB[AA]" );
            }
            else
            {
                cell["fill"] = std::move( fill );
            }

            continue;
        }

        if( head == "text_size" )
        {
            JSON size;

            if( !parseSchematicPoint( aDocument, child, size )
                || size["xNm"].get<int64_t>() < 100'000
                || size["yNm"].get<int64_t>() < 100'000
                || size["xNm"].get<int64_t>() > 50'000'000
                || size["yNm"].get<int64_t>() > 50'000'000 )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_cell_text_size",
                            "cell text_size requires two dimensions from 0.1 mm through 50 mm" );
            }
            else
            {
                cell["size"] = std::move( size );
            }

            continue;
        }

        if( head == "font" )
        {
            std::string font;

            if( !parseSingleValueForm( aDocument, child, font ) || font.empty()
                || font.size() > MAX_FONT_NAME_BYTES || font.find( '\0' ) != std::string::npos )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_cell_font",
                            "cell font requires stroke or a name up to 256 UTF-8 bytes" );
            }
            else
            {
                cell["font"] = font;
            }

            continue;
        }

        if( head == "line_spacing" )
        {
            std::string spacingText;
            double spacing = 0.0;

            if( !parseSingleValueForm( aDocument, child, spacingText )
                || !parseFiniteDecimal( spacingText, spacing ) || spacing < 0.5 || spacing > 5.0 )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_cell_line_spacing",
                            "cell line_spacing requires a finite value from 0.5 through 5" );
            }
            else
            {
                cell["lineSpacing"] = spacing;
            }

            continue;
        }

        if( head == "thickness" )
        {
            std::string thicknessText;
            int64_t thickness = 0;

            if( !parseSingleValueForm( aDocument, child, thicknessText )
                || ( thicknessText != "auto"
                     && ( !parseDistance( thicknessText, thickness ) || thickness < 10'000
                          || thickness > 10'000'000 ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_cell_thickness",
                            "cell thickness requires auto or 0.01 mm through 10 mm" );
            }
            else
            {
                cell["thicknessNm"] = thicknessText == "auto" ? 0 : thickness;
            }

            continue;
        }

        if( head == "color" )
        {
            std::string colorText;
            JSON color;

            if( !parseSingleValueForm( aDocument, child, colorText )
                || ( colorText != "default" && !parseHexColor( colorText, color ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_cell_color",
                            "cell color requires default or #RRGGBB[AA]" );
            }
            else
            {
                cell["color"] = colorText == "default" ? JSON( nullptr )
                                                          : schematicNativeColor( color );
            }

            continue;
        }

        if( head == "justify" )
        {
            const DOCUMENT::NODE& justify = aDocument.Nodes()[child];
            std::string horizontal;
            std::string vertical;

            if( justify.children.size() != 3
                || !scalarText( aDocument, justify.children[1], horizontal )
                || !scalarText( aDocument, justify.children[2], vertical )
                || ( horizontal != "left" && horizontal != "center" && horizontal != "right" )
                || ( vertical != "top" && vertical != "center" && vertical != "bottom" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_cell_justify",
                            "cell justify requires left|center|right and top|center|bottom" );
            }
            else
            {
                cell["justify"] = { { "horizontal", horizontal }, { "vertical", vertical } };
            }

            continue;
        }

        if( head == "hyperlink" )
        {
            std::string hyperlink;

            if( !parseSingleValueForm( aDocument, child, hyperlink )
                || hyperlink.size() > MAX_HYPERLINK_BYTES
                || hyperlink.find( '\0' ) != std::string::npos
                || hyperlink.find( '\r' ) != std::string::npos
                || hyperlink.find( '\n' ) != std::string::npos )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_cell_hyperlink",
                            "cell hyperlink requires none or one line up to 2048 UTF-8 bytes" );
            }
            else
            {
                cell["hyperlink"] = hyperlink == "none" ? "" : hyperlink;
            }

            continue;
        }

        if( head == "exclude_from_sim" || head == "mirror" || head == "bold"
            || head == "italic" )
        {
            std::string boolean;

            if( !parseSingleValueForm( aDocument, child, boolean )
                || ( boolean != "true" && boolean != "false" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_cell_" + head,
                            "cell " + head + " must be true or false" );
            }
            else
            {
                cell[head] = boolean == "true";
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_table_cell_field",
                    "cell supports margins, exclude_from_sim, fill, text_size, font, line_spacing, thickness, color, justify, mirror, bold, italic, and hyperlink" );
    }

    for( const char* required : { "margins", "exclude_from_sim", "fill", "size", "font",
                                  "lineSpacing", "thicknessNm", "color", "justify", "mirror",
                                  "bold", "italic", "hyperlink" } )
    {
        if( !cell.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_table_cell_field",
                        "cell " + rowText + "," + columnText + " is missing " + required );
        }
    }

    return cell;
}


JSON compileSchematicTable( const DOCUMENT& aDocument, size_t aNode,
                            KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_table",
                    "table requires a bounded stable ID" );
        return JSON::object();
    }

    JSON table = { { "kind", "table" }, { "id", id } };
    std::set<std::string> fields;
    size_t cellsNode = DOCUMENT::NO_NODE;
    size_t mergesNode = DOCUMENT::NO_NODE;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_table_field",
                        "table field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" )
        {
            std::string sheet;

            if( !parseSingleValueForm( aDocument, child, sheet ) || !validIdentifier( sheet ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_sheet",
                            "table sheet must be a bounded sheet ID" );
            }
            else
            {
                table["sheet"] = sheet;
            }

            continue;
        }

        if( head == "at" )
        {
            JSON position;

            if( !parseSchematicPoint( aDocument, child, position ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_at",
                            "table at requires two distances from 0 to 2 m" );
            }
            else
            {
                table["position"] = std::move( position );
            }

            continue;
        }

        if( head == "rotation" )
        {
            std::string rotationText;
            double rotation = 0.0;

            if( !parseSingleValueForm( aDocument, child, rotationText )
                || !parseFiniteDecimal( rotationText, rotation, "deg" )
                || ( rotation != 0.0 && rotation != 90.0 ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_rotation",
                            "table rotation requires 0deg or 90deg" );
            }
            else
            {
                table["rotationDegrees"] = static_cast<int>( rotation );
            }

            continue;
        }

        if( head == "columns" || head == "rows" )
        {
            const DOCUMENT::NODE& dimensions = aDocument.Nodes()[child];
            JSON values = JSON::array();
            bool valid = dimensions.children.size() >= 2
                         && dimensions.children.size() <= MAX_SCHEMATIC_TABLE_AXIS + 1;
            int64_t total = 0;

            for( size_t dimension = 1; dimension < dimensions.children.size(); ++dimension )
            {
                std::string valueText;
                int64_t value = 0;

                if( !scalarText( aDocument, dimensions.children[dimension], valueText )
                    || !parseDistance( valueText, value ) || value < 100'000
                    || value > 2'000'000'000 )
                {
                    valid = false;
                }

                total += value;
                values.push_back( value );
            }

            if( !valid || total > 2'000'000'000 )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_" + head,
                            "table " + head + " requires 1 through 256 dimensions from 0.1 mm through 2 m with a total no greater than 2 m" );
            }
            else
            {
                table[head == "columns" ? "columnWidthsNm" : "rowHeightsNm"] =
                        std::move( values );
            }

            continue;
        }

        if( head == "border" || head == "separators" )
        {
            JSON lines = JSON::object();

            if( compileSchematicTableLines( aDocument, child, head == "border", lines, aResult ) )
                table[head] = std::move( lines );

            continue;
        }

        if( head == "cells" )
        {
            cellsNode = child;
            continue;
        }

        if( head == "merges" )
        {
            mergesNode = child;
            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_table_field",
                    "table supports sheet, at, rotation, columns, rows, border, separators, cells, and merges" );
    }

    for( const char* required : { "sheet", "position", "rotationDegrees", "columnWidthsNm",
                                  "rowHeightsNm", "border", "separators" } )
    {
        if( !table.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_table_field",
                        "table " + id + " is missing " + required );
        }
    }

    if( cellsNode == DOCUMENT::NO_NODE )
        diagnostic( aResult, "error", "missing_schematic_table_field", "table " + id + " is missing cells" );

    if( mergesNode == DOCUMENT::NO_NODE )
        diagnostic( aResult, "error", "missing_schematic_table_field", "table " + id + " is missing merges" );

    if( !table.contains( "columnWidthsNm" ) || !table.contains( "rowHeightsNm" ) )
        return table;

    const size_t columnCount = table["columnWidthsNm"].size();
    const size_t rowCount = table["rowHeightsNm"].size();

    if( rowCount > MAX_SCHEMATIC_TABLE_CELLS / columnCount )
    {
        diagnostic( aResult, "error", "invalid_schematic_table_cell_count",
                    "table may contain no more than 65536 cells" );
        return table;
    }

    std::map<std::pair<int64_t, int64_t>, JSON> cells;

    if( cellsNode != DOCUMENT::NO_NODE )
    {
        const DOCUMENT::NODE& cellList = aDocument.Nodes()[cellsNode];

        for( size_t index = 1; index < cellList.children.size(); ++index )
        {
            const size_t child = cellList.children[index];

            if( aDocument.ListHead( child ) != "cell" )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_cells",
                            "table cells may contain only cell forms" );
                continue;
            }

            JSON cell = compileSchematicTableCell( aDocument, child, rowCount, columnCount, aResult );

            if( !cell.contains( "row" ) || !cell.contains( "column" ) )
                continue;

            const std::pair<int64_t, int64_t> key = {
                cell["row"].get<int64_t>(), cell["column"].get<int64_t>()
            };

            if( !cells.emplace( key, std::move( cell ) ).second )
            {
                diagnostic( aResult, "error", "duplicate_schematic_table_cell",
                            "table cell " + std::to_string( key.first + 1 ) + ","
                                    + std::to_string( key.second + 1 ) + " occurs more than once" );
            }
        }
    }

    if( cells.size() != rowCount * columnCount )
    {
        diagnostic( aResult, "error", "incomplete_schematic_table_cells",
                    "table " + id + " must define every cell exactly once" );
    }

    table["cells"] = JSON::array();

    for( size_t row = 0; row < rowCount; ++row )
    {
        for( size_t column = 0; column < columnCount; ++column )
        {
            auto cell = cells.find( { static_cast<int64_t>( row ), static_cast<int64_t>( column ) } );

            if( cell != cells.end() )
                table["cells"].push_back( std::move( cell->second ) );
        }
    }

    table["merges"] = JSON::array();
    std::vector<bool> mergedCells( rowCount * columnCount, false );
    std::vector<bool> coveredCells( rowCount * columnCount, false );

    if( mergesNode != DOCUMENT::NO_NODE )
    {
        const DOCUMENT::NODE& merges = aDocument.Nodes()[mergesNode];

        for( size_t index = 1; index < merges.children.size(); ++index )
        {
            const size_t child = merges.children[index];
            const DOCUMENT::NODE& merge = aDocument.Nodes()[child];
            std::string firstRowText;
            std::string firstColumnText;
            std::string lastRowText;
            std::string lastColumnText;
            int64_t firstRow = 0;
            int64_t firstColumn = 0;
            int64_t lastRow = 0;
            int64_t lastColumn = 0;

            if( aDocument.ListHead( child ) != "merge" || merge.children.size() != 5
                || !scalarText( aDocument, merge.children[1], firstRowText )
                || !scalarText( aDocument, merge.children[2], firstColumnText )
                || !scalarText( aDocument, merge.children[3], lastRowText )
                || !scalarText( aDocument, merge.children[4], lastColumnText )
                || !parseBoundedInteger( firstRowText, 1, static_cast<int64_t>( rowCount ), firstRow )
                || !parseBoundedInteger( firstColumnText, 1, static_cast<int64_t>( columnCount ), firstColumn )
                || !parseBoundedInteger( lastRowText, 1, static_cast<int64_t>( rowCount ), lastRow )
                || !parseBoundedInteger( lastColumnText, 1, static_cast<int64_t>( columnCount ), lastColumn )
                || firstRow > lastRow || firstColumn > lastColumn
                || ( firstRow == lastRow && firstColumn == lastColumn ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_table_merge",
                            "merge requires a nontrivial in-bounds rectangle as START_ROW START_COLUMN END_ROW END_COLUMN" );
                continue;
            }

            bool overlaps = false;

            for( int64_t row = firstRow - 1; row < lastRow; ++row )
            {
                for( int64_t column = firstColumn - 1; column < lastColumn; ++column )
                {
                    const size_t offset = static_cast<size_t>( row ) * columnCount
                                          + static_cast<size_t>( column );
                    overlaps = overlaps || mergedCells[offset];
                }
            }

            if( overlaps )
            {
                diagnostic( aResult, "error", "overlapping_schematic_table_merge",
                            "table merge rectangles must not overlap" );
                continue;
            }

            for( int64_t row = firstRow - 1; row < lastRow; ++row )
            {
                for( int64_t column = firstColumn - 1; column < lastColumn; ++column )
                {
                    mergedCells[static_cast<size_t>( row ) * columnCount
                                + static_cast<size_t>( column )] = true;

                    if( row != firstRow - 1 || column != firstColumn - 1 )
                    {
                        coveredCells[static_cast<size_t>( row ) * columnCount
                                     + static_cast<size_t>( column )] = true;
                    }
                }
            }

            table["merges"].push_back( { { "firstRow", firstRow - 1 },
                                          { "firstColumn", firstColumn - 1 },
                                          { "lastRow", lastRow - 1 },
                                          { "lastColumn", lastColumn - 1 } } );
        }
    }

    if( table["cells"].size() == rowCount * columnCount )
    {
        for( size_t offset = 0; offset < coveredCells.size(); ++offset )
        {
            if( coveredCells[offset]
                && !table["cells"][offset]["content"].get<std::string>().empty() )
            {
                diagnostic( aResult, "error", "covered_schematic_table_cell_content",
                            "cells covered by a merge must have empty content; content belongs to the merge's top-left cell" );
            }
        }
    }

    if( table.contains( "position" ) )
    {
        const int64_t x = table["position"]["xNm"].get<int64_t>();
        const int64_t y = table["position"]["yNm"].get<int64_t>();
        int64_t width = 0;
        int64_t height = 0;

        for( const JSON& value : table["columnWidthsNm"] )
            width += value.get<int64_t>();

        for( const JSON& value : table["rowHeightsNm"] )
            height += value.get<int64_t>();
        const int rotation = table.value( "rotationDegrees", 0 );
        const bool inBounds = rotation == 0
                                      ? x + width <= 2'000'000'000 && y + height <= 2'000'000'000
                                      : x + height <= 2'000'000'000 && y >= width;

        if( !inBounds )
        {
            diagnostic( aResult, "error", "invalid_schematic_table_extent",
                        "table geometry must remain within the 0 through 2 m schematic coordinate range" );
        }
    }

    return table;
}


JSON compileSchematicBusAlias( const DOCUMENT& aDocument, size_t aNode,
                               KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string name;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], name )
        || !validIdentifier( name ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_bus_alias",
                    "bus_alias requires a bounded name" );
        return JSON::object();
    }

    JSON alias = { { "name", name }, { "members", JSON::array() } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_bus_alias_field",
                        "bus_alias field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" )
        {
            std::string sheet;

            if( !parseSingleValueForm( aDocument, child, sheet )
                || !validIdentifier( sheet ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_bus_alias_sheet",
                            "bus_alias sheet must be a bounded sheet ID" );
            }
            else
            {
                alias["sheet"] = sheet;
            }

            continue;
        }

        if( head == "members" )
        {
            const DOCUMENT::NODE& members = aDocument.Nodes()[child];
            std::set<std::string> uniqueMembers;

            for( size_t memberIndex = 1; memberIndex < members.children.size(); ++memberIndex )
            {
                std::string member;

                if( !scalarText( aDocument, members.children[memberIndex], member )
                    || member.empty() || member.size() > MAX_IDENTIFIER_BYTES )
                {
                    diagnostic( aResult, "error", "invalid_schematic_bus_alias_member",
                                "bus_alias members must be bounded net names" );
                    continue;
                }

                if( !uniqueMembers.emplace( member ).second )
                {
                    diagnostic( aResult, "error", "duplicate_schematic_bus_alias_member",
                                "bus_alias member " + member + " occurs more than once" );
                    continue;
                }

                alias["members"].push_back( member );
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_bus_alias_field",
                    "bus_alias supports sheet and members" );
    }

    if( !alias.contains( "sheet" ) )
    {
        diagnostic( aResult, "error", "missing_schematic_bus_alias_field",
                    "bus_alias " + name + " is missing sheet" );
    }

    if( !fields.contains( "members" ) || alias["members"].empty()
        || alias["members"].size() > 256 )
    {
        diagnostic( aResult, "error", "invalid_schematic_bus_alias_members",
                    "bus_alias " + name + " requires 1 through 256 unique members" );
    }

    return alias;
}


JSON compileSchematicGroup( const DOCUMENT& aDocument, size_t aNode,
                            KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_group",
                    "group requires a bounded stable ID" );
        return JSON::object();
    }

    JSON group = { { "id", id }, { "members", JSON::array() } };
    std::set<std::string> fields;
    std::set<std::string> memberKeys;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_group_field",
                        "group field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" )
        {
            std::string sheet;

            if( !parseSingleValueForm( aDocument, child, sheet )
                || !validIdentifier( sheet ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_group_sheet",
                            "group sheet must be a bounded sheet ID" );
            }
            else
            {
                group["sheet"] = sheet;
            }

            continue;
        }

        if( head == "name" )
        {
            std::string name;

            if( !parseSingleValueForm( aDocument, child, name ) || name.empty()
                || name.size() > MAX_SCHEMATIC_PROPERTY_BYTES
                || name.find( '\0' ) != std::string::npos )
            {
                diagnostic( aResult, "error", "invalid_schematic_group_name",
                            "group name requires 1 through 4096 UTF-8 bytes" );
            }
            else
            {
                group["name"] = name;
            }

            continue;
        }

        if( head == "locked" )
        {
            std::string value;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "true" && value != "false" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_group_locked",
                            "group locked must be true or false" );
            }
            else
            {
                group["locked"] = value == "true";
            }

            continue;
        }

        if( head == "members" )
        {
            const DOCUMENT::NODE& members = aDocument.Nodes()[child];

            for( size_t memberIndex = 1; memberIndex < members.children.size(); ++memberIndex )
            {
                const size_t memberNode = members.children[memberIndex];
                const DOCUMENT::NODE& memberForm = aDocument.Nodes()[memberNode];
                std::string kind;

                if( aDocument.ListHead( memberNode ) != "member"
                    || memberForm.children.size() < 3
                    || !scalarText( aDocument, memberForm.children[1], kind ) )
                {
                    diagnostic( aResult, "error", "invalid_schematic_group_member",
                                "group members must use a typed (member ...) form" );
                    continue;
                }

                JSON member = { { "kind", kind } };
                std::string key = kind;
                bool valid = true;
                const auto scalar = [&]( size_t aOffset, std::string& aValue )
                {
                    return aOffset < memberForm.children.size()
                           && scalarText( aDocument, memberForm.children[aOffset], aValue );
                };

                if( kind == "drawing" || kind == "sheet" || kind == "group" )
                {
                    std::string target;
                    valid = memberForm.children.size() == 3 && scalar( 2, target )
                            && validIdentifier( target );
                    member["id"] = target;
                    key += "/" + target;
                }
                else if( kind == "component" )
                {
                    std::string reference;
                    std::string unitText;
                    int64_t unit = 0;
                    valid = memberForm.children.size() == 4 && scalar( 2, reference )
                            && scalar( 3, unitText ) && validIdentifier( reference )
                            && parseBoundedInteger( unitText, 1, 256, unit );
                    member["reference"] = reference;
                    member["unit"] = unit;
                    key += "/" + reference + "/" + unitText;
                }
                else if( kind == "hierarchical_label" )
                {
                    std::string sheet;
                    std::string pin;
                    valid = memberForm.children.size() == 4 && scalar( 2, sheet )
                            && scalar( 3, pin ) && validIdentifier( sheet ) && !pin.empty()
                            && pin.size() <= 64;
                    member["sheet"] = sheet;
                    member["pin"] = pin;
                    key += "/" + sheet + "/" + pin;
                }
                else if( kind == "net_label" || kind == "no_connect" )
                {
                    const size_t expected = kind == "net_label" ? 7 : 6;
                    size_t offset = kind == "net_label" ? 3 : 2;
                    std::string net;
                    std::string reference;
                    std::string unitText;
                    std::string pin;
                    std::string occurrenceText;
                    int64_t unit = 0;
                    int64_t occurrence = 0;

                    if( kind == "net_label" )
                        valid = scalar( 2, net ) && !net.empty()
                                && net.size() <= MAX_IDENTIFIER_BYTES;

                    valid = valid && memberForm.children.size() == expected
                            && scalar( offset, reference ) && scalar( offset + 1, unitText )
                            && scalar( offset + 2, pin )
                            && scalar( offset + 3, occurrenceText )
                            && validIdentifier( reference ) && !pin.empty() && pin.size() <= 64
                            && parseBoundedInteger( unitText, 1, 256, unit )
                            && parseBoundedInteger( occurrenceText, 1, 256, occurrence );

                    if( kind == "net_label" )
                        member["net"] = net;

                    member["reference"] = reference;
                    member["unit"] = unit;
                    member["pin"] = pin;
                    member["occurrence"] = occurrence;
                    key += "/" + net + "/" + reference + "/" + unitText + "/" + pin
                           + "/" + occurrenceText;
                }
                else
                {
                    valid = false;
                }

                if( !valid )
                {
                    diagnostic( aResult, "error", "invalid_schematic_group_member",
                                "group member must be drawing ID, component REF UNIT, sheet ID, "
                                "hierarchical_label SHEET PIN, net_label NET REF UNIT PIN "
                                "OCCURRENCE, no_connect REF UNIT PIN OCCURRENCE, or group ID" );
                    continue;
                }

                if( !memberKeys.emplace( key ).second )
                {
                    diagnostic( aResult, "error", "duplicate_schematic_group_member",
                                "group " + id + " contains duplicate member " + key );
                    continue;
                }

                group["members"].push_back( std::move( member ) );
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_group_field",
                    "group supports sheet, name, locked, and members" );
    }

    for( const char* required : { "sheet", "name", "locked" } )
    {
        if( !group.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_group_field",
                        "group " + id + " is missing " + required );
        }
    }

    if( !fields.contains( "members" ) || group["members"].empty()
        || group["members"].size() > MAX_SCHEMATIC_GROUP_MEMBERS )
    {
        diagnostic( aResult, "error", "invalid_schematic_group_members",
                    "group " + id + " requires 1 through 4096 unique typed members" );
    }

    return group;
}


JSON compileConformance( const DOCUMENT& aDocument, size_t aNode,
                         KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult,
                         std::vector<std::string>& aReferencedComponents )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           reference;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], reference )
        || !validIdentifier( reference ) )
    {
        diagnostic( aResult, "error", "invalid_conformance",
                    "conformance requires a bounded component reference" );
        return JSON::object();
    }

    aReferencedComponents.push_back( reference );
    JSON record = { { "component", reference }, { "deviations", JSON::array() } };

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t          child = node.children[index];
        const std::string     head = aDocument.ListHead( child );
        const DOCUMENT::NODE& childNode = aDocument.Nodes()[child];
        std::string           value;

        if( childNode.children.size() != 2
            || !scalarText( aDocument, childNode.children[1], value ) || value.empty()
            || value.size() > 4096 )
        {
            diagnostic( aResult, "error", "invalid_conformance",
                        "conformance " + reference + " field '" + head + "' is malformed" );
            continue;
        }

        if( head == "datasheet" )
        {
            if( !value.starts_with( "https://" ) || value.size() <= 8 )
            {
                diagnostic( aResult, "error", "invalid_conformance",
                            "conformance " + reference + " datasheet must be an https URL" );
                continue;
            }

            record["datasheet"] = value;
        }
        else if( head == "verified_on" )
        {
            if( value.size() != 10 || value[4] != '-' || value[7] != '-' )
            {
                diagnostic( aResult, "error", "invalid_conformance",
                            "conformance " + reference
                                    + " verified_on must be an ISO YYYY-MM-DD date" );
                continue;
            }

            record["verified_on"] = value;
        }
        else if( head == "pins" || head == "application" )
        {
            if( value != "verified" )
            {
                diagnostic( aResult, "error", "invalid_conformance",
                            "conformance " + reference + " " + head
                                    + " must be the literal 'verified'" );
                continue;
            }

            record[head] = true;
        }
        else if( head == "deviation" )
        {
            record["deviations"].push_back( value );
        }
        else
        {
            diagnostic( aResult, "error", "invalid_conformance",
                        "conformance " + reference + " has unknown field '" + head + "'" );
        }
    }

    for( const char* required : { "datasheet", "verified_on" } )
    {
        if( !record.contains( required ) )
        {
            diagnostic( aResult, "error", "invalid_conformance",
                        "conformance " + reference + " is missing " + required );
        }
    }

    if( !record.value( "pins", false ) || !record.value( "application", false ) )
    {
        diagnostic( aResult, "error", "invalid_conformance",
                    "conformance " + reference
                            + " must declare (pins verified) and (application verified)" );
    }

    return record;
}


JSON compileSource( const DOCUMENT& aDocument, size_t aNode,
                    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult,
                    std::vector<std::string>& aReferencedComponents )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           reference;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], reference )
        || !validIdentifier( reference ) )
    {
        diagnostic( aResult, "error", "invalid_source",
                    "source requires a bounded component reference" );
        return JSON::object();
    }

    JSON source = { { "component", reference } };
    const std::set<std::string> allowed = {
        "manufacturer", "mpn",       "datasheet", "lifecycle", "supplier", "sku",
        "product_url",  "available", "verified_on", "quantity", "unit_price", "notes"
    };
    std::set<std::string> fields;

    const auto boundedString = []( const JSON& aValue, size_t aMaximum )
    {
        if( !aValue.is_string() )
            return false;

        const std::string& value = aValue.get_ref<const std::string&>();
        return !value.empty() && value.size() <= aMaximum
               && std::none_of( value.begin(), value.end(),
                                []( unsigned char aCharacter )
                                {
                                    return aCharacter < 0x20 || aCharacter == 0x7F;
                                } );
    };

    const auto httpsUrl = [&]( const JSON& aValue )
    {
        if( !boundedString( aValue, 4096 ) )
            return false;

        const std::string& value = aValue.get_ref<const std::string&>();

        if( !value.starts_with( "https://" ) || value.size() <= 8 )
            return false;

        const size_t authorityEnd = value.find_first_of( "/?#", 8 );
        const size_t authoritySize = ( authorityEnd == std::string::npos ? value.size()
                                                                            : authorityEnd )
                                     - 8;
        return authoritySize > 0 && value.find_first_of( " \t\r\n" ) == std::string::npos;
    };

    const auto isoDate = []( const JSON& aValue )
    {
        if( !aValue.is_string() )
            return false;

        const std::string& value = aValue.get_ref<const std::string&>();

        if( value.size() != 10 || value[4] != '-' || value[7] != '-' )
            return false;

        for( size_t i = 0; i < value.size(); ++i )
        {
            if( i != 4 && i != 7 && !std::isdigit( static_cast<unsigned char>( value[i] ) ) )
                return false;
        }

        const int year = std::stoi( value.substr( 0, 4 ) );
        const int month = std::stoi( value.substr( 5, 2 ) );
        const int day = std::stoi( value.substr( 8, 2 ) );
        static constexpr int DAYS[] = { 31, 28, 31, 30, 31, 30,
                                        31, 31, 30, 31, 30, 31 };

        if( year < 2000 || year > 9999 || month < 1 || month > 12 || day < 1 )
            return false;

        int maximumDay = DAYS[month - 1];

        if( month == 2 && ( year % 400 == 0 || ( year % 4 == 0 && year % 100 != 0 ) ) )
            maximumDay = 29;

        return day <= maximumDay;
    };

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        const DOCUMENT::NODE& field = aDocument.Nodes()[child];

        if( !allowed.contains( head ) || field.children.size() != 2
            || !isScalar( aDocument, field.children[1] ) )
        {
            diagnostic( aResult, "error", "invalid_source_field",
                        "source fields must use one supported scalar evidence value" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_source_field",
                        "source field '" + head + "' occurs more than once" );
            continue;
        }

        JSON value = scalarValue( aDocument, field.children[1] );
        bool valid = true;

        if( head == "manufacturer" || head == "mpn" || head == "supplier" || head == "sku" )
            valid = boundedString( value, 512 );
        else if( head == "datasheet" || head == "product_url" )
            valid = httpsUrl( value );
        else if( head == "lifecycle" )
            valid = value.is_string()
                    && std::set<std::string>{ "active", "nrnd", "last_time_buy", "obsolete" }
                               .contains( value.get<std::string>() );
        else if( head == "verified_on" )
            valid = isoDate( value );
        else if( head == "available" )
            valid = value.is_number_integer() && value.get<int64_t>() >= 0
                    && value.get<int64_t>() <= 1'000'000'000'000LL;
        else if( head == "quantity" )
            valid = value.is_number_integer() && value.get<int64_t>() >= 1
                    && value.get<int64_t>() <= 1'000'000;
        else if( head == "unit_price" )
            valid = boundedString( value, 128 );
        else if( head == "notes" )
            valid = boundedString( value, 2048 );

        if( !valid )
        {
            diagnostic( aResult, "error", "invalid_source_value",
                        "source field '" + head + "' has an invalid evidence value" );
            continue;
        }

        source[head] = std::move( value );
    }

    if( !source.contains( "manufacturer" ) || !source.contains( "mpn" ) )
    {
        diagnostic( aResult, "warning", "incomplete_source",
                    "source for " + reference + " has no verified manufacturer and MPN pair" );
    }

    aReferencedComponents.emplace_back( reference );
    return source;
}


JSON compileRules( const DOCUMENT& aDocument, size_t aNode,
                   KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    struct DISTANCE_FIELD
    {
        const char* sourceName;
        const char* irName;
        int64_t     minimum;
        int64_t     maximum;
    };

    static constexpr DISTANCE_FIELD DISTANCES[] = {
        { "minimum_clearance", "minimumClearanceNm", 0, 25000000 },
        { "minimum_connection_width", "minimumConnectionWidthNm", 0, 100000000 },
        { "minimum_track_width", "minimumTrackWidthNm", 0, 25000000 },
        { "minimum_via_annular_width", "minimumViaAnnularWidthNm", 0, 25000000 },
        { "minimum_via_diameter", "minimumViaDiameterNm", 0, 25000000 },
        { "minimum_through_hole_diameter", "minimumThroughHoleDiameterNm", 0, 25000000 },
        { "minimum_microvia_diameter", "minimumMicroviaDiameterNm", 0, 10000000 },
        { "minimum_microvia_drill", "minimumMicroviaDrillNm", 0, 10000000 },
        { "minimum_hole_to_hole", "minimumHoleToHoleNm", 0, 10000000 },
        { "minimum_copper_to_hole_clearance", "minimumCopperToHoleClearanceNm", 0,
          100000000 },
        { "minimum_silkscreen_clearance", "minimumSilkscreenClearanceNm", -10000000,
          100000000 },
        { "minimum_groove_width", "minimumGrooveWidthNm", 0, 25000000 },
        { "minimum_silkscreen_text_height", "minimumSilkscreenTextHeightNm", 0,
          100000000 },
        { "minimum_silkscreen_text_thickness", "minimumSilkscreenTextThicknessNm", 0,
          25000000 },
        { "maximum_error", "maximumErrorNm", 1000, 100000 }
    };
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    const std::set<std::string> allowed = {
        "minimum_clearance",
        "minimum_connection_width",
        "minimum_track_width",
        "minimum_via_annular_width",
        "minimum_via_diameter",
        "minimum_through_hole_diameter",
        "minimum_microvia_diameter",
        "minimum_microvia_drill",
        "minimum_hole_to_hole",
        "minimum_copper_to_hole_clearance",
        "minimum_silkscreen_clearance",
        "minimum_groove_width",
        "minimum_resolved_spokes",
        "minimum_silkscreen_text_height",
        "minimum_silkscreen_text_thickness",
        "minimum_copper_to_edge_clearance",
        "use_height_for_length_calculations",
        "maximum_error",
        "allow_fillets_outside_zone_outline",
        "drc_severities",
        "erc_severities"
    };
    std::map<std::string, size_t> fields;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t      fieldNode = node.children[index];
        const std::string name = aDocument.ListHead( fieldNode );

        if( !allowed.contains( name ) )
        {
            diagnostic( aResult, "error", "unknown_rules_field",
                        "rules contains an unknown global board constraint" );
            continue;
        }

        if( !fields.emplace( name, fieldNode ).second )
        {
            diagnostic( aResult, "error", "duplicate_rules_field",
                        "rules field '" + name + "' occurs more than once" );
        }
    }

    JSON rules = { { "kind", "rules" } };

    for( const DISTANCE_FIELD& specification : DISTANCES )
    {
        int64_t     value = 0;
        std::string text;
        const auto  found = fields.find( specification.sourceName );

        if( found == fields.end()
            || !parseSingleValueForm( aDocument, found->second, text )
            || !parseDistance( text, value ) || value < specification.minimum
            || value > specification.maximum )
        {
            diagnostic( aResult, "error", "invalid_rules_distance",
                        std::string( "rules " ) + specification.sourceName
                                + " is required and outside its native range" );
        }

        rules[specification.irName] = value;
    }

    int         resolvedSpokes = 0;
    std::string resolvedSpokesText;
    auto        spokesField = fields.find( "minimum_resolved_spokes" );

    if( spokesField == fields.end()
        || !parseSingleValueForm( aDocument, spokesField->second, resolvedSpokesText ) )
    {
        diagnostic( aResult, "error", "invalid_rules_resolved_spokes",
                    "rules minimum_resolved_spokes is required from 0 through 99" );
    }
    else
    {
        std::from_chars_result converted =
                std::from_chars( resolvedSpokesText.data(),
                                 resolvedSpokesText.data() + resolvedSpokesText.size(),
                                 resolvedSpokes );

        if( converted.ec != std::errc()
            || converted.ptr != resolvedSpokesText.data() + resolvedSpokesText.size()
            || resolvedSpokes < 0 || resolvedSpokes > 99 )
        {
            diagnostic( aResult, "error", "invalid_rules_resolved_spokes",
                        "rules minimum_resolved_spokes is required from 0 through 99" );
        }
    }

    rules["minimumResolvedSpokes"] = resolvedSpokes;
    bool        useHeight = false;
    std::string useHeightText;
    auto        useHeightField = fields.find( "use_height_for_length_calculations" );

    if( useHeightField == fields.end()
        || !parseSingleValueForm( aDocument, useHeightField->second, useHeightText )
        || ( useHeightText != "true" && useHeightText != "false" ) )
    {
        diagnostic( aResult, "error", "invalid_rules_length_policy",
                    "rules use_height_for_length_calculations must be explicitly true or false" );
    }
    else
    {
        useHeight = useHeightText == "true";
    }

    rules["useHeightForLengthCalculations"] = useHeight;
    bool        allowExternalFillets = false;
    std::string allowExternalFilletsText;
    auto        allowExternalFilletsField = fields.find( "allow_fillets_outside_zone_outline" );

    if( allowExternalFilletsField == fields.end()
        || !parseSingleValueForm( aDocument, allowExternalFilletsField->second,
                                  allowExternalFilletsText )
        || ( allowExternalFilletsText != "true" && allowExternalFilletsText != "false" ) )
    {
        diagnostic( aResult, "error", "invalid_rules_zone_fillet_policy",
                    "rules allow_fillets_outside_zone_outline must be explicitly true or false" );
    }
    else
    {
        allowExternalFillets = allowExternalFilletsText == "true";
    }

    rules["allowFilletsOutsideZoneOutline"] = allowExternalFillets;
    JSON drcSeverities = nullptr;
    auto severitiesField = fields.find( "drc_severities" );

    if( severitiesField != fields.end() )
    {
        drcSeverities = { { "default", "" }, { "checks", JSON::object() } };
        const DOCUMENT::NODE& severitiesNode =
                aDocument.Nodes()[severitiesField->second];
        bool sawDefault = false;

        for( size_t index = 1; index < severitiesNode.children.size(); ++index )
        {
            const size_t child = severitiesNode.children[index];
            const std::string head = aDocument.ListHead( child );

            if( head == "default" )
            {
                std::string severity;

                if( sawDefault || !parseSingleValueForm( aDocument, child, severity )
                    || ( severity != "error" && severity != "warning"
                         && severity != "ignore" ) )
                {
                    diagnostic( aResult, "error", "invalid_drc_severity_default",
                                "drc_severities requires exactly one default of error, "
                                "warning, or ignore" );
                }
                else
                {
                    sawDefault = true;
                    drcSeverities["default"] = severity;
                }

                continue;
            }

            if( head != "check" )
            {
                diagnostic( aResult, "error", "unknown_drc_severity_field",
                            "drc_severities accepts only default and check forms" );
                continue;
            }

            const DOCUMENT::NODE& check = aDocument.Nodes()[child];
            std::string key;
            std::string severity;

            if( check.children.size() != 3
                || !scalarText( aDocument, check.children[1], key )
                || !scalarText( aDocument, check.children[2], severity )
                || !validNativeRuleKey( key )
                || ( severity != "error" && severity != "warning"
                     && severity != "ignore" ) )
            {
                diagnostic( aResult, "error", "invalid_drc_severity_check",
                            "drc severity check requires a bounded native key and error, "
                            "warning, or ignore" );
                continue;
            }

            if( drcSeverities["checks"].contains( key ) )
            {
                diagnostic( aResult, "error", "duplicate_drc_severity_check",
                            "DRC severity check '" + key + "' occurs more than once" );
                continue;
            }

            drcSeverities["checks"][key] = severity;
        }

        if( !sawDefault )
        {
            diagnostic( aResult, "error", "missing_drc_severity_default",
                        "drc_severities requires exactly one default policy" );
        }

        if( drcSeverities["checks"].size() > 256 )
        {
            diagnostic( aResult, "error", "too_many_drc_severity_checks",
                        "drc_severities supports at most 256 explicit overrides" );
        }
    }

    rules["drcSeverities"] = std::move( drcSeverities );
    JSON ercSeverities = nullptr;
    auto ercSeveritiesField = fields.find( "erc_severities" );

    if( ercSeveritiesField != fields.end() )
    {
        ercSeverities = { { "checks", JSON::object() } };
        const DOCUMENT::NODE& severitiesNode =
                aDocument.Nodes()[ercSeveritiesField->second];

        for( size_t index = 1; index < severitiesNode.children.size(); ++index )
        {
            const size_t child = severitiesNode.children[index];

            if( aDocument.ListHead( child ) != "check" )
            {
                diagnostic( aResult, "error", "unknown_erc_severity_field",
                            "erc_severities accepts only check forms" );
                continue;
            }

            const DOCUMENT::NODE& check = aDocument.Nodes()[child];
            std::string key;
            std::string severity;

            if( check.children.size() != 3
                || !scalarText( aDocument, check.children[1], key )
                || !scalarText( aDocument, check.children[2], severity )
                || !validNativeRuleKey( key )
                || ( severity != "error" && severity != "warning"
                     && severity != "ignore" ) )
            {
                diagnostic( aResult, "error", "invalid_erc_severity_check",
                            "ERC severity check requires a bounded native key and error, "
                            "warning, or ignore" );
                continue;
            }

            if( ercSeverities["checks"].contains( key ) )
            {
                diagnostic( aResult, "error", "duplicate_erc_severity_check",
                            "ERC severity check '" + key + "' occurs more than once" );
                continue;
            }

            ercSeverities["checks"][key] = severity;
        }

        if( ercSeverities["checks"].size() > 256 )
        {
            diagnostic( aResult, "error", "too_many_erc_severity_checks",
                        "erc_severities supports at most 256 explicit overrides" );
        }
    }

    rules["ercSeverities"] = std::move( ercSeverities );
    int64_t     edgeClearance = 0;
    std::string edgeText;
    std::string edgeMode;
    auto        edgeField = fields.find( "minimum_copper_to_edge_clearance" );

    if( edgeField == fields.end()
        || !parseSingleValueForm( aDocument, edgeField->second, edgeText ) )
    {
        diagnostic( aResult, "error", "invalid_rules_edge_clearance",
                    "rules minimum_copper_to_edge_clearance requires legacy or a distance" );
    }
    else if( edgeText == "legacy" )
    {
        edgeMode = "legacy";
    }
    else if( !parseDistance( edgeText, edgeClearance ) || edgeClearance < 0
             || edgeClearance > 25000000 )
    {
        diagnostic( aResult, "error", "invalid_rules_edge_clearance",
                    "rules minimum_copper_to_edge_clearance requires legacy or 0mm through 25mm" );
    }
    else
    {
        edgeMode = "explicit";
    }

    rules["copperEdgeClearanceMode"] = edgeMode;
    rules["minimumCopperToEdgeClearanceNm"] = edgeClearance;
    const int64_t annular = rules["minimumViaAnnularWidthNm"].get<int64_t>();

    if( rules["minimumViaDiameterNm"].get<int64_t>()
        < rules["minimumThroughHoleDiameterNm"].get<int64_t>() + 2 * annular )
    {
        diagnostic( aResult, "error", "inconsistent_rules_via_geometry",
                    "minimum_via_diameter cannot satisfy the drill and annular-width constraints" );
    }

    if( rules["minimumMicroviaDiameterNm"].get<int64_t>()
        < rules["minimumMicroviaDrillNm"].get<int64_t>() + 2 * annular )
    {
        diagnostic( aResult, "error", "inconsistent_rules_microvia_geometry",
                    "minimum_microvia_diameter cannot satisfy the drill and annular-width constraints" );
    }

    return rules;
}


JSON compileNetClass( const DOCUMENT& aDocument, size_t aNode, size_t aPriority,
                      KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    struct DISTANCE_FIELD
    {
        const char* sourceName;
        const char* irName;
        int64_t     minimum;
        int64_t     maximum;
        int64_t     quantum;
    };

    static constexpr DISTANCE_FIELD DISTANCES[] = {
        { "clearance", "clearanceNm", 0, 500000000, 1 },
        { "track_width", "trackWidthNm", 1, 25000000, 1 },
        { "via_diameter", "viaDiameterNm", 1, 25000000, 1 },
        { "via_drill", "viaDrillNm", 1, 25000000, 1 },
        { "microvia_diameter", "microviaDiameterNm", 1, 10000000, 1 },
        { "microvia_drill", "microviaDrillNm", 1, 10000000, 1 },
        { "diff_pair_width", "diffPairWidthNm", 1, 25000000, 1 },
        { "diff_pair_gap", "diffPairGapNm", 0, 100000000, 1 },
        { "diff_pair_via_gap", "diffPairViaGapNm", 0, 100000000, 1 },
        { "wire_width", "wireWidthNm", 100, 100000000, 100 },
        { "bus_width", "busWidthNm", 100, 100000000, 100 }
    };
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string name;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], name )
        || name.empty() || name.size() > 256 )
    {
        diagnostic( aResult, "error", "invalid_netclass_name",
                    "class requires a non-empty name of at most 256 bytes" );
        name.clear();
    }

    const bool isDefault = name == "Default";
    const std::set<std::string> allowed = {
        "clearance",          "track_width",       "via_diameter",
        "via_drill",          "microvia_diameter", "microvia_drill",
        "diff_pair_width",    "diff_pair_gap",     "diff_pair_via_gap",
        "tuning_profile",     "pcb_color",         "wire_width",
        "bus_width",          "schematic_color",   "line_style"
    };
    std::map<std::string, size_t> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t fieldNode = node.children[index];
        const std::string fieldName = aDocument.ListHead( fieldNode );

        if( !allowed.contains( fieldName ) )
        {
            diagnostic( aResult, "error", "unknown_netclass_field",
                        "class contains an unknown netclass field" );
            continue;
        }

        if( !fields.emplace( fieldName, fieldNode ).second )
        {
            diagnostic( aResult, "error", "duplicate_netclass_field",
                        "netclass field '" + fieldName + "' occurs more than once" );
        }
    }

    JSON netClass = { { "name", name },
                      { "priority", isDefault ? std::numeric_limits<int32_t>::max()
                                               : static_cast<int64_t>( aPriority ) } };

    for( const DISTANCE_FIELD& specification : DISTANCES )
    {
        const auto found = fields.find( specification.sourceName );
        std::string valueText;

        if( found == fields.end()
            || !parseSingleValueForm( aDocument, found->second, valueText ) )
        {
            diagnostic( aResult, "error", "invalid_netclass_distance",
                        std::string( "class " ) + specification.sourceName
                                + " is required with one distance or inherit" );
            netClass[specification.irName] = nullptr;
            continue;
        }

        if( valueText == "inherit" )
        {
            if( isDefault )
            {
                diagnostic( aResult, "error", "invalid_default_netclass_inheritance",
                            std::string( "Default class cannot inherit " )
                                    + specification.sourceName );
            }

            netClass[specification.irName] = nullptr;
            continue;
        }

        int64_t value = 0;

        if( !parseDistance( valueText, value ) || value < specification.minimum
            || value > specification.maximum || value % specification.quantum != 0 )
        {
            diagnostic( aResult, "error", "invalid_netclass_distance",
                        std::string( "class " ) + specification.sourceName
                                + " is outside its exact native range or resolution" );
        }

        netClass[specification.irName] = value;
    }

    const auto parsePolicy = [&]( const char* aField, const char* aIrName,
                                  const std::set<std::string>& aValues )
    {
        const auto found = fields.find( aField );
        std::string value;

        if( found == fields.end()
            || !parseSingleValueForm( aDocument, found->second, value )
            || !aValues.contains( value ) )
        {
            diagnostic( aResult, "error", "invalid_netclass_policy",
                        std::string( "class " ) + aField + " has an invalid semantic value" );
            netClass[aIrName] = nullptr;
            return;
        }

        if( value == "inherit" )
        {
            if( isDefault )
            {
                diagnostic( aResult, "error", "invalid_default_netclass_inheritance",
                            std::string( "Default class cannot inherit " ) + aField );
            }

            netClass[aIrName] = nullptr;
        }
        else
        {
            netClass[aIrName] = value;
        }
    };

    parsePolicy( "line_style", "lineStyle",
                 { "inherit", "solid", "dash", "dot", "dash_dot", "dash_dot_dot" } );

    const auto parseColor = [&]( const char* aField, const char* aIrName )
    {
        const auto found = fields.find( aField );
        std::string value;
        JSON color;

        if( found == fields.end()
            || !parseSingleValueForm( aDocument, found->second, value ) )
        {
            diagnostic( aResult, "error", "invalid_netclass_color",
                        std::string( "class " ) + aField
                                + " requires default, inherit, or #RRGGBBAA" );
            netClass[aIrName] = nullptr;
        }
        else if( ( isDefault && value == "default" )
                 || ( !isDefault && value == "inherit" ) )
        {
            netClass[aIrName] = nullptr;
        }
        else if( ( isDefault && value == "inherit" ) || ( !isDefault && value == "default" )
                 || !parseHexColor( value, color ) )
        {
            diagnostic( aResult, "error", "invalid_netclass_color",
                        std::string( "class " ) + aField
                                + " uses a color policy that is invalid for this class" );
            netClass[aIrName] = nullptr;
        }
        else
        {
            netClass[aIrName] = std::move( color );
        }
    };

    parseColor( "pcb_color", "pcbColor" );
    parseColor( "schematic_color", "schematicColor" );
    const auto tuningField = fields.find( "tuning_profile" );
    std::string tuningProfile;

    if( tuningField == fields.end()
        || !parseSingleValueForm( aDocument, tuningField->second, tuningProfile )
        || tuningProfile.empty() || tuningProfile.size() > 256
        || ( isDefault && tuningProfile == "inherit" )
        || ( !isDefault && tuningProfile == "none" ) )
    {
        diagnostic( aResult, "error", "invalid_netclass_tuning_profile",
                    "Default tuning_profile requires none or a name; other classes require "
                    "inherit or a name" );
        netClass["tuningProfile"] = nullptr;
    }
    else if( tuningProfile == "none" || tuningProfile == "inherit" )
    {
        netClass["tuningProfile"] = nullptr;
    }
    else
    {
        netClass["tuningProfile"] = tuningProfile;
    }

    return netClass;
}


JSON compileNetClasses( const DOCUMENT& aDocument, size_t aNode,
                        KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    JSON classes = JSON::array();
    JSON assignments = JSON::array();
    std::set<std::string> foldedNames;
    std::set<std::string> exactNames;
    std::set<std::pair<std::string, std::string>> assignmentPairs;
    bool sawAssignment = false;
    size_t userPriority = 0;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "class" )
        {
            if( sawAssignment )
            {
                diagnostic( aResult, "error", "noncanonical_netclass_order",
                            "all class declarations must precede netclass assignments" );
            }

            JSON netClass = compileNetClass( aDocument, child, userPriority, aResult );
            const std::string name = netClass.value( "name", "" );
            wxString folded = wxString::FromUTF8( name ).Lower();
            const wxScopedCharBuffer foldedBuffer = folded.ToUTF8();
            const std::string foldedUtf8 = foldedBuffer.data() ? foldedBuffer.data() : "";

            if( !name.empty() && !foldedNames.emplace( foldedUtf8 ).second )
            {
                diagnostic( aResult, "error", "duplicate_netclass",
                            "netclass names must be unique without regard to case" );
            }

            exactNames.emplace( name );

            if( name != "Default" )
                ++userPriority;

            classes.emplace_back( std::move( netClass ) );
            continue;
        }

        if( head != "assign" )
        {
            diagnostic( aResult, "error", "unknown_net_classes_form",
                        "net_classes accepts only class and assign forms" );
            continue;
        }

        sawAssignment = true;
        const DOCUMENT::NODE& assignmentNode = aDocument.Nodes()[child];
        std::map<std::string, size_t> fields;

        for( size_t fieldIndex = 1; fieldIndex < assignmentNode.children.size(); ++fieldIndex )
        {
            const size_t fieldNode = assignmentNode.children[fieldIndex];
            const std::string fieldName = aDocument.ListHead( fieldNode );

            if( fieldName != "pattern" && fieldName != "classes" )
            {
                diagnostic( aResult, "error", "unknown_netclass_assignment_field",
                            "assign supports only pattern and classes" );
                continue;
            }

            if( !fields.emplace( fieldName, fieldNode ).second )
            {
                diagnostic( aResult, "error", "duplicate_netclass_assignment_field",
                            "netclass assignment fields may occur once" );
            }
        }

        std::string pattern;
        const auto patternField = fields.find( "pattern" );

        if( patternField == fields.end()
            || !parseSingleValueForm( aDocument, patternField->second, pattern )
            || pattern.empty() || pattern.size() > 256 || !boundedBusRanges( pattern ) )
        {
            diagnostic( aResult, "error", "invalid_netclass_pattern",
                        "assign pattern must be non-empty, at most 256 bytes, and have bounded "
                        "bus ranges" );
        }

        JSON classNames = JSON::array();
        const auto classesField = fields.find( "classes" );

        if( classesField == fields.end() )
        {
            diagnostic( aResult, "error", "invalid_netclass_assignment",
                        "assign requires a non-empty classes list" );
        }
        else
        {
            const DOCUMENT::NODE& classesNode = aDocument.Nodes()[classesField->second];
            std::set<std::string> seenClasses;

            if( classesNode.children.size() < 2 )
            {
                diagnostic( aResult, "error", "invalid_netclass_assignment",
                            "assign requires a non-empty classes list" );
            }

            for( size_t classIndex = 1; classIndex < classesNode.children.size(); ++classIndex )
            {
                std::string className;

                if( !scalarText( aDocument, classesNode.children[classIndex], className )
                    || className.empty() || className.size() > 256 )
                {
                    diagnostic( aResult, "error", "invalid_netclass_assignment",
                                "assignment class names must be bounded scalar values" );
                    continue;
                }

                if( className == "Default" || !exactNames.contains( className ) )
                {
                    diagnostic( aResult, "error", "unknown_netclass_assignment",
                                "assignment references an unknown or redundant Default class" );
                }

                if( !seenClasses.emplace( className ).second
                    || !assignmentPairs.emplace( pattern, className ).second )
                {
                    diagnostic( aResult, "error", "duplicate_netclass_assignment",
                                "pattern/class assignments must be unique" );
                }

                classNames.emplace_back( className );
            }
        }

        assignments.push_back( { { "pattern", pattern }, { "classes", std::move( classNames ) } } );
    }

    if( classes.empty() || classes.front().value( "name", "" ) != "Default"
        || std::count_if( classes.begin(), classes.end(),
                          []( const JSON& aClass )
                          {
                              return aClass.value( "name", "" ) == "Default";
                          } ) != 1 )
    {
        diagnostic( aResult, "error", "invalid_default_netclass",
                    "net_classes requires exactly one Default class as its first declaration" );
    }

    if( classes.size() > 256 )
    {
        diagnostic( aResult, "error", "too_many_netclasses",
                    "net_classes supports at most 256 classes" );
    }

    if( assignmentPairs.size() > 1024 )
    {
        diagnostic( aResult, "error", "too_many_netclass_assignments",
                    "net_classes supports at most 1024 pattern/class assignments" );
    }

    if( !classes.empty() && classes.front().value( "name", "" ) == "Default"
        && classes.front()["viaDiameterNm"].is_number_integer()
        && classes.front()["viaDrillNm"].is_number_integer()
        && classes.front()["microviaDiameterNm"].is_number_integer()
        && classes.front()["microviaDrillNm"].is_number_integer() )
    {
        const JSON& defaults = classes.front();

        if( defaults["viaDiameterNm"].get<int64_t>()
            < defaults["viaDrillNm"].get<int64_t>() )
        {
            diagnostic( aResult, "error", "inconsistent_netclass_via_geometry",
                        "Default netclass via diameter cannot be smaller than its drill" );
        }

        if( defaults["microviaDiameterNm"].get<int64_t>()
            < defaults["microviaDrillNm"].get<int64_t>() )
        {
            diagnostic( aResult, "error", "inconsistent_netclass_microvia_geometry",
                        "Default netclass microvia diameter cannot be smaller than its drill" );
        }

        for( size_t index = 1; index < classes.size(); ++index )
        {
            const JSON& netClass = classes[index];
            const int64_t viaDiameter = netClass["viaDiameterNm"].is_null()
                                                ? defaults["viaDiameterNm"].get<int64_t>()
                                                : netClass["viaDiameterNm"].get<int64_t>();
            const int64_t viaDrill = netClass["viaDrillNm"].is_null()
                                             ? defaults["viaDrillNm"].get<int64_t>()
                                             : netClass["viaDrillNm"].get<int64_t>();
            const int64_t microviaDiameter = netClass["microviaDiameterNm"].is_null()
                                                     ? defaults["microviaDiameterNm"].get<int64_t>()
                                                     : netClass["microviaDiameterNm"].get<int64_t>();
            const int64_t microviaDrill = netClass["microviaDrillNm"].is_null()
                                                  ? defaults["microviaDrillNm"].get<int64_t>()
                                                  : netClass["microviaDrillNm"].get<int64_t>();

            if( viaDiameter < viaDrill )
            {
                diagnostic( aResult, "error", "inconsistent_netclass_via_geometry",
                            "effective netclass via diameter cannot be smaller than its drill" );
            }

            if( microviaDiameter < microviaDrill )
            {
                diagnostic( aResult, "error", "inconsistent_netclass_microvia_geometry",
                            "effective netclass microvia diameter cannot be smaller than its drill" );
            }
        }
    }

    return { { "kind", "net_classes" },
             { "classes", std::move( classes ) },
             { "assignments", std::move( assignments ) } };
}


JSON compileCustomRuleValue( const std::string& aText, const std::string& aDomain,
                             KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult,
                             const std::string& aConstraint )
{
    if( aDomain == "distance" || aDomain == "distance_or_time" )
    {
        int64_t nanometers = 0;

        if( parseDistance( aText, nanometers )
            && nanometers >= -10000000000LL && nanometers <= 10000000000LL )
        {
            return { { "domain", "distance" }, { "value", nanometers } };
        }

        if( aDomain == "distance_or_time" )
        {
            int64_t femtoseconds = 0;

            if( parseTime( aText, femtoseconds )
                && femtoseconds >= -1000000000000000LL
                && femtoseconds <= 1000000000000000LL )
            {
                return { { "domain", "time" }, { "value", femtoseconds } };
            }
        }

        diagnostic( aResult, "error", "invalid_custom_rule_value",
                    "custom rule constraint " + aConstraint
                            + " requires a bounded distance"
                            + ( aDomain == "distance_or_time" ? " or time" : "" )
                            + " literal" );
        return JSON::object();
    }

    if( aDomain == "integer" )
    {
        int64_t value = 0;

        if( !parseBoundedInteger( aText, 0, 1000000, value ) )
        {
            diagnostic( aResult, "error", "invalid_custom_rule_value",
                        "custom rule constraint " + aConstraint
                                + " requires an integer from 0 through 1000000" );
        }

        return { { "domain", "integer" }, { "value", value } };
    }

    double degrees = 0.0;

    if( !parseFiniteDecimal( aText, degrees, "deg" ) || degrees < 0.0 || degrees > 180.0 )
    {
        diagnostic( aResult, "error", "invalid_custom_rule_value",
                    "custom rule track_angle values require 0deg through 180deg" );
    }

    return { { "domain", "angle" }, { "value", degrees } };
}


JSON compileCustomConstraint( const DOCUMENT& aDocument, size_t aNode,
                              KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           type;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], type ) )
    {
        diagnostic( aResult, "error", "invalid_custom_constraint",
                    "custom rule constraints require a supported type" );
        return JSON::object();
    }

    if( type == "via_dangling" || type == "bridged_mask" )
    {
        if( node.children.size() != 2 )
        {
            diagnostic( aResult, "error", "invalid_custom_constraint",
                        type + " does not accept values" );
        }

        return { { "type", type }, { "kind", "flag" } };
    }

    if( type == "assertion" )
    {
        std::string test;

        if( node.children.size() != 3 || aDocument.ListHead( node.children[2] ) != "test"
            || !parseSingleValueForm( aDocument, node.children[2], test ) || test.empty()
            || test.size() > 8192 || test.find( "${" ) != std::string::npos )
        {
            diagnostic( aResult, "error", "invalid_custom_assertion",
                        "assertion requires one bounded (test EXPRESSION)" );
        }

        return { { "type", type }, { "kind", "assertion" }, { "test", test } };
    }

    if( type == "zone_connection" )
    {
        std::string style;

        if( node.children.size() != 3 || aDocument.ListHead( node.children[2] ) != "style"
            || !parseSingleValueForm( aDocument, node.children[2], style )
            || ( style != "solid" && style != "thermal_reliefs" && style != "none" ) )
        {
            diagnostic( aResult, "error", "invalid_custom_zone_connection",
                        "zone_connection requires (style solid|thermal_reliefs|none)" );
        }

        return { { "type", type }, { "kind", "zone_connection" }, { "style", style } };
    }

    if( type == "disallow" )
    {
        static const std::vector<std::string> ORDER = {
            "track", "via", "through_via", "blind_via", "buried_via", "micro_via",
            "pad", "zone", "text", "graphic", "hole", "footprint"
        };
        JSON items = JSON::array();
        std::set<std::string> seen;
        size_t previousRank = 0;
        bool haveRank = false;

        if( node.children.size() != 3 || aDocument.ListHead( node.children[2] ) != "items" )
        {
            diagnostic( aResult, "error", "invalid_custom_disallow",
                        "disallow requires one non-empty (items TYPE ...) list" );
            return { { "type", type }, { "kind", "disallow" }, { "items", items } };
        }

        const DOCUMENT::NODE& itemsNode = aDocument.Nodes()[node.children[2]];

        if( itemsNode.children.size() < 2 )
        {
            diagnostic( aResult, "error", "invalid_custom_disallow",
                        "disallow items cannot be empty" );
        }

        for( size_t index = 1; index < itemsNode.children.size(); ++index )
        {
            std::string item;

            if( !scalarText( aDocument, itemsNode.children[index], item ) )
            {
                diagnostic( aResult, "error", "invalid_custom_disallow",
                            "disallow items must be scalar object types" );
                continue;
            }

            const auto found = std::find( ORDER.begin(), ORDER.end(), item );

            if( found == ORDER.end() || !seen.emplace( item ).second )
            {
                diagnostic( aResult, "error", "invalid_custom_disallow",
                            "disallow contains an unknown or duplicate object type" );
                continue;
            }

            const size_t rank = static_cast<size_t>( found - ORDER.begin() );

            if( haveRank && rank <= previousRank )
            {
                diagnostic( aResult, "error", "noncanonical_custom_disallow_order",
                            "disallow items must use canonical object-type order" );
            }

            previousRank = rank;
            haveRank = true;
            items.emplace_back( item );
        }

        if( seen.contains( "via" )
            && ( seen.contains( "through_via" ) || seen.contains( "blind_via" )
                 || seen.contains( "buried_via" ) || seen.contains( "micro_via" ) ) )
        {
            diagnostic( aResult, "error", "redundant_custom_disallow",
                        "disallow via cannot be combined with individual via types" );
        }

        return { { "type", type }, { "kind", "disallow" }, { "items", items } };
    }

    if( type == "min_resolved_spokes" )
    {
        std::string valueText;
        int64_t     value = 0;

        if( node.children.size() != 3 || aDocument.ListHead( node.children[2] ) != "count"
            || !parseSingleValueForm( aDocument, node.children[2], valueText )
            || !parseBoundedInteger( valueText, 0, 4, value ) )
        {
            diagnostic( aResult, "error", "invalid_custom_spoke_count",
                        "min_resolved_spokes requires (count 0..4)" );
        }

        return { { "type", type }, { "kind", "count" }, { "value", value } };
    }

    if( type == "solder_paste_rel_margin" )
    {
        std::string valueText;
        double      value = 0.0;
        int64_t     valuePermille = 0;

        if( node.children.size() != 3 || aDocument.ListHead( node.children[2] ) != "ratio"
            || !parseSingleValueForm( aDocument, node.children[2], valueText )
            || !parseFiniteDecimal( valueText, value ) || value < -10.0 || value > 10.0
            || std::abs( value * 1000.0 - std::round( value * 1000.0 ) ) > 1e-9 )
        {
            diagnostic( aResult, "error", "invalid_custom_paste_ratio",
                        "solder_paste_rel_margin requires a finite (ratio VALUE) from -10 to 10 "
                        "at 0.001 resolution" );
        }
        else
        {
            valuePermille = static_cast<int64_t>( std::llround( value * 1000.0 ) );
        }

        return { { "type", type },
                 { "kind", "ratio" },
                 { "valuePermille", valuePermille } };
    }

    struct RANGE_SPEC
    {
        const char*            domain;
        std::set<std::string>  allowed;
        std::set<std::string>  required;
    };
    static const std::map<std::string, RANGE_SPEC> SPECS = {
        { "annular_width", { "distance", { "min", "max" }, {} } },
        { "clearance", { "distance", { "min" }, { "min" } } },
        { "creepage", { "distance", { "min" }, { "min" } } },
        { "connection_width", { "distance", { "min" }, { "min" } } },
        { "courtyard_clearance", { "distance", { "min" }, { "min" } } },
        { "diff_pair_gap", { "distance", { "min", "opt", "max" }, {} } },
        { "diff_pair_uncoupled", { "distance", { "max" }, { "max" } } },
        { "edge_clearance", { "distance", { "min" }, { "min" } } },
        { "hole_clearance", { "distance", { "min" }, { "min" } } },
        { "hole_size", { "distance", { "min", "opt", "max" }, {} } },
        { "hole_to_hole", { "distance", { "min" }, { "min" } } },
        { "length", { "distance_or_time", { "min", "opt", "max" }, {} } },
        { "physical_clearance", { "distance", { "min" }, { "min" } } },
        { "physical_hole_clearance", { "distance", { "min" }, { "min" } } },
        { "silk_clearance", { "distance", { "min" }, { "min" } } },
        { "skew", { "distance_or_time", { "min", "opt", "max" }, {} } },
        { "solder_mask_expansion", { "distance", { "opt" }, { "opt" } } },
        { "solder_mask_sliver", { "distance", { "min" }, { "min" } } },
        { "solder_paste_abs_margin", { "distance", { "opt" }, { "opt" } } },
        { "text_height", { "distance", { "min", "max" }, {} } },
        { "text_thickness", { "distance", { "min", "max" }, {} } },
        { "thermal_relief_gap", { "distance", { "min" }, { "min" } } },
        { "thermal_spoke_width", { "distance", { "opt" }, { "opt" } } },
        { "track_width", { "distance", { "min", "opt", "max" }, {} } },
        { "track_angle", { "angle", { "min", "max" }, {} } },
        { "track_segment_length", { "distance", { "min", "max" }, {} } },
        { "via_count", { "integer", { "min", "max" }, {} } },
        { "via_diameter", { "distance", { "min", "opt", "max" }, {} } }
    };
    const auto specification = SPECS.find( type );

    if( specification == SPECS.end() )
    {
        diagnostic( aResult, "error", "unknown_custom_constraint",
                    "unsupported custom rule constraint '" + type + "'" );
        return JSON::object();
    }

    JSON values = JSON::object();
    std::string skewDomain;
    int previousRank = -1;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t fieldNode = node.children[index];
        const std::string field = aDocument.ListHead( fieldNode );

        if( type == "skew" && field == "domain" )
        {
            if( !skewDomain.empty()
                || !parseSingleValueForm( aDocument, fieldNode, skewDomain )
                || ( skewDomain != "nets" && skewDomain != "diff_pairs" ) )
            {
                diagnostic( aResult, "error", "invalid_custom_skew_domain",
                            "skew requires one (domain nets|diff_pairs)" );
            }

            continue;
        }

        const auto allowed = specification->second.allowed.find( field );
        const int rank = field == "min" ? 0 : field == "opt" ? 1 : field == "max" ? 2 : -1;
        std::string text;

        if( allowed == specification->second.allowed.end() || rank < 0
            || !parseSingleValueForm( aDocument, fieldNode, text ) )
        {
            diagnostic( aResult, "error", "invalid_custom_constraint_field",
                        "constraint " + type + " contains an unsupported value field" );
            continue;
        }

        if( values.contains( field ) || rank <= previousRank )
        {
            diagnostic( aResult, "error", "noncanonical_custom_constraint_values",
                        "constraint values must occur once in min, opt, max order" );
        }

        previousRank = rank;
        values[field] = compileCustomRuleValue( text, specification->second.domain,
                                                aResult, type );
    }

    if( values.empty() )
    {
        diagnostic( aResult, "error", "missing_custom_constraint_value",
                    "constraint " + type + " requires at least one value" );
    }

    for( const std::string& required : specification->second.required )
    {
        if( !values.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_custom_constraint_value",
                        "constraint " + type + " requires " + required );
        }
    }

    if( type == "skew" && skewDomain.empty() )
    {
        diagnostic( aResult, "error", "invalid_custom_skew_domain",
                    "skew requires explicit (domain nets|diff_pairs)" );
    }

    std::string valueDomain;
    long double minimum = 0.0L;
    bool        haveMinimum = false;

    for( const char* field : { "min", "opt", "max" } )
    {
        if( !values.contains( field ) || !values[field].is_object()
            || !values[field].contains( "domain" ) )
        {
            continue;
        }

        const std::string domain = values[field]["domain"].get<std::string>();

        if( valueDomain.empty() )
            valueDomain = domain;
        else if( domain != valueDomain )
            diagnostic( aResult, "error", "mixed_custom_constraint_domains",
                        "constraint " + type + " cannot mix distance and time values" );

        if( domain == "distance" || domain == "time" || domain == "integer"
            || domain == "angle" )
        {
            const long double current = domain == "angle"
                                                ? values[field]["value"].get<double>()
                                                : values[field]["value"].get<int64_t>();

            if( haveMinimum && current < minimum )
            {
                diagnostic( aResult, "error", "unordered_custom_constraint_range",
                            "constraint " + type + " requires min <= opt <= max" );
            }

            minimum = current;
            haveMinimum = true;
        }
    }

    JSON constraint = { { "type", type }, { "kind", "range" }, { "values", values } };

    if( type == "skew" )
        constraint["domain"] = skewDomain;

    return constraint;
}


JSON compileCustomRules( const DOCUMENT& aDocument, size_t aNode,
                         KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    JSON rules = JSON::array();
    std::set<std::string> names;
    size_t constraintCount = 0;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t ruleNode = node.children[index];
        const DOCUMENT::NODE& ruleForm = aDocument.Nodes()[ruleNode];
        std::string name;

        if( aDocument.ListHead( ruleNode ) != "rule" || ruleForm.children.size() < 6
            || !scalarText( aDocument, ruleForm.children[1], name ) || name.empty()
            || name.size() > 256 )
        {
            diagnostic( aResult, "error", "invalid_custom_rule",
                        "custom_rules entries require a bounded rule name, condition, layer, "
                        "severity, and at least one constraint" );
            continue;
        }

        if( !names.emplace( name ).second )
        {
            diagnostic( aResult, "error", "duplicate_custom_rule",
                        "custom rule names must be unique" );
        }

        std::string condition;
        std::string layer;
        std::string severity;

        if( aDocument.ListHead( ruleForm.children[2] ) != "condition"
            || !parseSingleValueForm( aDocument, ruleForm.children[2], condition )
            || condition.empty() || condition.size() > 8192
            || condition.find( "${" ) != std::string::npos )
        {
            diagnostic( aResult, "error", "invalid_custom_rule_condition",
                        "custom rule condition must be always or one bounded native expression" );
        }

        auto validLayer = []( const std::string& aLayer )
        {
            return !aLayer.empty() && aLayer.size() <= 64
                   && std::all_of( aLayer.begin(), aLayer.end(),
                                   []( unsigned char aCharacter )
                                   {
                                       return std::isalnum( aCharacter ) || aCharacter == '_'
                                              || aCharacter == '-' || aCharacter == '.'
                                              || aCharacter == '*';
                                   } );
        };

        if( aDocument.ListHead( ruleForm.children[3] ) != "layer"
            || !parseSingleValueForm( aDocument, ruleForm.children[3], layer )
            || !validLayer( layer ) )
        {
            diagnostic( aResult, "error", "invalid_custom_rule_layer",
                        "custom rule layer must be all, outer, inner, or a bounded KiCad layer "
                        "pattern" );
        }

        if( aDocument.ListHead( ruleForm.children[4] ) != "severity"
            || !parseSingleValueForm( aDocument, ruleForm.children[4], severity )
            || ( severity != "ignore" && severity != "warning" && severity != "error"
                 && severity != "exclusion" ) )
        {
            diagnostic( aResult, "error", "invalid_custom_rule_severity",
                        "custom rule severity must be ignore, warning, error, or exclusion" );
        }

        JSON constraints = JSON::array();
        std::set<std::string> constraintTypes;

        for( size_t constraintIndex = 5; constraintIndex < ruleForm.children.size();
             ++constraintIndex )
        {
            const size_t constraintNode = ruleForm.children[constraintIndex];

            if( aDocument.ListHead( constraintNode ) != "constraint" )
            {
                diagnostic( aResult, "error", "noncanonical_custom_rule_clause",
                            "custom rule clauses must be condition, layer, severity, then "
                            "constraints" );
                continue;
            }

            JSON constraint = compileCustomConstraint( aDocument, constraintNode, aResult );
            const std::string type = constraint.value( "type", "" );

            if( !type.empty() && !constraintTypes.emplace( type ).second )
            {
                diagnostic( aResult, "error", "duplicate_custom_constraint",
                            "a custom rule may contain each constraint type once" );
            }

            constraints.emplace_back( std::move( constraint ) );
            ++constraintCount;
        }

        if( constraints.size() > 64 )
        {
            diagnostic( aResult, "error", "too_many_custom_constraints",
                        "a custom rule supports at most 64 constraints" );
        }

        rules.push_back( { { "name", name },
                           { "condition", condition },
                           { "layer", layer },
                           { "severity", severity },
                           { "constraints", std::move( constraints ) } } );
    }

    if( rules.size() > 512 || constraintCount > 4096 )
    {
        diagnostic( aResult, "error", "too_many_custom_rules",
                    "custom_rules supports at most 512 rules and 4096 constraints" );
    }

    return { { "kind", "custom_rules" }, { "rules", std::move( rules ) } };
}


JSON compileSheet( const DOCUMENT& aDocument, size_t aNode,
                   KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_sheet",
                    "sheet requires a bounded logical identifier" );
        return JSON::object();
    }

    JSON sheet = { { "id", id }, { "parent", nullptr }, { "pins", JSON::array() } };
    std::set<std::string> fields;
    std::set<std::string> pinNames;

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        const DOCUMENT::NODE& field = aDocument.Nodes()[child];

        if( head == "pin" )
        {
            std::string name;
            std::string direction;

            if( field.children.size() < 5
                || !scalarText( aDocument, field.children[1], name ) || name.empty()
                || name.size() > MAX_IDENTIFIER_BYTES
                || !scalarText( aDocument, field.children[2], direction )
                || !std::set<std::string>( { "input", "output", "bidirectional",
                                             "tri_state", "passive" } )
                            .contains( direction ) )
            {
                diagnostic( aResult, "error", "invalid_sheet_pin",
                            "sheet pin requires a bounded name, native electrical direction, "
                            "position, and side" );
                continue;
            }

            if( !pinNames.emplace( name ).second )
            {
                diagnostic( aResult, "error", "duplicate_sheet_pin",
                            "sheet " + id + " pin " + name + " occurs more than once" );
                continue;
            }

            JSON pin = { { "name", name }, { "direction", direction } };
            std::set<std::string> pinFields;

            for( size_t fieldIndex = 3; fieldIndex < field.children.size(); ++fieldIndex )
            {
                const size_t pinFieldNode = field.children[fieldIndex];
                const std::string pinHead = aDocument.ListHead( pinFieldNode );

                if( !pinFields.emplace( pinHead ).second )
                {
                    diagnostic( aResult, "error", "duplicate_sheet_pin_field",
                                "sheet pin field '" + pinHead + "' occurs more than once" );
                    continue;
                }

                if( pinHead == "at" )
                {
                    const DOCUMENT::NODE& at = aDocument.Nodes()[pinFieldNode];
                    std::string xText;
                    std::string yText;
                    int64_t x = 0;
                    int64_t y = 0;

                    if( at.children.size() != 3
                        || !scalarText( aDocument, at.children[1], xText )
                        || !scalarText( aDocument, at.children[2], yText )
                        || !parseDistance( xText, x ) || !parseDistance( yText, y )
                        || x < 0 || y < 0 || x > 2'000'000'000LL
                        || y > 2'000'000'000LL )
                    {
                        diagnostic( aResult, "error", "invalid_sheet_pin_position",
                                    "sheet pin position must contain two distances from 0 to 2 m" );
                    }
                    else
                    {
                        pin["position"] = { { "xNm", x }, { "yNm", y } };
                    }
                }
                else if( pinHead == "side" )
                {
                    std::string side;

                    if( !parseSingleValueForm( aDocument, pinFieldNode, side )
                        || !std::set<std::string>( { "left", "right", "top", "bottom" } )
                                    .contains( side ) )
                    {
                        diagnostic( aResult, "error", "invalid_sheet_pin_side",
                                    "sheet pin side must be left, right, top, or bottom" );
                    }
                    else
                    {
                        pin["side"] = side;
                    }
                }
                else
                {
                    diagnostic( aResult, "error", "unknown_sheet_pin_field",
                                "sheet pin supports only at and side" );
                }
            }

            if( !pin.contains( "position" ) || !pin.contains( "side" ) )
            {
                diagnostic( aResult, "error", "missing_sheet_pin_field",
                            "sheet pin requires exactly one at and side" );
            }

            sheet["pins"].emplace_back( std::move( pin ) );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_sheet_field",
                        "sheet field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "title" || head == "file" || head == "parent" )
        {
            std::string value;

            if( !parseSingleValueForm( aDocument, child, value ) || value.empty()
                || value.size() > 4096 )
            {
                diagnostic( aResult, "error", "invalid_sheet_field",
                            "sheet title, file, and parent require one bounded value" );
                continue;
            }

            if( head == "title" && value.size() > 256 )
            {
                diagnostic( aResult, "error", "invalid_sheet_title",
                            "sheet title may contain at most 256 bytes" );
            }
            else if( head == "file" )
            {
                const bool safe = value.ends_with( ".kicad_sch" )
                                  && value.front() != '/' && value.front() != '\\'
                                  && value.find( '\0' ) == std::string::npos
                                  && value.find_first_of( "\r\n\\" ) == std::string::npos
                                  && !value.starts_with( "../" )
                                  && value.find( "/../" ) == std::string::npos
                                  && !value.ends_with( "/.." ) && value.find( ':' ) == std::string::npos;

                if( !safe )
                {
                    diagnostic( aResult, "error", "invalid_sheet_file",
                                "sheet file must be a safe project-relative .kicad_sch path" );
                }
            }
            else if( head == "parent" && value != "none" && !validIdentifier( value ) )
            {
                diagnostic( aResult, "error", "invalid_sheet_parent",
                            "sheet parent must be none or a bounded sheet identifier" );
            }

            sheet[head] = head == "parent" && value == "none" ? JSON( nullptr ) : JSON( value );
        }
        else if( head == "at" || head == "size" )
        {
            std::string xText;
            std::string yText;
            int64_t x = 0;
            int64_t y = 0;

            if( field.children.size() != 3
                || !scalarText( aDocument, field.children[1], xText )
                || !scalarText( aDocument, field.children[2], yText )
                || !parseDistance( xText, x ) || !parseDistance( yText, y )
                || ( head == "at" && ( x < 0 || y < 0 ) )
                || ( head == "size" && ( x <= 0 || y <= 0 ) )
                || x > 2'000'000'000LL || y > 2'000'000'000LL )
            {
                diagnostic( aResult, "error", "invalid_sheet_" + head,
                            "sheet " + head + " requires two bounded physical distances" );
            }
            else
            {
                sheet[head == "at" ? "position" : "size"] = {
                    { "xNm", x }, { "yNm", y }
                };
            }
        }
        else
        {
            diagnostic( aResult, "error", "unknown_sheet_field",
                        "sheet supports title, file, parent, at, size, and pin" );
        }
    }

    for( const char* required : { "title", "file" } )
    {
        if( !sheet.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_sheet_field",
                        "sheet " + id + " is missing " + required );
        }
    }

    if( !fields.contains( "parent" ) )
    {
        diagnostic( aResult, "error", "missing_sheet_field",
                    "sheet " + id + " is missing parent" );
    }

    if( sheet["pins"].size() > 256 )
    {
        diagnostic( aResult, "error", "too_many_sheet_pins",
                    "a sheet may declare at most 256 hierarchical pins" );
    }

    return sheet;
}


JSON compileEnumeratedFacet( const DOCUMENT& aDocument, size_t aNode, const std::string& aFacet,
                             const std::set<std::string>& aAllowed,
                             KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    std::string kind;

    if( !parseSingleValueForm( aDocument, aNode, kind ) || !aAllowed.contains( kind ) )
    {
        diagnostic( aResult, "error", "invalid_" + aFacet,
                    aFacet + " must contain exactly one supported KDS version 1 kind" );
        return JSON::object();
    }

    return { { "kind", kind } };
}


bool containsError( const JSON& aDiagnostics )
{
    return std::any_of( aDiagnostics.begin(), aDiagnostics.end(),
                        []( const JSON& aDiagnostic )
                        {
                            return aDiagnostic.value( "severity", "error" ) == "error";
                        } );
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_COMPILER::JSON DESIGN_SCRIPT_COMPILER::Describe()
{
    JSON description = {
        { "language", "kichad-design" },
        { "version", LANGUAGE_VERSION },
        { "syntax", "s-expression" },
        { "root", "kichad_design" },
        { "deterministic", true },
        { "hostCodeExecution", false },
        { "topLevelForms",
          JSON::array( {
                  { { "form", "(version 1)" }, { "required", true } },
                  { { "form",
                      "(project NAME (title TEXT) (company TEXT) (revision TEXT) (date TEXT) "
                      "(comment 1..9 TEXT) "
                      "(text_variables (variable NAME VALUE) ...) "
                      "(field_templates "
                      "(field NAME (visible BOOL) (url BOOL)) ...) ...)" },
                    { "required", true } },
                  { { "form", "(units mm|mil)" }, { "default", "mm" } },
                  { { "form",
                      "(library symbol|footprint|model ID (table global)) | "
                      "(library symbol|footprint|model ID (table project) "
                      "(uri ${KIPRJMOD}/PATH) [(managed true)])" } },
                  { { "form",
                      "(symbol LIBRARY:NAME (reference PREFIX [FIELD_LAYOUT]) "
                      "(value TEXT [FIELD_LAYOUT]) (footprint TEXT [FIELD_LAYOUT]) "
                      "(datasheet TEXT [FIELD_LAYOUT]) (description TEXT [FIELD_LAYOUT]) "
                      "(keywords TEXT [FIELD_LAYOUT]) (property NAME VALUE [FIELD_LAYOUT]) "
                      "FIELD_LAYOUT=(at X Y) (rotation ANGLE) (visible BOOL) "
                      "(show_name BOOL) (autoplace BOOL) (private BOOL) (size W H) "
                      "(font stroke|NAME) (line_spacing NUMBER) (thickness auto|DISTANCE) "
                      "(color default|R G B A) (justify H V) (bold BOOL) (italic BOOL) "
                      "(hyperlink none|URI) [(power normal|global|local)] "
                      "[(extends SAME_LIBRARY_PARENT)] "
                      "(exclude_from_sim BOOL) (in_bom BOOL) (on_board BOOL) "
                      "(in_pos_files BOOL) (hide_pin_names BOOL) "
                      "(duplicate_pin_numbers_are_jumpers BOOL) (jumper_group PIN...) "
                      "(hide_pin_numbers BOOL) (pin_names_offset DISTANCE) "
                      "(unit common|1..256 [(body_style 1..64)] "
                      "(rectangle ID (from X Y) (to X Y) [(radius DISTANCE)]) | "
                      "(circle ID (center X Y) (radius DISTANCE)) | "
                      "(arc ID (start X Y) (mid X Y) (end X Y)) | "
                      "(bezier ID (start X Y) (control1 X Y) (control2 X Y) (end X Y)) | "
                      "(polyline ID (point X Y)...) "
                      "[(private BOOL)] [(stroke WIDTH STYLE [(color R G B A)])] "
                      "[(fill TYPE [(color R G B A)])] | "
                      "(text ID TEXT (at X Y) [(rotation ANGLE)] [(size WIDTH HEIGHT)] "
                      "[(font stroke|NAME)] [(thickness auto|DISTANCE)] [(bold BOOL)] "
                      "[(italic BOOL)] [(line_spacing NUMBER)] [(color default|R G B A)] "
                      "[(justify left|center|right top|center|bottom)] [(hyperlink URI)]) | "
                      "(text_box ID TEXT (at X Y) (box_size WIDTH HEIGHT) "
                      "[(margins LEFT TOP RIGHT BOTTOM)] [TEXT_FIELDS] [STROKE] [FILL]) | "
                      "(pin NUMBER (name TEXT) (electrical TYPE) (shape SHAPE) "
                      "(at X Y) (orientation right|down|left|up) (length DISTANCE) "
                      "(hidden BOOL) (name_size DISTANCE) (number_size DISTANCE) "
                      "(alternate NAME ELECTRICAL_TYPE SHAPE)... ) ...))" } },
                  { { "form",
                      "(footprint LIBRARY:NAME (reference TEXT) (value TEXT) "
                      "(datasheet TEXT) (description TEXT) (keywords TEXT) "
                      "(attributes (smd BOOL) (through_hole BOOL) (board_only BOOL) "
                      "(exclude_from_position BOOL) (exclude_from_bom BOOL) "
                      "(allow_missing_courtyard BOOL) (dnp BOOL) "
                      "(allow_soldermask_bridges BOOL)) "
                      "(duplicate_pad_numbers_are_jumpers BOOL) "
                      "(jumper_group PAD_NUMBER...) (net_tie_group PAD_NUMBER...) "
                      "(pad ID (number TEXT) (type smd|thru_hole|connect|np_thru_hole) "
                      "(shape circle|rect|oval|trapezoid|roundrect|chamfered_rect|custom) (at X Y) "
                      "(rotation ANGLE) (size W H) (layers LAYER...) "
                      "[(drill round DIAMETER)|(drill oval WIDTH HEIGHT)] "
                      "[(shape_offset X Y)] [(trapezoid_delta X Y)] "
                      "[(roundrect_radius DISTANCE)] [(chamfer_ratio DECIMAL)] "
                      "[(chamfer CORNER...)] "
                      "[(custom (anchor circle|rect) (clearance outline|convex_hull) "
                      "(line|rectangle|arc|circle|polygon|bezier ID ...)...)] "
                      "[(padstack front_inner_back|custom "
                      "(layer inner|InN.Cu|B.Cu (shape SHAPE) (size W H) "
                      "[(custom (anchor circle|rect) "
                      "(line|rectangle|arc|circle|polygon|bezier ID ...)...)] ...)...)] "
                      "[(backdrills (top (diameter D) (stop_layer InN.Cu)) "
                      "(bottom (diameter D) (stop_layer InN.Cu)))] "
                      "[(teardrop (enabled BOOL) (target_length RATIO MAX) "
                      "(target_width RATIO MAX) (edges straight|curved) "
                      "(track_width_limit RATIO) (allow_two_segments BOOL) "
                      "(prefer_zone_connections BOOL))] "
                      "[(hole_treatment "
                      "(tenting (front inherit|open|tented) (back inherit|open|tented)) "
                      "(post_machining front|back counterbore|countersink "
                      "(diameter DISTANCE) [(depth DISTANCE)] [(angle ANGLE)])...)] "
                      "[(property TYPE)] "
                      "[(pin_function TEXT)] [(pin_type TEXT)] [(die_length DISTANCE)] "
                      "[(solder_mask_margin inherit|DISTANCE)] "
                      "[(solder_paste_margin inherit|DISTANCE)] "
                      "[(solder_paste_margin_ratio inherit|DECIMAL)] "
                      "[(clearance inherit|DISTANCE)] "
                      "[(zone_connection inherit|none|thermal|solid|tht_thermal)] "
                      "[(thermal_spoke_width inherit|DISTANCE)] "
                      "[(thermal_spoke_angle inherit|ANGLE)] "
                      "[(thermal_gap inherit|DISTANCE)] "
                      "[(remove_unused_layers BOOL)] [(keep_end_layers BOOL)]) ... "
                      "(line ID (start X Y) (end X Y) GRAPHIC_STYLE) | "
                      "(rectangle ID (start X Y) (end X Y) [(radius DISTANCE)] GRAPHIC_STYLE) | "
                      "(arc ID (start X Y) (mid X Y) (end X Y) GRAPHIC_STYLE) | "
                      "(circle ID (center X Y) (radius DISTANCE) GRAPHIC_STYLE) | "
                      "(polygon ID (point X Y)... GRAPHIC_STYLE) | "
                      "(bezier ID (start X Y) (control1 X Y) (control2 X Y) "
                      "(end X Y) GRAPHIC_STYLE), where GRAPHIC_STYLE is "
                      "(stroke WIDTH solid|dash|dash_dot|dash_dot_dot|dot) "
                      "(layers LAYER...) [(fill none|solid|hatch|reverse_hatch|cross_hatch)] "
                      "[(locked BOOL)] [(solder_mask_margin inherit|DISTANCE)]; "
                      "(text ID TEXT (at X Y) (rotation ANGLE) (layer LAYER) "
                      "(font [(face default|NAME)] (size HEIGHT WIDTH) "
                      "[(line_spacing DECIMAL)] [(thickness auto|DISTANCE)] "
                      "[(bold BOOL)] [(italic BOOL)]) "
                      "(justify left|center|right top|center|bottom MIRRORED_BOOL) "
                      "[(locked BOOL)] [(keep_upright BOOL)] [(knockout BOOL)]); "
                      "(text_box ID TEXT (box X1 Y1 X2 Y2)|"
                      "(polygon (point X Y)...) (rotation ANGLE) (layer LAYER) "
                      "(margins LEFT TOP RIGHT BOTTOM) FONT JUSTIFY "
                      "(stroke WIDTH STYLE) (border BOOL) (knockout BOOL) (locked BOOL) "
                      "[(hyperlink URI)]); "
                      "(property ID (name TEXT) (value TEXT) (at X Y) (rotation ANGLE) "
                      "(layer LAYER) (visible BOOL) (keep_upright BOOL) (knockout BOOL) "
                      "FONT JUSTIFY); "
                      "[(rules (clearance inherit|DISTANCE) "
                      "(solder_mask_margin inherit|DISTANCE) "
                      "(solder_paste_margin inherit|DISTANCE) "
                      "(solder_paste_margin_ratio inherit|DECIMAL) "
                      "(zone_connection inherit|none|thermal|solid|pth_thermal))] "
                      "[(stackup custom (layers F.Cu In1.Cu... B.Cu))] "
                      "[(private_layers LAYER...)] "
                      "[(component_classes NAME...)] "
                      "(zone ID (purpose copper|keepout) [(name TEXT)] (layers LAYER...) "
                      "(outline (polygon (point X Y)... [(hole (point X Y)...)...])) "
                      "[(clearance DISTANCE) (min_thickness DISTANCE) "
                      "(connection none|solid|thermal|pth_thermal "
                      "[(thermal_gap DISTANCE) (thermal_spoke_width DISTANCE)]) "
                      "(islands remove_all|keep_all|remove_below [(minimum_area AREA)]) "
                      "(fill solid|hatched [HATCH_FIELDS]) [(priority UINT)] "
                      "[(corner_smoothing none|chamfer|fillet [(radius DISTANCE)])]] "
                      "[(prohibit (copper BOOL) (vias BOOL) (tracks BOOL) "
                      "(pads BOOL) (footprints BOOL))] [(border STYLE [(pitch DISTANCE)])] "
                      "[(locked BOOL)]); "
                      "(group ID [(name TEXT)] [(locked BOOL)] "
                      "(member pad|graphic|text|text_box|property|zone|group ID)...); "
                      "(variant ID (dnp BOOL) (exclude_from_bom BOOL) "
                      "(exclude_from_position BOOL) [(field NAME VALUE)...]); "
                      "(model ${KIPRJMOD}/PATH.step|.stp|.wrl [(visible BOOL)] "
                      "[(opacity DECIMAL)] [(offset X Y Z)] [(scale X Y Z)] "
                      "[(rotation X Y Z)]) ...)" } },
                  { { "form",
                      "(component REF (symbol LIB:ID) (value VALUE) "
                      "(footprint LIB:ID|none) "
                      "(datasheet TEXT) (description TEXT) (property NAME VALUE) "
                      "(dnp true|false) "
                      "(unit NUMBER (sheet ID) (at X Y) "
                      "(rotation 0deg|90deg|180deg|270deg) (mirror none|x|y|xy) "
                      "(fields_autoplaced BOOL) "
                      "(field NAME (at X Y) (rotation ANGLE) (visible BOOL) "
                      "(show_name BOOL) (autoplace BOOL) (size W H) "
                      "(font stroke|NAME) (line_spacing NUMBER) "
                      "(thickness auto|SIZE) (color default|#RRGGBB[AA]) "
                      "(justify left|center|right top|center|bottom) "
                      "(bold BOOL) (italic BOOL) (hyperlink none|TEXT) "
                      "(private BOOL)) ...)) ...)" } },
                  { { "form",
                      "(net NAME (presentation wired|labels) "
                      "(pin REF UNIT NUMBER) (pin REF UNIT NUMBER) ...)" } },
                  { { "form", "(no_connect REF UNIT NUMBER)" } },
                  { { "form",
                      "(wire|bus|bus_entry ID (sheet ID) (from X Y) (to X Y) "
                      "(stroke default|WIDTH default|solid|dash|dot|dash_dot|dash_dot_dot))" } },
                  { { "form",
                      "(junction ID (sheet ID) (at X Y) (diameter auto|SIZE) "
                      "(color default|#RRGGBB[AA]))" } },
                  { { "form",
                      "(label ID (sheet ID) (scope local|global) (net NAME) (at X Y) "
                      "(rotation ORTHOGONAL) (shape none|input|output|bidirectional|tri_state|passive) "
                      "(size W H) (thickness auto|SIZE) "
                      "(justify left|center|right top|center|bottom) "
                      "(bold true|false) (italic true|false))" } },
                  { { "form",
                      "(rule_area ID (sheet ID) (polygon (point X Y) ...) "
                      "(stroke default|WIDTH STYLE default|#RRGGBB[AA]) "
                      "(fill TYPE default|#RRGGBB[AA]) (exclude_from_sim BOOL) "
                      "(exclude_from_bom BOOL) (exclude_from_board BOOL) (dnp BOOL))" } },
                  { { "form",
                      "(directive ID (sheet ID) (target net|rule_area NAME) (at X Y) "
                      "(rotation ORTHOGONAL) (shape dot|round|diamond|rectangle) "
                      "(length SIZE) (property NAME VALUE (at X Y) "
                      "(rotation ORTHOGONAL) (size W H) (thickness auto|SIZE) "
                      "(justify left|center|right top|center|bottom) "
                      "(bold true|false) (italic true|false) (visible true|false)) ... )" } },
                  { { "form",
                      "(text CONTENT (id ID) (sheet ID) (at X Y) (rotation ANGLE) "
                      "(exclude_from_sim BOOL) (size W H) (font stroke|NAME) "
                      "(line_spacing NUMBER) (thickness auto|SIZE) "
                      "(color default|#RRGGBB[AA]) "
                      "(justify left|center|right top|center|bottom) (mirror BOOL) "
                      "(bold BOOL) (italic BOOL) (hyperlink none|TEXT))" } },
                  { { "form",
                      "(text_box CONTENT (id ID) (sheet ID) (at X Y) (rotation ANGLE) "
                      "(box_size W H) (margins LEFT TOP RIGHT BOTTOM) "
                      "(exclude_from_sim BOOL) (stroke none|default|WIDTH STYLE COLOR) "
                      "(fill TYPE COLOR) (text_size W H) (font stroke|NAME) "
                      "(line_spacing NUMBER) (thickness auto|SIZE) "
                      "(color default|#RRGGBB[AA]) "
                      "(justify left|center|right top|center|bottom) (mirror BOOL) "
                      "(bold BOOL) (italic BOOL) (hyperlink none|TEXT))" } },
                  { { "form",
                      "(polyline ID (sheet ID) (points (point X Y) ...) STROKE FILL) | "
                      "(rectangle ID (sheet ID) (from X Y) (to X Y) (radius R) STROKE FILL) | "
                      "(circle ID (sheet ID) (center X Y) (radius R) STROKE FILL) | "
                      "(arc ID (sheet ID) (start X Y) (mid X Y) (end X Y) STROKE FILL) | "
                      "(bezier ID (sheet ID) (points (point X Y) x4) STROKE FILL); "
                      "STROKE=(stroke none|default|WIDTH STYLE default|#RRGGBB[AA]); "
                      "FILL=(fill TYPE default|#RRGGBB[AA])" } },
                  { { "form",
                      "(image ID (sheet ID) (at X Y) (scale FACTOR) "
                      "(media_type image/png|jpeg|gif|bmp|webp) (sha256 DIGEST) "
                      "(description TEXT) (data_base64 DATA))" } },
                  { { "form",
                      "(table ID (sheet ID) (at X Y) (rotation 0deg|90deg) "
                      "(columns WIDTH ...) (rows HEIGHT ...) "
                      "(border (external BOOL) (header BOOL) (stroke WIDTH STYLE COLOR)) "
                      "(separators (rows BOOL) (columns BOOL) (stroke WIDTH STYLE COLOR)) "
                      "(cells (cell ROW COLUMN CONTENT (margins LEFT TOP RIGHT BOTTOM) "
                      "(exclude_from_sim BOOL) (fill TYPE COLOR) (text_size W H) "
                      "(font stroke|NAME) (line_spacing NUMBER) (thickness auto|SIZE) "
                      "(color default|#RRGGBB[AA]) "
                      "(justify left|center|right top|center|bottom) (mirror BOOL) "
                      "(bold BOOL) (italic BOOL) (hyperlink none|TEXT)) ...) "
                      "(merges (merge START_ROW START_COLUMN END_ROW END_COLUMN) ...))" } },
                  { { "form", "(bus_alias NAME (sheet ID) (members NET ...))" } },
                  { { "form",
                      "(group ID (sheet ID) (name TEXT) (locked BOOL) "
                      "(members (member drawing ID) (member component REF UNIT) "
                      "(member sheet ID) (member hierarchical_label SHEET PIN) "
                      "(member net_label NET REF UNIT PIN OCCURRENCE) "
                      "(member no_connect REF UNIT PIN OCCURRENCE) "
                      "(member group ID) ...))" } },
                  { { "form",
                      "(sheet ID (parent none|ID) (file PROJECT_PATH.kicad_sch) "
                      "(title TEXT) [(at X Y) (size W H) "
                      "(pin NAME input|output|bidirectional|tri_state|passive "
                      "(at X Y) (side left|right|top|bottom)) ...])" } },
                  { { "form",
                      "(board (stackup ...) (outline "
                      "(line|rectangle|arc|circle|polygon|bezier ID ... "
                      "(stroke WIDTH solid) (layers Edge.Cuts) (fill none)) ...) "
                      "(layout (board (maximum_width D) (maximum_height D)) "
                      "[(placement (near REF REF (maximum D)) "
                      "(align REF REF (axis x|y) (tolerance D)) "
                      "(edge REF left|right|top|bottom (maximum D)) "
                      "(group ID (members REF...) (maximum_span D)) ...)] "
                      "(routing (defaults (geometry any|orthogonal|octilinear) "
                      "(maximum_vias UINT)) "
                      "[(net NET [(geometry ...)] [(maximum_vias UINT)] "
                      "[(maximum_length D)]) ...] "
                      "[(bundle ID (nets NET...) (maximum_skew D)) ...]))) "
                      "[(synthesize "
                      "[(placement [(grid D)] [(clearance D)] [(edge_clearance D)] "
                      "[(rotations 0deg|90deg|180deg|270deg ...)])] "
                      "[(routing [(grid D)] [(clearance D)] [(width D)] "
                      "[(layer F.Cu|B.Cu)])])] "
                      "(place REF (at X Y) [(rotation A)] [(side front|back)] [(locked BOOL)] "
                      "[(reference|value [(visible BOOL)] [(layer F.SilkS|B.SilkS|F.Fab|B.Fab)] "
                      "[(at X Y)] [(size W H)] [(stroke D)] [(angle A)] "
                      "[(justify H V)] [(font stroke|NAME)] [(bold BOOL)] [(italic BOOL)] "
                      "[(underlined BOOL)] [(mirrored BOOL)] [(keep_upright BOOL)])]) "
                      "(route NET (id ID) (from X Y) (to X Y) ...) "
                      "(line|rectangle|arc|circle|polygon|bezier ID ... "
                      "(stroke WIDTH STYLE) (layers LAYER...) (fill none|solid) "
                      "[(net none|NET)] [(solder_mask_margin inherit|DISTANCE)] "
                      "[(locked BOOL)]) ... "
                      "(via NET (id ID) (at X Y) (drill D) "
                      "((diameter D)|(padstack front_inner_back|custom "
                      "(layer LAYER (shape SHAPE) (size W H) "
                      "[(custom (anchor circle|rect) "
                      "(line|rectangle|arc|circle|polygon|bezier ID ...)...)] ...)...)) "
                      "[(layers START END)] [(type through|blind|buried|micro)] "
                      "[(unconnected_layers keep|remove|keep_start_end|start_end_only)] "
                      "[(force_flash LAYER...)] "
                      "[(teardrop (enabled BOOL) (target_length RATIO MAX) "
                      "(target_width RATIO MAX) (edges straight|curved) "
                      "(track_width_limit RATIO) (allow_two_segments BOOL) "
                      "(prefer_zone_connections BOOL))] "
                      "[(backdrills (top (diameter D) (stop_layer LAYER)) "
                      "(bottom (diameter D) (stop_layer LAYER)))] "
                      "[(protection (tenting ...) (covering ...) (plugging ...) "
                      "(filling inherit|filled|unfilled) (capping inherit|capped|uncapped) "
                      "(post_machining front|back counterbore|countersink ...))]) "
                      "(zone NET ...) (text ...) (text_box ...) (table ...) (dimension ...) "
                      "(keepout ...) (image ...) (barcode ...))" } },
                  { { "form",
                      "(stackup (finish NAME) (impedance_controlled BOOL) "
                      "(edge_connector none|yes|bevelled) (edge_plating BOOL) "
                      "(layers (silkscreen LAYER ...) (solderpaste LAYER) "
                      "(soldermask LAYER ...) (copper LAYER (thickness D)) "
                      "(dielectric core|prepreg (thickness D) (material NAME) "
                      "(epsilon_r N) (loss_tangent N) (locked BOOL)) ...))" } },
                  { { "form",
                      "(zone NET (id ID) (layers F.Cu ...) "
                      "(outline (polygon (point X Y) ... (hole (point X Y) ...))) "
                      "(clearance D) (min_thickness D) "
                      "(connection none|solid|thermal|pth_thermal "
                      "(thermal_gap D) (thermal_spoke_width D)) "
                      "(islands remove_all|keep_all|remove_below ...) "
                      "(fill solid|hatched ...) "
                      "(hatch_offsets (layer F.Cu X Y) ...))" } },
                  { { "form",
                      "(keepout (id ID) (layers F.Cu ...) "
                      "(outline (polygon (point X Y) ... (hole (point X Y) ...))) "
                      "(prohibit (copper BOOL) (vias BOOL) (tracks BOOL) "
                      "(pads BOOL) (footprints BOOL)))" } },
                  { { "form",
                      "(text VALUE (id ID) (layer LAYER) (at X Y) (size W H) "
                      "(stroke D) (angle A) (justify HORIZONTAL VERTICAL) "
                      "(font stroke|NAME) ...)" } },
                  { { "form",
                      "(text_box ID TEXT ((box X1 Y1 X2 Y2)|"
                      "(polygon (point X Y)...)) (rotation ANGLE) (layer LAYER) "
                      "(margins LEFT TOP RIGHT BOTTOM) "
                      "(font (face default|NAME) (size HEIGHT WIDTH) "
                      "(line_spacing RATIO) (thickness auto|DISTANCE) "
                      "(bold BOOL) (italic BOOL)) "
                      "(justify HORIZONTAL VERTICAL MIRRORED_BOOL) "
                      "(stroke WIDTH STYLE) (border BOOL) (knockout BOOL) "
                      "(locked BOOL) [(hyperlink URI)])" } },
                  { { "form",
                      "(table ID (at X Y) (rotation 0deg|90deg) (layer LAYER) "
                      "(locked BOOL) (columns WIDTH...) (rows HEIGHT...) "
                      "(border (external BOOL) (header BOOL) (stroke WIDTH STYLE COLOR)) "
                      "(separators (rows BOOL) (columns BOOL) (stroke WIDTH STYLE COLOR)) "
                      "(cells (cell ROW COLUMN TEXT (margins L T R B) "
                      "(text_size W H) (font stroke|NAME) (line_spacing RATIO) "
                      "(thickness auto|DISTANCE) (justify H V) (mirror BOOL) "
                      "(bold BOOL) (italic BOOL) (hyperlink none|URI) (locked BOOL))...) "
                      "(merges (merge START_ROW START_COLUMN END_ROW END_COLUMN)...))" } },
                  { { "form",
                      "(dimension aligned|orthogonal|radial|leader|center (id ID) "
                      "(layer LAYER) STYLE_GEOMETRY (line_width D) (arrow_length D) ...)" } },
                  { { "form",
                      "(image ID (at X Y) (origin_offset X Y) (layer LAYER) "
                      "(scale FACTOR) (locked BOOL) "
                      "(media_type image/png|jpeg|gif|bmp|webp) (sha256 DIGEST) "
                      "(description TEXT) (data_base64 DATA))" } },
                  { { "form",
                      "(barcode ID (text TEXT) "
                      "(kind code_39|code_128|data_matrix|qr_code|micro_qr_code) "
                      "(error_correction L|M|Q|H) (at X Y) (rotation ANGLE) "
                      "(layer LAYER) (size WIDTH HEIGHT) (show_text BOOL) "
                      "(text_height HEIGHT) (knockout BOOL) "
                      "(knockout_margin X Y) (locked BOOL))" } },
                  { { "form",
                      "(rules (minimum_clearance D) (minimum_connection_width D) "
                      "(minimum_track_width D) (minimum_via_annular_width D) "
                      "(minimum_via_diameter D) (minimum_through_hole_diameter D) "
                      "(minimum_microvia_diameter D) (minimum_microvia_drill D) "
                      "(minimum_hole_to_hole D) (minimum_copper_to_hole_clearance D) "
                      "(minimum_silkscreen_clearance D) (minimum_groove_width D) "
                      "(minimum_resolved_spokes N) (minimum_silkscreen_text_height D) "
                      "(minimum_silkscreen_text_thickness D) "
                      "(minimum_copper_to_edge_clearance D|legacy) "
                      "(use_height_for_length_calculations BOOL) (maximum_error D) "
                      "(allow_fillets_outside_zone_outline BOOL))" } },
                  { { "form",
                      "(net_classes (class Default COMPLETE_FIELDS) "
                      "(class NAME VALUE_OR_INHERIT_FIELDS) ... "
                      "(assign (pattern PATTERN) (classes NAME ...)) ...)" } },
                  { { "form",
                      "(custom_rules (rule NAME (condition always|EXPRESSION) "
                      "(layer all|outer|inner|LAYER) "
                      "(severity ignore|warning|error|exclusion) "
                      "(constraint TYPE TYPED_VALUES) ...))" } },
                  { { "form",
                      "(source REF (manufacturer NAME) (mpn PART) (datasheet HTTPS_URL) "
                      "(lifecycle active|nrnd|last_time_buy|obsolete) (supplier NAME) "
                      "(sku PART) (product_url HTTPS_URL) (available N) "
                      "(verified_on YYYY-MM-DD) (quantity N) [(unit_price TEXT) (notes TEXT)])" } },
                  { { "form",
                      "(conformance REF (datasheet HTTPS_URL) (verified_on YYYY-MM-DD) "
                      "(pins verified) (application verified) [(deviation TEXT) ...])" } },
                  { { "form",
                      "(electrical "
                      "(rail ID (net NET) (voltage MIN NOMINAL MAX) "
                      "(source_current CURRENT) (reserve PERCENT) "
                      "(load COMPONENT CURRENT) ...) "
                      "(rating COMPONENT [(operating_voltage V) (rated_voltage V)] "
                      "[(operating_current A) (rated_current A)] "
                      "[(operating_power W) (rated_power W)] (derating PERCENT)) ... "
                      "(thermal COMPONENT (dissipation W) (theta_ja C/W) "
                      "(ambient C) (maximum_junction C)) ... "
                      "(logic ID (net NET) (driver COMPONENT UNIT PIN) "
                      "(receiver COMPONENT UNIT PIN) ... (output_low V) "
                      "(output_high V) (input_low V) (input_high V) "
                      "(signal_min V) (signal_max V) "
                      "[(absolute_min V) (absolute_max V)]) ... "
                      "(simulation ID (ground NET) "
                      "(device ID (component REF) (unit NUMBER) "
                      "(kind resistor|capacitor|inductor|diode|bjt|mosfet) "
                      "(nodes NET...) (pins PIN...) [(value QUANTITY)] [(model ID)]) ... "
                      "[(model ID (kind diode|npn|pnp|nmos|pmos) "
                      "(parameter NAME NUMBER) ...)] ... "
                      "(source ID (kind voltage|current) (positive NET) (negative NET) "
                      "(dc QUANTITY) [(ac MAGNITUDE PHASE)] "
                      "[(pulse INITIAL PULSED DELAY RISE FALL WIDTH PERIOD)]) ... "
                      "(analysis ID (kind operating_point|transient|dc_sweep|ac_sweep) ...) ... "
                      "(assert ID (analysis ID) "
                      "(probe voltage NET [RETURN_NET]|current SOURCE) "
                      "[(minimum QUANTITY)] [(maximum QUANTITY)] [(scope all|final)]) ...)) ...)" } },
                  { { "form",
                      "(production "
                      "(assembly (acceptance ipc-a-610-class-1|ipc-a-610-class-2|ipc-a-610-class-3) "
                      "(process lead_free_reflow|leaded_reflow|hand|mixed) (solder_alloy TEXT) "
                      "(stencil none|top|bottom|both) [(stencil_thickness_um N)] "
                      "(cleaning no_clean|aqueous|solvent) "
                      "(coating none|acrylic|silicone|urethane|parylene) "
                      "[(instruction ID (scope board|component REF) (text TEXT)) ...]) "
                      "(firmware ID ((path PROJECT_RELATIVE_FILE)|(data_base64 DATA)) "
                      "(format binary|ihex|elf|uf2|srec) (sha256 DIGEST) (bytes N) "
                      "(version TEXT) (target REF) "
                      "[(device_code (toolchain arduino_cli|platformio|zephyr_west|esp_idf|pico_sdk|cmake|cargo_embedded) "
                      "(toolchain_version TEXT) (target TEXT) (entry PATH) "
                      "(file PATH (language c|cpp|rust|assembly|arduino|linker_script|configuration) "
                      "(sha256 DIGEST) (data_base64 DATA)) ... "
                      "[(dependency NAME VERSION SHA256) ...])]) ... "
                      "(program ID (firmware ID) (target REF) "
                      "(interface swd|jtag|isp|updi|pdi|debugwire|uart_bootloader|usb_dfu|can_bootloader) "
                      "(connector REF) (device TEXT) (voltage V) (speed_khz N) "
                      "(erase none|chip|required_regions) (reset none|run|halt) (verify true) "
                      "(signal NAME CONNECTOR_PIN) ... [(option NAME VALUE) ...]) ... "
                      "(power ID (connector REF) (positive_pin PIN) (return_pin PIN) "
                      "(voltage V) (current_limit A) (settle_ms N)) ... "
                      "(test ID (stage unpowered|power_on|programmed|functional) "
                      "(method voltage|current|resistance|continuity|logic|frequency|visual|functional) "
                      "(instrument dmm|oscilloscope|logic_analyzer|frequency_counter|ict|flying_probe|camera|operator|fixture) "
                      "(target net NAME|component REF|pin REF PIN) "
                      "[(range MIN MAX V|A|ohm|Hz)|(expected open|closed|high|low|pulse|pass)] "
                      "[(testpoint REF)] [(power ID)] [(program ID)] "
                      "(after_ms N) (timeout_ms N) (required true) "
                      "[(procedure TEXT)]) ... )" } },
                  { { "form",
                      "(check erc|electrical|drc|layout|sourcing|footprints|fabrication)" } },
                  { { "form", "(fab \"NAME\")" },
                    { "description",
                      "Declares the fabrication vendor profile for external layout and "
                      "fabrication tools that read the KDS directly." } },
                  { { "form",
                      "(output gerbers|drill|ipcd356|netlist|ipc2581|odbpp|pick_place|bom|step|"
                      "stepz|brep|glb|stl|u3d|xao|3d_pdf|pdf|board_ps|"
                      "schematic_pdf|schematic_svg|schematic_dxf|schematic_ps|"
                      "schematic_bom|legacy_bom_xml|"
                      "board_render|assembly_svg|assembly_dxf|gencad|vrml|board_stats)" } }
          } ) },
        { "compilerPasses",
          JSON::array( { "parse", "typecheck", "resolve", "plan", "snapshot", "schematic",
                         "libraries", "pcb", "electrical", "layout", "sourcing",
                         "production_handoff", "erc", "drc", "fabrication" } ) },
        { "example",
          "(kichad_design\n"
          "  (version 1)\n"
          "  (project sensor (title \"Sensor Board\"))\n"
          "  (units mm)\n"
          "  (library symbol Device (table project) "
          "(uri \"${KIPRJMOD}/Device.kicad_sym\"))\n"
          "  (library footprint Resistor_SMD (table project) "
          "(uri \"${KIPRJMOD}/Resistor_SMD.pretty\"))\n"
          "  (library footprint LED_SMD (table project) "
          "(uri \"${KIPRJMOD}/LED_SMD.pretty\"))\n"
          "  (sheet root (parent none) (file \"sensor.kicad_sch\") (title \"Main\"))\n"
          "  (component R1 (symbol \"Device:R\") (value \"10k\")\n"
          "    (footprint \"Resistor_SMD:R_0603_1608Metric\")\n"
          "    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none)))\n"
          "  (component LED1 (symbol \"Device:LED\") (value \"GREEN\")\n"
          "    (footprint \"LED_SMD:LED_0603_1608Metric\")\n"
          "    (unit 1 (sheet root) (at 50mm 40mm) (rotation 0deg) (mirror none)))\n"
          "  (net LED_A (pin R1 1 1) (pin LED1 1 1))\n"
          "  (check erc)\n"
          "  (check drc)\n"
          "  (output gerbers))" }
    };
    description["capabilityCoverage"] = DesignScriptCapabilities();
    return description;
}


DESIGN_SCRIPT_COMPILER::RESULT DESIGN_SCRIPT_COMPILER::Compile( const std::string& aSource )
{
    RESULT result;

    if( aSource.empty() || aSource.size() > MAX_SCRIPT_BYTES )
    {
        diagnostic( result, "error", "invalid_source_size",
                    "KiChad Design Script source must contain 1 byte to 16 MiB" );
        return result;
    }

    picosha2::hash256_hex_string( aSource, result.sourceSha256 );

    const size_t nulOffset = aSource.find( '\0' );

    if( nulOffset != std::string::npos )
    {
        diagnostic( result, "error", "invalid_encoding",
                    "KiChad Design Script source must not contain embedded NUL bytes",
                    sourceLocation( aSource, nulOffset ) );
        return result;
    }

    const size_t invalidUtf8Offset = firstInvalidUtf8Byte( aSource );

    if( invalidUtf8Offset != std::string::npos )
    {
        diagnostic( result, "error", "invalid_encoding",
                    "KiChad Design Script source must be valid UTF-8",
                    sourceLocation( aSource, invalidUtf8Offset ) );
        return result;
    }

    const wxString decoded = wxString::FromUTF8( aSource.data(), aSource.size() );
    const wxScopedCharBuffer reencoded = decoded.ToUTF8();

    if( reencoded.length() != aSource.size()
        || std::memcmp( reencoded.data(), aSource.data(), aSource.size() ) != 0 )
    {
        const size_t commonBytes = std::min( reencoded.length(), aSource.size() );
        size_t mismatch = 0;

        while( mismatch < commonBytes && reencoded.data()[mismatch] == aSource[mismatch] )
            ++mismatch;

        diagnostic( result, "error", "invalid_encoding",
                    "KiChad could not preserve the Design Script UTF-8 source",
                    sourceLocation( aSource, mismatch ) );
        return result;
    }

    std::string parseError;
    std::unique_ptr<DOCUMENT> document = DOCUMENT::Parse( aSource, &parseError );

    if( !document )
    {
        diagnostic( result, "error", "parse_failed", parseError );
        return result;
    }

    if( document->Roots().size() != 1
        || document->ListHead( document->Roots().front() ) != "kichad_design" )
    {
        diagnostic( result, "error", "invalid_root",
                    "script must contain exactly one kichad_design root expression" );
        return result;
    }

    const DOCUMENT::NODE& root = document->Nodes()[document->Roots().front()];

    if( root.children.size() > MAX_TOP_LEVEL_FORMS + 1 )
    {
        diagnostic( result, "error", "program_too_large",
                    "script contains more than 20000 top-level forms" );
        return result;
    }

    result.ir = {
        { "language", "kichad-design" },
        { "version", LANGUAGE_VERSION },
        { "sourceSha256", result.sourceSha256 },
        { "units", "mm" },
        { "project", JSON::object() },
        { "libraries", JSON::array() },
        { "authoredSymbols", JSON::array() },
        { "authoredFootprints", JSON::array() },
        { "schematic",
          { { "sheets", JSON::array() }, { "components", JSON::array() },
            { "nets", JSON::array() }, { "noConnects", JSON::array() },
            { "drawings", JSON::array() }, { "busAliases", JSON::array() },
            { "groups", JSON::array() } } },
        { "pcb", JSON::array() },
        { "fab", nullptr },
        { "synthesis", nullptr },
        { "rules", nullptr },
        { "netClasses", nullptr },
        { "customRules", nullptr },
        { "electrical", nullptr },
        { "sourcing", JSON::array() },
        { "conformance", JSON::array() },
        { "production", nullptr },
        { "checks", JSON::array() },
        { "outputs", JSON::array() }
    };

    bool                     sawVersion = false;
    bool                     sawProject = false;
    bool                     sawUnits = false;
    bool                     sawBoard = false;
    bool                     boardFullyTyped = true;
    std::set<std::string>    componentIds;
    std::set<std::string>    netNames;
    std::set<std::string>    libraryIds;
    std::set<std::string>    authoredSymbolIds;
    std::set<std::string>    authoredFootprintIds;
    std::set<std::string>    sheetIds;
    bool                     sawRules = false;
    bool                     sawNetClasses = false;
    bool                     sawCustomRules = false;
    bool                     sawElectrical = false;
    bool                     sawProduction = false;
    std::set<std::string>    sourceIds;
    std::set<std::string>    conformanceIds;
    std::set<std::string>    checkKinds;
    std::set<std::string>    outputKinds;
    std::set<std::string>    connectedPins;
    std::set<std::string>    schematicDrawingIds;
    std::set<std::string>    schematicBusAliasIds;
    std::set<std::string>    schematicGroupIds;
    std::vector<std::string> referencedComponents;
    std::vector<std::string> referencedNets;
    size_t                   pinConnections = 0;

    for( size_t i = 1; i < root.children.size(); ++i )
    {
        const size_t      formNode = root.children[i];
        const std::string form = document->ListHead( formNode );

        if( form.empty() )
        {
            diagnostic( result, "error", "invalid_top_level_form",
                        "every top-level design form must be a named list" );
            continue;
        }

        if( form == "version" )
        {
            std::string version;

            if( sawVersion || !parseSingleValueForm( *document, formNode, version )
                || version != std::to_string( LANGUAGE_VERSION ) )
            {
                diagnostic( result, "error", "invalid_version",
                            "script requires exactly one (version 1) form" );
            }

            sawVersion = true;
        }
        else if( form == "project" )
        {
            if( sawProject )
                diagnostic( result, "error", "duplicate_project", "project occurs more than once" );

            result.ir["project"] = compileProject( *document, formNode, result );
            sawProject = true;
        }
        else if( form == "units" )
        {
            std::string units;

            if( sawUnits || !parseSingleValueForm( *document, formNode, units )
                || ( units != "mm" && units != "mil" ) )
            {
                diagnostic( result, "error", "invalid_units",
                            "units may occur once and must be mm or mil" );
            }
            else
            {
                result.ir["units"] = units;
            }

            sawUnits = true;
        }
        else if( form == "library" )
        {
            JSON library = compileLibrary( *document, formNode, result );
            const std::string key = library.value( "kind", "" ) + ":"
                                    + library.value( "id", "" );

            if( key != ":" && !libraryIds.emplace( key ).second )
            {
                diagnostic( result, "error", "duplicate_library",
                            "library " + key + " occurs more than once" );
            }

            result.ir["libraries"].emplace_back( std::move( library ) );
        }
        else if( form == "symbol" )
        {
            KICHAD::DESIGN_SCRIPT_SYMBOL_COMPILER::RESULT authored =
                    KICHAD::DESIGN_SCRIPT_SYMBOL_COMPILER::Compile( *document, formNode );

            for( JSON& symbolDiagnostic : authored.diagnostics )
                result.diagnostics.emplace_back( std::move( symbolDiagnostic ) );

            const std::string id = authored.symbol.value( "id", "" );

            if( !id.empty() && !authoredSymbolIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_authored_symbol",
                            "authored symbol " + id + " occurs more than once" );
            }

            result.ir["authoredSymbols"].emplace_back( std::move( authored.symbol ) );
        }
        else if( form == "footprint" )
        {
            KICHAD::DESIGN_SCRIPT_FOOTPRINT_COMPILER::RESULT authored =
                    KICHAD::DESIGN_SCRIPT_FOOTPRINT_COMPILER::Compile( *document, formNode );

            for( JSON& footprintDiagnostic : authored.diagnostics )
                result.diagnostics.emplace_back( std::move( footprintDiagnostic ) );

            const std::string id = authored.footprint.value( "id", "" );

            if( !id.empty() && !authoredFootprintIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_authored_footprint",
                            "authored footprint " + id + " occurs more than once" );
            }

            result.ir["authoredFootprints"].emplace_back(
                    std::move( authored.footprint ) );
        }
        else if( form == "component" )
        {
            JSON component = compileComponent( *document, formNode, result );
            const std::string reference = component.value( "reference", "" );

            if( !reference.empty() && !componentIds.emplace( reference ).second )
            {
                diagnostic( result, "error", "duplicate_component",
                            "component " + reference + " occurs more than once" );
            }

            result.ir["schematic"]["components"].emplace_back( std::move( component ) );
        }
        else if( form == "net" )
        {
            JSON net = compileNet( *document, formNode, result, referencedComponents,
                                   connectedPins, pinConnections );
            const std::string name = net.value( "name", "" );

            if( !name.empty() && !netNames.emplace( name ).second )
            {
                diagnostic( result, "error", "duplicate_net",
                            "net " + name + " occurs more than once" );
            }

            result.ir["schematic"]["nets"].emplace_back( std::move( net ) );
        }
        else if( form == "no_connect" )
        {
            result.ir["schematic"]["noConnects"].emplace_back(
                    compileNoConnect( *document, formNode, result, referencedComponents,
                                      connectedPins, pinConnections ) );
        }
        else if( form == "wire" || form == "bus" || form == "bus_entry" )
        {
            JSON drawing = compileSchematicLine( *document, formNode, form, result );
            const std::string id = drawing.value( "id", "" );

            if( !id.empty() && !schematicDrawingIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_drawing_id",
                            "schematic drawing ID " + id + " occurs more than once" );
            }

            result.ir["schematic"]["drawings"].emplace_back( std::move( drawing ) );
        }
        else if( form == "junction" )
        {
            JSON drawing = compileSchematicJunction( *document, formNode, result );
            const std::string id = drawing.value( "id", "" );

            if( !id.empty() && !schematicDrawingIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_drawing_id",
                            "schematic drawing ID " + id + " occurs more than once" );
            }

            result.ir["schematic"]["drawings"].emplace_back( std::move( drawing ) );
        }
        else if( form == "rule_area" )
        {
            JSON drawing = compileSchematicRuleArea( *document, formNode, result );
            const std::string id = drawing.value( "id", "" );

            if( !id.empty() && !schematicDrawingIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_drawing_id",
                            "schematic drawing ID " + id + " occurs more than once" );
            }

            result.ir["schematic"]["drawings"].emplace_back( std::move( drawing ) );
        }
        else if( form == "label" )
        {
            JSON drawing = compileSchematicLabel( *document, formNode, result );
            const std::string id = drawing.value( "id", "" );

            if( !id.empty() && !schematicDrawingIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_drawing_id",
                            "schematic drawing ID " + id + " occurs more than once" );
            }

            result.ir["schematic"]["drawings"].emplace_back( std::move( drawing ) );
        }
        else if( form == "directive" )
        {
            JSON drawing = compileSchematicDirective( *document, formNode, result );
            const std::string id = drawing.value( "id", "" );

            if( !id.empty() && !schematicDrawingIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_drawing_id",
                            "schematic drawing ID " + id + " occurs more than once" );
            }

            result.ir["schematic"]["drawings"].emplace_back( std::move( drawing ) );
        }
        else if( form == "text" )
        {
            JSON drawing = compileSchematicText( *document, formNode, result );
            const std::string id = drawing.value( "id", "" );

            if( !id.empty() && !schematicDrawingIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_drawing_id",
                            "schematic drawing ID " + id + " occurs more than once" );
            }

            result.ir["schematic"]["drawings"].emplace_back( std::move( drawing ) );
        }
        else if( form == "text_box" )
        {
            JSON drawing = compileSchematicTextBox( *document, formNode, result );
            const std::string id = drawing.value( "id", "" );

            if( !id.empty() && !schematicDrawingIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_drawing_id",
                            "schematic drawing ID " + id + " occurs more than once" );
            }

            result.ir["schematic"]["drawings"].emplace_back( std::move( drawing ) );
        }
        else if( form == "polyline" || form == "rectangle" || form == "circle"
                 || form == "arc" || form == "bezier" )
        {
            JSON drawing = compileSchematicGraphic( *document, formNode, form, result );
            const std::string id = drawing.value( "id", "" );

            if( !id.empty() && !schematicDrawingIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_drawing_id",
                            "schematic drawing ID " + id + " occurs more than once" );
            }

            result.ir["schematic"]["drawings"].emplace_back( std::move( drawing ) );
        }
        else if( form == "image" )
        {
            JSON drawing = compileSchematicImage( *document, formNode, result );
            const std::string id = drawing.value( "id", "" );

            if( !id.empty() && !schematicDrawingIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_drawing_id",
                            "schematic drawing ID " + id + " occurs more than once" );
            }

            result.ir["schematic"]["drawings"].emplace_back( std::move( drawing ) );
        }
        else if( form == "table" )
        {
            JSON drawing = compileSchematicTable( *document, formNode, result );
            const std::string id = drawing.value( "id", "" );

            if( !id.empty() && !schematicDrawingIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_drawing_id",
                            "schematic drawing ID " + id + " occurs more than once" );
            }

            result.ir["schematic"]["drawings"].emplace_back( std::move( drawing ) );
        }
        else if( form == "bus_alias" )
        {
            JSON alias = compileSchematicBusAlias( *document, formNode, result );
            const std::string key = alias.value( "sheet", "" ) + "\n"
                                    + alias.value( "name", "" );

            if( key != "\n" && !schematicBusAliasIds.emplace( key ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_bus_alias",
                            "bus_alias " + alias.value( "name", "" )
                                    + " occurs more than once on its sheet" );
            }

            result.ir["schematic"]["busAliases"].emplace_back( std::move( alias ) );
        }
        else if( form == "group" )
        {
            JSON group = compileSchematicGroup( *document, formNode, result );
            const std::string id = group.value( "id", "" );

            if( !id.empty() && !schematicGroupIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_group",
                            "group " + id + " occurs more than once" );
            }

            result.ir["schematic"]["groups"].emplace_back( std::move( group ) );
        }
        else if( form == "sheet" )
        {
            JSON sheet = compileSheet( *document, formNode, result );
            const std::string name = sheet.value( "id", "" );

            if( !name.empty() && !sheetIds.emplace( name ).second )
            {
                diagnostic( result, "error", "duplicate_sheet",
                            "sheet " + name + " occurs more than once" );
            }

            result.ir["schematic"]["sheets"].emplace_back( std::move( sheet ) );
        }
        else if( form == "board" )
        {
            if( sawBoard )
                diagnostic( result, "error", "duplicate_board", "board occurs more than once" );

            KICHAD::DESIGN_SCRIPT_BOARD_COMPILER::RESULT board =
                    KICHAD::DESIGN_SCRIPT_BOARD_COMPILER::Compile( *document, formNode );

            for( JSON& statement : board.statements )
                result.ir["pcb"].emplace_back( std::move( statement ) );

            for( JSON& boardDiagnostic : board.diagnostics )
                result.diagnostics.emplace_back( std::move( boardDiagnostic ) );

            referencedComponents.insert( referencedComponents.end(),
                                         board.componentReferences.begin(),
                                         board.componentReferences.end() );
            referencedNets.insert( referencedNets.end(), board.netReferences.begin(),
                                   board.netReferences.end() );
            boardFullyTyped = boardFullyTyped && board.fullyTyped;
            result.ir["synthesis"] = std::move( board.synthesis );

            sawBoard = true;
        }
        else if( form == "rules" )
        {
            if( sawRules )
                diagnostic( result, "error", "duplicate_rules", "rules occurs more than once" );
            else
                result.ir["rules"] = compileRules( *document, formNode, result );

            sawRules = true;
        }
        else if( form == "net_classes" )
        {
            if( sawNetClasses )
            {
                diagnostic( result, "error", "duplicate_net_classes",
                            "net_classes occurs more than once" );
            }
            else
            {
                result.ir["netClasses"] = compileNetClasses( *document, formNode, result );
            }

            sawNetClasses = true;
        }
        else if( form == "custom_rules" )
        {
            if( sawCustomRules )
            {
                diagnostic( result, "error", "duplicate_custom_rules",
                            "custom_rules occurs more than once" );
            }
            else
            {
                result.ir["customRules"] = compileCustomRules( *document, formNode, result );
            }

            sawCustomRules = true;
        }
        else if( form == "electrical" )
        {
            if( sawElectrical )
            {
                diagnostic( result, "error", "duplicate_electrical",
                            "electrical occurs more than once" );
            }
            else
            {
                KICHAD::DESIGN_SCRIPT_ELECTRICAL_COMPILER::RESULT electrical =
                        KICHAD::DESIGN_SCRIPT_ELECTRICAL_COMPILER::Compile( *document,
                                                                           formNode );

                for( JSON& electricalDiagnostic : electrical.diagnostics )
                    result.diagnostics.emplace_back( std::move( electricalDiagnostic ) );

                referencedComponents.insert( referencedComponents.end(),
                                             electrical.referencedComponents.begin(),
                                             electrical.referencedComponents.end() );
                referencedNets.insert( referencedNets.end(),
                                      electrical.referencedNets.begin(),
                                      electrical.referencedNets.end() );
                result.ir["electrical"] = std::move( electrical.electrical );
            }

            sawElectrical = true;
        }
        else if( form == "source" )
        {
            JSON source = compileSource( *document, formNode, result, referencedComponents );
            const std::string reference = source.value( "component", "" );

            if( !reference.empty() && !sourceIds.emplace( reference ).second )
            {
                diagnostic( result, "error", "duplicate_source",
                            "source for " + reference + " occurs more than once" );
            }

            result.ir["sourcing"].emplace_back( std::move( source ) );
        }
        else if( form == "conformance" )
        {
            JSON conformance =
                    compileConformance( *document, formNode, result, referencedComponents );
            const std::string reference = conformance.value( "component", "" );

            if( !reference.empty() && !conformanceIds.emplace( reference ).second )
            {
                diagnostic( result, "error", "duplicate_conformance",
                            "conformance for " + reference + " occurs more than once" );
            }

            result.ir["conformance"].emplace_back( std::move( conformance ) );
        }
        else if( form == "production" )
        {
            if( sawProduction )
            {
                diagnostic( result, "error", "duplicate_production",
                            "production occurs more than once" );
            }
            else
            {
                KICHAD::DESIGN_SCRIPT_PRODUCTION_COMPILER::RESULT production =
                        KICHAD::DESIGN_SCRIPT_PRODUCTION_COMPILER::Compile( *document,
                                                                           formNode );

                for( JSON& productionDiagnostic : production.diagnostics )
                    result.diagnostics.emplace_back( std::move( productionDiagnostic ) );

                referencedComponents.insert( referencedComponents.end(),
                                             production.referencedComponents.begin(),
                                             production.referencedComponents.end() );
                referencedNets.insert( referencedNets.end(),
                                      production.referencedNets.begin(),
                                      production.referencedNets.end() );
                result.ir["production"] = std::move( production.production );
            }

            sawProduction = true;
        }
        else if( form == "fab" )
        {
            std::string vendor;

            if( !parseSingleValueForm( *document, formNode, vendor ) || vendor.empty()
                || vendor.size() > 64 )
            {
                diagnostic( result, "error", "invalid_fab",
                            "fab must contain exactly one vendor profile name of at most 64 "
                            "characters" );
            }
            else if( !result.ir["fab"].is_null() )
            {
                diagnostic( result, "error", "duplicate_fab", "fab occurs more than once" );
            }
            else
            {
                result.ir["fab"] = vendor;
            }
        }
        else if( form == "check" )
        {
            static const std::set<std::string> allowed = {
                "erc", "electrical", "drc", "layout", "sourcing", "footprints",
                "fabrication"
            };
            JSON check = compileEnumeratedFacet( *document, formNode, "check", allowed, result );
            const std::string kind = check.value( "kind", "" );

            if( !kind.empty() && !checkKinds.emplace( kind ).second )
            {
                diagnostic( result, "error", "duplicate_check",
                            "check " + kind + " occurs more than once" );
            }

            result.ir["checks"].emplace_back( std::move( check ) );
        }
        else if( form == "output" )
        {
            static const std::set<std::string> allowed = {
                "gerbers", "drill", "ipcd356", "netlist", "ipc2581", "odbpp",
                "pick_place", "bom", "step", "stepz", "brep", "glb", "stl", "u3d",
                "xao", "3d_pdf", "pdf", "schematic_pdf", "schematic_svg",
                "schematic_dxf", "schematic_ps", "board_ps", "assembly_svg",
                "assembly_dxf", "gencad", "vrml", "board_stats",
                "schematic_bom", "legacy_bom_xml", "board_render"
            };
            JSON output = compileEnumeratedFacet( *document, formNode, "output", allowed, result );
            const std::string kind = output.value( "kind", "" );

            if( !kind.empty() && !outputKinds.emplace( kind ).second )
            {
                diagnostic( result, "error", "duplicate_output",
                            "output " + kind + " occurs more than once" );
            }

            result.ir["outputs"].emplace_back( std::move( output ) );
        }
        else
        {
            diagnostic( result, "error", "unknown_top_level_form",
                        "unknown top-level form '" + form + "'" );
        }
    }

    if( !sawVersion )
        diagnostic( result, "error", "missing_version", "script is missing (version 1)" );

    if( !sawProject )
        diagnostic( result, "error", "missing_project", "script is missing project metadata" );

    if( checkKinds.contains( "electrical" ) && !result.ir["electrical"].is_object() )
    {
        diagnostic( result, "error", "missing_electrical_contract",
                    "check electrical requires one electrical contract" );
    }

    const JSON& sheets = result.ir["schematic"]["sheets"];

    if( sheets.size() > 128 )
    {
        diagnostic( result, "error", "too_many_sheets",
                    "a script may declare at most 128 schematic sheets" );
    }

    std::map<std::string, const JSON*> sheetsById;
    std::map<std::string, std::string> sheetParents;
    std::map<std::string, std::string> siblingTitles;
    std::map<std::string, std::string> caseFoldedFiles;
    std::vector<std::string> rootSheets;
    size_t sheetPinCount = 0;

    for( const JSON& sheet : sheets )
    {
        if( !sheet.is_object() )
            continue;

        const std::string id = sheet.value( "id", "" );

        if( id.empty() )
            continue;

        sheetsById[id] = &sheet;
        sheetPinCount += sheet.value( "pins", JSON::array() ).size();

        if( sheet["parent"].is_null() )
        {
            rootSheets.emplace_back( id );

            if( sheet.contains( "position" ) || sheet.contains( "size" )
                || !sheet.value( "pins", JSON::array() ).empty() )
            {
                diagnostic( result, "error", "invalid_root_sheet_geometry",
                            "the root sheet cannot have a parent symbol position, size, or pins" );
            }
        }
        else if( sheet["parent"].is_string() )
        {
            const std::string parent = sheet["parent"].get<std::string>();
            sheetParents[id] = parent;

            if( parent == id )
            {
                diagnostic( result, "error", "recursive_sheet_hierarchy",
                            "sheet " + id + " cannot parent itself" );
            }

            if( !sheet.contains( "position" ) || !sheet.contains( "size" ) )
            {
                diagnostic( result, "error", "missing_sheet_geometry",
                            "non-root sheet " + id + " requires at and size" );
            }
            else
            {
                const int64_t x = sheet["position"]["xNm"].get<int64_t>();
                const int64_t y = sheet["position"]["yNm"].get<int64_t>();
                const int64_t right = x + sheet["size"]["xNm"].get<int64_t>();
                const int64_t bottom = y + sheet["size"]["yNm"].get<int64_t>();

                for( const JSON& pin : sheet["pins"] )
                {
                    if( !pin.contains( "position" ) || !pin.contains( "side" ) )
                        continue;

                    const int64_t pinX = pin["position"]["xNm"].get<int64_t>();
                    const int64_t pinY = pin["position"]["yNm"].get<int64_t>();
                    const std::string side = pin["side"].get<std::string>();
                    const bool onBoundary = side == "left" ? pinX == x && pinY >= y && pinY <= bottom
                                          : side == "right" ? pinX == right && pinY >= y && pinY <= bottom
                                          : side == "top" ? pinY == y && pinX >= x && pinX <= right
                                                            : pinY == bottom && pinX >= x && pinX <= right;

                    if( !onBoundary )
                    {
                        diagnostic( result, "error", "sheet_pin_off_edge",
                                    "sheet " + id + " pin " + pin.value( "name", "" )
                                            + " must lie on its declared sheet side" );
                    }
                }
            }

            const std::string siblingKey = parent + "\n" + sheet.value( "title", "" );

            if( !siblingTitles.emplace( siblingKey, id ).second )
            {
                diagnostic( result, "error", "duplicate_sibling_sheet_title",
                            "sibling sheets under " + parent + " cannot share title "
                                    + sheet.value( "title", "" ) );
            }
        }

        if( sheet.contains( "file" ) && sheet["file"].is_string() )
        {
            const std::string file = sheet["file"].get<std::string>();
            std::string folded = file;
            std::transform( folded.begin(), folded.end(), folded.begin(),
                            []( unsigned char aCharacter )
                            {
                                return static_cast<char>( std::tolower( aCharacter ) );
                            } );
            auto [entry, inserted] = caseFoldedFiles.emplace( folded, file );

            if( !inserted && entry->second != file )
            {
                diagnostic( result, "error", "ambiguous_sheet_file_case",
                            "sheet files cannot differ only by letter case" );
            }
        }
    }

    if( sheetPinCount > 4096 )
    {
        diagnostic( result, "error", "too_many_sheet_pins",
                    "a script may declare at most 4096 hierarchical sheet pins" );
    }

    if( !sheets.empty() && rootSheets.size() != 1 )
    {
        diagnostic( result, "error", "invalid_root_sheet_count",
                    "a schematic hierarchy requires exactly one sheet with parent none" );
    }

    if( rootSheets.size() == 1 && result.ir["project"].is_object() )
    {
        const JSON* rootSheet = sheetsById[rootSheets.front()];
        const std::string expected = result.ir["project"].value( "name", "" ) + ".kicad_sch";

        if( rootSheet && rootSheet->value( "file", "" ) != expected )
        {
            diagnostic( result, "error", "invalid_root_sheet_file",
                        "root sheet file must be " + expected );
        }
    }

    for( const auto& [ id, parent ] : sheetParents )
    {
        if( !sheetsById.contains( parent ) )
        {
            diagnostic( result, "error", "unresolved_sheet_parent",
                        "sheet " + id + " references undeclared parent " + parent );
        }
    }

    std::map<std::string, int> sheetVisitState;
    std::function<void( const std::string& )> visitSheet = [&]( const std::string& aId )
    {
        if( sheetVisitState[aId] == 2 )
            return;

        if( sheetVisitState[aId] == 1 )
        {
            diagnostic( result, "error", "recursive_sheet_hierarchy",
                        "schematic sheet hierarchy contains a parent cycle at " + aId );
            return;
        }

        sheetVisitState[aId] = 1;
        auto parent = sheetParents.find( aId );

        if( parent != sheetParents.end() && sheetsById.contains( parent->second ) )
            visitSheet( parent->second );

        sheetVisitState[aId] = 2;
    };

    for( const auto& entry : sheetsById )
        visitSheet( entry.first );

    for( const auto& entry : sheetParents )
    {
        const std::string& id = entry.first;
        auto child = sheetsById.find( id );
        std::set<std::string> ancestorIds;
        std::set<std::string> ancestorFiles;
        std::string current = id;

        while( child != sheetsById.end() )
        {
            if( !ancestorIds.emplace( current ).second )
                break;

            const std::string file = child->second->value( "file", "" );

            if( !file.empty() && !ancestorFiles.emplace( file ).second )
            {
                diagnostic( result, "error", "recursive_sheet_file",
                            "sheet " + id + " recursively reuses file " + file );
                break;
            }

            auto currentParent = sheetParents.find( current );

            if( currentParent == sheetParents.end()
                || !sheetsById.contains( currentParent->second ) )
            {
                break;
            }

            current = currentParent->second;
            child = sheetsById.find( current );
        }
    }

    std::map<std::string, std::set<int64_t>> componentUnits;
    std::map<std::string, std::string>       componentUnitSheets;
    std::map<std::string, bool>              componentHasFootprint;

    if( sheets.empty() && !result.ir["schematic"]["noConnects"].empty() )
    {
        diagnostic( result, "error", "no_connect_without_hierarchy",
                    "no_connect requires an executable schematic hierarchy" );
    }

    for( const JSON& component : result.ir["schematic"]["components"] )
    {
        if( !component.is_object() || !component.contains( "reference" )
            || !component["reference"].is_string() || !component.contains( "units" )
            || !component["units"].is_array() )
        {
            continue;
        }

        const std::string reference = component["reference"].get<std::string>();
        std::set<int64_t>& units = componentUnits[reference];
        componentHasFootprint[reference] = component.contains( "footprint" )
                                           && component["footprint"].is_string();

        if( !sheets.empty() && component["units"].empty() )
        {
            diagnostic( result, "error", "missing_component_units",
                        "component " + reference
                                + " requires at least one explicit schematic unit placement" );
        }
        else if( sheets.empty() && !component["units"].empty() )
        {
            diagnostic( result, "error", "component_unit_without_hierarchy",
                        "component " + reference
                                + " cannot place schematic units without a declared root sheet" );
        }

        for( const JSON& unit : component["units"] )
        {
            if( !unit.is_object() || !unit.contains( "number" )
                || !unit["number"].is_number_integer() || !unit.contains( "sheet" )
                || !unit["sheet"].is_string() )
            {
                continue;
            }

            const int64_t number = unit["number"].get<int64_t>();
            const std::string sheet = unit["sheet"].get<std::string>();
            units.emplace( number );
            componentUnitSheets[reference + "/" + std::to_string( number )] = sheet;

            if( !sheetsById.contains( sheet ) )
            {
                diagnostic( result, "error", "unresolved_component_sheet",
                            "component " + reference + " unit " + std::to_string( number )
                                    + " references undeclared sheet " + sheet );
            }
        }
    }

    const auto validateEndpointUnit = [&]( const JSON& aEndpoint )
    {
        if( sheets.empty() || !aEndpoint.is_object() || !aEndpoint.contains( "component" )
            || !aEndpoint["component"].is_string() || !aEndpoint.contains( "unit" )
            || !aEndpoint["unit"].is_number_integer() )
        {
            return;
        }

        const std::string reference = aEndpoint["component"].get<std::string>();
        const int64_t unit = aEndpoint["unit"].get<int64_t>();
        auto component = componentUnits.find( reference );

        if( component != componentUnits.end() && !component->second.contains( unit ) )
        {
            diagnostic( result, "error", "unresolved_component_unit",
                        "schematic endpoint " + reference + ":" + std::to_string( unit )
                                + " has no matching unit placement" );
        }
    };

    for( const JSON& net : result.ir["schematic"]["nets"] )
    {
        if( net.is_object() && net.contains( "pins" ) && net["pins"].is_array() )
        {
            for( const JSON& pin : net["pins"] )
                validateEndpointUnit( pin );
        }
    }

    for( const JSON& noConnect : result.ir["schematic"]["noConnects"] )
        validateEndpointUnit( noConnect );

    if( result.ir["electrical"].is_object() )
    {
        for( const JSON& logic : result.ir["electrical"].value( "logic", JSON::array() ) )
        {
            if( logic.is_object() && logic.contains( "driver" ) )
                validateEndpointUnit( logic["driver"] );

            for( const JSON& receiver : logic.value( "receivers", JSON::array() ) )
                validateEndpointUnit( receiver );
        }

        for( const JSON& simulation :
             result.ir["electrical"].value( "simulations", JSON::array() ) )
        {
            for( const JSON& device : simulation.value( "devices", JSON::array() ) )
            {
                for( const JSON& pin : device.value( "pins", JSON::array() ) )
                {
                    validateEndpointUnit(
                            { { "component", device.value( "component", "" ) },
                              { "unit", device.value( "unit", int64_t( 0 ) ) },
                              { "pin", pin } } );
                }
            }
        }
    }

    for( const JSON& statement : result.ir["pcb"] )
    {
        if( !statement.is_object() || statement.value( "kind", "" ) != "place"
            || !statement.contains( "component" ) || !statement["component"].is_string() )
        {
            continue;
        }

        const std::string reference = statement["component"].get<std::string>();
        auto component = componentHasFootprint.find( reference );

        if( component != componentHasFootprint.end() && !component->second )
        {
            diagnostic( result, "error", "component_without_footprint_placement",
                        "board placement references virtual component " + reference
                                + " whose canonical footprint is none" );
        }
    }

    std::map<std::string, JSON> schematicRuleAreas;

    for( const JSON& drawing : result.ir["schematic"]["drawings"] )
    {
        if( drawing.is_object() && drawing.value( "kind", "" ) == "rule_area"
            && drawing.contains( "id" ) && drawing["id"].is_string() )
        {
            schematicRuleAreas.emplace( drawing["id"].get<std::string>(), drawing );
        }
    }

    for( const JSON& drawing : result.ir["schematic"]["drawings"] )
    {
        if( !drawing.is_object() || !drawing.contains( "sheet" )
            || !drawing["sheet"].is_string() )
        {
            continue;
        }

        const std::string sheet = drawing["sheet"].get<std::string>();

        if( sheets.empty() )
        {
            diagnostic( result, "error", "schematic_drawing_without_hierarchy",
                        "schematic drawing " + drawing.value( "id", "" )
                                + " requires a declared root sheet" );
        }
        else if( !sheetsById.contains( sheet ) )
        {
            diagnostic( result, "error", "unresolved_schematic_drawing_sheet",
                        "schematic drawing " + drawing.value( "id", "" )
                                + " references undeclared sheet " + sheet );
        }

        if( drawing.value( "kind", "" ) == "label"
            && drawing.contains( "net" ) && drawing["net"].is_string()
            && !netNames.contains( drawing["net"].get<std::string>() ) )
        {
            diagnostic( result, "error", "unresolved_schematic_label_net",
                        "schematic label " + drawing.value( "id", "" )
                                + " references undeclared net "
                                + drawing["net"].get<std::string>() );
        }

        if( drawing.value( "kind", "" ) != "directive"
            || !drawing.contains( "target" ) || !drawing["target"].is_object()
            || !drawing["target"].contains( "name" )
            || !drawing["target"]["name"].is_string() )
        {
            continue;
        }

        const std::string targetKind = drawing["target"].value( "kind", "" );
        const std::string targetName = drawing["target"]["name"].get<std::string>();

        if( targetKind == "net" && !netNames.contains( targetName ) )
        {
            diagnostic( result, "error", "unresolved_schematic_directive_net",
                        "schematic directive " + drawing.value( "id", "" )
                                + " references undeclared net " + targetName );
        }
        else if( targetKind == "rule_area" )
        {
            auto area = schematicRuleAreas.find( targetName );

            if( area == schematicRuleAreas.end() )
            {
                diagnostic( result, "error", "unresolved_schematic_directive_rule_area",
                            "schematic directive " + drawing.value( "id", "" )
                                    + " references undeclared rule_area " + targetName );
            }
            else if( area->second.value( "sheet", "" ) != sheet )
            {
                diagnostic( result, "error", "mismatched_schematic_directive_rule_area_sheet",
                            "schematic directive " + drawing.value( "id", "" )
                                    + " and rule_area " + targetName
                                    + " must be on the same sheet" );
            }
            else if( drawing.contains( "position" ) && drawing["position"].is_object()
                     && area->second.contains( "polygon" )
                     && area->second["polygon"].is_array()
                     && !schematicPointOnPolygonBoundary( drawing["position"],
                                                          area->second["polygon"] ) )
            {
                diagnostic( result, "error", "detached_schematic_directive_rule_area",
                            "schematic directive " + drawing.value( "id", "" )
                                    + " anchor must lie exactly on rule_area " + targetName
                                    + " boundary" );
            }
        }
    }

    for( const JSON& alias : result.ir["schematic"]["busAliases"] )
    {
        if( !alias.is_object() || !alias.contains( "sheet" ) || !alias["sheet"].is_string()
            || !alias.contains( "members" ) || !alias["members"].is_array() )
        {
            continue;
        }

        const std::string sheet = alias["sheet"].get<std::string>();

        if( !sheetsById.contains( sheet ) )
        {
            diagnostic( result, "error", "unresolved_schematic_bus_alias_sheet",
                        "bus_alias " + alias.value( "name", "" )
                                + " references undeclared sheet " + sheet );
        }

        for( const JSON& member : alias["members"] )
        {
            if( member.is_string() && !netNames.contains( member.get<std::string>() ) )
            {
                diagnostic( result, "error", "unresolved_schematic_bus_alias_member",
                            "bus_alias " + alias.value( "name", "" )
                                    + " references undeclared net "
                                    + member.get<std::string>() );
            }
        }
    }

    std::map<std::string, std::string> drawingSheets;

    for( const JSON& drawing : result.ir["schematic"]["drawings"] )
    {
        if( drawing.is_object() && drawing.contains( "id" ) && drawing["id"].is_string()
            && drawing.contains( "sheet" ) && drawing["sheet"].is_string() )
        {
            drawingSheets[drawing["id"].get<std::string>()] =
                    drawing["sheet"].get<std::string>();
        }
    }

    std::map<std::string, std::set<std::string>> sheetPinNames;

    for( const auto& [ sheetId, sheet ] : sheetsById )
    {
        if( !sheet || !sheet->contains( "pins" ) || !( *sheet )["pins"].is_array() )
            continue;

        for( const JSON& pin : ( *sheet )["pins"] )
        {
            if( pin.is_object() && pin.contains( "name" ) && pin["name"].is_string() )
                sheetPinNames[sheetId].insert( pin["name"].get<std::string>() );
        }
    }

    std::set<std::string> netEndpoints;

    for( const JSON& net : result.ir["schematic"]["nets"] )
    {
        if( !net.is_object() || !net.contains( "name" ) || !net["name"].is_string()
            || !net.contains( "pins" ) || !net["pins"].is_array() )
        {
            continue;
        }

        for( const JSON& pin : net["pins"] )
        {
            if( pin.is_object() && pin.contains( "component" )
                && pin["component"].is_string() && pin.contains( "unit" )
                && pin["unit"].is_number_integer() && pin.contains( "number" )
                && pin["number"].is_string() )
            {
                netEndpoints.insert( net["name"].get<std::string>() + "\n"
                                     + pin["component"].get<std::string>() + "/"
                                     + std::to_string( pin["unit"].get<int64_t>() ) + "/"
                                     + pin["number"].get<std::string>() );
            }
        }
    }

    if( result.ir["electrical"].is_object() )
    {
        const auto validateLogicNetEndpoint = [&]( const JSON& aLogic,
                                                   const JSON& aEndpoint,
                                                   const char* aRole )
        {
            if( !aLogic.is_object() || !aLogic.contains( "net" )
                || !aLogic["net"].is_string() || !aEndpoint.is_object()
                || !aEndpoint.contains( "component" )
                || !aEndpoint["component"].is_string() || !aEndpoint.contains( "unit" )
                || !aEndpoint["unit"].is_number_integer() || !aEndpoint.contains( "pin" )
                || !aEndpoint["pin"].is_string() )
            {
                return;
            }

            const std::string key = aLogic["net"].get<std::string>() + "\n"
                                    + aEndpoint["component"].get<std::string>() + "/"
                                    + std::to_string( aEndpoint["unit"].get<int64_t>() ) + "/"
                                    + aEndpoint["pin"].get<std::string>();

            if( !netEndpoints.contains( key ) )
            {
                diagnostic( result, "error", "logic_endpoint_not_on_net",
                            "logic " + aLogic.value( "id", "" ) + " " + aRole + " "
                                    + aEndpoint["component"].get<std::string>() + ":"
                                    + aEndpoint["pin"].get<std::string>()
                                    + " is not connected to declared net "
                                    + aLogic["net"].get<std::string>() );
            }
        };

        for( const JSON& logic : result.ir["electrical"].value( "logic", JSON::array() ) )
        {
            if( logic.contains( "driver" ) )
                validateLogicNetEndpoint( logic, logic["driver"], "driver" );

            for( const JSON& receiver : logic.value( "receivers", JSON::array() ) )
                validateLogicNetEndpoint( logic, receiver, "receiver" );
        }

        for( const JSON& simulation :
             result.ir["electrical"].value( "simulations", JSON::array() ) )
        {
            for( const JSON& device : simulation.value( "devices", JSON::array() ) )
            {
                const JSON nodes = device.value( "nodes", JSON::array() );
                const JSON pins = device.value( "pins", JSON::array() );

                if( nodes.size() != pins.size() )
                    continue;

                for( size_t index = 0; index < nodes.size(); ++index )
                {
                    if( !nodes[index].is_string() || !pins[index].is_string() )
                        continue;

                    const std::string key = nodes[index].get<std::string>() + "\n"
                                            + device.value( "component", "" ) + "/"
                                            + std::to_string(
                                                      device.value( "unit", int64_t( 0 ) ) )
                                            + "/" + pins[index].get<std::string>();

                    if( !netEndpoints.contains( key ) )
                    {
                        diagnostic( result, "error", "simulation_device_pin_not_on_net",
                                    "simulation " + simulation.value( "id", "" )
                                            + " device " + device.value( "id", "" )
                                            + " maps " + device.value( "component", "" )
                                            + ":" + pins[index].get<std::string>()
                                            + " to net " + nodes[index].get<std::string>()
                                            + " but the schematic does not" );
                    }
                }
            }
        }
    }

    std::set<std::string> noConnectEndpoints;

    for( const JSON& endpoint : result.ir["schematic"]["noConnects"] )
    {
        if( endpoint.is_object() && endpoint.contains( "component" )
            && endpoint["component"].is_string() && endpoint.contains( "unit" )
            && endpoint["unit"].is_number_integer() && endpoint.contains( "number" )
            && endpoint["number"].is_string() )
        {
            noConnectEndpoints.insert(
                    endpoint["component"].get<std::string>() + "/"
                    + std::to_string( endpoint["unit"].get<int64_t>() ) + "/"
                    + endpoint["number"].get<std::string>() );
        }
    }

    std::map<std::string, const JSON*> groupsById;

    for( const JSON& group : result.ir["schematic"]["groups"] )
    {
        if( group.is_object() && group.contains( "id" ) && group["id"].is_string() )
            groupsById.emplace( group["id"].get<std::string>(), &group );
    }

    std::map<std::string, std::string> memberOwners;
    std::map<std::string, std::vector<std::string>> nestedGroups;

    for( const JSON& group : result.ir["schematic"]["groups"] )
    {
        if( !group.is_object() || !group.contains( "id" ) || !group["id"].is_string()
            || !group.contains( "sheet" ) || !group["sheet"].is_string()
            || !group.contains( "members" ) || !group["members"].is_array() )
        {
            continue;
        }

        const std::string id = group["id"].get<std::string>();
        const std::string sheet = group["sheet"].get<std::string>();

        if( !sheetsById.contains( sheet ) )
        {
            diagnostic( result, "error", "unresolved_schematic_group_sheet",
                        "group " + id + " references undeclared sheet " + sheet );
        }

        for( const JSON& member : group["members"] )
        {
            if( !member.is_object() || !member.contains( "kind" )
                || !member["kind"].is_string() )
            {
                continue;
            }

            const std::string kind = member["kind"].get<std::string>();
            std::string targetKey;
            std::string targetSheet;
            bool resolved = false;

            if( ( kind == "drawing" || kind == "sheet" || kind == "group" )
                && member.contains( "id" ) && member["id"].is_string() )
            {
                const std::string target = member["id"].get<std::string>();
                targetKey = kind + "/" + target;

                if( kind == "drawing" )
                {
                    auto drawing = drawingSheets.find( target );
                    resolved = drawing != drawingSheets.end();

                    if( resolved )
                        targetSheet = drawing->second;
                }
                else if( kind == "sheet" )
                {
                    auto targetObject = sheetsById.find( target );
                    resolved = targetObject != sheetsById.end()
                               && sheetParents.contains( target );

                    if( resolved )
                        targetSheet = sheetParents.at( target );
                }
                else
                {
                    auto nested = groupsById.find( target );
                    resolved = nested != groupsById.end();

                    if( resolved )
                    {
                        targetSheet = nested->second->value( "sheet", "" );
                        nestedGroups[id].push_back( target );
                    }

                    if( target == id )
                    {
                        diagnostic( result, "error", "recursive_schematic_group",
                                    "group " + id + " cannot contain itself" );
                    }
                }
            }
            else if( kind == "component" && member.contains( "reference" )
                     && member["reference"].is_string() && member.contains( "unit" )
                     && member["unit"].is_number_integer() )
            {
                const std::string endpoint = member["reference"].get<std::string>() + "/"
                                             + std::to_string(
                                                     member["unit"].get<int64_t>() );
                targetKey = "component/" + endpoint;
                auto component = componentUnitSheets.find( endpoint );
                resolved = component != componentUnitSheets.end();

                if( resolved )
                    targetSheet = component->second;
            }
            else if( kind == "hierarchical_label" && member.contains( "sheet" )
                     && member["sheet"].is_string() && member.contains( "pin" )
                     && member["pin"].is_string() )
            {
                const std::string target = member["sheet"].get<std::string>();
                const std::string pin = member["pin"].get<std::string>();
                targetKey = "hierarchical_label/" + target + "/" + pin;
                targetSheet = target;
                resolved = sheetParents.contains( target )
                           && sheetPinNames[target].contains( pin );
            }
            else if( ( kind == "net_label" || kind == "no_connect" )
                     && member.contains( "reference" ) && member["reference"].is_string()
                     && member.contains( "unit" ) && member["unit"].is_number_integer()
                     && member.contains( "pin" ) && member["pin"].is_string()
                     && member.contains( "occurrence" )
                     && member["occurrence"].is_number_integer() )
            {
                const std::string endpoint = member["reference"].get<std::string>() + "/"
                                             + std::to_string(
                                                     member["unit"].get<int64_t>() ) + "/"
                                             + member["pin"].get<std::string>();
                const std::string occurrence =
                        std::to_string( member["occurrence"].get<int64_t>() );
                targetKey = kind + "/";

                if( kind == "net_label" && member.contains( "net" )
                    && member["net"].is_string() )
                {
                    const std::string net = member["net"].get<std::string>();
                    targetKey += net + "/" + endpoint + "/" + occurrence;
                    resolved = netEndpoints.contains( net + "\n" + endpoint );
                }
                else if( kind == "no_connect" )
                {
                    targetKey += endpoint + "/" + occurrence;
                    resolved = noConnectEndpoints.contains( endpoint );
                }

                auto component = componentUnitSheets.find(
                        member["reference"].get<std::string>() + "/"
                        + std::to_string( member["unit"].get<int64_t>() ) );

                if( component != componentUnitSheets.end() )
                    targetSheet = component->second;
                else
                    resolved = false;
            }

            if( !resolved )
            {
                diagnostic( result, "error", "unresolved_schematic_group_member",
                            "group " + id + " has unresolved " + kind + " member "
                                    + targetKey );
                continue;
            }

            if( targetSheet != sheet )
            {
                diagnostic( result, "error", "mismatched_schematic_group_member_sheet",
                            "group " + id + " and member " + targetKey
                                    + " must be on the same native schematic screen" );
            }

            auto [owner, inserted] = memberOwners.emplace( targetKey, id );

            if( !inserted )
            {
                diagnostic( result, "error", "multiply_grouped_schematic_item",
                            "schematic item " + targetKey + " is a direct member of both "
                                    + owner->second + " and " + id );
            }
        }
    }

    std::map<std::string, int> groupVisitState;
    std::function<void( const std::string& )> visitGroup = [&]( const std::string& aId )
    {
        if( groupVisitState[aId] == 2 )
            return;

        if( groupVisitState[aId] == 1 )
        {
            diagnostic( result, "error", "recursive_schematic_group",
                        "schematic group nesting contains a cycle at " + aId );
            return;
        }

        groupVisitState[aId] = 1;

        for( const std::string& child : nestedGroups[aId] )
        {
            if( groupsById.contains( child ) )
                visitGroup( child );
        }

        groupVisitState[aId] = 2;
    };

    for( const auto& [ id, group ] : groupsById )
        visitGroup( id );

    if( result.ir["libraries"].size() > MAX_LIBRARIES )
    {
        diagnostic( result, "error", "too_many_libraries",
                    "a script may declare at most 512 library dependencies" );
    }

    size_t projectSymbolLibraries = 0;
    size_t projectFootprintLibraries = 0;
    std::map<std::string, const JSON*> symbolLibraries;
    std::map<std::string, const JSON*> footprintLibraries;
    std::map<std::string, size_t> managedSymbolCounts;
    std::map<std::string, size_t> managedFootprintCounts;

    for( const JSON& library : result.ir["libraries"] )
    {
        if( library.is_object() && library.value( "kind", "" ) == "symbol"
            && !library.value( "id", "" ).empty() )
        {
            symbolLibraries[library.value( "id", "" )] = &library;
        }

        if( library.is_object() && library.value( "kind", "" ) == "footprint"
            && !library.value( "id", "" ).empty() )
        {
            footprintLibraries[library.value( "id", "" )] = &library;
        }

        if( !library.is_object() || library.value( "table", "" ) != "project" )
            continue;

        if( library.value( "kind", "" ) == "symbol" )
            ++projectSymbolLibraries;
        else if( library.value( "kind", "" ) == "footprint" )
            ++projectFootprintLibraries;
    }

    if( result.ir["authoredSymbols"].size() > 4096 )
    {
        diagnostic( result, "error", "too_many_authored_symbols",
                    "a script may author at most 4096 symbols" );
    }

    for( const JSON& symbol : result.ir["authoredSymbols"] )
    {
        if( !symbol.is_object() || !symbol.contains( "library" )
            || !symbol["library"].is_string() )
        {
            continue;
        }

        const std::string nickname = symbol["library"].get<std::string>();
        auto library = symbolLibraries.find( nickname );

        if( library == symbolLibraries.end() )
        {
            diagnostic( result, "error", "unresolved_authored_symbol_library",
                        "authored symbol " + symbol.value( "id", "" )
                                + " has no declared symbol library " + nickname );
        }
        else if( library->second->value( "table", "" ) != "project"
                 || !library->second->value( "managed", false ) )
        {
            diagnostic( result, "error", "unmanaged_authored_symbol_library",
                        "authored symbol " + symbol.value( "id", "" )
                                + " requires a project symbol library with (managed true)" );
        }
        else
        {
            ++managedSymbolCounts[nickname];
        }
    }

    for( const auto& [nickname, library] : symbolLibraries )
    {
        if( library->value( "managed", false ) && managedSymbolCounts[nickname] == 0 )
        {
            diagnostic( result, "error", "empty_managed_symbol_library",
                        "managed symbol library " + nickname
                                + " requires at least one top-level symbol declaration" );
        }
    }

    if( result.ir["authoredFootprints"].size() > 4096 )
    {
        diagnostic( result, "error", "too_many_authored_footprints",
                    "a script may author at most 4096 footprints" );
    }

    for( const JSON& footprint : result.ir["authoredFootprints"] )
    {
        if( !footprint.is_object() || !footprint.contains( "library" )
            || !footprint["library"].is_string() )
        {
            continue;
        }

        const std::string nickname = footprint["library"].get<std::string>();
        auto library = footprintLibraries.find( nickname );

        if( library == footprintLibraries.end() )
        {
            diagnostic( result, "error", "unresolved_authored_footprint_library",
                        "authored footprint " + footprint.value( "id", "" )
                                + " has no declared footprint library " + nickname );
        }
        else if( library->second->value( "table", "" ) != "project"
                 || !library->second->value( "managed", false ) )
        {
            diagnostic( result, "error", "unmanaged_authored_footprint_library",
                        "authored footprint " + footprint.value( "id", "" )
                                + " requires a project footprint library with (managed true)" );
        }
        else
        {
            ++managedFootprintCounts[nickname];
        }
    }

    for( const auto& [nickname, library] : footprintLibraries )
    {
        if( library->value( "managed", false ) && managedFootprintCounts[nickname] == 0 )
        {
            diagnostic( result, "error", "empty_managed_footprint_library",
                        "managed footprint library " + nickname
                                + " requires at least one top-level footprint declaration" );
        }
    }

    if( projectSymbolLibraries > MAX_PROJECT_LIBRARIES_PER_TABLE
        || projectFootprintLibraries > MAX_PROJECT_LIBRARIES_PER_TABLE )
    {
        diagnostic( result, "error", "too_many_project_libraries",
                    "a project symbol or footprint table may contain at most 256 libraries" );
    }

    if( !result.ir["libraries"].empty() )
    {
        for( const JSON& component : result.ir["schematic"]["components"] )
        {
            for( const char* kind : { "symbol", "footprint" } )
            {
                if( !component.is_object() || !component.contains( kind )
                    || !component[kind].is_string() )
                {
                    continue;
                }

                std::string nickname;

                if( !libraryIdNickname( component[kind].get<std::string>(), nickname ) )
                    continue;

                if( !libraryIds.contains( std::string( kind ) + ":" + nickname ) )
                {
                    diagnostic( result, "error", "unresolved_component_library",
                                "component " + component.value( "reference", "" ) + " uses "
                                        + kind + " library " + nickname
                                        + " without a matching declaration" );
                }
            }
        }
    }

    for( const std::string& reference : referencedComponents )
    {
        if( !componentIds.contains( reference ) )
        {
            diagnostic( result, "error", "unresolved_component",
                        "component reference " + reference + " is not declared" );
        }
    }

    for( const std::string& name : referencedNets )
    {
        if( !netNames.contains( name ) )
        {
            diagnostic( result, "error", "unresolved_net",
                        "net reference " + name + " is not declared" );
        }
    }

    if( result.ir["production"].is_object() )
    {
        std::set<std::string> physicalPins;

        for( const JSON& net : result.ir["schematic"]["nets"] )
        {
            if( !net.is_object() || !net.contains( "pins" ) || !net["pins"].is_array() )
                continue;

            for( const JSON& pin : net["pins"] )
            {
                if( pin.is_object() && pin.contains( "component" )
                    && pin["component"].is_string() && pin.contains( "number" )
                    && pin["number"].is_string() )
                {
                    physicalPins.emplace( pin["component"].get<std::string>() + "\n"
                                          + pin["number"].get<std::string>() );
                }
            }
        }

        for( const JSON& pin : result.ir["schematic"]["noConnects"] )
        {
            if( pin.is_object() && pin.contains( "component" )
                && pin["component"].is_string() && pin.contains( "number" )
                && pin["number"].is_string() )
            {
                physicalPins.emplace( pin["component"].get<std::string>() + "\n"
                                      + pin["number"].get<std::string>() );
            }
        }

        const auto requirePhysicalComponent = [&]( const std::string& aReference,
                                                   const std::string& aContext )
        {
            const auto component = componentHasFootprint.find( aReference );

            if( component != componentHasFootprint.end() && !component->second )
            {
                diagnostic( result, "error", "virtual_production_component",
                            aContext + " references virtual component " + aReference
                                    + " without a physical footprint" );
            }
        };

        const auto requirePhysicalPin = [&]( const std::string& aReference,
                                             const std::string& aPin,
                                             const std::string& aContext )
        {
            requirePhysicalComponent( aReference, aContext );

            if( !aReference.empty() && !aPin.empty()
                && !physicalPins.contains( aReference + "\n" + aPin ) )
            {
                diagnostic( result, "error", "unresolved_production_connector_pin",
                            aContext + " references " + aReference + " pin " + aPin
                                    + ", which is absent from KDS schematic connectivity" );
            }
        };

        const JSON& production = result.ir["production"];

        if( production.contains( "firmware" ) && production["firmware"].is_array() )
        {
            for( const JSON& firmware : production["firmware"] )
            {
                requirePhysicalComponent( firmware.value( "target", "" ),
                                          "firmware " + firmware.value( "id", "" ) );
            }
        }

        if( production.contains( "programming" ) && production["programming"].is_array() )
        {
            for( const JSON& program : production["programming"] )
            {
                const std::string id = program.value( "id", "" );
                const std::string connector = program.value( "connector", "" );
                requirePhysicalComponent( program.value( "target", "" ),
                                          "program " + id + " target" );

                if( program.contains( "signals" ) && program["signals"].is_array() )
                {
                    for( const JSON& signal : program["signals"] )
                    {
                        requirePhysicalPin( connector, signal.value( "pin", "" ),
                                            "program " + id + " signal "
                                                    + signal.value( "name", "" ) );
                    }
                }
            }
        }

        if( production.contains( "power" ) && production["power"].is_array() )
        {
            for( const JSON& power : production["power"] )
            {
                const std::string id = power.value( "id", "" );
                const std::string connector = power.value( "connector", "" );
                requirePhysicalPin( connector, power.value( "positivePin", "" ),
                                    "power " + id + " positive pin" );
                requirePhysicalPin( connector, power.value( "returnPin", "" ),
                                    "power " + id + " return pin" );
            }
        }

        if( production.contains( "tests" ) && production["tests"].is_array() )
        {
            for( const JSON& test : production["tests"] )
            {
                const std::string id = test.value( "id", "" );

                if( test.contains( "target" ) && test["target"].is_object()
                    && test["target"].value( "kind", "" ) == "pin" )
                {
                    requirePhysicalPin( test["target"].value( "component", "" ),
                                        test["target"].value( "pin", "" ),
                                        "test " + id + " target" );
                }

                if( test.contains( "testpoint" ) && test["testpoint"].is_string() )
                {
                    requirePhysicalComponent( test["testpoint"].get<std::string>(),
                                              "test " + id + " testpoint" );
                }
            }
        }

        if( production.contains( "assembly" ) && production["assembly"].is_object()
            && production["assembly"].contains( "instructions" )
            && production["assembly"]["instructions"].is_array() )
        {
            for( const JSON& instruction : production["assembly"]["instructions"] )
            {
                if( instruction.contains( "scope" ) && instruction["scope"].is_object()
                    && instruction["scope"].value( "kind", "" ) == "component" )
                {
                    requirePhysicalComponent(
                            instruction["scope"].value( "component", "" ),
                            "assembly instruction " + instruction.value( "id", "" ) );
                }
            }
        }
    }

    JSON passes = JSON::array( { "parse", "typecheck", "resolve", "plan", "snapshot" } );

    if( !result.ir["libraries"].empty() )
        passes.emplace_back( "libraries" );

    if( result.ir["project"].contains( "textVariables" )
        || result.ir["project"].contains( "fieldTemplates" ) )
    {
        passes.emplace_back( "project_settings" );
    }

    if( !result.ir["schematic"]["components"].empty()
        || !result.ir["schematic"]["nets"].empty()
        || !result.ir["schematic"]["noConnects"].empty()
        || !result.ir["schematic"]["drawings"].empty()
        || !result.ir["schematic"]["busAliases"].empty()
        || !result.ir["schematic"]["groups"].empty() )
        passes.emplace_back( "schematic" );

    if( !result.ir["pcb"].empty() )
        passes.emplace_back( "pcb" );

    if( result.ir["synthesis"].is_object() )
        passes.emplace_back( "physical_synthesis" );

    if( result.ir["customRules"].is_object() )
        passes.emplace_back( "custom_rules" );

    if( result.ir["electrical"].is_object() )
        passes.emplace_back( "electrical" );

    if( !result.ir["sourcing"].empty() )
        passes.emplace_back( "sourcing" );

    if( result.ir["production"].is_object() )
        passes.emplace_back( "production_handoff" );

    if( !result.ir["checks"].empty() )
        passes.emplace_back( "verification" );

    if( !result.ir["outputs"].empty() )
        passes.emplace_back( "fabrication" );

    result.plan = {
        { "passes", std::move( passes ) },
        { "mutationRequired", true },
        { "transactional", true },
        { "boardFullyTyped", boardFullyTyped },
        { "counts",
          { { "libraries", result.ir["libraries"].size() },
            { "authoredSymbols", result.ir["authoredSymbols"].size() },
            { "authoredFootprints", result.ir["authoredFootprints"].size() },
            { "sheets", result.ir["schematic"]["sheets"].size() },
            { "components", result.ir["schematic"]["components"].size() },
            { "nets", result.ir["schematic"]["nets"].size() },
            { "noConnects", result.ir["schematic"]["noConnects"].size() },
            { "drawings", result.ir["schematic"]["drawings"].size() },
            { "busAliases", result.ir["schematic"]["busAliases"].size() },
            { "groups", result.ir["schematic"]["groups"].size() },
            { "textVariables", result.ir["project"].contains( "textVariables" )
                                       ? result.ir["project"]["textVariables"].size()
                                       : 0 },
            { "fieldTemplates", result.ir["project"].contains( "fieldTemplates" )
                                       ? result.ir["project"]["fieldTemplates"].size()
                                       : 0 },
            { "pinConnections", pinConnections },
            { "boardStatements", result.ir["pcb"].size() },
            { "rules", result.ir["rules"].is_object() ? 1 : 0 },
            { "netClasses", result.ir["netClasses"].is_object()
                                      ? result.ir["netClasses"]["classes"].size()
                                      : 0 },
            { "netClassAssignments", result.ir["netClasses"].is_object()
                                               ? result.ir["netClasses"]["assignments"].size()
                                               : 0 },
            { "customRules", result.ir["customRules"].is_object()
                                     ? result.ir["customRules"]["rules"].size()
                                     : 0 },
            { "electricalRails", result.ir["electrical"].is_object()
                                           ? result.ir["electrical"]["rails"].size()
                                           : 0 },
            { "electricalRatings", result.ir["electrical"].is_object()
                                             ? result.ir["electrical"]["ratings"].size()
                                             : 0 },
            { "electricalThermalModels", result.ir["electrical"].is_object()
                                                  ? result.ir["electrical"]["thermal"].size()
                                                  : 0 },
            { "electricalLogicInterfaces", result.ir["electrical"].is_object()
                                                    ? result.ir["electrical"]["logic"].size()
                                                    : 0 },
            { "electricalSimulations", result.ir["electrical"].is_object()
                                               ? result.ir["electrical"]["simulations"].size()
                                               : 0 },
            { "sourcingRecords", result.ir["sourcing"].size() },
            { "firmwareImages", result.ir["production"].is_object()
                                          ? result.ir["production"]["firmware"].size()
                                          : 0 },
            { "assemblyProfiles", result.ir["production"].is_object()
                                           && result.ir["production"]["assembly"].is_object()
                                           ? 1
                                           : 0 },
            { "programmingSteps", result.ir["production"].is_object()
                                            ? result.ir["production"]["programming"].size()
                                            : 0 },
            { "powerProfiles", result.ir["production"].is_object()
                                         ? result.ir["production"]["power"].size()
                                         : 0 },
            { "bringupTests", result.ir["production"].is_object()
                                        ? result.ir["production"]["tests"].size()
                                        : 0 },
            { "checks", result.ir["checks"].size() },
            { "outputs", result.ir["outputs"].size() } } }
    };

    result.ok = !containsError( result.diagnostics );
    return result;
}

} // namespace KICHAD
