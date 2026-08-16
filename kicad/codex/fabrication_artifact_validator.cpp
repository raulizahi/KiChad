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

#include "fabrication_artifact_validator.h"

#include "lossless_sexpr_document.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <wx/string.h>
#include <wx/xml/xml.h>


namespace
{

using JSON = nlohmann::json;

constexpr uintmax_t MAX_AUXILIARY_ARTIFACT_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr size_t MAX_AUXILIARY_LINES = 4'000'000;
constexpr size_t MAX_AUXILIARY_LINE_BYTES = 1024 * 1024;
constexpr size_t MAX_XML_NODES = 2'000'000;
constexpr size_t MAX_XML_DEPTH = 256;
constexpr size_t MAX_VRML_DEPTH = 1024;
constexpr size_t MAX_DRILL_HOLE_ROWS = 100'000;
constexpr size_t MAX_NETLIST_COMPONENTS = 100'000;
constexpr size_t MAX_NETLIST_NETS = 250'000;
constexpr size_t MAX_NETLIST_NODES = 1'000'000;


bool boundedFile( const std::filesystem::path& aPath, std::string& aError )
{
    std::error_code filesystemError;
    const uintmax_t bytes = std::filesystem::file_size( aPath, filesystemError );

    if( filesystemError || bytes == 0 || bytes > MAX_AUXILIARY_ARTIFACT_BYTES )
    {
        aError = "auxiliary fabrication artifact has an invalid size";
        return false;
    }

    return true;
}


void trimCarriageReturn( std::string& aLine )
{
    if( !aLine.empty() && aLine.back() == '\r' )
        aLine.pop_back();
}


bool validBoundedLine( const std::string& aLine )
{
    return aLine.size() <= MAX_AUXILIARY_LINE_BYTES
           && aLine.find( '\0' ) == std::string::npos;
}


bool nonNegativeInteger( const JSON& aValue )
{
    return aValue.is_number_integer() && aValue.get<int64_t>() >= 0;
}


bool validateCountObject( const JSON& aObject, const std::set<std::string>& aKeys )
{
    if( !aObject.is_object() || aObject.size() != aKeys.size() )
        return false;

    for( const std::string& key : aKeys )
    {
        if( !aObject.contains( key ) || !nonNegativeInteger( aObject[key] ) )
            return false;
    }

    return true;
}

} // namespace


