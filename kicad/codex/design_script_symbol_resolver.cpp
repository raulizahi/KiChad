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

#include "design_script_symbol_resolver.h"
#include "kichad_from_chars.h"

#include "lossless_sexpr_document.h"

#include <string_utils.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT;

constexpr size_t MAX_LIBRARY_BYTES = 32 * 1024 * 1024;
constexpr size_t MAX_RESOLVED_SYMBOLS = 4096;
constexpr size_t MAX_PINS_PER_UNIT = 1024;
constexpr size_t MAX_INHERITANCE_DEPTH = 64;
constexpr size_t MAX_PROPERTIES_PER_SYMBOL = 1024;
constexpr int64_t MAX_SYMBOL_LIBRARY_VERSION = 20251024;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


std::vector<size_t> directLists( const DOCUMENT& aDocument, size_t aParent,
                                 const std::string& aHead )
{
    std::vector<size_t> result;

    for( size_t child : aDocument.Nodes().at( aParent ).children )
    {
        if( aDocument.Nodes().at( child ).kind == DOCUMENT::NODE_KIND::LIST
            && aDocument.ListHead( child ) == aHead )
        {
            result.push_back( child );
        }
    }

    return result;
}


bool directScalar( const DOCUMENT& aDocument, size_t aParent, const std::string& aHead,
                   std::string& aValue )
{
    const std::vector<size_t> lists = directLists( aDocument, aParent, aHead );

    if( lists.size() != 1 )
        return false;

    const DOCUMENT::NODE& node = aDocument.Nodes().at( lists.front() );

    if( node.children.size() < 2
        || aDocument.Nodes().at( node.children[1] ).kind == DOCUMENT::NODE_KIND::LIST )
    {
        return false;
    }

    aValue = aDocument.AtomText( node.children[1] );
    return true;
}


bool optionalNativeBool( const DOCUMENT& aDocument, size_t aParent,
                         const std::string& aHead, bool aDefault, bool& aValue )
{
    const std::vector<size_t> lists = directLists( aDocument, aParent, aHead );

    if( lists.empty() )
    {
        aValue = aDefault;
        return true;
    }

    if( lists.size() != 1 )
        return false;

    const DOCUMENT::NODE& node = aDocument.Nodes().at( lists.front() );

    if( node.children.size() != 2
        || aDocument.Nodes().at( node.children[1] ).kind == DOCUMENT::NODE_KIND::LIST )
    {
        return false;
    }

    const std::string value = aDocument.AtomText( node.children[1] );

    if( value != "yes" && value != "no" )
        return false;

    aValue = value == "yes";
    return true;
}


struct DIRECT_PROPERTY
{
    std::string name;
    std::string value;
    std::string source;
    size_t      node;
    JSON        layout;
};


bool directPropertyLayout( const DOCUMENT& aDocument, size_t aProperty, JSON& aLayout );


bool directProperties( const DOCUMENT& aDocument, size_t aSymbol,
                       std::vector<DIRECT_PROPERTY>& aProperties )
{
    const std::vector<size_t> properties = directLists( aDocument, aSymbol, "property" );

    if( properties.size() > MAX_PROPERTIES_PER_SYMBOL )
        return false;

    std::set<std::string> names;

    for( size_t property : properties )
    {
        const DOCUMENT::NODE& node = aDocument.Nodes().at( property );
        size_t nameIndex = 1;

        if( node.children.size() > nameIndex
            && aDocument.Nodes().at( node.children[nameIndex] ).kind
                       != DOCUMENT::NODE_KIND::LIST
            && aDocument.AtomText( node.children[nameIndex] ) == "private" )
        {
            ++nameIndex;
        }

        const size_t valueIndex = nameIndex + 1;

        if( node.children.size() <= valueIndex
            || aDocument.Nodes().at( node.children[nameIndex] ).kind
                       == DOCUMENT::NODE_KIND::LIST
            || aDocument.Nodes().at( node.children[valueIndex] ).kind
                       == DOCUMENT::NODE_KIND::LIST )
        {
            return false;
        }

        JSON layout;

        if( !directPropertyLayout( aDocument, property, layout ) )
            return false;

        DIRECT_PROPERTY parsed = { aDocument.AtomText( node.children[nameIndex] ),
                                   aDocument.AtomText( node.children[valueIndex] ),
                                   aDocument.RawText( property ), property,
                                   std::move( layout ) };

        if( parsed.name.empty() || parsed.name.size() > 256
            || !names.emplace( parsed.name ).second )
        {
            return false;
        }

        aProperties.emplace_back( std::move( parsed ) );
    }

    return true;
}


