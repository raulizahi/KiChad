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

#include "block_diagram.h"
#include "svg_raster.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <type_traits>

#include <base_units.h>
#include <filesystem>
#include <kiid.h>
#include <drawing_sheet/ds_painter.h>
#include <font/font.h>
#include <gal/color4d.h>
#include <page_info.h>
#include <plotters/plotters_pslike.h>
#include <math/vector2d.h>
#include <stroke_params.h>

using KIGFX::COLOR4D;


namespace
{

using namespace KICHAD::BLOCK_DIAGRAM;

// ---------------------------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------------------------

std::string trim( std::string_view aText )
{
    size_t begin = 0;
    size_t end = aText.size();

    while( begin < end && std::isspace( static_cast<unsigned char>( aText[begin] ) ) )
        ++begin;

    while( end > begin && std::isspace( static_cast<unsigned char>( aText[end - 1] ) ) )
        --end;

    return std::string( aText.substr( begin, end - begin ) );
}


std::string lower( std::string aText )
{
    for( char& c : aText )
        c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );

    return aText;
}


bool startsWithNoCase( std::string_view aText, std::string_view aPrefix )
{
    if( aText.size() < aPrefix.size() )
        return false;

    for( size_t i = 0; i < aPrefix.size(); ++i )
    {
        if( std::tolower( static_cast<unsigned char>( aText[i] ) )
            != std::tolower( static_cast<unsigned char>( aPrefix[i] ) ) )
            return false;
    }

    return true;
}


void appendUtf8( std::string& aOut, unsigned long aCodePoint )
{
    if( aCodePoint < 0x80 )
    {
        aOut.push_back( static_cast<char>( aCodePoint ) );
    }
    else if( aCodePoint < 0x800 )
    {
        aOut.push_back( static_cast<char>( 0xC0 | ( aCodePoint >> 6 ) ) );
        aOut.push_back( static_cast<char>( 0x80 | ( aCodePoint & 0x3F ) ) );
    }
    else if( aCodePoint < 0x10000 )
    {
        aOut.push_back( static_cast<char>( 0xE0 | ( aCodePoint >> 12 ) ) );
        aOut.push_back( static_cast<char>( 0x80 | ( ( aCodePoint >> 6 ) & 0x3F ) ) );
        aOut.push_back( static_cast<char>( 0x80 | ( aCodePoint & 0x3F ) ) );
    }
    else if( aCodePoint < 0x110000 )
    {
        aOut.push_back( static_cast<char>( 0xF0 | ( aCodePoint >> 18 ) ) );
        aOut.push_back( static_cast<char>( 0x80 | ( ( aCodePoint >> 12 ) & 0x3F ) ) );
        aOut.push_back( static_cast<char>( 0x80 | ( ( aCodePoint >> 6 ) & 0x3F ) ) );
        aOut.push_back( static_cast<char>( 0x80 | ( aCodePoint & 0x3F ) ) );
    }
}


/**
 * Turn a Mermaid label into bounded display lines: honour <br> breaks and literal "\n",
 * drop other markup tags, decode the common entity forms, and trim.
 */
std::vector<std::string> labelLines( std::string_view aRaw )
{
    std::string text;
    text.reserve( aRaw.size() );

    for( size_t i = 0; i < aRaw.size(); )
    {
        if( aRaw[i] == '<' )
        {
            const size_t close = aRaw.find( '>', i );

            if( close == std::string_view::npos )
            {
                text.push_back( '<' );
                ++i;
                continue;
            }

            const std::string tag = lower( trim( aRaw.substr( i + 1, close - i - 1 ) ) );

            if( tag == "br" || tag == "br/" || tag == "br /" )
                text.push_back( '\n' );

            i = close + 1;
            continue;
        }

        if( aRaw[i] == '\\' && i + 1 < aRaw.size() && aRaw[i + 1] == 'n' )
        {
            text.push_back( '\n' );
            i += 2;
            continue;
        }

        if( aRaw[i] == '#' || aRaw[i] == '&' )
        {
            const size_t semi = aRaw.find( ';', i );

            if( semi != std::string_view::npos && semi - i <= 10 )
            {
                const std::string entity = lower( std::string( aRaw.substr( i + 1, semi - i - 1 ) ) );
                bool handled = true;

                if( entity == "quot" )
                    text.push_back( '"' );
                else if( entity == "amp" )
                    text.push_back( '&' );
                else if( entity == "lt" )
                    text.push_back( '<' );
                else if( entity == "gt" )
                    text.push_back( '>' );
                else if( entity == "nbsp" )
                    text.push_back( ' ' );
                else if( !entity.empty() && entity[0] == '#'
                         && std::all_of( entity.begin() + 1, entity.end(),
                                         []( char c ) { return std::isdigit( (unsigned char) c ); } ) )
                    appendUtf8( text, std::stoul( entity.substr( 1 ) ) );
                else if( std::all_of( entity.begin(), entity.end(),
                                      []( char c ) { return std::isdigit( (unsigned char) c ); } )
                         && !entity.empty() )
                    appendUtf8( text, std::stoul( entity ) );
                else
                    handled = false;

                if( handled )
                {
                    i = semi + 1;
                    continue;
                }
            }
        }

        if( aRaw[i] == '`' || aRaw[i] == '*' )
        {
            ++i;
            continue;
        }

        text.push_back( aRaw[i] );
        ++i;
    }

    std::vector<std::string> lines;
    std::stringstream        stream( text );
    std::string              line;

    while( std::getline( stream, line ) )
    {
        line = trim( line );

        if( line.size() > MAX_LINE_CHARS )
            line = line.substr( 0, MAX_LINE_CHARS - 1 ) + "\xE2\x80\xA6";

        lines.push_back( line );

        if( lines.size() >= MAX_TEXT_LINES )
            break;
    }

    while( !lines.empty() && lines.back().empty() )
        lines.pop_back();

    return lines;
}


std::optional<std::string> parseColor( std::string aValue )
{
    aValue = lower( trim( aValue ) );

    if( aValue.empty() )
        return std::nullopt;

    if( aValue[0] == '#' )
    {
        const std::string hex = aValue.substr( 1 );

        if( !std::all_of( hex.begin(), hex.end(),
                          []( char c ) { return std::isxdigit( (unsigned char) c ); } ) )
            return std::nullopt;

        if( hex.size() == 3 || hex.size() == 4 )
        {
            std::string expanded = "#";

            for( size_t i = 0; i < 3; ++i )
            {
                expanded.push_back( hex[i] );
                expanded.push_back( hex[i] );
            }

            return expanded;
        }

        if( hex.size() == 6 || hex.size() == 8 )
            return "#" + hex.substr( 0, 6 );

        return std::nullopt;
    }

    static const std::map<std::string, std::string> named = {
        { "white", "#ffffff" },       { "black", "#000000" },     { "red", "#e53935" },
        { "green", "#43a047" },       { "blue", "#1e88e5" },      { "yellow", "#fdd835" },
        { "orange", "#fb8c00" },      { "purple", "#8e24aa" },    { "pink", "#ec407a" },
        { "gray", "#9e9e9e" },        { "grey", "#9e9e9e" },      { "lightgray", "#e0e0e0" },
        { "lightgrey", "#e0e0e0" },   { "darkgray", "#616161" },  { "darkgrey", "#616161" },
        { "lightblue", "#bbdefb" },   { "lightgreen", "#c8e6c9" }, { "lightyellow", "#fff9c4" },
        { "cyan", "#00acc1" },        { "magenta", "#d81b60" },   { "brown", "#6d4c41" },
        { "navy", "#1a237e" },        { "teal", "#00897b" },      { "maroon", "#880e4f" },
        { "olive", "#827717" },       { "silver", "#c0c0c0" },    { "gold", "#ffca28" },
        { "beige", "#f5f5dc" },       { "ivory", "#fffff0" },     { "lavender", "#e6e6fa" },
        { "salmon", "#ff8a65" },      { "coral", "#ff7043" },     { "tan", "#d2b48c" },
        { "khaki", "#f0e68c" },       { "lime", "#c0ca33" },      { "indigo", "#3949ab" },
        { "violet", "#7e57c2" },      { "transparent", "#ffffff" }
    };

    const auto it = named.find( aValue );

    if( it != named.end() )
        return it->second;

    return std::nullopt;
}


/** Parse "fill:#fff,stroke:#333,color:#000,stroke-width:2px" into a STYLE. */
STYLE parseStyle( const std::string& aSpec )
{
    STYLE             style;
    std::stringstream stream( aSpec );
    std::string       item;

    while( std::getline( stream, item, ',' ) )
    {
        const size_t colon = item.find( ':' );

        if( colon == std::string::npos )
            continue;

        const std::string key = lower( trim( item.substr( 0, colon ) ) );
        const std::string value = trim( item.substr( colon + 1 ) );

        if( key == "fill" )
            style.fill = parseColor( value );
        else if( key == "stroke" )
            style.stroke = parseColor( value );
        else if( key == "color" )
            style.text = parseColor( value );
    }

    return style;
}


void mergeStyle( STYLE& aTarget, const STYLE& aSource )
{
    if( aSource.fill )
        aTarget.fill = aSource.fill;

    if( aSource.stroke )
        aTarget.stroke = aSource.stroke;

    if( aSource.text )
        aTarget.text = aSource.text;
}