bool KICHAD::FABRICATION_ARTIFACT_VALIDATOR::ValidateAssemblySvg(
        const std::filesystem::path& aPath, std::string& aError )
{
    if( !boundedFile( aPath, aError ) )
        return false;

    std::ifstream source( aPath, std::ios::binary );
    std::string text( ( std::istreambuf_iterator<char>( source ) ),
                      std::istreambuf_iterator<char>() );
    std::string lowered = text;
    std::transform( lowered.begin(), lowered.end(), lowered.begin(),
                    []( unsigned char aCharacter )
                    {
                        return static_cast<char>( std::tolower( aCharacter ) );
                    } );

    if( source.bad() || text.find( '\0' ) != std::string::npos
        || lowered.find( "<!entity" ) != std::string::npos
        || lowered.find( "javascript:" ) != std::string::npos
        || lowered.find( "url(http" ) != std::string::npos
        || lowered.find( "url(file" ) != std::string::npos
        || lowered.find( "@import" ) != std::string::npos )
    {
        aError = "assembly SVG could not be read safely";
        return false;
    }

    wxXmlDocument document;

    if( !document.Load( wxString::FromUTF8( aPath.string().c_str() ) ) )
    {
        aError = "assembly SVG is not well-formed XML";
        return false;
    }

    wxXmlNode* root = document.GetRoot();

    if( !root || root->GetName() != wxS( "svg" )
        || root->GetAttribute( wxS( "version" ) ) != wxS( "1.1" )
        || root->GetAttribute( wxS( "xmlns" ) ) != wxS( "http://www.w3.org/2000/svg" )
        || root->GetAttribute( wxS( "width" ) ).empty()
        || root->GetAttribute( wxS( "height" ) ).empty()
        || root->GetAttribute( wxS( "viewBox" ) ).empty() )
    {
        aError = "assembly SVG has the wrong native root schema";
        return false;
    }

    std::vector<std::pair<wxXmlNode*, size_t>> pending = { { root, 0 } };
    size_t nodeCount = 0;
    size_t graphics = 0;
    bool nativeDescription = false;

    while( !pending.empty() )
    {
        const auto [node, depth] = pending.back();
        pending.pop_back();

        if( depth > MAX_XML_DEPTH || ++nodeCount > MAX_XML_NODES )
        {
            aError = "assembly SVG exceeds structural limits";
            return false;
        }

        if( node->GetType() == wxXML_ELEMENT_NODE )
        {
            const wxString name = node->GetName().Lower();

            if( name == wxS( "script" ) || name == wxS( "foreignobject" )
                || name == wxS( "iframe" ) )
            {
                aError = "assembly SVG contains active content";
                return false;
            }

            if( name == wxS( "path" ) || name == wxS( "line" )
                || name == wxS( "polyline" ) || name == wxS( "polygon" )
                || name == wxS( "circle" ) || name == wxS( "rect" ) )
            {
                ++graphics;
            }

            if( name == wxS( "desc" )
                && node->GetNodeContent().Upper().Contains( wxS( "PCBNEW" ) ) )
            {
                nativeDescription = true;
            }

            for( wxXmlAttribute* attribute = node->GetAttributes(); attribute;
                 attribute = attribute->GetNext() )
            {
                const wxString attributeName = attribute->GetName().Lower();
                const wxString value = attribute->GetValue().Lower();

                if( attributeName.StartsWith( wxS( "on" ) )
                    || ( ( attributeName == wxS( "href" )
                           || attributeName == wxS( "xlink:href" ) )
                         && !value.StartsWith( wxS( "#" ) )
                         && !value.StartsWith( wxS( "data:image/" ) ) ) )
                {
                    aError = "assembly SVG contains an unsafe reference or event handler";
                    return false;
                }
            }
        }

        for( wxXmlNode* child = node->GetChildren(); child; child = child->GetNext() )
            pending.emplace_back( child, depth + 1 );
    }

    if( graphics == 0 || !nativeDescription )
    {
        aError = "assembly SVG contains no native PCB graphics";
        return false;
    }

    return true;
}