std::string quoted( const std::string& aText )
{
    std::string result = "\"";

    for( char character : aText )
    {
        if( character == '\\' || character == '"' )
            result.push_back( '\\' );

        result.push_back( character );
    }

    result.push_back( '"' );
    return result;
}


bool millimetresToNanometres( const std::string& aText, int64_t& aValue )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    std::from_chars_result converted = KICHAD::FromChars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr != end || !std::isfinite( value ) )
        return false;

    const long double rounded = std::round( value * 1'000'000.0L );

    if( !std::isfinite( rounded ) || rounded < -2'000'000'000.0L
        || rounded > 2'000'000'000.0L )
    {
        return false;
    }

    aValue = static_cast<int64_t>( rounded );
    return true;
}


bool directPropertyLayout( const DOCUMENT& aDocument, size_t aProperty, JSON& aLayout )
{
    const std::vector<size_t> positions = directLists( aDocument, aProperty, "at" );

    if( positions.size() != 1 )
        return false;

    const DOCUMENT::NODE& position = aDocument.Nodes().at( positions.front() );
    std::string xText;
    std::string yText;
    int64_t x = 0;
    int64_t y = 0;
    double angle = 0.0;

    if( ( position.children.size() != 3 && position.children.size() != 4 )
        || aDocument.Nodes().at( position.children[1] ).kind == DOCUMENT::NODE_KIND::LIST
        || aDocument.Nodes().at( position.children[2] ).kind == DOCUMENT::NODE_KIND::LIST )
    {
        return false;
    }

    xText = aDocument.AtomText( position.children[1] );
    yText = aDocument.AtomText( position.children[2] );

    if( !millimetresToNanometres( xText, x ) || !millimetresToNanometres( yText, y ) )
        return false;

    if( position.children.size() == 4 )
    {
        if( aDocument.Nodes().at( position.children[3] ).kind == DOCUMENT::NODE_KIND::LIST )
            return false;

        const std::string angleText = aDocument.AtomText( position.children[3] );
        const char* begin = angleText.data();
        const char* end = begin + angleText.size();
        const std::from_chars_result converted = KICHAD::FromChars( begin, end, angle );

        if( converted.ec != std::errc() || converted.ptr != end || !std::isfinite( angle )
            || angle < 0.0 || angle >= 360.0 )
        {
            return false;
        }
    }

    bool hidden = false;
    bool showName = false;
    bool doNotAutoplace = false;

    if( !optionalNativeBool( aDocument, aProperty, "hide", false, hidden )
        || !optionalNativeBool( aDocument, aProperty, "show_name", false, showName )
        || !optionalNativeBool( aDocument, aProperty, "do_not_autoplace", false,
                                doNotAutoplace ) )
    {
        return false;
    }

    aLayout = { { "position", { { "xNm", x }, { "yNm", y } } },
                { "rotationDegrees", angle },
                { "visible", !hidden },
                { "showName", showName },
                { "autoplace", !doNotAutoplace } };
    return true;
}


bool unitIdentity( const std::string& aName, int64_t& aUnit, int64_t& aBodyStyle )
{
    const size_t bodySeparator = aName.rfind( '_' );

    if( bodySeparator == std::string::npos )
        return false;

    const size_t unitSeparator = aName.rfind( '_', bodySeparator - 1 );

    if( unitSeparator == std::string::npos )
        return false;

    const char* unitBegin = aName.data() + unitSeparator + 1;
    const char* unitEnd = aName.data() + bodySeparator;
    const char* bodyBegin = unitEnd + 1;
    const char* bodyEnd = aName.data() + aName.size();
    std::from_chars_result unitResult = std::from_chars( unitBegin, unitEnd, aUnit );
    std::from_chars_result bodyResult = std::from_chars( bodyBegin, bodyEnd, aBodyStyle );
    return unitResult.ec == std::errc() && unitResult.ptr == unitEnd
           && bodyResult.ec == std::errc() && bodyResult.ptr == bodyEnd
           && aUnit >= 0 && aUnit <= 256 && aBodyStyle >= 0 && aBodyStyle <= 64;
}