// ---------------------------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------------------------

class PARSER
{
public:
    PARSER( DIAGRAM& aDiagram ) : m_diagram( aDiagram ) {}

    bool Parse( const std::string& aSource, std::string& aError )
    {
        std::vector<std::string> lines;
        std::stringstream        stream( aSource );
        std::string              line;

        while( std::getline( stream, line ) )
        {
            if( !line.empty() && line.back() == '\r' )
                line.pop_back();

            lines.push_back( line );
        }

        // Optional YAML front matter and fenced code markers are tolerated so a pasted
        // Markdown block works unchanged.
        size_t index = 0;
        bool   headerSeen = false;

        for( ; index < lines.size(); ++index )
        {
            m_line = index + 1;
            std::string text = stripComment( lines[index] );
            text = trim( text );

            if( text.empty() )
                continue;

            if( text.rfind( "```", 0 ) == 0 )
                continue;

            if( !headerSeen && text == "---" )
            {
                ++index;

                while( index < lines.size() && trim( lines[index] ) != "---" )
                    ++index;

                continue;
            }

            if( text.rfind( "%%{", 0 ) == 0 )
                continue;

            if( !headerSeen )
            {
                if( !parseHeader( text, aError ) )
                    return false;

                headerSeen = true;
                continue;
            }

            // Multiple statements may share a line separated by ';'.
            std::vector<std::string> statements = splitStatements( text );

            for( const std::string& statement : statements )
            {
                if( !parseStatement( statement, aError ) )
                    return false;
            }
        }

        if( !headerSeen )
        {
            aError = "expected a 'flowchart' or 'graph' header";
            return false;
        }

        if( !m_groupStack.empty() )
        {
            aError = "subgraph '" + m_groupStack.back() + "' is missing its 'end'";
            return false;
        }

        if( m_diagram.nodes.empty() )
        {
            aError = "the flowchart declares no nodes";
            return false;
        }

        applyClasses();
        return true;
    }

private:
    std::string stripComment( const std::string& aLine ) const
    {
        bool inQuote = false;

        for( size_t i = 0; i + 1 < aLine.size(); ++i )
        {
            if( aLine[i] == '"' )
                inQuote = !inQuote;
            else if( !inQuote && aLine[i] == '%' && aLine[i + 1] == '%' )
            {
                // Keep init directives for the caller to skip as a whole.
                if( i + 2 < aLine.size() && aLine[i + 2] == '{' )
                    return aLine;

                return aLine.substr( 0, i );
            }
        }

        return aLine;
    }

    std::vector<std::string> splitStatements( const std::string& aText ) const
    {
        std::vector<std::string> statements;
        std::string              current;
        bool                     inQuote = false;

        for( const char c : aText )
        {
            if( c == '"' )
                inQuote = !inQuote;

            if( c == ';' && !inQuote )
            {
                statements.push_back( trim( current ) );
                current.clear();
                continue;
            }

            current.push_back( c );
        }

        if( !trim( current ).empty() )
            statements.push_back( trim( current ) );

        return statements;
    }

    bool parseHeader( const std::string& aText, std::string& aError )
    {
        std::string rest;

        if( startsWithNoCase( aText, "flowchart" ) )
            rest = trim( aText.substr( 9 ) );
        else if( startsWithNoCase( aText, "graph" ) )
            rest = trim( aText.substr( 5 ) );
        else
        {
            aError = lineError( "expected a 'flowchart' or 'graph' header before '" + aText
                                + "'" );
            return false;
        }

        if( !rest.empty() && rest.back() == ';' )
            rest.pop_back();

        const std::string direction = rest.empty() ? "TB" : rest;

        if( direction == "TB" || direction == "TD" )
            m_diagram.direction = "TB";
        else if( direction == "BT" || direction == "LR" || direction == "RL" )
            m_diagram.direction = direction;
        else
        {
            aError = lineError( "unsupported flowchart direction '" + direction + "'" );
            return false;
        }

        return true;
    }

    std::string lineError( const std::string& aMessage ) const
    {
        return "line " + std::to_string( m_line ) + ": " + aMessage;
    }

    bool parseStatement( const std::string& aText, std::string& aError )
    {
        if( startsWithNoCase( aText, "subgraph" )
            && ( aText.size() == 8 || std::isspace( (unsigned char) aText[8] ) ) )
            return parseSubgraph( trim( aText.substr( 8 ) ), aError );

        if( aText == "end" )
        {
            if( m_groupStack.empty() )
            {
                aError = lineError( "'end' without an open subgraph" );
                return false;
            }

            m_groupStack.pop_back();
            return true;
        }

        if( startsWithNoCase( aText, "classDef " ) )
            return parseClassDef( trim( aText.substr( 9 ) ), aError );

        if( startsWithNoCase( aText, "class " ) )
            return parseClassAssignment( trim( aText.substr( 6 ) ), aError );

        if( startsWithNoCase( aText, "style " ) )
            return parseStyleStatement( trim( aText.substr( 6 ) ), aError );

        if( startsWithNoCase( aText, "linkStyle " ) || startsWithNoCase( aText, "click " )
            || startsWithNoCase( aText, "direction " )
            || startsWithNoCase( aText, "accTitle" ) || startsWithNoCase( aText, "accDescr" ) )
            return true;

        return parseLinkChain( aText, aError );
    }

    bool parseSubgraph( const std::string& aRest, std::string& aError )
    {
        if( m_diagram.groups.size() >= MAX_GROUPS )
        {
            aError = lineError( "too many subgraphs" );
            return false;
        }

        std::string id;
        std::string title;
        const size_t bracket = aRest.find( '[' );

        if( bracket != std::string::npos && aRest.back() == ']' )
        {
            id = trim( aRest.substr( 0, bracket ) );
            title = trim( aRest.substr( bracket + 1, aRest.size() - bracket - 2 ) );
        }
        else
        {
            id = aRest;
            title = aRest;
        }

        if( title.size() >= 2 && title.front() == '"' && title.back() == '"' )
            title = title.substr( 1, title.size() - 2 );

        if( id.size() >= 2 && id.front() == '"' && id.back() == '"' )
            id = id.substr( 1, id.size() - 2 );

        if( id.empty() )
        {
            aError = lineError( "subgraph requires an identifier or title" );
            return false;
        }

        if( m_diagram.FindGroup( id ) )
        {
            aError = lineError( "subgraph '" + id + "' is declared twice" );
            return false;
        }

        GROUP group;
        group.id = id;
        group.lines = labelLines( title );
        group.parent = m_groupStack.empty() ? std::string() : m_groupStack.back();
        m_diagram.groups.push_back( std::move( group ) );
        m_groupStack.push_back( id );
        return true;
    }

    bool parseClassDef( const std::string& aRest, std::string& aError )
    {
        const size_t space = aRest.find_first_of( " \t" );

        if( space == std::string::npos )
        {
            aError = lineError( "classDef requires a name and a style list" );
            return false;
        }

        const std::string names = aRest.substr( 0, space );
        const STYLE       style = parseStyle( trim( aRest.substr( space + 1 ) ) );
        std::stringstream stream( names );
        std::string       name;

        while( std::getline( stream, name, ',' ) )
        {
            name = trim( name );

            if( !name.empty() )
                mergeStyle( m_diagram.classDefs[name], style );
        }

        return true;
    }

    bool parseClassAssignment( const std::string& aRest, std::string& aError )
    {
        const size_t space = aRest.find_last_of( " \t" );

        if( space == std::string::npos )
        {
            aError = lineError( "class requires node ids and a class name" );
            return false;
        }

        const std::string className = trim( aRest.substr( space + 1 ) );
        std::stringstream stream( aRest.substr( 0, space ) );
        std::string       id;

        while( std::getline( stream, id, ',' ) )
        {
            id = trim( id );

            if( id.empty() )
                continue;

            NODE& node = nodeFor( id );
            node.classes.push_back( className );
        }

        return true;
    }

    bool parseStyleStatement( const std::string& aRest, std::string& aError )
    {
        const size_t space = aRest.find_first_of( " \t" );

        if( space == std::string::npos )
        {
            aError = lineError( "style requires a node id and a style list" );
            return false;
        }

        const std::string id = trim( aRest.substr( 0, space ) );
        const STYLE       style = parseStyle( trim( aRest.substr( space + 1 ) ) );

        if( GROUP* group = findGroup( id ) )
            mergeStyle( group->style, style );
        else
            mergeStyle( nodeFor( id ).style, style );

        return true;
    }

    GROUP* findGroup( const std::string& aId )
    {
        for( GROUP& group : m_diagram.groups )
        {
            if( group.id == aId )
                return &group;
        }

        return nullptr;
    }

    NODE& nodeFor( const std::string& aId )
    {
        for( NODE& node : m_diagram.nodes )
        {
            if( node.id == aId )
                return node;
        }

        NODE node;
        node.id = aId;
        node.lines = { aId };
        node.group = m_groupStack.empty() ? std::string() : m_groupStack.back();
        m_diagram.nodes.push_back( std::move( node ) );
        return m_diagram.nodes.back();
    }

    // -- statement cursor helpers -------------------------------------------------------

    void skipSpace()
    {
        while( m_pos < m_text.size() && std::isspace( (unsigned char) m_text[m_pos] ) )
            ++m_pos;
    }