bool KICHAD::FABRICATION_ARTIFACT_VALIDATOR::ValidateAssemblyDxf(
        const std::filesystem::path& aPath, std::string& aError )
{
    if( !boundedFile( aPath, aError ) )
        return false;

    std::ifstream input( aPath, std::ios::binary );
    std::string codeLine;
    std::string valueLine;
    bool insideSection = false;
    bool expectSectionName = false;
    bool sawEof = false;
    size_t pairs = 0;
    std::map<std::string, size_t> sections;

    while( std::getline( input, codeLine ) )
    {
        if( !std::getline( input, valueLine ) )
        {
            aError = "assembly DXF has an incomplete group pair";
            return false;
        }

        trimCarriageReturn( codeLine );
        trimCarriageReturn( valueLine );

        if( !validBoundedLine( codeLine ) || !validBoundedLine( valueLine )
            || ++pairs > MAX_AUXILIARY_LINES / 2 )
        {
            aError = "assembly DXF exceeds structural limits";
            return false;
        }

        const size_t first = codeLine.find_first_not_of( " \t" );
        const size_t last = codeLine.find_last_not_of( " \t" );

        if( first == std::string::npos )
        {
            aError = "assembly DXF has an empty group code";
            return false;
        }

        const std::string_view codeText( codeLine.data() + first, last - first + 1 );
        int code = -1;
        const auto parsed = std::from_chars( codeText.data(), codeText.data() + codeText.size(), code );

        if( parsed.ec != std::errc() || parsed.ptr != codeText.data() + codeText.size()
            || code < 0 || code > 1071 || sawEof )
        {
            aError = "assembly DXF has an invalid group code or trailing data";
            return false;
        }

        const size_t valueFirst = valueLine.find_first_not_of( " \t" );
        const size_t valueLast = valueLine.find_last_not_of( " \t" );
        const std::string value = valueFirst == std::string::npos
                                          ? std::string()
                                          : valueLine.substr( valueFirst,
                                                              valueLast - valueFirst + 1 );

        if( expectSectionName )
        {
            if( code != 2 || value.empty() || !sections.emplace( value, 1 ).second )
            {
                aError = "assembly DXF has a duplicate or malformed section";
                return false;
            }

            expectSectionName = false;
            continue;
        }

        if( code == 0 && value == "SECTION" )
        {
            if( insideSection )
            {
                aError = "assembly DXF nests sections";
                return false;
            }

            insideSection = true;
            expectSectionName = true;
        }
        else if( code == 0 && value == "ENDSEC" )
        {
            if( !insideSection )
            {
                aError = "assembly DXF closes an unopened section";
                return false;
            }

            insideSection = false;
        }
        else if( code == 0 && value == "EOF" )
        {
            if( insideSection )
            {
                aError = "assembly DXF ends inside a section";
                return false;
            }

            sawEof = true;
        }
    }

    if( input.bad() || insideSection || expectSectionName || !sawEof
        || sections["HEADER"] != 1 || sections["ENTITIES"] != 1 )
    {
        aError = "assembly DXF is missing its HEADER, ENTITIES, or EOF structure";
        return false;
    }

    return true;
}


bool KICHAD::FABRICATION_ARTIFACT_VALIDATOR::ValidateGenCad(
        const std::filesystem::path& aPath, std::string& aError )
{
    if( !boundedFile( aPath, aError ) )
        return false;

    std::ifstream input( aPath, std::ios::binary );
    std::string line;
    std::string openSection;
    std::string lastNonEmpty;
    std::map<std::string, size_t> sections;
    size_t lineCount = 0;
    bool versionSeen = false;

    while( std::getline( input, line ) )
    {
        trimCarriageReturn( line );

        if( !validBoundedLine( line ) || ++lineCount > MAX_AUXILIARY_LINES )
        {
            aError = "GenCAD artifact exceeds structural limits";
            return false;
        }

        if( line.empty() )
            continue;

        lastNonEmpty = line;

        if( line.front() != '$' )
        {
            if( openSection == "HEADER" && line == "GENCAD 1.4" )
                versionSeen = true;

            continue;
        }

        if( line.starts_with( "$END" ) )
        {
            const std::string closed = line.substr( 4 );

            if( openSection.empty() || closed != openSection )
            {
                aError = "GenCAD artifact has mismatched section delimiters";
                return false;
            }

            openSection.clear();
            continue;
        }

        const std::string section = line.substr( 1 );

        if( !openSection.empty() || section.empty()
            || !std::all_of( section.begin(), section.end(),
                             []( unsigned char aCharacter )
                             {
                                 return std::isupper( aCharacter ) || aCharacter == '_';
                             } )
            || ++sections[section] != 1 )
        {
            aError = "GenCAD artifact has a nested, duplicate, or invalid section";
            return false;
        }

        openSection = section;
    }

    static const std::set<std::string> required = {
        "HEADER", "BOARD", "COMPONENTS", "SIGNALS", "TRACKS", "ROUTES"
    };

    if( input.bad() || !openSection.empty() || !versionSeen || lastNonEmpty != "$ENDROUTES" )
    {
        aError = "GenCAD artifact is incomplete";
        return false;
    }

    for( const std::string& section : required )
    {
        if( sections[section] != 1 )
        {
            aError = "GenCAD artifact is missing required section " + section;
            return false;
        }
    }

    return true;
}