bool pinMetadata( const DOCUMENT& aDocument, size_t aPin, JSON& aPinJson )
{
    std::string number;
    std::string name;
    const std::vector<size_t> at = directLists( aDocument, aPin, "at" );

    if( !directScalar( aDocument, aPin, "number", number ) || number.empty()
        || !directScalar( aDocument, aPin, "name", name )
        || number.size() > 64 || at.size() != 1 )
    {
        return false;
    }

    const DOCUMENT::NODE& position = aDocument.Nodes().at( at.front() );
    std::string xText;
    std::string yText;
    int64_t x = 0;
    int64_t y = 0;
    int angle = 0;

    if( position.children.size() < 3
        || aDocument.Nodes().at( position.children[1] ).kind == DOCUMENT::NODE_KIND::LIST
        || aDocument.Nodes().at( position.children[2] ).kind == DOCUMENT::NODE_KIND::LIST )
    {
        return false;
    }

    xText = aDocument.AtomText( position.children[1] );
    yText = aDocument.AtomText( position.children[2] );

    if( !millimetresToNanometres( xText, x ) || !millimetresToNanometres( yText, y ) )
        return false;

    if( position.children.size() >= 4 )
    {
        const DOCUMENT::NODE& angleNode = aDocument.Nodes().at( position.children[3] );

        if( angleNode.kind == DOCUMENT::NODE_KIND::LIST )
            return false;

        const std::string angleText = aDocument.AtomText( position.children[3] );
        const char* begin = angleText.data();
        const char* end = begin + angleText.size();
        std::from_chars_result converted = std::from_chars( begin, end, angle );

        if( converted.ec != std::errc() || converted.ptr != end
            || ( angle != 0 && angle != 90 && angle != 180 && angle != 270 ) )
        {
            return false;
        }
    }

    bool validStackedNumber = false;
    const std::vector<wxString> logicalNumbers = ExpandStackedPinNotation(
            wxString::FromUTF8( number ), &validStackedNumber );
    const std::string effectivePadNumber = validStackedNumber && !logicalNumbers.empty()
                                                   ? logicalNumbers.front().ToStdString()
                                                   : number;

    aPinJson = { { "number", number }, { "name", name },
                 { "effectivePadNumber", effectivePadNumber },
                 { "xNm", x }, { "yNm", y }, { "rotationDegrees", angle } };
    return true;
}


bool splitLibraryId( const std::string& aLibraryId, std::string& aNickname,
                     std::string& aItem )
{
    const size_t separator = aLibraryId.find( ':' );

    if( separator == std::string::npos || separator == 0 || separator + 1 >= aLibraryId.size()
        || aLibraryId.find( ':', separator + 1 ) != std::string::npos )
    {
        return false;
    }

    aNickname = aLibraryId.substr( 0, separator );
    aItem = aLibraryId.substr( separator + 1 );
    return !aNickname.empty() && !aItem.empty();
}


bool applyPropertyOverrides( std::string& aCacheSource,
                             const std::vector<DIRECT_PROPERTY>& aOverrides,
                             JSON& aProperties, JSON& aPropertyLayouts,
                             std::string& aError )
{
    std::unique_ptr<DOCUMENT> cache = DOCUMENT::Parse( aCacheSource, &aError );

    if( !cache || cache->Roots().size() != 1
        || cache->ListHead( cache->Roots().front() ) != "symbol" )
    {
        aError = aError.empty() ? "flattened cache is not one symbol" : aError;
        return false;
    }

    const size_t root = cache->Roots().front();
    std::vector<DIRECT_PROPERTY> existing;

    if( !directProperties( *cache, root, existing ) )
    {
        aError = "flattened cache contains malformed or duplicate properties";
        return false;
    }

    std::map<std::string, size_t> nodes;

    for( const DIRECT_PROPERTY& property : existing )
        nodes[property.name] = property.node;

    // Mandatory native fields only override an inherited value when the derived field is nonempty.
    const std::set<std::string> mandatory = { "Reference", "Value", "Footprint",
                                               "Datasheet", "Description" };
    std::string insertions;

    for( const DIRECT_PROPERTY& property : aOverrides )
    {
        if( mandatory.contains( property.name ) && property.value.empty() )
            continue;

        if( nodes.contains( property.name ) )
        {
            if( !cache->ReplaceNode( nodes.at( property.name ), property.source, &aError ) )
                return false;
        }
        else
        {
            insertions += "\n    " + property.source;
        }

        aProperties[property.name] = property.value;
        aPropertyLayouts[property.name] = property.layout;
    }

    if( !insertions.empty()
        && !cache->InsertBeforeClosingList( root, insertions, &aError ) )
    {
        return false;
    }

    return cache->Render( aCacheSource, &aError );
}