    bool atEnd() const { return m_pos >= m_text.size(); }

    bool lookingAt( std::string_view aToken ) const
    {
        return m_text.compare( m_pos, aToken.size(), aToken ) == 0;
    }

    static bool isIdChar( char c )
    {
        return std::isalnum( (unsigned char) c ) || c == '_' || c == '.'
               || static_cast<unsigned char>( c ) >= 0x80;
    }

    std::string readId()
    {
        const size_t start = m_pos;

        while( m_pos < m_text.size() )
        {
            const char c = m_text[m_pos];

            if( isIdChar( c ) )
            {
                ++m_pos;
                continue;
            }

            // A dash is part of an identifier only when it joins two identifier characters.
            if( c == '-' && m_pos > start && m_pos + 1 < m_text.size()
                && isIdChar( m_text[m_pos + 1] ) )
            {
                ++m_pos;
                continue;
            }

            break;
        }

        return m_text.substr( start, m_pos - start );
    }

    bool parseLinkChain( const std::string& aText, std::string& aError )
    {
        m_text = aText;
        m_pos = 0;

        std::vector<std::string> previous;

        if( !parseNodeGroup( previous, aError ) )
            return false;

        skipSpace();

        while( !atEnd() )
        {
            LINK link;

            if( !parseLink( link, aError ) )
                return false;

            std::vector<std::string> next;

            if( !parseNodeGroup( next, aError ) )
                return false;

            for( const std::string& from : previous )
            {
                for( const std::string& to : next )
                {
                    if( m_diagram.links.size() >= MAX_LINKS )
                    {
                        aError = lineError( "too many links" );
                        return false;
                    }

                    LINK copy = link;
                    copy.from = from;
                    copy.to = to;
                    m_diagram.links.push_back( std::move( copy ) );
                }
            }

            previous = std::move( next );
            skipSpace();
        }

        return true;
    }

    bool parseNodeGroup( std::vector<std::string>& aIds, std::string& aError )
    {
        if( !parseNode( aIds, aError ) )
            return false;

        skipSpace();

        while( !atEnd() && m_text[m_pos] == '&' )
        {
            ++m_pos;
            skipSpace();

            if( !parseNode( aIds, aError ) )
                return false;

            skipSpace();
        }

        return true;
    }

    bool parseNode( std::vector<std::string>& aIds, std::string& aError )
    {
        skipSpace();
        const std::string id = readId();

        if( id.empty() )
        {
            aError = lineError( "expected a node identifier at column "
                                + std::to_string( m_pos + 1 ) + " in '" + m_text + "'" );
            return false;
        }

        if( m_diagram.nodes.size() >= MAX_NODES && !m_diagram.FindNode( id ) )
        {
            aError = lineError( "too many nodes" );
            return false;
        }

        NODE& node = nodeFor( id );

        if( !m_groupStack.empty() )
            node.group = m_groupStack.back();

        // Optional shape with label.
        static const std::vector<std::pair<std::string, NODE_SHAPE>> openers = {
            { "(((", NODE_SHAPE::CIRCLE },      { "([", NODE_SHAPE::STADIUM },
            { "[[", NODE_SHAPE::SUBROUTINE },   { "[(", NODE_SHAPE::CYLINDER },
            { "((", NODE_SHAPE::CIRCLE },       { "{{", NODE_SHAPE::HEXAGON },
            { "[/", NODE_SHAPE::PARALLELOGRAM }, { "[\\", NODE_SHAPE::TRAPEZOID },
            { "(", NODE_SHAPE::ROUNDED },       { "[", NODE_SHAPE::RECTANGLE },
            { "{", NODE_SHAPE::DIAMOND },       { ">", NODE_SHAPE::ASYMMETRIC }
        };
        static const std::map<std::string, std::vector<std::string>> closers = {
            { "(((", { ")))" } }, { "([", { "])" } },  { "[[", { "]]" } },
            { "[(", { ")]" } },   { "((", { "))" } },  { "{{", { "}}" } },
            { "[/", { "/]", "\\]" } }, { "[\\", { "\\]", "/]" } },
            { "(", { ")" } },     { "[", { "]" } },    { "{", { "}" } },
            { ">", { "]" } }
        };

        for( const auto& [opener, shape] : openers )
        {
            if( !lookingAt( opener ) )
                continue;

            m_pos += opener.size();
            std::string raw;
            skipSpace();

            const std::vector<std::string>& ends = closers.at( opener );

            if( !atEnd() && m_text[m_pos] == '"' )
            {
                const size_t close = m_text.find( '"', m_pos + 1 );

                if( close == std::string::npos )
                {
                    aError = lineError( "unterminated quoted label for node '" + id + "'" );
                    return false;
                }

                raw = m_text.substr( m_pos + 1, close - m_pos - 1 );
                m_pos = close + 1;
                skipSpace();

                bool closed = false;

                for( const std::string& end : ends )
                {
                    if( lookingAt( end ) )
                    {
                        m_pos += end.size();
                        closed = true;
                        break;
                    }
                }

                if( !closed )
                {
                    aError = lineError( "expected '" + ends.front() + "' after the label of node '"
                                        + id + "'" );
                    return false;
                }
            }
            else
            {
                size_t      best = std::string::npos;
                std::string bestEnd;

                for( const std::string& end : ends )
                {
                    const size_t found = m_text.find( end, m_pos );

                    if( found != std::string::npos && found < best )
                    {
                        best = found;
                        bestEnd = end;
                    }
                }

                if( best == std::string::npos )
                {
                    aError = lineError( "expected '" + ends.front() + "' after the label of node '"
                                        + id + "'" );
                    return false;
                }

                raw = m_text.substr( m_pos, best - m_pos );
                m_pos = best + bestEnd.size();
            }

            // A trailing shape character may remain for the mixed parallelogram forms.
            if( shape == NODE_SHAPE::PARALLELOGRAM && raw.size() && raw.back() == '\\' )
                raw.pop_back();

            std::vector<std::string> lines = labelLines( raw );

            if( lines.empty() )
                lines = { id };

            node.lines = std::move( lines );
            node.shape = shape;
            break;
        }

        if( lookingAt( ":::" ) )
        {
            m_pos += 3;
            const std::string className = readId();

            if( className.empty() )
            {
                aError = lineError( "expected a class name after ':::' on node '" + id + "'" );
                return false;
            }

            node.classes.push_back( className );
        }

        aIds.push_back( id );
        return true;
    }

    bool parseLink( LINK& aLink, std::string& aError )
    {
        skipSpace();

        if( !atEnd() && m_text[m_pos] == '<'
            && m_pos + 1 < m_text.size()
            && ( m_text[m_pos + 1] == '-' || m_text[m_pos + 1] == '=' || m_text[m_pos + 1] == '.' ) )
        {
            aLink.arrowFrom = true;
            ++m_pos;
        }

        const size_t runStart = m_pos;

        while( !atEnd() && ( m_text[m_pos] == '-' || m_text[m_pos] == '=' || m_text[m_pos] == '.' ) )
            ++m_pos;

        const std::string run = m_text.substr( runStart, m_pos - runStart );

        if( run.size() < 2 )
        {
            aError = lineError( "expected a link such as '-->' at column "
                                + std::to_string( runStart + 1 ) + " in '" + m_text + "'" );
            return false;
        }

        if( run.find( '=' ) != std::string::npos )
            aLink.style = LINK_STYLE::THICK;
        else if( run.find( '.' ) != std::string::npos )
            aLink.style = LINK_STYLE::DOTTED;
        else
            aLink.style = LINK_STYLE::SOLID;

        aLink.arrowTo = false;

        if( !atEnd() && m_text[m_pos] == '>' )
        {
            aLink.arrowTo = true;
            ++m_pos;
        }
        else if( !atEnd() && ( m_text[m_pos] == 'o' || m_text[m_pos] == 'x' )
                 && ( m_pos + 1 >= m_text.size() || std::isspace( (unsigned char) m_text[m_pos + 1] ) ) )
        {
            aLink.arrowTo = true;
            ++m_pos;
        }

        // Two-character stubs without an arrowhead introduce inline link text:
        //   A -- text --> B      A -. text .-> B      A == text ==> B
        if( !aLink.arrowTo && ( run == "--" || run == "==" || run == "-." ) )
        {
            static const std::vector<std::string> terminators = {
                "-->", "---", "==>", "===", ".->", ".-"
            };
            size_t      best = std::string::npos;
            std::string bestEnd;

            for( const std::string& end : terminators )
            {
                const size_t found = m_text.find( end, m_pos );

                if( found != std::string::npos && found < best )
                {
                    best = found;
                    bestEnd = end;
                }
            }

            if( best == std::string::npos )
            {
                aError = lineError( "link text after '" + run + "' is not closed in '" + m_text
                                    + "'" );
                return false;
            }

            std::string label = trim( m_text.substr( m_pos, best - m_pos ) );

            if( label.size() >= 2 && label.front() == '"' && label.back() == '"' )
                label = label.substr( 1, label.size() - 2 );

            aLink.lines = labelLines( label );
            m_pos = best + bestEnd.size();
            aLink.arrowTo = bestEnd.back() == '>';
        }

        skipSpace();

        if( !atEnd() && m_text[m_pos] == '|' )
        {
            const size_t close = m_text.find( '|', m_pos + 1 );

            if( close == std::string::npos )
            {
                aError = lineError( "link label is missing its closing '|' in '" + m_text + "'" );
                return false;
            }

            std::string label = trim( m_text.substr( m_pos + 1, close - m_pos - 1 ) );

            if( label.size() >= 2 && label.front() == '"' && label.back() == '"' )
                label = label.substr( 1, label.size() - 2 );

            aLink.lines = labelLines( label );
            m_pos = close + 1;
            skipSpace();
        }

        return true;
    }