bool KICHAD::FABRICATION_ARTIFACT_VALIDATOR::ValidateVrml(
        const std::filesystem::path& aPath, std::string& aError )
{
    if( !boundedFile( aPath, aError ) )
        return false;

    std::ifstream input( aPath, std::ios::binary );
    std::string source( ( std::istreambuf_iterator<char>( input ) ),
                        std::istreambuf_iterator<char>() );

    if( input.bad() || !source.starts_with( "#VRML V2.0 utf8" )
        || source.find( '\0' ) != std::string::npos )
    {
        aError = "VRML artifact has the wrong native header or could not be read";
        return false;
    }

    std::vector<char> stack;
    bool comment = false;
    bool quoted = false;
    bool escaped = false;

    for( size_t index = source.find( '\n' ) + 1; index < source.size(); ++index )
    {
        const unsigned char character = static_cast<unsigned char>( source[index] );

        if( comment )
        {
            comment = character != '\n';
            continue;
        }

        if( quoted )
        {
            if( escaped )
                escaped = false;
            else if( character == '\\' )
                escaped = true;
            else if( character == '"' )
                quoted = false;

            continue;
        }

        if( character == '#' )
            comment = true;
        else if( character == '"' )
            quoted = true;
        else if( character == '{' || character == '[' )
        {
            if( stack.size() >= MAX_VRML_DEPTH )
            {
                aError = "VRML artifact exceeds the nesting limit";
                return false;
            }

            stack.push_back( character == '{' ? '}' : ']' );
        }
        else if( character == '}' || character == ']' )
        {
            if( stack.empty() || stack.back() != character )
            {
                aError = "VRML artifact has mismatched delimiters";
                return false;
            }

            stack.pop_back();
        }
        else if( std::iscntrl( character ) && !std::isspace( character ) )
        {
            aError = "VRML artifact contains an invalid control character";
            return false;
        }
    }

    if( quoted || !stack.empty() || source.find( "Transform {" ) == std::string::npos
        || source.find( "IndexedFaceSet" ) == std::string::npos
        || source.find( "Script {" ) != std::string::npos
        || source.find( "Inline {" ) != std::string::npos
        || source.find( "Anchor {" ) != std::string::npos
        || source.find( "EXTERNPROTO" ) != std::string::npos )
    {
        aError = "VRML artifact is incomplete or lacks native board geometry";
        return false;
    }

    return true;
}