bool finalizeCacheSource( std::string& aCacheSource, const std::string& aLibraryId,
                          const JSON& aFlags, bool aNormalizeFlags, std::string& aError )
{
    std::unique_ptr<DOCUMENT> cache = DOCUMENT::Parse( aCacheSource, &aError );

    if( !cache || cache->Roots().size() != 1
        || cache->ListHead( cache->Roots().front() ) != "symbol" )
    {
        aError = aError.empty() ? "flattened cache is not one symbol" : aError;
        return false;
    }

    const size_t root = cache->Roots().front();
    const DOCUMENT::NODE& rootNode = cache->Nodes().at( root );

    if( rootNode.children.size() < 2
        || !cache->ReplaceNode( rootNode.children[1], quoted( aLibraryId ), &aError ) )
    {
        return false;
    }

    std::string nickname;
    std::string itemName;

    if( !splitLibraryId( aLibraryId, nickname, itemName ) )
    {
        aError = "flattened cache has an invalid library ID";
        return false;
    }

    for( size_t unitNode : directLists( *cache, root, "symbol" ) )
    {
        const DOCUMENT::NODE& unit = cache->Nodes().at( unitNode );
        int64_t unitNumber = 0;
        int64_t bodyStyle = 0;

        if( unit.children.size() < 2
            || cache->Nodes().at( unit.children[1] ).kind == DOCUMENT::NODE_KIND::LIST
            || !unitIdentity( cache->AtomText( unit.children[1] ), unitNumber, bodyStyle )
            || !cache->ReplaceNode( unit.children[1],
                                    quoted( itemName + "_" + std::to_string( unitNumber )
                                            + "_" + std::to_string( bodyStyle ) ),
                                    &aError ) )
        {
            aError = aError.empty() ? "flattened cache contains a malformed unit name" : aError;
            return false;
        }
    }

    if( aNormalizeFlags )
    {
        const std::vector<std::pair<std::string, bool>> flags = {
            { "exclude_from_sim", aFlags.at( "excludeFromSim" ).get<bool>() },
            { "in_bom", aFlags.at( "inBom" ).get<bool>() },
            { "on_board", aFlags.at( "onBoard" ).get<bool>() },
            { "in_pos_files", aFlags.at( "inPosFiles" ).get<bool>() }
        };
        std::string insertions;

        for( const auto& [ head, value ] : flags )
        {
            const std::vector<size_t> nodes = directLists( *cache, root, head );

            if( nodes.size() > 1 )
            {
                aError = "flattened cache contains duplicate native inclusion flags";
                return false;
            }

            const std::string expression = "(" + head + " " + ( value ? "yes" : "no" ) + ")";

            if( nodes.empty() )
                insertions += "\n    " + expression;
            else if( !cache->ReplaceNode( nodes.front(), expression, &aError ) )
                return false;
        }

        if( !insertions.empty()
            && !cache->InsertBeforeClosingList( root, insertions, &aError ) )
        {
            return false;
        }
    }

    return cache->Render( aCacheSource, &aError );
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT
DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve( const JSON& aCompilerIr, const JSON& aLibrarySources )
{
    RESULT result;
    result.counts = { { "libraries", 0 }, { "symbols", 0 }, { "pins", 0 } };

    if( !aCompilerIr.is_object() || aCompilerIr.value( "language", "" ) != "kichad-design"
        || aCompilerIr.value( "version", 0 ) != 1 || !aCompilerIr.contains( "libraries" )
        || !aCompilerIr["libraries"].is_array() || !aCompilerIr.contains( "schematic" )
        || !aCompilerIr["schematic"].is_object()
        || !aCompilerIr["schematic"].contains( "components" )
        || !aCompilerIr["schematic"]["components"].is_array()
        || !aLibrarySources.is_object() )
    {
        diagnostic( result, "invalid_compiler_ir",
                    "symbol resolution requires valid KiChad Design Script version 1 IR" );
        return result;
    }

    std::map<std::string, JSON> declarations;

    for( const JSON& library : aCompilerIr["libraries"] )
    {
        if( library.is_object() && library.value( "kind", "" ) == "symbol"
            && library.contains( "id" ) && library["id"].is_string() )
        {
            declarations[library["id"].get<std::string>()] = library;
        }
    }

    std::map<std::string, std::set<int64_t>> requested;

    for( const JSON& component : aCompilerIr["schematic"]["components"] )
    {
        if( !component.is_object() || !component.contains( "symbol" )
            || !component["symbol"].is_string() || !component.contains( "units" )
            || !component["units"].is_array() )
        {
            diagnostic( result, "invalid_component_ir", "component symbol IR is malformed" );
            continue;
        }

        const std::string libraryId = component["symbol"].get<std::string>();

        for( const JSON& unit : component["units"] )
        {
            if( unit.is_object() && unit.contains( "number" )
                && unit["number"].is_number_integer() )
            {
                requested[libraryId].insert( unit["number"].get<int64_t>() );
            }
        }
    }

    if( requested.size() > MAX_RESOLVED_SYMBOLS )
    {
        diagnostic( result, "too_many_symbols", "at most 4096 distinct symbols may be resolved" );
        return result;
    }

    std::map<std::string, std::unique_ptr<DOCUMENT>> parsedLibraries;

    for( const auto& [ libraryId, requestedUnits ] : requested )
    {
        std::string nickname;
        std::string itemName;

        if( !splitLibraryId( libraryId, nickname, itemName ) || !declarations.contains( nickname ) )
        {
            diagnostic( result, "unresolved_symbol_library",
                        "symbol " + libraryId + " has no declared library" );
            continue;
        }

        if( !aLibrarySources.contains( nickname ) || !aLibrarySources[nickname].is_string() )
        {
            diagnostic( result, "missing_symbol_library_source",
                        "symbol library " + nickname + " was not inventoried" );
            continue;
        }

        if( !parsedLibraries.contains( nickname ) )
        {
            const std::string source = aLibrarySources[nickname].get<std::string>();
            std::string parseError;

            if( source.empty() || source.size() > MAX_LIBRARY_BYTES )
            {
                diagnostic( result, "invalid_symbol_library",
                            "project symbol library " + nickname
                                    + " must contain 1 byte to 32 MiB" );
                continue;
            }

            std::unique_ptr<DOCUMENT> document = DOCUMENT::Parse( source, &parseError );

            if( !document || document->Roots().size() != 1
                || document->ListHead( document->Roots().front() ) != "kicad_symbol_lib" )
            {
                diagnostic( result, "invalid_symbol_library",
                            nickname + ": "
                                    + ( parseError.empty() ? "expected one kicad_symbol_lib root"
                                                           : parseError ) );
                continue;
            }

            std::string versionText;
            int64_t version = 0;
            const size_t root = document->Roots().front();

            if( !directScalar( *document, root, "version", versionText ) )
            {
                diagnostic( result, "invalid_symbol_library",
                            nickname + ": symbol library has no unique version" );
                continue;
            }

            const char* versionBegin = versionText.data();
            const char* versionEnd = versionBegin + versionText.size();
            const std::from_chars_result converted =
                    std::from_chars( versionBegin, versionEnd, version );

            if( converted.ec != std::errc() || converted.ptr != versionEnd
                || version < 20200126 || version > MAX_SYMBOL_LIBRARY_VERSION )
            {
                diagnostic( result, "unsupported_symbol_library_version",
                            nickname + ": symbol library version is not supported by KiCad 10" );
                continue;
            }

            parsedLibraries[nickname] = std::move( document );
            ++result.counts["libraries"].get_ref<int64_t&>();
        }

        if( !parsedLibraries.contains( nickname ) )
            continue;

        const DOCUMENT& library = *parsedLibraries.at( nickname );
        const size_t root = library.Roots().front();
        std::map<std::string, std::vector<size_t>> symbolsByName;

        for( size_t symbol : directLists( library, root, "symbol" ) )
        {
            const DOCUMENT::NODE& node = library.Nodes().at( symbol );

            if( node.children.size() >= 2
                && library.Nodes().at( node.children[1] ).kind != DOCUMENT::NODE_KIND::LIST )
            {
                symbolsByName[library.AtomText( node.children[1] )].push_back( symbol );
            }
        }

        if( !symbolsByName.contains( itemName ) || symbolsByName.at( itemName ).size() != 1 )
        {
            diagnostic( result, "unresolved_symbol",
                        "project library " + nickname + " must contain exactly one symbol "
                                + itemName );
            continue;
        }

        std::vector<size_t> inheritanceChain;
        std::set<std::string> visitedNames;
        std::string currentName = itemName;
        bool inheritanceValid = true;

        while( true )
        {
            if( inheritanceChain.size() >= MAX_INHERITANCE_DEPTH )
            {
                diagnostic( result, "symbol_inheritance_too_deep",
                            "symbol " + libraryId
                                    + " exceeds the 64-level inheritance bound" );
                inheritanceValid = false;
                break;
            }

            if( !visitedNames.emplace( currentName ).second )
            {
                diagnostic( result, "recursive_symbol_inheritance",
                            "symbol " + libraryId + " has a recursive inheritance chain" );
                inheritanceValid = false;
                break;
            }

            auto current = symbolsByName.find( currentName );

            if( current == symbolsByName.end() || current->second.size() != 1 )
            {
                diagnostic( result, "unresolved_symbol_parent",
                            "symbol " + libraryId + " requires exactly one parent "
                                    + currentName + " in the same project library" );
                inheritanceValid = false;
                break;
            }

            const size_t currentSymbol = current->second.front();
            inheritanceChain.push_back( currentSymbol );
            const std::vector<size_t> extends = directLists( library, currentSymbol, "extends" );

            if( extends.empty() )
                break;

            std::string parentName;

            if( extends.size() != 1
                || !directScalar( library, currentSymbol, "extends", parentName )
                || parentName.empty() || parentName.size() > 256 )
            {
                diagnostic( result, "invalid_symbol_inheritance",
                            "symbol " + libraryId
                                    + " has a duplicate or malformed extends field" );
                inheritanceValid = false;
                break;
            }

            currentName = parentName;
        }

        if( !inheritanceValid )
            continue;

        const size_t symbol = inheritanceChain.back();

        bool excludeFromSim = false;
        bool inBom = true;
        bool onBoard = true;
        bool inPosFiles = true;
        const size_t flagSymbol =
                inheritanceChain.size() == 1 ? symbol : inheritanceChain[1];

        if( !optionalNativeBool( library, flagSymbol, "exclude_from_sim", false,
                                 excludeFromSim )
            || !optionalNativeBool( library, flagSymbol, "in_bom", true, inBom )
            || !optionalNativeBool( library, flagSymbol, "on_board", true, onBoard )
            || !optionalNativeBool( library, flagSymbol, "in_pos_files", true, inPosFiles ) )
        {
            diagnostic( result, "invalid_symbol_flags",
                        "symbol " + libraryId
                                + " has duplicate or malformed native inclusion flags" );
            continue;
        }

        JSON flags = { { "excludeFromSim", excludeFromSim },
                       { "inBom", inBom },
                       { "onBoard", onBoard },
                       { "inPosFiles", inPosFiles } };

        JSON properties = JSON::object();
        JSON propertyLayouts = JSON::object();
        std::vector<DIRECT_PROPERTY> rootProperties;

        if( !directProperties( library, symbol, rootProperties ) )
        {
            diagnostic( result, "invalid_symbol_properties",
                        "symbol " + libraryId
                                + " has malformed, duplicate, or excessive properties" );
            continue;
        }

        for( const DIRECT_PROPERTY& property : rootProperties )
        {
            properties[property.name] = property.value;
            propertyLayouts[property.name] = property.layout;
        }

        std::map<int64_t, JSON> pinsByUnit;
        std::set<int64_t> presentUnits;

        for( size_t unitNode : directLists( library, symbol, "symbol" ) )
        {
            const DOCUMENT::NODE& node = library.Nodes().at( unitNode );
            int64_t unit = 0;
            int64_t bodyStyle = 0;

            if( node.children.size() < 2
                || library.Nodes().at( node.children[1] ).kind == DOCUMENT::NODE_KIND::LIST
                || !unitIdentity( library.AtomText( node.children[1] ), unit, bodyStyle ) )
            {
                diagnostic( result, "invalid_symbol_unit",
                            "symbol " + libraryId + " contains a malformed unit name" );
                continue;
            }

            if( bodyStyle != 0 && bodyStyle != 1 )
                continue;

            presentUnits.emplace( unit );

            for( size_t pin : directLists( library, unitNode, "pin" ) )
            {
                JSON metadata;

                if( !pinMetadata( library, pin, metadata ) )
                {
                    diagnostic( result, "invalid_symbol_pin",
                                "symbol " + libraryId + " contains a malformed pin" );
                    continue;
                }

                pinsByUnit[unit].push_back( std::move( metadata ) );
            }
        }

        JSON units = JSON::object();
        size_t unitCount = 0;

        for( int64_t presentUnit : presentUnits )
        {
            if( presentUnit > 0 )
                ++unitCount;
        }

        for( int64_t unit : requestedUnits )
        {
            if( !presentUnits.contains( unit ) )
            {
                diagnostic( result, "unresolved_symbol_unit",
                            "symbol " + libraryId + " has no unit " + std::to_string( unit ) );
                continue;
            }

            JSON pins = JSON::array();

            if( pinsByUnit.contains( 0 ) )
            {
                for( const JSON& pin : pinsByUnit.at( 0 ) )
                    pins.push_back( pin );
            }

            if( pinsByUnit.contains( unit ) )
            {
                for( const JSON& pin : pinsByUnit.at( unit ) )
                    pins.push_back( pin );
            }

            // A unit may legitimately expose no pins: mechanical parts such as lens holders,
            // mounting hardware and logos are placed and sourced like any component but carry
            // no electrical connection, exactly as KiCad's own mounting-hole symbols do.
            if( pins.size() > MAX_PINS_PER_UNIT )
            {
                diagnostic( result, "invalid_symbol_pin_count",
                            "symbol " + libraryId + " unit " + std::to_string( unit )
                                    + " must expose at most 1024 pins" );
                continue;
            }

            std::sort( pins.begin(), pins.end(), []( const JSON& aLeft, const JSON& aRight )
                       {
                           if( aLeft["number"] != aRight["number"] )
                               return aLeft["number"].get<std::string>()
                                      < aRight["number"].get<std::string>();

                           if( aLeft["xNm"] != aRight["xNm"] )
                               return aLeft["xNm"].get<int64_t>()
                                      < aRight["xNm"].get<int64_t>();

                           return aLeft["yNm"].get<int64_t>()
                                  < aRight["yNm"].get<int64_t>();
                       } );
            result.counts["pins"] = result.counts["pins"].get<int64_t>() + pins.size();
            units[std::to_string( unit )] = std::move( pins );
        }

        std::string cacheSource = library.RawText( symbol );
        std::string editError;
        bool cacheValid = true;

        for( size_t index = inheritanceChain.size(); index > 1; --index )
        {
            const size_t derived = inheritanceChain[index - 2];
            std::vector<DIRECT_PROPERTY> overrides;
            bool embeddedFonts = false;

            if( !directLists( library, derived, "symbol" ).empty()
                || !directLists( library, derived, "embedded_files" ).empty()
                || !optionalNativeBool( library, derived, "embedded_fonts", false,
                                        embeddedFonts )
                || embeddedFonts
                || !directProperties( library, derived, overrides )
                || !applyPropertyOverrides( cacheSource, overrides, properties,
                                             propertyLayouts, editError ) )
            {
                cacheValid = false;
                break;
            }
        }

        if( !cacheValid
            || !finalizeCacheSource( cacheSource, libraryId, flags,
                                     inheritanceChain.size() > 1, editError ) )
        {
            diagnostic( result, "invalid_symbol_cache_source",
                        libraryId + ": "
                                + ( editError.empty()
                                            ? "derived symbol contains unsupported content"
                                            : editError ) );
            continue;
        }

        result.symbols[libraryId] = { { "libraryId", libraryId },
                                      { "cacheSource", cacheSource },
                                      { "inheritanceDepth", inheritanceChain.size() - 1 },
                                      { "flags", std::move( flags ) },
                                      { "properties", std::move( properties ) },
                                      { "propertyLayouts", std::move( propertyLayouts ) },
                                      { "unitCount", unitCount },
                                      { "units", std::move( units ) } };
    }

    result.counts["symbols"] = result.symbols.size();
    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