    void applyClasses()
    {
        const auto defaults = m_diagram.classDefs.find( "default" );

        for( NODE& node : m_diagram.nodes )
        {
            STYLE resolved;

            if( defaults != m_diagram.classDefs.end() )
                mergeStyle( resolved, defaults->second );

            for( const std::string& className : node.classes )
            {
                const auto it = m_diagram.classDefs.find( className );

                if( it != m_diagram.classDefs.end() )
                    mergeStyle( resolved, it->second );
            }

            mergeStyle( resolved, node.style );
            node.style = resolved;
        }
    }

    DIAGRAM&                 m_diagram;
    std::vector<std::string> m_groupStack;
    std::string              m_text;
    size_t                   m_pos = 0;
    size_t                   m_line = 0;
};


// ---------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------

constexpr double TEXT_MM = 2.8;          ///< node label glyph height
constexpr double LINE_PITCH_MM = 4.4;    ///< baseline pitch for multi-line labels
constexpr double LABEL_TEXT_MM = 2.4;    ///< link label glyph height
constexpr double TITLE_TEXT_MM = 3.2;    ///< subgraph title glyph height
constexpr double NODE_PAD_X_MM = 6.0;
constexpr double NODE_PAD_Y_MM = 4.0;
constexpr double NODE_MIN_W_MM = 30.0;
constexpr double NODE_MIN_H_MM = 12.0;
constexpr double NODE_GAP_MM = 14.0;     ///< gap between neighbours in one layer
constexpr double LAYER_GAP_MM = 20.0;    ///< gap between layers
constexpr double BAND_GAP_MM = 18.0;     ///< gap between subgraph bands
constexpr double GROUP_PAD_MM = 6.0;
constexpr double GROUP_TITLE_MM = 8.0;
constexpr double MARGIN_MM = 12.0;
constexpr double CHANNEL_PITCH_MM = 7.0;   ///< spacing between parallel link channels in a gap
constexpr double PEN_MM = 0.25;


double textWidthMm( const std::string& aText, double aGlyphMm, bool aBold )
{
    KIFONT::FONT* font = KIFONT::FONT::GetFont();
    const int     size = KiROUND( aGlyphMm * schIUScale.IU_PER_MM );
    const int     thickness = KiROUND( ( aBold ? 0.25 : 0.15 ) * aGlyphMm * schIUScale.IU_PER_MM );
    const VECTOR2I limits = font->StringBoundaryLimits( wxString::FromUTF8( aText ),
                                                        VECTOR2I( size, size ), thickness,
                                                        aBold, false,
                                                        KIFONT::METRICS::Default() );
    return limits.x / schIUScale.IU_PER_MM;
}


double maxLineWidthMm( const std::vector<std::string>& aLines, double aGlyphMm, bool aBold )
{
    double width = 0.0;

    for( const std::string& line : aLines )
        width = std::max( width, textWidthMm( line, aGlyphMm, aBold ) );

    return width;
}


void measureNode( NODE& aNode )
{
    const double textWidth = maxLineWidthMm( aNode.lines, TEXT_MM, false );
    const double textHeight = std::max<size_t>( aNode.lines.size(), 1 ) * LINE_PITCH_MM;
    double       w = std::max( NODE_MIN_W_MM, textWidth + 2 * NODE_PAD_X_MM );
    double       h = std::max( NODE_MIN_H_MM, textHeight + 2 * NODE_PAD_Y_MM );

    switch( aNode.shape )
    {
    case NODE_SHAPE::DIAMOND:
        w = ( textWidth + 2 * NODE_PAD_X_MM ) * 1.7;
        h = ( textHeight + 2 * NODE_PAD_Y_MM ) * 1.7;
        w = std::max( w, NODE_MIN_W_MM );
        h = std::max( h, NODE_MIN_H_MM * 1.5 );
        break;
    case NODE_SHAPE::HEXAGON:
        w += h;
        break;
    case NODE_SHAPE::CIRCLE:
        w = h = std::max( w, h ) * 1.05;
        break;
    case NODE_SHAPE::STADIUM:
        w += h * 0.6;
        break;
    case NODE_SHAPE::ASYMMETRIC:
        w += 6.0;
        break;
    case NODE_SHAPE::PARALLELOGRAM:
    case NODE_SHAPE::TRAPEZOID:
        w += h * 0.7;
        break;
    case NODE_SHAPE::CYLINDER:
        h += 7.0;
        break;
    case NODE_SHAPE::SUBROUTINE:
        w += 8.0;
        break;
    default:
        break;
    }

    aNode.width = w;
    aNode.height = h;
}


struct AXES
{
    bool horizontal;   ///< LR/RL: layers progress along x

    double sizeP( const NODE& n ) const { return horizontal ? n.width : n.height; }
    double sizeS( const NODE& n ) const { return horizontal ? n.height : n.width; }
};


class LAYOUT
{
public:
    explicit LAYOUT( DIAGRAM& aDiagram ) :
            m_d( aDiagram ),
            m_axes{ aDiagram.direction == "LR" || aDiagram.direction == "RL" }
    {
    }

    void Run()
    {
        for( NODE& node : m_d.nodes )
            measureNode( node );

        buildIndex();
        assignRanks();
        orderLayers();
        placeNodes();
        applyDirection();
        placeGroups();
        routeLinks();
        finish();
    }

private:
    void buildIndex()
    {
        for( size_t i = 0; i < m_d.nodes.size(); ++i )
            m_index[m_d.nodes[i].id] = i;

        m_out.assign( m_d.nodes.size(), {} );
        m_in.assign( m_d.nodes.size(), {} );

        for( const LINK& link : m_d.links )
        {
            const size_t a = m_index.at( link.from );
            const size_t b = m_index.at( link.to );

            if( a == b )
                continue;

            m_out[a].push_back( b );
            m_in[b].push_back( a );
        }
    }

    void assignRanks()
    {
        const size_t n = m_d.nodes.size();
        std::vector<int> state( n, 0 ); // 0 new, 1 on stack, 2 done
        std::set<std::pair<size_t, size_t>> backEdges;

        // Iterative DFS in declaration order marks back edges so cycles do not defeat ranking.
        for( size_t root = 0; root < n; ++root )
        {
            if( state[root] != 0 )
                continue;

            std::vector<std::pair<size_t, size_t>> stack = { { root, 0 } };
            state[root] = 1;

            while( !stack.empty() )
            {
                auto& [node, next] = stack.back();

                if( next < m_out[node].size() )
                {
                    const size_t child = m_out[node][next++];

                    if( state[child] == 0 )
                    {
                        state[child] = 1;
                        stack.push_back( { child, 0 } );
                    }
                    else if( state[child] == 1 )
                    {
                        backEdges.insert( { node, child } );
                    }
                }
                else
                {
                    state[node] = 2;
                    stack.pop_back();
                }
            }
        }

        std::vector<size_t> indegree( n, 0 );

        for( size_t a = 0; a < n; ++a )
        {
            for( const size_t b : m_out[a] )
            {
                if( !backEdges.count( { a, b } ) )
                    ++indegree[b];
            }
        }

        std::vector<size_t> queue;

        for( size_t i = 0; i < n; ++i )
        {
            m_d.nodes[i].rank = 0;

            if( indegree[i] == 0 )
                queue.push_back( i );
        }

        for( size_t head = 0; head < queue.size(); ++head )
        {
            const size_t a = queue[head];

            for( const size_t b : m_out[a] )
            {
                if( backEdges.count( { a, b } ) )
                    continue;

                m_d.nodes[b].rank = std::max( m_d.nodes[b].rank, m_d.nodes[a].rank + 1 );

                if( --indegree[b] == 0 )
                    queue.push_back( b );
            }
        }

        int maxRank = 0;

        for( const NODE& node : m_d.nodes )
            maxRank = std::max( maxRank, node.rank );

        m_layers.assign( static_cast<size_t>( maxRank ) + 1, {} );

        for( size_t i = 0; i < n; ++i )
            m_layers[m_d.nodes[i].rank].push_back( i );
    }