bool KICHAD::FABRICATION_ARTIFACT_VALIDATOR::ValidateBoardStatistics(
        const std::filesystem::path& aPath, const std::string& aExpectedStem,
        std::string& aError )
{
    if( !boundedFile( aPath, aError ) )
        return false;

    std::ifstream input( aPath, std::ios::binary );
    JSON document = JSON::parse( input, nullptr, false );
    static const std::set<std::string> topLevel = {
        "metadata", "board", "pads", "vias", "components", "drill_holes"
    };

    if( document.is_discarded() || !document.is_object()
        || document.size() != topLevel.size() )
    {
        aError = "board statistics artifact is not the expected JSON object";
        return false;
    }

    for( const std::string& key : topLevel )
    {
        if( !document.contains( key ) )
        {
            aError = "board statistics artifact is missing " + key;
            return false;
        }
    }

    const JSON& metadata = document["metadata"];
    const JSON& board = document["board"];

    if( !metadata.is_object() || metadata.value( "project", "" ) != aExpectedStem
        || metadata.value( "board_name", "" ) != aExpectedStem
        || !metadata.value( "generator", "" ).starts_with( "KiCad 10.0.4" )
        || metadata.value( "date", "" ).empty()
        || !board.is_object() || !board.value( "has_outline", false )
        || !board.contains( "width" ) || !board["width"].is_string()
        || !board.contains( "height" ) || !board["height"].is_string()
        || !board.contains( "area" ) || !board["area"].is_string()
        || !board.contains( "board_thickness" ) || !board["board_thickness"].is_string() )
    {
        aError = "board statistics metadata or outline does not match the planned board";
        return false;
    }

    if( !validateCountObject( document["pads"],
                              { "through_hole", "smd", "connector", "npth",
                                "castellated", "press_fit" } )
        || !validateCountObject( document["vias"],
                                 { "through", "blind", "buried", "micro" } ) )
    {
        aError = "board statistics contains invalid pad or via counts";
        return false;
    }

    const JSON& components = document["components"];

    if( !components.is_object() || components.size() != 4 )
    {
        aError = "board statistics contains invalid component groups";
        return false;
    }

    for( const char* kind : { "tht", "smd", "unspecified", "total" } )
    {
        if( !components.contains( kind )
            || !validateCountObject( components[kind], { "front", "back", "total" } )
            || components[kind]["total"].get<int64_t>()
                       != components[kind]["front"].get<int64_t>()
                                  + components[kind]["back"].get<int64_t>() )
        {
            aError = "board statistics contains inconsistent component counts";
            return false;
        }
    }

    const JSON& drillHoles = document["drill_holes"];

    if( !drillHoles.is_array() || drillHoles.size() > MAX_DRILL_HOLE_ROWS )
    {
        aError = "board statistics contains an invalid drill-hole table";
        return false;
    }

    for( const JSON& hole : drillHoles )
    {
        if( !hole.is_object() || !hole.contains( "count" )
            || !nonNegativeInteger( hole["count"] ) || hole["count"].get<int64_t>() == 0
            || !hole.contains( "shape" ) || !hole["shape"].is_string()
            || !hole.contains( "x_size" ) || !hole["x_size"].is_string()
            || !hole.contains( "y_size" ) || !hole["y_size"].is_string()
            || !hole.contains( "plated" ) || !hole["plated"].is_boolean() )
        {
            aError = "board statistics contains a malformed drill-hole row";
            return false;
        }
    }

    return true;
}