    void orderLayers()
    {
        std::vector<double> position( m_d.nodes.size(), 0.0 );

        const auto refreshPositions = [&]()
        {
            for( const auto& layer : m_layers )
            {
                for( size_t i = 0; i < layer.size(); ++i )
                    position[layer[i]] = static_cast<double>( i );
            }
        };

        const auto sweep = [&]( bool aDownward )
        {
            for( size_t li = 0; li < m_layers.size(); ++li )
            {
                const size_t layerIndex = aDownward ? li : m_layers.size() - 1 - li;
                auto&        layer = m_layers[layerIndex];
                std::vector<std::pair<double, size_t>> keyed;

                for( const size_t node : layer )
                {
                    const auto& neighbours = aDownward ? m_in[node] : m_out[node];
                    double      sum = 0.0;
                    size_t      count = 0;

                    for( const size_t other : neighbours )
                    {
                        const int otherRank = m_d.nodes[other].rank;

                        if( ( aDownward && otherRank < static_cast<int>( layerIndex ) )
                            || ( !aDownward && otherRank > static_cast<int>( layerIndex ) ) )
                        {
                            sum += position[other];
                            ++count;
                        }
                    }

                    keyed.push_back( { count ? sum / count : position[node], node } );
                }

                std::stable_sort( keyed.begin(), keyed.end(),
                                  []( const auto& a, const auto& b ) { return a.first < b.first; } );

                for( size_t i = 0; i < layer.size(); ++i )
                    layer[i] = keyed[i].second;

                refreshPositions();
            }
        };

        refreshPositions();

        for( int iteration = 0; iteration < 6; ++iteration )
        {
            sweep( true );
            sweep( false );
        }

    }

    int nestingDepth( const std::string& aGroup ) const
    {
        int         depth = 0;
        std::string current = aGroup;

        for( int guard = 0; guard < 64 && !current.empty(); ++guard )
        {
            ++depth;
            const GROUP* group = m_d.FindGroup( current );

            if( !group )
                break;

            current = group->parent;
        }

        return depth;
    }

    struct CONTAINER
    {
        std::string              id;
        std::vector<std::string> children;
        std::vector<size_t>      direct;
        size_t                   firstIndex = SIZE_MAX;
        double                   width = 0.0;
        double                   directWidth = 0.0;
        double                   start = 0.0;
        double                   directStart = 0.0;
    };

    std::map<std::string, CONTAINER> m_containers;

    void buildContainers()
    {
        m_containers.clear();
        m_containers[""].id = "";

        for( const GROUP& group : m_d.groups )
            m_containers[group.id].id = group.id;

        for( const GROUP& group : m_d.groups )
        {
            const std::string parent = m_d.FindGroup( group.parent ) ? group.parent : "";
            m_containers[parent].children.push_back( group.id );
        }

        for( size_t i = 0; i < m_d.nodes.size(); ++i )
        {
            const std::string group = m_d.FindGroup( m_d.nodes[i].group ) ? m_d.nodes[i].group : "";
            m_containers[group].direct.push_back( i );
        }

        std::function<size_t( CONTAINER& )> firstIndex = [&]( CONTAINER& c ) -> size_t
        {
            for( const size_t index : c.direct )
                c.firstIndex = std::min( c.firstIndex, index );

            for( const std::string& child : c.children )
                c.firstIndex = std::min( c.firstIndex, firstIndex( m_containers[child] ) );

            return c.firstIndex;
        };
        firstIndex( m_containers[""] );

        // Widths along the secondary axis: the direct nodes of a container share one
        // sub-band sized by their widest layer row; every child subgraph is its own
        // sub-band, so a subgraph box can never overlap a node outside it.
        std::function<double( CONTAINER& )> width = [&]( CONTAINER& c ) -> double
        {
            std::map<int, double> rowWidth;
            std::map<int, int>    rowCount;

            for( const size_t index : c.direct )
            {
                const int rank = m_d.nodes[index].rank;
                rowWidth[rank] += m_axes.sizeS( m_d.nodes[index] );
                ++rowCount[rank];
            }

            c.directWidth = 0.0;

            for( auto& [rank, w] : rowWidth )
                c.directWidth = std::max( c.directWidth, w + NODE_GAP_MM * ( rowCount[rank] - 1 ) );

            double total = 0.0;
            int    elements = 0;

            if( c.directWidth > 0.0 )
            {
                total += c.directWidth;
                ++elements;
            }

            for( const std::string& child : c.children )
            {
                const double childWidth = width( m_containers[child] );

                if( childWidth > 0.0 )
                {
                    total += childWidth;
                    ++elements;
                }
            }

            if( elements == 0 )
                return c.width = 0.0;

            total += BAND_GAP_MM * ( elements - 1 );

            if( !c.id.empty() )
                total += 2 * GROUP_PAD_MM;

            return c.width = total;
        };
        width( m_containers[""] );

        std::function<void( CONTAINER&, double )> place = [&]( CONTAINER& c, double aStart )
        {
            c.start = aStart;
            double cursor = aStart + ( c.id.empty() ? 0.0 : GROUP_PAD_MM );

            // Sub-bands in first-appearance order so the drawing follows the source.
            std::vector<std::pair<size_t, std::string>> elements;

            if( c.directWidth > 0.0 )
            {
                size_t first = SIZE_MAX;

                for( const size_t index : c.direct )
                    first = std::min( first, index );

                elements.push_back( { first, "" } );
            }

            for( const std::string& child : c.children )
            {
                if( m_containers[child].width > 0.0 )
                    elements.push_back( { m_containers[child].firstIndex, child } );
            }

            std::stable_sort( elements.begin(), elements.end(),
                              []( const auto& a, const auto& b ) { return a.first < b.first; } );

            for( const auto& [first, id] : elements )
            {
                if( id.empty() )
                {
                    c.directStart = cursor;
                    cursor += c.directWidth + BAND_GAP_MM;
                }
                else
                {
                    place( m_containers[id], cursor );
                    cursor += m_containers[id].width + BAND_GAP_MM;
                }
            }
        };
        place( m_containers[""], MARGIN_MM );
    }

    void placeNodes()
    {
        buildContainers();

        // Links that need a routing channel in the gap before their target layer.
        std::vector<int> channels( m_layers.size(), 0 );

        for( const LINK& link : m_d.links )
        {
            const NODE& a = m_d.nodes[m_index.at( link.from )];
            const NODE& b = m_d.nodes[m_index.at( link.to )];

            if( b.rank > a.rank )
                ++channels[b.rank];
        }

        m_layerStartP.assign( m_layers.size(), 0.0 );
        m_layerEndP.assign( m_layers.size(), 0.0 );

        double p = MARGIN_MM;

        for( size_t layerIndex = 0; layerIndex < m_layers.size(); ++layerIndex )
        {
            const auto& layer = m_layers[layerIndex];
            double      layerSize = 0.0;
            int         depth = 0;

            for( const size_t index : layer )
            {
                layerSize = std::max( layerSize, m_axes.sizeP( m_d.nodes[index] ) );
                depth = std::max( depth, nestingDepth( m_d.nodes[index].group ) );
            }

            if( layerIndex > 0 )
            {
                const double gap = std::max( LAYER_GAP_MM, CHANNEL_PITCH_MM * ( channels[layerIndex] + 1 ) )
                                   + depth * ( GROUP_PAD_MM + GROUP_TITLE_MM ) * 0.5;
                p += gap;
            }

            m_layerStartP[layerIndex] = p;
            m_layerEndP[layerIndex] = p + layerSize;
            const double centreP = p + layerSize / 2.0;

            // Rows per container, in the layer's barycenter order.
            std::map<std::string, std::vector<size_t>> rows;

            for( const size_t index : layer )
            {
                const std::string group =
                        m_d.FindGroup( m_d.nodes[index].group ) ? m_d.nodes[index].group : "";
                rows[group].push_back( index );
            }

            for( auto& [group, members] : rows )
            {
                const CONTAINER& container = m_containers[group];
                double           rowWidth = NODE_GAP_MM * ( members.size() - 1 );

                for( const size_t index : members )
                    rowWidth += m_axes.sizeS( m_d.nodes[index] );

                double cursor = container.directStart + ( container.directWidth - rowWidth ) / 2.0;

                for( const size_t index : members )
                {
                    NODE&        node = m_d.nodes[index];
                    const double sizeS = m_axes.sizeS( node );
                    const double centreS = cursor + sizeS / 2.0;
                    cursor += sizeS + NODE_GAP_MM;

                    if( m_axes.horizontal )
                    {
                        node.x = centreP;
                        node.y = centreS;
                    }
                    else
                    {
                        node.x = centreS;
                        node.y = centreP;
                    }
                }
            }

            p += layerSize;
        }

        m_extentP = p + MARGIN_MM;
    }

    void applyDirection()
    {
        if( m_d.direction == "BT" )
        {
            for( NODE& node : m_d.nodes )
                node.y = m_extentP - node.y;
        }
        else if( m_d.direction == "RL" )
        {
            for( NODE& node : m_d.nodes )
                node.x = m_extentP - node.x;
        }
    }

    struct BOX
    {
        double left = 1e9, top = 1e9, right = -1e9, bottom = -1e9;
        bool   valid = false;

        void add( double l, double t, double r, double b )
        {
            left = std::min( left, l );
            top = std::min( top, t );
            right = std::max( right, r );
            bottom = std::max( bottom, b );
            valid = true;
        }
    };

    BOX groupBox( const std::string& aGroup, int aGuard = 0 )
    {
        BOX box;

        if( aGuard > 64 )
            return box;

        for( const NODE& node : m_d.nodes )
        {
            if( node.group == aGroup )
                box.add( node.x - node.width / 2, node.y - node.height / 2,
                         node.x + node.width / 2, node.y + node.height / 2 );
        }

        for( const GROUP& child : m_d.groups )
        {
            if( child.parent == aGroup && child.id != aGroup )
            {
                const BOX childBox = groupBox( child.id, aGuard + 1 );

                if( childBox.valid )
                    box.add( childBox.left, childBox.top, childBox.right, childBox.bottom );
            }
        }

        if( box.valid )
        {
            box.left -= GROUP_PAD_MM;
            box.right += GROUP_PAD_MM;
            box.bottom += GROUP_PAD_MM;
            box.top -= GROUP_PAD_MM + GROUP_TITLE_MM;
        }

        return box;
    }

    void placeGroups()
    {
        for( GROUP& group : m_d.groups )
        {
            const BOX box = groupBox( group.id );

            if( !box.valid )
            {
                group.width = group.height = 0.0;
                continue;
            }

            // Make sure the title fits.
            const double titleWidth = maxLineWidthMm( group.lines, TITLE_TEXT_MM, true )
                                      + 2 * GROUP_PAD_MM;
            group.x = box.left;
            group.y = box.top;
            group.width = std::max( box.right - box.left, titleWidth );
            group.height = box.bottom - box.top;
        }
    }

    static std::pair<double, double> clipToRect( const NODE& aNode, double aTowardX,
                                                 double aTowardY )
    {
        const double dx = aTowardX - aNode.x;
        const double dy = aTowardY - aNode.y;

        if( std::abs( dx ) < 1e-6 && std::abs( dy ) < 1e-6 )
            return { aNode.x, aNode.y };

        const double hw = aNode.width / 2.0;
        const double hh = aNode.height / 2.0;
        double       t = 1e9;

        if( std::abs( dx ) > 1e-6 )
            t = std::min( t, hw / std::abs( dx ) );

        if( std::abs( dy ) > 1e-6 )
            t = std::min( t, hh / std::abs( dy ) );

        return { aNode.x + dx * t, aNode.y + dy * t };
    }

    /// Evenly spread the links entering layer aTargetRank across that layer's leading gap.
    double channelFor( const LINK& aLink, int aTargetRank )
    {
        std::vector<std::pair<std::pair<double, double>, const LINK*>> keyed;

        for( const LINK& other : m_d.links )
        {
            const NODE& oa = m_d.nodes[m_index.at( other.from )];
            const NODE& ob = m_d.nodes[m_index.at( other.to )];

            if( ob.rank != aTargetRank || oa.rank >= ob.rank )
                continue;

            const double sA = m_axes.horizontal ? oa.y : oa.x;
            const double sB = m_axes.horizontal ? ob.y : ob.x;

            if( std::abs( sA - sB ) < 0.5 && ob.rank == oa.rank + 1 )
                continue;

            keyed.push_back( { { sA, sB }, &other } );
        }

        std::stable_sort( keyed.begin(), keyed.end(),
                          []( const auto& x, const auto& y ) { return x.first < y.first; } );

        size_t slot = 0;

        for( size_t i = 0; i < keyed.size(); ++i )
        {
            if( keyed[i].second == &aLink )
                slot = i;
        }

        const double gapStart = m_layerEndP[aTargetRank - 1];
        const double gapEnd = m_layerStartP[aTargetRank];
        double       channel = gapStart + ( gapEnd - gapStart ) * ( slot + 1 ) / ( keyed.size() + 1 );

        if( m_d.direction == "BT" || m_d.direction == "RL" )
            channel = m_extentP - channel;

        return channel;
    }

    void routeLinks()
    {
        const bool horizontal = m_axes.horizontal;
        const bool reversed = m_d.direction == "BT" || m_d.direction == "RL";

        for( LINK& link : m_d.links )
        {
            const NODE& a = m_d.nodes[m_index.at( link.from )];
            const NODE& b = m_d.nodes[m_index.at( link.to )];
            link.path.clear();

            if( &a == &b )
            {
                // Self loop on the trailing side.
                const double r = a.x + a.width / 2.0;
                const double loop = 8.0;
                link.path = { { r, a.y - 3.0 }, { r + loop, a.y - 3.0 }, { r + loop, a.y + 3.0 },
                              { r, a.y + 3.0 } };
                continue;
            }

            const auto primary = [&]( const NODE& n ) { return horizontal ? n.x : n.y; };
            const auto secondary = [&]( const NODE& n ) { return horizontal ? n.y : n.x; };
            const auto halfP = [&]( const NODE& n ) { return m_axes.sizeP( n ) / 2.0; };
            const auto point = [&]( double p, double s ) -> std::pair<double, double>
            {
                return horizontal ? std::make_pair( p, s ) : std::make_pair( s, p );
            };

            const double sign = reversed ? -1.0 : 1.0;

            if( b.rank > a.rank )
            {
                const double startP = primary( a ) + sign * halfP( a );
                const double endP = primary( b ) - sign * halfP( b );
                const double sA = secondary( a );
                const double sB = secondary( b );

                if( std::abs( sA - sB ) < 0.5 && b.rank == a.rank + 1 )
                {
                    link.path = { point( startP, sA ), point( endP, sB ) };
                }
                else
                {
                    // Channel in the gap just before the target layer; the run along the
                    // source column passes earlier gaps without entering the nodes there.
                    const double channelP = channelFor( link, b.rank );
                    link.path = { point( startP, sA ), point( channelP, sA ),
                                  point( channelP, sB ), point( endP, sB ) };
                }
            }
            else if( b.rank == a.rank )
            {
                const auto start = clipToRect( a, b.x, b.y );
                const auto end = clipToRect( b, a.x, a.y );
                link.path = { start, end };
            }
            else
            {
                // Back edge: leave the source on its trailing side, run a channel beside
                // the wider of the two nodes, and enter the target from that side.
                const double channelS = std::max( secondary( a ) + m_axes.sizeS( a ) / 2.0,
                                                  secondary( b ) + m_axes.sizeS( b ) / 2.0 )
                                        + 7.0;
                const double startP = primary( a );
                const double endP = primary( b );
                link.path = { point( startP, secondary( a ) + m_axes.sizeS( a ) / 2.0 ),
                              point( startP, channelS ), point( endP, channelS ),
                              point( endP, secondary( b ) + m_axes.sizeS( b ) / 2.0 ) };
            }
        }
    }

    void finish()
    {
        BOX box;

        for( const NODE& node : m_d.nodes )
            box.add( node.x - node.width / 2, node.y - node.height / 2, node.x + node.width / 2,
                     node.y + node.height / 2 );

        for( const GROUP& group : m_d.groups )
        {
            if( group.width > 0 )
                box.add( group.x, group.y, group.x + group.width, group.y + group.height );
        }

        for( const LINK& link : m_d.links )
        {
            for( const auto& [x, y] : link.path )
                box.add( x - 2, y - 2, x + 2, y + 2 );
        }

        if( !box.valid )
            return;

        const double dx = MARGIN_MM - box.left;
        const double dy = MARGIN_MM - box.top;

        for( NODE& node : m_d.nodes )
        {
            node.x += dx;
            node.y += dy;
        }

        for( GROUP& group : m_d.groups )
        {
            group.x += dx;
            group.y += dy;
        }

        for( LINK& link : m_d.links )
        {
            for( auto& [x, y] : link.path )
            {
                x += dx;
                y += dy;
            }
        }

        m_d.width = box.right - box.left + 2 * MARGIN_MM;
        m_d.height = box.bottom - box.top + 2 * MARGIN_MM;
    }

    DIAGRAM&                         m_d;
    AXES                             m_axes;
    std::map<std::string, size_t>    m_index;
    std::vector<std::vector<size_t>> m_out;
    std::vector<std::vector<size_t>> m_in;
    std::vector<std::vector<size_t>> m_layers;
    std::vector<double>              m_layerStartP;
    std::vector<double>              m_layerEndP;
    double                           m_extentP = 0.0;
};


// ---------------------------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------------------------

COLOR4D colorFrom( const std::optional<std::string>& aHex, const COLOR4D& aDefault )
{
    if( !aHex || aHex->size() != 7 )
        return aDefault;

    const auto channel = [&]( size_t aOffset )
    {
        return std::stoul( aHex->substr( aOffset, 2 ), nullptr, 16 ) / 255.0;
    };

    return COLOR4D( channel( 1 ), channel( 3 ), channel( 5 ), 1.0 );
}


class RENDERER
{
public:
    RENDERER( const DIAGRAM& aDiagram, PLOTTER& aPlotter, double aTitleOffset ) :
            m_d( aDiagram ),
            m_plotter( aPlotter ),
            m_dy( aTitleOffset ),
            m_font( KIFONT::FONT::GetFont() )
    {
    }

    void Draw( const wxString& aTitle )
    {
        if( !aTitle.IsEmpty() )
        {
            m_plotter.Text( iu( MARGIN_MM, MARGIN_MM / 2.0 + 1.0, false ), COLOR4D( 0.1, 0.1, 0.12, 1 ),
                            aTitle, ANGLE_0, VECTOR2I( mm( 4.0 ), mm( 4.0 ) ),
                            GR_TEXT_H_ALIGN_LEFT, GR_TEXT_V_ALIGN_CENTER, mm( 0.7 ), false, true,
                            false, m_font, KIFONT::METRICS::Default() );
        }

        // Outer groups first so nested boxes paint on top.
        std::vector<const GROUP*> groups;

        for( const GROUP& group : m_d.groups )
        {
            if( group.width > 0 )
                groups.push_back( &group );
        }

        std::stable_sort( groups.begin(), groups.end(),
                          [&]( const GROUP* a, const GROUP* b )
                          { return depth( a->id ) < depth( b->id ); } );

        for( const GROUP* group : groups )
            drawGroup( *group );

        for( const LINK& link : m_d.links )
            drawLinkPath( link );

        for( const NODE& node : m_d.nodes )
            drawNode( node );

        for( const LINK& link : m_d.links )
            drawLinkLabel( link );
    }

private:
    int depth( const std::string& aGroup ) const
    {
        int         d = 0;
        std::string current = aGroup;

        for( int guard = 0; guard < 64 && !current.empty(); ++guard )
        {
            ++d;
            const GROUP* group = m_d.FindGroup( current );

            if( !group )
                break;

            current = group->parent;
        }

        return d;
    }