bool KICHAD::FABRICATION_ARTIFACT_VALIDATOR::ValidateKiCadNetlist(
        const std::filesystem::path& aPath, const nlohmann::json& aPlan,
        std::string& aError )
{
    if( !boundedFile( aPath, aError ) )
        return false;

    std::ifstream input( aPath, std::ios::binary );
    std::string source( ( std::istreambuf_iterator<char>( input ) ),
                        std::istreambuf_iterator<char>() );

    if( input.bad() || source.find( '\0' ) != std::string::npos )
    {
        aError = "KiCad netlist could not be read safely";
        return false;
    }

    std::unique_ptr<KICHAD::LOSSLESS_SEXPR_DOCUMENT> document =
            KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( std::move( source ), &aError );

    if( !document || document->Roots().size() != 1
        || document->ListHead( document->Roots().front() ) != "export" )
    {
        if( aError.empty() )
            aError = "KiCad netlist has no unique export root";

        return false;
    }

    const auto directList = [&]( size_t aParent, const std::string& aHead )
    {
        size_t match = KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE;

        for( size_t child : document->Nodes().at( aParent ).children )
        {
            if( document->Nodes().at( child ).kind
                        != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
                || document->ListHead( child ) != aHead )
            {
                continue;
            }

            if( match != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE )
                return KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE;

            match = child;
        }

        return match;
    };
    const auto scalar = [&]( size_t aParent, const std::string& aHead )
    {
        const size_t field = directList( aParent, aHead );

        if( field == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
            || document->Nodes().at( field ).children.size() != 2
            || document->Nodes().at( document->Nodes().at( field ).children[1] ).kind
                       == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST )
        {
            return std::string();
        }

        return document->AtomText( document->Nodes().at( field ).children[1] );
    };
    const size_t root = document->Roots().front();
    const size_t design = directList( root, "design" );
    const size_t components = directList( root, "components" );
    const size_t nets = directList( root, "nets" );

    if( scalar( root, "version" ) != "E"
        || design == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
        || components == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
        || nets == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
        || !scalar( design, "tool" ).starts_with( "Eeschema 10.0.4" )
        || std::filesystem::path( scalar( design, "source" ) ).filename()
                   != aPlan.at( "fileStem" ).get<std::string>() + ".kicad_sch" )
    {
        aError = "KiCad netlist has the wrong version, source, or native producer";
        return false;
    }

    std::set<std::string> actualReferences;

    for( size_t child : document->Nodes().at( components ).children )
    {
        if( document->Nodes().at( child ).kind
                    != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
            || document->ListHead( child ) != "comp" )
        {
            continue;
        }

        const std::string reference = scalar( child, "ref" );

        if( reference.empty() || !actualReferences.emplace( reference ).second
            || actualReferences.size() > MAX_NETLIST_COMPONENTS )
        {
            aError = "KiCad netlist has an empty, duplicate, or excessive component reference";
            return false;
        }
    }

    std::set<std::string> expectedReferences;

    for( const JSON& reference : aPlan.at( "expectedNetlistReferences" ) )
        expectedReferences.emplace( reference.get<std::string>() );

    if( actualReferences != expectedReferences )
    {
        const auto describeDifference =
                []( const std::set<std::string>& aLeft,
                    const std::set<std::string>& aRight )
                {
                    constexpr size_t MAX_REPORTED_REFERENCES = 12;
                    std::ostringstream description;
                    size_t count = 0;

                    for( const std::string& reference : aLeft )
                    {
                        if( aRight.contains( reference ) )
                            continue;

                        if( count != 0 )
                            description << ", ";

                        if( count == MAX_REPORTED_REFERENCES )
                        {
                            description << "...";
                            break;
                        }

                        description << reference;
                        ++count;
                    }

                    return description.str();
                };
        const std::string missing =
                describeDifference( expectedReferences, actualReferences );
        const std::string unexpected =
                describeDifference( actualReferences, expectedReferences );
        aError = "KiCad netlist component references differ from compiled KDS"
                 " (expected " + std::to_string( expectedReferences.size() )
                 + ", native " + std::to_string( actualReferences.size() ) + ")";

        if( !missing.empty() )
            aError += "; missing from native export: " + missing;

        if( !unexpected.empty() )
            aError += "; unexpected in native export: " + unexpected;

        return false;
    }

    using NETLIST_NODE = std::pair<std::string, std::string>;
    std::map<std::string, std::set<NETLIST_NODE>> actualNets;
    std::set<NETLIST_NODE> actualNoConnects;
    std::set<std::string> actualNetNames;
    size_t totalNodes = 0;

    for( size_t child : document->Nodes().at( nets ).children )
    {
        if( document->Nodes().at( child ).kind
                    != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
            || document->ListHead( child ) != "net" )
        {
            continue;
        }

        const std::string name = scalar( child, "name" );
        std::set<NETLIST_NODE> nodes;

        if( name.empty() || !actualNetNames.emplace( name ).second
            || actualNetNames.size() > MAX_NETLIST_NETS )
        {
            aError = "KiCad netlist has an empty, duplicate, or excessive net";
            return false;
        }

        size_t noConnectNodeCount = 0;

        for( size_t netChild : document->Nodes().at( child ).children )
        {
            if( document->Nodes().at( netChild ).kind
                        != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
                || document->ListHead( netChild ) != "node" )
            {
                continue;
            }

            const std::string reference = scalar( netChild, "ref" );
            const std::string pin = scalar( netChild, "pin" );
            const std::string pinType = scalar( netChild, "pintype" );

            if( reference.empty() || pin.empty()
                || !nodes.emplace( reference, pin ).second
                || ++totalNodes > MAX_NETLIST_NODES )
            {
                aError = "KiCad netlist has an invalid, duplicate, or excessive node";
                return false;
            }

            if( pinType.find( "no_connect" ) != std::string::npos )
                ++noConnectNodeCount;
        }

        if( noConnectNodeCount != 0 )
        {
            if( nodes.size() != 1 || noConnectNodeCount != 1
                || !name.starts_with( "unconnected-(" ) || !name.ends_with( ')' )
                || !actualNoConnects.emplace( *nodes.begin() ).second )
            {
                aError = "KiCad netlist has a malformed or duplicate no-connect endpoint";
                return false;
            }
        }
        else
        {
            actualNets.emplace( name, std::move( nodes ) );
        }
    }

    std::map<std::string, std::set<NETLIST_NODE>> expectedNets;

    for( const JSON& net : aPlan.at( "expectedNetlistNets" ) )
    {
        std::set<NETLIST_NODE> nodes;

        for( const JSON& node : net.at( "nodes" ) )
        {
            if( !node.is_object() || !node.contains( "reference" )
                || !node["reference"].is_string() || !node.contains( "pin" )
                || !node["pin"].is_string()
                || !nodes.emplace( node["reference"].get<std::string>(),
                                   node["pin"].get<std::string>() ).second )
            {
                aError = "fabrication plan contains an invalid expected net node";
                return false;
            }
        }

        if( !expectedNets.emplace( net.at( "name" ).get<std::string>(),
                                  std::move( nodes ) ).second )
        {
            aError = "fabrication plan contains a duplicate expected net";
            return false;
        }
    }

    std::set<NETLIST_NODE> expectedNoConnects;

    for( const JSON& endpoint : aPlan.at( "expectedNetlistNoConnects" ) )
    {
        if( !endpoint.is_object() || !endpoint.contains( "reference" )
            || !endpoint["reference"].is_string() || !endpoint.contains( "pin" )
            || !endpoint["pin"].is_string()
            || !expectedNoConnects.emplace(
                    endpoint["reference"].get<std::string>(),
                    endpoint["pin"].get<std::string>() ).second )
        {
            aError = "fabrication plan contains an invalid expected no-connect endpoint";
            return false;
        }
    }

    if( actualNoConnects != expectedNoConnects )
    {
        for( const NETLIST_NODE& endpoint : expectedNoConnects )
        {
            if( !actualNoConnects.contains( endpoint ) )
            {
                aError = "KiCad netlist is missing KDS no-connect endpoint "
                         + endpoint.first + "." + endpoint.second;
                return false;
            }
        }

        const NETLIST_NODE& endpoint = *std::find_if(
                actualNoConnects.begin(), actualNoConnects.end(),
                [&]( const NETLIST_NODE& aEndpoint )
                {
                    return !expectedNoConnects.contains( aEndpoint );
                } );
        aError = "KiCad netlist contains unexpected no-connect endpoint "
                 + endpoint.first + "." + endpoint.second;
        return false;
    }

    if( actualNets != expectedNets )
    {
        for( const auto& [name, nodes] : expectedNets )
        {
            auto actual = actualNets.find( name );

            if( actual == actualNets.end() )
            {
                aError = "KiCad netlist is missing KDS net '" + name + "'";
                return false;
            }

            for( const NETLIST_NODE& endpoint : nodes )
            {
                if( !actual->second.contains( endpoint ) )
                {
                    aError = "KiCad netlist net '" + name + "' is missing KDS endpoint "
                             + endpoint.first + "." + endpoint.second;
                    return false;
                }
            }

            for( const NETLIST_NODE& endpoint : actual->second )
            {
                if( !nodes.contains( endpoint ) )
                {
                    aError = "KiCad netlist net '" + name
                             + "' contains unexpected endpoint " + endpoint.first + "."
                             + endpoint.second;
                    return false;
                }
            }
        }

        const auto unexpected = std::find_if(
                actualNets.begin(), actualNets.end(),
                [&]( const auto& aNet ) { return !expectedNets.contains( aNet.first ); } );
        aError = "KiCad netlist contains unexpected connected net '"
                 + unexpected->first + "'";
        return false;
    }

    return true;
}