    static int mm( double aMm ) { return KiROUND( aMm * schIUScale.IU_PER_MM ); }

    VECTOR2I iu( double aX, double aY, bool aShift = true ) const
    {
        return VECTOR2I( mm( aX ), mm( aY + ( aShift ? m_dy : 0.0 ) ) );
    }

    void polygon( const std::vector<std::pair<double, double>>& aPoints, const COLOR4D& aFill,
                  const COLOR4D& aStroke, double aPenMm )
    {
        std::vector<VECTOR2I> corners;

        for( const auto& [x, y] : aPoints )
            corners.push_back( iu( x, y ) );

        m_plotter.SetColor( aFill );
        m_plotter.PlotPoly( corners, FILL_T::FILLED_SHAPE, 0, nullptr );
        m_plotter.SetColor( aStroke );
        m_plotter.PlotPoly( corners, FILL_T::NO_FILL, mm( aPenMm ), nullptr );
    }

    void polyline( const std::vector<std::pair<double, double>>& aPoints, const COLOR4D& aColor,
                   double aPenMm, LINE_STYLE aStyle )
    {
        if( aPoints.size() < 2 )
            return;

        m_plotter.SetColor( aColor );
        m_plotter.SetCurrentLineWidth( mm( aPenMm ) );
        m_plotter.SetDash( mm( aPenMm ), aStyle );
        m_plotter.MoveTo( iu( aPoints.front().first, aPoints.front().second ) );

        for( size_t i = 1; i + 1 < aPoints.size(); ++i )
            m_plotter.LineTo( iu( aPoints[i].first, aPoints[i].second ) );

        m_plotter.FinishTo( iu( aPoints.back().first, aPoints.back().second ) );
        m_plotter.SetDash( mm( aPenMm ), LINE_STYLE::SOLID );
    }

    static void arcPoints( std::vector<std::pair<double, double>>& aOut, double aCx, double aCy,
                           double aRx, double aRy, double aFromDeg, double aToDeg, int aSteps = 8 )
    {
        for( int i = 0; i <= aSteps; ++i )
        {
            const double t = aFromDeg + ( aToDeg - aFromDeg ) * i / aSteps;
            const double rad = t * M_PI / 180.0;
            aOut.push_back( { aCx + aRx * std::cos( rad ), aCy + aRy * std::sin( rad ) } );
        }
    }

    static std::vector<std::pair<double, double>> roundedRect( double l, double t, double r,
                                                               double b, double aRadius )
    {
        std::vector<std::pair<double, double>> pts;
        const double rad = std::min( aRadius, std::min( r - l, b - t ) / 2.0 );
        arcPoints( pts, r - rad, t + rad, rad, rad, -90, 0 );
        arcPoints( pts, r - rad, b - rad, rad, rad, 0, 90 );
        arcPoints( pts, l + rad, b - rad, rad, rad, 90, 180 );
        arcPoints( pts, l + rad, t + rad, rad, rad, 180, 270 );
        return pts;
    }

    void drawText( const std::vector<std::string>& aLines, double aCx, double aCy, double aGlyphMm,
                   const COLOR4D& aColor, bool aBold, GR_TEXT_H_ALIGN_T aAlign = GR_TEXT_H_ALIGN_CENTER )
    {
        const double pitch = aGlyphMm * LINE_PITCH_MM / TEXT_MM;
        const double top = aCy - ( static_cast<double>( aLines.size() ) - 1.0 ) * pitch / 2.0;

        for( size_t i = 0; i < aLines.size(); ++i )
        {
            if( aLines[i].empty() )
                continue;

            m_plotter.Text( iu( aCx, top + i * pitch ), aColor, wxString::FromUTF8( aLines[i] ),
                            ANGLE_0, VECTOR2I( mm( aGlyphMm ), mm( aGlyphMm ) ), aAlign,
                            GR_TEXT_V_ALIGN_CENTER, mm( ( aBold ? 0.22 : 0.15 ) * aGlyphMm ),
                            false, aBold, false, m_font, KIFONT::METRICS::Default() );
        }
    }

    void drawGroup( const GROUP& aGroup )
    {
        const COLOR4D fill = colorFrom( aGroup.style.fill, COLOR4D( 0.965, 0.972, 0.98, 1 ) );
        const COLOR4D stroke = colorFrom( aGroup.style.stroke, COLOR4D( 0.55, 0.6, 0.68, 1 ) );
        const COLOR4D text = colorFrom( aGroup.style.text, COLOR4D( 0.2, 0.24, 0.3, 1 ) );
        const double  l = aGroup.x;
        const double  t = aGroup.y;
        const double  r = aGroup.x + aGroup.width;
        const double  b = aGroup.y + aGroup.height;

        polygon( roundedRect( l, t, r, b, 2.5 ), fill, stroke, PEN_MM );
        drawText( aGroup.lines, l + GROUP_PAD_MM, t + GROUP_TITLE_MM / 2.0 + 1.0, TITLE_TEXT_MM,
                  text, true, GR_TEXT_H_ALIGN_LEFT );
    }

    void drawNode( const NODE& aNode )
    {
        const COLOR4D fill = colorFrom( aNode.style.fill, COLOR4D( 0.93, 0.95, 0.985, 1 ) );
        const COLOR4D stroke = colorFrom( aNode.style.stroke, COLOR4D( 0.25, 0.3, 0.4, 1 ) );
        const COLOR4D text = colorFrom( aNode.style.text, COLOR4D( 0.08, 0.08, 0.1, 1 ) );
        const double  l = aNode.x - aNode.width / 2.0;
        const double  t = aNode.y - aNode.height / 2.0;
        const double  r = aNode.x + aNode.width / 2.0;
        const double  b = aNode.y + aNode.height / 2.0;
        const double  cx = aNode.x;
        const double  cy = aNode.y;
        const double  w = aNode.width;
        const double  h = aNode.height;

        switch( aNode.shape )
        {
        case NODE_SHAPE::ROUNDED:
            polygon( roundedRect( l, t, r, b, 3.0 ), fill, stroke, PEN_MM * 1.4 );
            break;

        case NODE_SHAPE::STADIUM:
            polygon( roundedRect( l, t, r, b, h / 2.0 ), fill, stroke, PEN_MM * 1.4 );
            break;

        case NODE_SHAPE::CIRCLE:
            m_plotter.SetColor( fill );
            m_plotter.Circle( iu( cx, cy ), mm( w ), FILL_T::FILLED_SHAPE, 0 );
            m_plotter.SetColor( stroke );
            m_plotter.Circle( iu( cx, cy ), mm( w ), FILL_T::NO_FILL, mm( PEN_MM * 1.4 ) );
            break;

        case NODE_SHAPE::DIAMOND:
            polygon( { { cx, t }, { r, cy }, { cx, b }, { l, cy } }, fill, stroke, PEN_MM * 1.4 );
            break;

        case NODE_SHAPE::HEXAGON:
        {
            const double inset = h / 2.0;
            polygon( { { l + inset, t }, { r - inset, t }, { r, cy }, { r - inset, b },
                       { l + inset, b }, { l, cy } },
                     fill, stroke, PEN_MM * 1.4 );
            break;
        }

        case NODE_SHAPE::ASYMMETRIC:
        {
            const double inset = 5.0;
            polygon( { { l + inset, t }, { r, t }, { r, b }, { l + inset, b }, { l, cy } }, fill,
                     stroke, PEN_MM * 1.4 );
            break;
        }

        case NODE_SHAPE::PARALLELOGRAM:
        {
            const double skew = h * 0.35;
            polygon( { { l + skew, t }, { r, t }, { r - skew, b }, { l, b } }, fill, stroke,
                     PEN_MM * 1.4 );
            break;
        }

        case NODE_SHAPE::TRAPEZOID:
        {
            const double skew = h * 0.35;
            polygon( { { l + skew, t }, { r - skew, t }, { r, b }, { l, b } }, fill, stroke,
                     PEN_MM * 1.4 );
            break;
        }

        case NODE_SHAPE::CYLINDER:
        {
            const double ry = 3.0;
            std::vector<std::pair<double, double>> body;
            arcPoints( body, cx, t + ry, w / 2.0, ry, 180, 360, 12 );
            arcPoints( body, cx, b - ry, w / 2.0, ry, 0, 180, 12 );
            polygon( body, fill, stroke, PEN_MM * 1.4 );
            std::vector<std::pair<double, double>> lip;
            arcPoints( lip, cx, t + ry, w / 2.0, ry, 0, 180, 12 );
            polyline( lip, stroke, PEN_MM * 1.4, LINE_STYLE::SOLID );
            break;
        }

        case NODE_SHAPE::SUBROUTINE:
            polygon( { { l, t }, { r, t }, { r, b }, { l, b } }, fill, stroke, PEN_MM * 1.4 );
            polyline( { { l + 3.0, t }, { l + 3.0, b } }, stroke, PEN_MM, LINE_STYLE::SOLID );
            polyline( { { r - 3.0, t }, { r - 3.0, b } }, stroke, PEN_MM, LINE_STYLE::SOLID );
            break;

        case NODE_SHAPE::RECTANGLE:
        default:
            polygon( { { l, t }, { r, t }, { r, b }, { l, b } }, fill, stroke, PEN_MM * 1.4 );
            break;
        }

        drawText( aNode.lines, cx, cy, TEXT_MM, text, false );
    }

    void arrowHead( const std::pair<double, double>& aFrom, const std::pair<double, double>& aTo,
                    const COLOR4D& aColor )
    {
        const double dx = aTo.first - aFrom.first;
        const double dy = aTo.second - aFrom.second;
        const double length = std::hypot( dx, dy );

        if( length < 1e-6 )
            return;

        const double ux = dx / length;
        const double uy = dy / length;
        const double size = 3.0;
        const double half = 1.3;
        const std::pair<double, double> base = { aTo.first - ux * size, aTo.second - uy * size };

        polygon( { aTo, { base.first - uy * half, base.second + ux * half },
                   { base.first + uy * half, base.second - ux * half } },
                 aColor, aColor, PEN_MM * 0.6 );
    }

    void drawLinkPath( const LINK& aLink )
    {
        if( aLink.path.size() < 2 )
            return;

        const COLOR4D color( 0.25, 0.3, 0.4, 1 );
        const double  pen = aLink.style == LINK_STYLE::THICK ? PEN_MM * 3.0 : PEN_MM * 1.5;
        const LINE_STYLE style = aLink.style == LINK_STYLE::DOTTED ? LINE_STYLE::DASH
                                                                    : LINE_STYLE::SOLID;
        polyline( aLink.path, color, pen, style );

        if( aLink.arrowTo )
            arrowHead( aLink.path[aLink.path.size() - 2], aLink.path.back(), color );

        if( aLink.arrowFrom )
            arrowHead( aLink.path[1], aLink.path.front(), color );
    }

    void drawLinkLabel( const LINK& aLink )
    {
        if( aLink.lines.empty() || aLink.path.size() < 2 )
            return;

        // Midpoint along the polyline length.
        double total = 0.0;

        for( size_t i = 1; i < aLink.path.size(); ++i )
            total += std::hypot( aLink.path[i].first - aLink.path[i - 1].first,
                                 aLink.path[i].second - aLink.path[i - 1].second );

        double remaining = total / 2.0;
        std::pair<double, double> mid = aLink.path.front();

        for( size_t i = 1; i < aLink.path.size(); ++i )
        {
            const double dx = aLink.path[i].first - aLink.path[i - 1].first;
            const double dy = aLink.path[i].second - aLink.path[i - 1].second;
            const double segment = std::hypot( dx, dy );

            if( segment >= remaining && segment > 1e-9 )
            {
                const double f = remaining / segment;
                mid = { aLink.path[i - 1].first + dx * f, aLink.path[i - 1].second + dy * f };
                break;
            }

            remaining -= segment;
        }

        const double width = maxLineWidthMm( aLink.lines, LABEL_TEXT_MM, false ) + 3.0;
        const double height = aLink.lines.size() * LABEL_TEXT_MM * LINE_PITCH_MM / TEXT_MM + 1.5;
        polygon( { { mid.first - width / 2, mid.second - height / 2 },
                   { mid.first + width / 2, mid.second - height / 2 },
                   { mid.first + width / 2, mid.second + height / 2 },
                   { mid.first - width / 2, mid.second + height / 2 } },
                 COLOR4D( 1, 1, 1, 1 ), COLOR4D( 1, 1, 1, 1 ), PEN_MM * 0.5 );
        drawText( aLink.lines, mid.first, mid.second, LABEL_TEXT_MM, COLOR4D( 0.2, 0.22, 0.28, 1 ),
                  false );
    }

    const DIAGRAM& m_d;
    PLOTTER&       m_plotter;
    double         m_dy;
    KIFONT::FONT*  m_font;
};

} // namespace


namespace KICHAD::BLOCK_DIAGRAM
{

const NODE* DIAGRAM::FindNode( const std::string& aId ) const
{
    for( const NODE& node : nodes )
    {
        if( node.id == aId )
            return &node;
    }

    return nullptr;
}


const GROUP* DIAGRAM::FindGroup( const std::string& aId ) const
{
    for( const GROUP& group : groups )
    {
        if( group.id == aId )
            return &group;
    }

    return nullptr;
}


bool ParseMermaidFlowchart( const std::string& aSource, DIAGRAM& aDiagram, std::string& aError )
{
    if( aSource.empty() )
    {
        aError = "the Mermaid source is empty";
        return false;
    }

    if( aSource.size() > MAX_SOURCE_BYTES )
    {
        aError = "the Mermaid source exceeds 64 KiB";
        return false;
    }

    aDiagram = DIAGRAM();
    PARSER parser( aDiagram );
    return parser.Parse( aSource, aError );
}


void LayoutDiagram( DIAGRAM& aDiagram )
{
    if( aDiagram.nodes.empty() )
        return;

    LAYOUT layout( aDiagram );
    layout.Run();
}


namespace
{

template <typename PLOTTER_T>
bool renderWithPlotter( const DIAGRAM& aDiagram, const wxString& aOutputPath, const wxString& aTitle,
                        const char* aFormat, std::string& aError )
{
    if( aDiagram.nodes.empty() || aDiagram.width <= 0.0 || aDiagram.height <= 0.0 )
    {
        aError = "the diagram has not been laid out";
        return false;
    }

    const double titleOffset = aTitle.IsEmpty() ? 0.0 : 10.0;
    const double pageWidthMm = aDiagram.width;
    const double pageHeightMm = aDiagram.height + titleOffset;

    KIGFX::DS_RENDER_SETTINGS settings;
    settings.SetDefaultPenWidth( KiROUND( PEN_MM * schIUScale.IU_PER_MM ) );
    settings.SetBackgroundColor( COLOR4D( 1, 1, 1, 1 ) );

    PAGE_INFO::SetCustomWidthMils( pageWidthMm * 1000.0 / 25.4 );
    PAGE_INFO::SetCustomHeightMils( pageHeightMm * 1000.0 / 25.4 );
    PAGE_INFO page( PAGE_SIZE_TYPE::User );

    PLOTTER_T plotter;
    plotter.SetRenderSettings( &settings );
    plotter.SetPageSettings( page );
    plotter.SetColorMode( true );
    plotter.SetViewport( VECTOR2I( 0, 0 ), schIUScale.IU_PER_MILS / 10, 1.0, false );
    plotter.SetCreator( wxS( "KiChad" ) );
    plotter.SetTitle( aTitle.IsEmpty() ? wxS( "Block diagram" ) : aTitle );
    plotter.SetSubject( wxS( "KiChad block diagram" ) );

    if( !plotter.OpenFile( aOutputPath ) )
    {
        aError = std::string( "could not create the " ) + aFormat + " document";
        return false;
    }

    bool started;

    if constexpr( std::is_same_v<PLOTTER_T, PDF_PLOTTER> )
        started = plotter.StartPlot( wxString( wxS( "1" ) ), wxEmptyString );
    else
        started = plotter.StartPlot( wxString( wxS( "1" ) ) );

    if( !started )
    {
        aError = std::string( "could not start the " ) + aFormat + " page";
        return false;
    }

    RENDERER renderer( aDiagram, plotter, titleOffset );
    renderer.Draw( aTitle );

    if( !plotter.EndPlot() )
    {
        aError = std::string( "could not finish the " ) + aFormat + " document";
        return false;
    }

    return true;
}

} // namespace


bool RenderDiagramPdf( const DIAGRAM& aDiagram, const wxString& aOutputPath,
                       const wxString& aTitle, std::string& aError )
{
    return renderWithPlotter<PDF_PLOTTER>( aDiagram, aOutputPath, aTitle, "PDF", aError );
}


bool RenderDiagramSvg( const DIAGRAM& aDiagram, const wxString& aOutputPath,
                       const wxString& aTitle, std::string& aError )
{
    return renderWithPlotter<SVG_PLOTTER>( aDiagram, aOutputPath, aTitle, "SVG", aError );
}


bool RenderDiagramPng( const DIAGRAM& aDiagram, const wxString& aOutputPath,
                       const wxString& aTitle, int aMaxDimension, std::string& aError )
{
    const std::filesystem::path svgPath =
            std::filesystem::temp_directory_path()
            / ( "kichad-diagram-" + KIID().AsString().ToStdString() + ".svg" );
    const wxString svgFile = wxString::FromUTF8( svgPath.string() );

    if( !RenderDiagramSvg( aDiagram, svgFile, aTitle, aError ) )
        return false;

    const bool ok = KICHAD::SVG_RASTER::RasterizeSvgFile( wxFileName( svgFile ),
                                                          wxFileName( aOutputPath ),
                                                          aMaxDimension, aError );
    wxRemoveFile( svgFile );
    return ok;
}

} // namespace KICHAD::BLOCK_DIAGRAM
