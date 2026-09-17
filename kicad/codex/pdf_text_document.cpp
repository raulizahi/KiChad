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

#include "pdf_text_document.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <unordered_map>

#include <zlib.h>


namespace KICHAD::PDF
{

namespace
{

constexpr size_t MAX_DECODED_STREAM_BYTES = 48 * 1024 * 1024;
constexpr size_t MAX_OBJECTS = 2000000;
constexpr size_t MAX_PAGE_TEXT_BYTES = 2 * 1024 * 1024;
constexpr size_t MAX_GLYPH_RUNS = 400000;
constexpr int    MAX_FORM_DEPTH = 12;
constexpr size_t MAX_CONTENT_OPERATIONS = 4000000;

// ---------------------------------------------------------------------------------------------
// Object model
// ---------------------------------------------------------------------------------------------

struct OBJ;
using ARRAY = std::vector<OBJ>;
using DICT = std::map<std::string, OBJ>;

struct OBJ
{
    enum KIND
    {
        NUL,
        BOOLEAN,
        NUMBER,
        STRING,
        NAME,
        ARR,
        DIC,
        STREAM,
        REFERENCE,
        OPERATOR
    };

    KIND                   kind = NUL;
    bool                   boolean = false;
    double                 number = 0.0;
    std::string            text;      ///< string bytes, name, or operator
    std::shared_ptr<ARRAY> array;
    std::shared_ptr<DICT>  dict;
    std::shared_ptr<std::string> raw;  ///< undecoded stream bytes
    int                    refNum = 0;
    int                    refGen = 0;

    bool IsNumber() const { return kind == NUMBER; }
    bool IsName( std::string_view aName ) const { return kind == NAME && text == aName; }
    bool IsOperator( std::string_view aOp ) const { return kind == OPERATOR && text == aOp; }
    bool IsDict() const { return kind == DIC || kind == STREAM; }
    int  Int() const { return kind == NUMBER ? static_cast<int>( number ) : 0; }
};


bool isWhitespace( char c )
{
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0';
}


bool isDelimiter( char c )
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{'
           || c == '}' || c == '/' || c == '%';
}


bool isRegular( char c )
{
    return !isWhitespace( c ) && !isDelimiter( c );
}


int hexValue( char c )
{
    if( c >= '0' && c <= '9' )
        return c - '0';

    if( c >= 'a' && c <= 'f' )
        return c - 'a' + 10;

    if( c >= 'A' && c <= 'F' )
        return c - 'A' + 10;

    return -1;
}


/** Tokenizer and recursive-descent object parser over a byte range. */
class LEXER
{
public:
    LEXER( std::string_view aText, size_t aPos = 0 ) : m_text( aText ), m_pos( aPos ) {}

    size_t Pos() const { return m_pos; }
    void   Seek( size_t aPos ) { m_pos = std::min( aPos, m_text.size() ); }
    bool   AtEnd() { skipSpace(); return m_pos >= m_text.size(); }
    std::string_view Text() const { return m_text; }

    void skipSpace()
    {
        while( m_pos < m_text.size() )
        {
            const char c = m_text[m_pos];

            if( isWhitespace( c ) )
                ++m_pos;
            else if( c == '%' )
            {
                while( m_pos < m_text.size() && m_text[m_pos] != '\n' && m_text[m_pos] != '\r' )
                    ++m_pos;
            }
            else
                break;
        }
    }

    /** Parse one object; operators and keywords come back as OPERATOR tokens. */
    bool Next( OBJ& aOut, int aDepth = 0 )
    {
        skipSpace();

        if( m_pos >= m_text.size() || aDepth > 256 )
            return false;

        const char c = m_text[m_pos];

        if( c == '/' )
        {
            ++m_pos;
            aOut = OBJ();
            aOut.kind = OBJ::NAME;
            aOut.text = readName();
            return true;
        }

        if( c == '(' )
        {
            ++m_pos;
            aOut = OBJ();
            aOut.kind = OBJ::STRING;
            aOut.text = readLiteralString();
            return true;
        }

        if( c == '<' )
        {
            if( m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '<' )
            {
                m_pos += 2;
                aOut = OBJ();
                aOut.kind = OBJ::DIC;
                aOut.dict = std::make_shared<DICT>();

                while( true )
                {
                    skipSpace();

                    if( m_pos >= m_text.size() )
                        return true;

                    if( m_text.compare( m_pos, 2, ">>" ) == 0 )
                    {
                        m_pos += 2;
                        return true;
                    }

                    OBJ key;

                    if( !Next( key, aDepth + 1 ) )
                        return true;

                    if( key.kind != OBJ::NAME )
                    {
                        // Tolerate junk: skip until a name or the closing delimiter.
                        if( key.kind == OBJ::OPERATOR && key.text == ">>" )
                            return true;

                        continue;
                    }

                    OBJ value;

                    if( !NextValue( value, aDepth + 1 ) )
                        return true;

                    ( *aOut.dict )[key.text] = std::move( value );
                }
            }

            ++m_pos;
            aOut = OBJ();
            aOut.kind = OBJ::STRING;
            aOut.text = readHexString();
            return true;
        }

        if( c == '[' )
        {
            ++m_pos;
            aOut = OBJ();
            aOut.kind = OBJ::ARR;
            aOut.array = std::make_shared<ARRAY>();

            while( true )
            {
                skipSpace();

                if( m_pos >= m_text.size() )
                    return true;

                if( m_text[m_pos] == ']' )
                {
                    ++m_pos;
                    return true;
                }

                OBJ item;

                if( !NextValue( item, aDepth + 1 ) )
                    return true;

                if( item.kind == OBJ::OPERATOR && ( item.text == "]" || item.text == ">>" ) )
                    return true;

                aOut.array->push_back( std::move( item ) );
            }
        }

        if( c == ']' || c == '>' || c == ')' || c == '{' || c == '}' )
        {
            // Stray delimiter: surface it as an operator token so callers can resynchronize.
            aOut = OBJ();
            aOut.kind = OBJ::OPERATOR;

            if( c == '>' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '>' )
            {
                aOut.text = ">>";
                m_pos += 2;
            }
            else
            {
                aOut.text = std::string( 1, c );
                ++m_pos;
            }

            return true;
        }

        if( ( c >= '0' && c <= '9' ) || c == '+' || c == '-' || c == '.' )
        {
            const size_t start = m_pos;

            while( m_pos < m_text.size() && isRegular( m_text[m_pos] ) )
                ++m_pos;

            const std::string_view token = m_text.substr( start, m_pos - start );
            aOut = OBJ();
            aOut.kind = OBJ::NUMBER;
            aOut.number = parseNumber( token );
            return true;
        }

        const size_t start = m_pos;

        while( m_pos < m_text.size() && isRegular( m_text[m_pos] ) )
            ++m_pos;

        if( m_pos == start )
            ++m_pos;

        aOut = OBJ();
        aOut.kind = OBJ::OPERATOR;
        aOut.text = std::string( m_text.substr( start, m_pos - start ) );

        if( aOut.text == "true" || aOut.text == "false" )
        {
            aOut.kind = OBJ::BOOLEAN;
            aOut.boolean = aOut.text == "true";
        }
        else if( aOut.text == "null" )
        {
            aOut.kind = OBJ::NUL;
        }

        return true;
    }

    /** Like Next(), but folds "num gen R" into a reference. */
    bool NextValue( OBJ& aOut, int aDepth = 0 )
    {
        if( !Next( aOut, aDepth ) )
            return false;

        if( aOut.kind != OBJ::NUMBER || aOut.number < 0 || aOut.number != std::floor( aOut.number ) )
            return true;

        const size_t save = m_pos;
        OBJ          gen;
        OBJ          marker;

        if( Next( gen, aDepth ) && gen.kind == OBJ::NUMBER && Next( marker, aDepth )
            && marker.IsOperator( "R" ) )
        {
            OBJ ref;
            ref.kind = OBJ::REFERENCE;
            ref.refNum = aOut.Int();
            ref.refGen = gen.Int();
            aOut = ref;
            return true;
        }

        m_pos = save;
        return true;
    }

private:
    static double parseNumber( std::string_view aToken )
    {
        // PDF numbers may carry oddities like "--5" or "3.4.5"; take the leading valid part.
        std::string cleaned;
        bool        seenDot = false;
        bool        seenDigit = false;

        for( const char c : aToken )
        {
            if( c >= '0' && c <= '9' )
            {
                cleaned.push_back( c );
                seenDigit = true;
            }
            else if( c == '.' && !seenDot )
            {
                cleaned.push_back( c );
                seenDot = true;
            }
            else if( ( c == '-' || c == '+' ) && !seenDigit && cleaned.find( '-' ) == std::string::npos
                     && cleaned.empty() )
            {
                if( c == '-' )
                    cleaned.push_back( c );
            }
            else if( c == '-' || c == '+' )
            {
                continue;
            }
            else
                break;
        }

        try
        {
            return cleaned.empty() || cleaned == "-" || cleaned == "." || cleaned == "-."
                           ? 0.0
                           : std::stod( cleaned );
        }
        catch( ... )
        {
            return 0.0;
        }
    }

    std::string readName()
    {
        std::string name;

        while( m_pos < m_text.size() && isRegular( m_text[m_pos] ) )
        {
            char c = m_text[m_pos++];

            if( c == '#' && m_pos + 1 < m_text.size() )
            {
                const int high = hexValue( m_text[m_pos] );
                const int low = hexValue( m_text[m_pos + 1] );

                if( high >= 0 && low >= 0 )
                {
                    c = static_cast<char>( high * 16 + low );
                    m_pos += 2;
                }
            }

            name.push_back( c );
        }

        return name;
    }

    std::string readLiteralString()
    {
        std::string out;
        int         depth = 1;

        while( m_pos < m_text.size() && depth > 0 )
        {
            char c = m_text[m_pos++];

            if( c == '\\' )
            {
                if( m_pos >= m_text.size() )
                    break;

                c = m_text[m_pos++];

                switch( c )
                {
                case 'n': out.push_back( '\n' ); break;
                case 'r': out.push_back( '\r' ); break;
                case 't': out.push_back( '\t' ); break;
                case 'b': out.push_back( '\b' ); break;
                case 'f': out.push_back( '\f' ); break;
                case '\r':
                    if( m_pos < m_text.size() && m_text[m_pos] == '\n' )
                        ++m_pos;
                    break;
                case '\n': break;
                default:
                    if( c >= '0' && c <= '7' )
                    {
                        int value = c - '0';

                        for( int i = 0; i < 2 && m_pos < m_text.size() && m_text[m_pos] >= '0'
                                        && m_text[m_pos] <= '7'; ++i )
                            value = value * 8 + ( m_text[m_pos++] - '0' );

                        out.push_back( static_cast<char>( value & 0xFF ) );
                    }
                    else
                        out.push_back( c );
                }

                continue;
            }

            if( c == '(' )
                ++depth;
            else if( c == ')' )
            {
                if( --depth == 0 )
                    break;
            }

            out.push_back( c );
        }

        return out;
    }

    std::string readHexString()
    {
        std::string out;
        int         pending = -1;

        while( m_pos < m_text.size() )
        {
            const char c = m_text[m_pos++];

            if( c == '>' )
                break;

            const int value = hexValue( c );

            if( value < 0 )
                continue;

            if( pending < 0 )
                pending = value;
            else
            {
                out.push_back( static_cast<char>( pending * 16 + value ) );
                pending = -1;
            }
        }

        if( pending >= 0 )
            out.push_back( static_cast<char>( pending * 16 ) );

        return out;
    }

    std::string_view m_text;
    size_t           m_pos;
};


// ---------------------------------------------------------------------------------------------
// Stream filters
// ---------------------------------------------------------------------------------------------

bool inflateBytes( const std::string& aInput, std::string& aOutput )
{
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>( const_cast<char*>( aInput.data() ) );
    stream.avail_in = static_cast<uInt>( aInput.size() );

    // Some producers omit the zlib header; retry raw deflate when the wrapped form fails.
    for( const int windowBits : { MAX_WBITS, -MAX_WBITS } )
    {
        stream.next_in = reinterpret_cast<Bytef*>( const_cast<char*>( aInput.data() ) );
        stream.avail_in = static_cast<uInt>( aInput.size() );
        stream.zalloc = nullptr;
        stream.zfree = nullptr;
        stream.opaque = nullptr;
        aOutput.clear();

        if( inflateInit2( &stream, windowBits ) != Z_OK )
            continue;

        std::array<unsigned char, 64 * 1024> buffer{};
        int status = Z_OK;

        while( status == Z_OK )
        {
            stream.next_out = buffer.data();
            stream.avail_out = static_cast<uInt>( buffer.size() );
            status = inflate( &stream, Z_NO_FLUSH );

            const size_t produced = buffer.size() - stream.avail_out;
            aOutput.append( reinterpret_cast<const char*>( buffer.data() ), produced );

            if( aOutput.size() > MAX_DECODED_STREAM_BYTES )
            {
                inflateEnd( &stream );
                return false;
            }

            if( status == Z_BUF_ERROR && stream.avail_in == 0 )
                break;
        }

        inflateEnd( &stream );

        // Corrupt tails are common in damaged files; keep whatever decoded cleanly.
        if( status == Z_STREAM_END || ( status != Z_OK && !aOutput.empty() ) || status == Z_OK )
            return !aOutput.empty() || aInput.empty();
    }

    return false;
}


std::string applyPredictor( const std::string& aData, int aPredictor, int aColors, int aBpc,
                            int aColumns )
{
    if( aPredictor < 2 || aColors < 1 || aBpc < 1 || aColumns < 1 )
        return aData;

    const size_t bytesPerPixel = std::max<size_t>( 1, ( aColors * aBpc + 7 ) / 8 );
    const size_t rowLength = ( static_cast<size_t>( aColumns ) * aColors * aBpc + 7 ) / 8;

    if( aPredictor == 2 )
    {
        if( aBpc != 8 )
            return aData;

        std::string out = aData;

        for( size_t row = 0; row + rowLength <= out.size(); row += rowLength )
        {
            for( size_t i = bytesPerPixel; i < rowLength; ++i )
                out[row + i] = static_cast<char>( out[row + i] + out[row + i - bytesPerPixel] );
        }

        return out;
    }

    // PNG predictors: each row is prefixed with a filter type byte.
    std::string out;
    std::string previous( rowLength, '\0' );
    size_t      pos = 0;

    while( pos + 1 <= aData.size() )
    {
        const unsigned char filter = static_cast<unsigned char>( aData[pos++] );
        const size_t        available = std::min( rowLength, aData.size() - pos );
        std::string         row( aData.data() + pos, available );
        row.resize( rowLength, '\0' );
        pos += available;

        for( size_t i = 0; i < rowLength; ++i )
        {
            const unsigned char a = i >= bytesPerPixel ? static_cast<unsigned char>( row[i - bytesPerPixel] ) : 0;
            const unsigned char b = static_cast<unsigned char>( previous[i] );
            const unsigned char c = i >= bytesPerPixel ? static_cast<unsigned char>( previous[i - bytesPerPixel] ) : 0;
            unsigned char       x = static_cast<unsigned char>( row[i] );

            switch( filter )
            {
            case 1: x += a; break;
            case 2: x += b; break;
            case 3: x += static_cast<unsigned char>( ( a + b ) / 2 ); break;
            case 4:
            {
                const int p = a + b - c;
                const int pa = std::abs( p - a );
                const int pb = std::abs( p - b );
                const int pc = std::abs( p - c );
                x += ( pa <= pb && pa <= pc ) ? a : ( pb <= pc ? b : c );
                break;
            }
            default: break;
            }

            row[i] = static_cast<char>( x );
        }

        out += row;
        previous = row;

        if( available < rowLength )
            break;
    }

    return out;
}


std::string asciiHexDecode( const std::string& aData )
{
    std::string out;
    int         pending = -1;

    for( const char c : aData )
    {
        if( c == '>' )
            break;

        const int value = hexValue( c );

        if( value < 0 )
            continue;

        if( pending < 0 )
            pending = value;
        else
        {
            out.push_back( static_cast<char>( pending * 16 + value ) );
            pending = -1;
        }
    }

    if( pending >= 0 )
        out.push_back( static_cast<char>( pending * 16 ) );

    return out;
}


std::string ascii85Decode( const std::string& aData )
{
    std::string out;
    uint32_t    tuple = 0;
    int         count = 0;
    size_t      pos = 0;

    if( aData.compare( 0, 2, "<~" ) == 0 )
        pos = 2;

    for( ; pos < aData.size(); ++pos )
    {
        const char c = aData[pos];

        if( isWhitespace( c ) )
            continue;

        if( c == '~' )
            break;

        if( c == 'z' && count == 0 )
        {
            out.append( 4, '\0' );
            continue;
        }

        if( c < '!' || c > 'u' )
            continue;

        tuple = tuple * 85 + static_cast<uint32_t>( c - '!' );

        if( ++count == 5 )
        {
            for( int shift = 24; shift >= 0; shift -= 8 )
                out.push_back( static_cast<char>( ( tuple >> shift ) & 0xFF ) );

            tuple = 0;
            count = 0;
        }
    }

    if( count > 0 )
    {
        for( int i = count; i < 5; ++i )
            tuple = tuple * 85 + 84;

        for( int i = 0, shift = 24; i < count - 1; ++i, shift -= 8 )
            out.push_back( static_cast<char>( ( tuple >> shift ) & 0xFF ) );
    }

    return out;
}


std::string runLengthDecode( const std::string& aData )
{
    std::string out;
    size_t      pos = 0;

    while( pos < aData.size() )
    {
        const unsigned char length = static_cast<unsigned char>( aData[pos++] );

        if( length == 128 )
            break;

        if( length < 128 )
        {
            const size_t n = std::min<size_t>( length + 1, aData.size() - pos );
            out.append( aData, pos, n );
            pos += n;
        }
        else if( pos < aData.size() )
        {
            out.append( 257 - length, aData[pos++] );
        }

        if( out.size() > MAX_DECODED_STREAM_BYTES )
            break;
    }

    return out;
}


std::string lzwDecode( const std::string& aData, bool aEarlyChange )
{
    std::string                        out;
    std::vector<std::string>           table;
    const auto                         resetTable = [&]()
    {
        table.clear();

        for( int i = 0; i < 256; ++i )
            table.push_back( std::string( 1, static_cast<char>( i ) ) );

        table.push_back( std::string() ); // 256 clear
        table.push_back( std::string() ); // 257 eod
    };
    resetTable();

    int         codeLength = 9;
    uint32_t    bitBuffer = 0;
    int         bitCount = 0;
    std::string previous;
    size_t      pos = 0;

    while( true )
    {
        while( bitCount < codeLength && pos < aData.size() )
        {
            bitBuffer = ( bitBuffer << 8 ) | static_cast<unsigned char>( aData[pos++] );
            bitCount += 8;
        }

        if( bitCount < codeLength )
            break;

        const uint32_t code = ( bitBuffer >> ( bitCount - codeLength ) ) & ( ( 1u << codeLength ) - 1 );
        bitCount -= codeLength;

        if( code == 256 )
        {
            resetTable();
            codeLength = 9;
            previous.clear();
            continue;
        }

        if( code == 257 )
            break;

        std::string entry;

        if( code < table.size() )
        {
            entry = table[code];

            if( !previous.empty() )
                table.push_back( previous + entry[0] );
        }
        else if( !previous.empty() )
        {
            entry = previous + previous[0];
            table.push_back( entry );
        }
        else
            break;

        out += entry;
        previous = entry;

        const size_t limit = table.size() + ( aEarlyChange ? 1 : 0 );

        if( limit >= 4096 )
        {
            // Table is full; wait for a clear code.
        }
        else if( limit >= 2048 )
            codeLength = 12;
        else if( limit >= 1024 )
            codeLength = 11;
        else if( limit >= 512 )
            codeLength = 10;

        if( out.size() > MAX_DECODED_STREAM_BYTES )
            break;
    }

    return out;
}


// ---------------------------------------------------------------------------------------------
// UTF-8 / encoding tables
// ---------------------------------------------------------------------------------------------

void appendUtf8( std::string& aOut, uint32_t aCodePoint )
{
    if( aCodePoint < 0x80 )
        aOut.push_back( static_cast<char>( aCodePoint ) );
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


std::string utf16BeToUtf8( const std::string& aBytes )
{
    std::string out;

    for( size_t i = 0; i + 1 < aBytes.size(); i += 2 )
    {
        uint32_t unit = ( static_cast<unsigned char>( aBytes[i] ) << 8 )
                        | static_cast<unsigned char>( aBytes[i + 1] );

        if( unit >= 0xD800 && unit <= 0xDBFF && i + 3 < aBytes.size() )
        {
            const uint32_t low = ( static_cast<unsigned char>( aBytes[i + 2] ) << 8 )
                                 | static_cast<unsigned char>( aBytes[i + 3] );

            if( low >= 0xDC00 && low <= 0xDFFF )
            {
                unit = 0x10000 + ( ( unit - 0xD800 ) << 10 ) + ( low - 0xDC00 );
                i += 2;
            }
        }

        appendUtf8( out, unit );
    }

    if( aBytes.size() == 1 )
        appendUtf8( out, static_cast<unsigned char>( aBytes[0] ) );

    return out;
}


/** Decode a PDF text string (PDFDocEncoding or UTF-16BE with BOM) to UTF-8. */
std::string pdfTextToUtf8( const std::string& aBytes )
{
    if( aBytes.size() >= 2 && static_cast<unsigned char>( aBytes[0] ) == 0xFE
        && static_cast<unsigned char>( aBytes[1] ) == 0xFF )
        return utf16BeToUtf8( aBytes.substr( 2 ) );

    std::string out;

    for( const char c : aBytes )
        appendUtf8( out, static_cast<unsigned char>( c ) );

    return out;
}


// WinAnsi (cp1252) code points for 128..255; 0 means undefined.
const std::array<uint16_t, 128> WIN_ANSI_HIGH = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7, 0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF
};

// MacRoman code points for 128..255.
const std::array<uint16_t, 128> MAC_ROMAN_HIGH = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
};


/** A compact Adobe Glyph List subset covering Latin text, symbols and Greek used by datasheets. */
uint32_t glyphNameToCodePoint( const std::string& aName )
{
    static const std::unordered_map<std::string, uint32_t> table = {
        { "space", 0x20 }, { "exclam", 0x21 }, { "quotedbl", 0x22 }, { "numbersign", 0x23 },
        { "dollar", 0x24 }, { "percent", 0x25 }, { "ampersand", 0x26 }, { "quotesingle", 0x27 },
        { "quoteright", 0x2019 }, { "quoteleft", 0x2018 }, { "parenleft", 0x28 }, { "parenright", 0x29 },
        { "asterisk", 0x2A }, { "plus", 0x2B }, { "comma", 0x2C }, { "hyphen", 0x2D }, { "minus", 0x2212 },
        { "period", 0x2E }, { "slash", 0x2F }, { "zero", 0x30 }, { "one", 0x31 }, { "two", 0x32 },
        { "three", 0x33 }, { "four", 0x34 }, { "five", 0x35 }, { "six", 0x36 }, { "seven", 0x37 },
        { "eight", 0x38 }, { "nine", 0x39 }, { "colon", 0x3A }, { "semicolon", 0x3B }, { "less", 0x3C },
        { "equal", 0x3D }, { "greater", 0x3E }, { "question", 0x3F }, { "at", 0x40 },
        { "bracketleft", 0x5B }, { "backslash", 0x5C }, { "bracketright", 0x5D }, { "asciicircum", 0x5E },
        { "underscore", 0x5F }, { "grave", 0x60 }, { "braceleft", 0x7B }, { "bar", 0x7C },
        { "braceright", 0x7D }, { "asciitilde", 0x7E }, { "bullet", 0x2022 }, { "endash", 0x2013 },
        { "emdash", 0x2014 }, { "ellipsis", 0x2026 }, { "quotedblleft", 0x201C }, { "quotedblright", 0x201D },
        { "quotesinglbase", 0x201A }, { "quotedblbase", 0x201E }, { "dagger", 0x2020 }, { "daggerdbl", 0x2021 },
        { "trademark", 0x2122 }, { "registered", 0xAE }, { "copyright", 0xA9 }, { "degree", 0xB0 },
        { "plusminus", 0xB1 }, { "multiply", 0xD7 }, { "divide", 0xF7 }, { "mu", 0xB5 }, { "micro", 0xB5 },
        { "Omega", 0x3A9 }, { "Omegagreek", 0x3A9 }, { "omega", 0x3C9 }, { "Delta", 0x394 }, { "delta", 0x3B4 },
        { "alpha", 0x3B1 }, { "beta", 0x3B2 }, { "gamma", 0x3B3 }, { "theta", 0x3B8 }, { "lambda", 0x3BB },
        { "pi", 0x3C0 }, { "sigma", 0x3C3 }, { "Sigma", 0x3A3 }, { "tau", 0x3C4 }, { "phi", 0x3C6 },
        { "rho", 0x3C1 }, { "eta", 0x3B7 }, { "epsilon", 0x3B5 }, { "infinity", 0x221E },
        { "lessequal", 0x2264 }, { "greaterequal", 0x2265 }, { "notequal", 0x2260 }, { "approxequal", 0x2248 },
        { "arrowright", 0x2192 }, { "arrowleft", 0x2190 }, { "arrowup", 0x2191 }, { "arrowdown", 0x2193 },
        { "arrowboth", 0x2194 }, { "radical", 0x221A }, { "summation", 0x2211 }, { "partialdiff", 0x2202 },
        { "section", 0xA7 }, { "paragraph", 0xB6 }, { "periodcentered", 0xB7 }, { "middot", 0xB7 },
        { "onehalf", 0xBD }, { "onequarter", 0xBC }, { "threequarters", 0xBE }, { "onesuperior", 0xB9 },
        { "twosuperior", 0xB2 }, { "threesuperior", 0xB3 }, { "fi", 0xFB01 }, { "fl", 0xFB02 },
        { "ff", 0xFB00 }, { "ffi", 0xFB03 }, { "ffl", 0xFB04 }, { "nbspace", 0xA0 }, { "sfthyphen", 0xAD },
        { "guillemotleft", 0xAB }, { "guillemotright", 0xBB }, { "guilsinglleft", 0x2039 },
        { "guilsinglright", 0x203A }, { "cent", 0xA2 }, { "sterling", 0xA3 }, { "yen", 0xA5 }, { "Euro", 0x20AC },
        { "florin", 0x192 }, { "logicalnot", 0xAC }, { "brokenbar", 0xA6 }, { "dieresis", 0xA8 },
        { "macron", 0xAF }, { "acute", 0xB4 }, { "cedilla", 0xB8 }, { "ordfeminine", 0xAA },
        { "ordmasculine", 0xBA }, { "AE", 0xC6 }, { "ae", 0xE6 }, { "Oslash", 0xD8 }, { "oslash", 0xF8 },
        { "germandbls", 0xDF }, { "checkmark", 0x2713 }, { "circlemultiply", 0x2297 }, { "square", 0x25A1 },
        { "filledbox", 0x25A0 }, { "triagup", 0x25B2 }, { "triagdn", 0x25BC }, { "diamond", 0x25CA },
        { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "agrave", 0xE0 }, { "aacute", 0xE1 }, { "acircumflex", 0xE2 },
        { "ecircumflex", 0xEA }, { "ccedilla", 0xE7 }, { "ntilde", 0xF1 }, { "ouml", 0xF6 }, { "uuml", 0xFC 
        }, { "auml", 0xE4 }, { "Auml", 0xC4 }, { "Ouml", 0xD6 }, { "Uuml", 0xDC }, { "udieresis", 0xFC },
        { "odieresis", 0xF6 }, { "adieresis", 0xE4 }, { "Udieresis", 0xDC }, { "Odieresis", 0xD6 },
        { "Adieresis", 0xC4 }, { "Eacute", 0xC9 }, { "iacute", 0xED }, { "oacute", 0xF3 }, { "uacute", 0xFA },
        { "idieresis", 0xEF }, { "atilde", 0xE3 }, { "otilde", 0xF5 }, { "aring", 0xE5 }, { "Aring", 0xC5 }
    };

    const auto it = table.find( aName );

    if( it != table.end() )
        return it->second;

    if( aName.size() == 1 )
        return static_cast<unsigned char>( aName[0] );

    // uniXXXX and uXXXX[XX] forms.
    if( aName.rfind( "uni", 0 ) == 0 && aName.size() >= 7 )
    {
        try
        {
            return static_cast<uint32_t>( std::stoul( aName.substr( 3, 4 ), nullptr, 16 ) );
        }
        catch( ... )
        {
        }
    }

    if( aName.rfind( "u", 0 ) == 0 && aName.size() >= 5 && aName.size() <= 7 )
    {
        try
        {
            return static_cast<uint32_t>( std::stoul( aName.substr( 1 ), nullptr, 16 ) );
        }
        catch( ... )
        {
        }
    }

    // gXX / cidXX / GXX glyph-index names carry no meaning.
    return 0;
}


// ---------------------------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------------------------

struct CODESPACE
{
    int      bytes;
    uint32_t low;
    uint32_t high;
};

struct FONT
{
    bool                                 composite = false;
    std::vector<CODESPACE>               codespaces;   ///< composite fonts; empty means 2 bytes
    std::unordered_map<uint32_t, std::string> toUnicode;
    struct RANGE
    {
        uint32_t    low;
        uint32_t    high;
        uint32_t    dstStart;   ///< first code point (single-value ranges)
        std::string dstPrefix;  ///< UTF-16BE bytes minus the incremented last byte
    };
    std::vector<RANGE>                   toUnicodeRanges;
    bool                                 hasToUnicode = false;
    std::array<std::string, 256>         simpleEncoding; ///< UTF-8 per code for simple fonts
    std::unordered_map<uint32_t, double> widths;        ///< glyph space units (1/1000)
    double                               defaultWidth = 500.0;
    bool                                 type3 = false;
    double                               type3Scale = 0.001;
};


class DOCUMENT
{
public:
    bool Load( std::string aBytes, std::string& aError );

    int PageCount() const { return static_cast<int>( m_pages.size() ); }

    bool PageText( int aPage, std::string& aText, std::string& aError ) const;

    std::string Title() const { return m_title; }

private:
    struct PAGE
    {
        OBJ  dict;       ///< the page dictionary
        DICT inherited;  ///< inherited attributes (Resources, MediaBox, Rotate)
    };

    // Object access ------------------------------------------------------------------
    OBJ resolve( const OBJ& aObject, int aDepth = 0 ) const
    {
        if( aObject.kind != OBJ::REFERENCE || aDepth > 32 )
            return aObject;

        const auto it = m_objects.find( aObject.refNum );

        if( it == m_objects.end() )
            return OBJ();

        return resolve( it->second, aDepth + 1 );
    }

    OBJ get( const OBJ& aDict, const std::string& aKey ) const
    {
        if( !aDict.IsDict() || !aDict.dict )
            return OBJ();

        const auto it = aDict.dict->find( aKey );

        if( it == aDict.dict->end() )
            return OBJ();

        return resolve( it->second );
    }

    double number( const OBJ& aDict, const std::string& aKey, double aDefault ) const
    {
        const OBJ value = get( aDict, aKey );
        return value.IsNumber() ? value.number : aDefault;
    }

    // Streams --------------------------------------------------------------------------
    bool decodeStream( const OBJ& aStream, std::string& aOut ) const;

    // Parsing --------------------------------------------------------------------------
    void scanObjects( std::string_view aText );
    bool parseObjectAt( std::string_view aText, size_t aPos, int aNum, size_t& aEnd );
    void expandObjectStreams();
    void collectTrailers( std::string_view aText );
    bool buildPageList( std::string& aError );
    void walkPages( const OBJ& aNode, DICT aInherited, std::set<int>& aVisited, int aDepth );

    // Fonts ----------------------------------------------------------------------------
    std::shared_ptr<FONT> loadFont( const OBJ& aFontRef ) const;
    void parseToUnicode( const std::string& aCMap, FONT& aFont ) const;
    void parseCodespaces( const std::string& aCMap, FONT& aFont ) const;
    void loadSimpleEncoding( const OBJ& aFont, FONT& aOut ) const;
    void loadWidths( const OBJ& aFont, FONT& aOut ) const;

    std::string                           m_bytes;
    std::unordered_map<int, OBJ>          m_objects;
    OBJ                                   m_root;
    OBJ                                   m_info;
    std::vector<PAGE>                     m_pages;
    std::string                           m_title;
    mutable std::unordered_map<int, std::shared_ptr<FONT>> m_fontCache;
    mutable std::unordered_map<const DICT*, std::shared_ptr<FONT>> m_inlineFontCache;

    friend class INTERPRETER;
};


bool DOCUMENT::decodeStream( const OBJ& aStream, std::string& aOut ) const
{
    if( aStream.kind != OBJ::STREAM || !aStream.raw )
        return false;

    aOut = *aStream.raw;

    std::vector<std::string> filters;
    std::vector<OBJ>         params;
    const OBJ                filter = get( aStream, "Filter" );
    const OBJ                parms = get( aStream, "DecodeParms" );

    if( filter.kind == OBJ::NAME )
    {
        filters.push_back( filter.text );
        params.push_back( parms );
    }
    else if( filter.kind == OBJ::ARR )
    {
        for( size_t i = 0; i < filter.array->size(); ++i )
        {
            const OBJ name = resolve( ( *filter.array )[i] );

            if( name.kind == OBJ::NAME )
                filters.push_back( name.text );

            if( parms.kind == OBJ::ARR && i < parms.array->size() )
                params.push_back( resolve( ( *parms.array )[i] ) );
            else
                params.push_back( parms.kind == OBJ::DIC && filter.array->size() == 1 ? parms : OBJ() );
        }
    }

    for( size_t i = 0; i < filters.size(); ++i )
    {
        const std::string& name = filters[i];
        const OBJ&         parm = params[i];
        std::string        decoded;

        if( name == "FlateDecode" || name == "Fl" )
        {
            if( !inflateBytes( aOut, decoded ) )
                return false;
        }
        else if( name == "LZWDecode" || name == "LZW" )
        {
            decoded = lzwDecode( aOut, number( parm, "EarlyChange", 1 ) != 0 );
        }
        else if( name == "ASCIIHexDecode" || name == "AHx" )
        {
            decoded = asciiHexDecode( aOut );
        }
        else if( name == "ASCII85Decode" || name == "A85" )
        {
            decoded = ascii85Decode( aOut );
        }
        else if( name == "RunLengthDecode" || name == "RL" )
        {
            decoded = runLengthDecode( aOut );
        }
        else if( name == "Crypt" )
        {
            decoded = aOut;
        }
        else
        {
            // Image codecs (DCT, JPX, CCITT, JBIG2) carry no text.
            return false;
        }

        const int predictor = static_cast<int>( number( parm, "Predictor", 1 ) );

        if( predictor > 1 )
        {
            decoded = applyPredictor( decoded, predictor,
                                      static_cast<int>( number( parm, "Colors", 1 ) ),
                                      static_cast<int>( number( parm, "BitsPerComponent", 8 ) ),
                                      static_cast<int>( number( parm, "Columns", 1 ) ) );
        }

        aOut = std::move( decoded );

        if( aOut.size() > MAX_DECODED_STREAM_BYTES )
            return false;
    }

    return true;
}


bool DOCUMENT::parseObjectAt( std::string_view aText, size_t aPos, int aNum, size_t& aEnd )
{
    LEXER lexer( aText, aPos );
    OBJ   object;

    if( !lexer.NextValue( object ) )
        return false;

    if( object.kind == OBJ::OPERATOR )
    {
        if( object.text == "endobj" )
        {
            m_objects[aNum] = OBJ();
            aEnd = lexer.Pos();
            return true;
        }

        return false;
    }

    OBJ next;
    const size_t afterObject = lexer.Pos();

    if( object.kind == OBJ::DIC && lexer.Next( next ) && next.IsOperator( "stream" ) )
    {
        size_t dataStart = lexer.Pos();

        if( dataStart < aText.size() && aText[dataStart] == '\r' )
            ++dataStart;

        if( dataStart < aText.size() && aText[dataStart] == '\n' )
            ++dataStart;

        // Trust /Length only when "endstream" really follows it; split or rewritten files
        // frequently carry stale lengths.
        size_t dataEnd = std::string::npos;
        const OBJ lengthObject = object.dict->count( "Length" ) ? object.dict->at( "Length" ) : OBJ();
        double    length = -1;

        if( lengthObject.IsNumber() )
            length = lengthObject.number;
        else if( lengthObject.kind == OBJ::REFERENCE )
        {
            const auto it = m_objects.find( lengthObject.refNum );

            if( it != m_objects.end() && it->second.IsNumber() )
                length = it->second.number;
        }

        if( length >= 0 && dataStart + static_cast<size_t>( length ) <= aText.size() )
        {
            size_t probe = dataStart + static_cast<size_t>( length );

            while( probe < aText.size() && isWhitespace( aText[probe] ) )
                ++probe;

            if( aText.compare( probe, 9, "endstream" ) == 0 )
                dataEnd = dataStart + static_cast<size_t>( length );
        }

        if( dataEnd == std::string::npos )
        {
            const size_t marker = aText.find( "endstream", dataStart );

            if( marker == std::string::npos )
                dataEnd = aText.size();
            else
            {
                dataEnd = marker;

                if( dataEnd > dataStart && aText[dataEnd - 1] == '\n' )
                    --dataEnd;

                if( dataEnd > dataStart && aText[dataEnd - 1] == '\r' )
                    --dataEnd;
            }
        }

        object.kind = OBJ::STREAM;
        object.raw = std::make_shared<std::string>( aText.substr( dataStart, dataEnd - dataStart ) );
        aEnd = std::min( aText.size(), aText.find( "endobj", dataEnd ) );
    }
    else
    {
        aEnd = afterObject;
    }

    m_objects[aNum] = std::move( object );
    return true;
}


void DOCUMENT::scanObjects( std::string_view aText )
{
    size_t pos = 0;

    while( ( pos = aText.find( "obj", pos ) ) != std::string_view::npos )
    {
        const size_t keyword = pos;
        pos += 3;

        // Must be a standalone keyword: "N G obj" with whitespace separators and not part of
        // "endobj".
        if( keyword >= 3 && aText.compare( keyword - 3, 3, "end" ) == 0 )
            continue;

        if( keyword + 3 < aText.size() && isRegular( aText[keyword + 3] ) )
            continue;

        size_t cursor = keyword;

        while( cursor > 0 && isWhitespace( aText[cursor - 1] ) )
            --cursor;

        size_t genEnd = cursor;

        while( cursor > 0 && aText[cursor - 1] >= '0' && aText[cursor - 1] <= '9' )
            --cursor;

        if( cursor == genEnd || cursor == 0 || !isWhitespace( aText[cursor - 1] ) )
            continue;

        while( cursor > 0 && isWhitespace( aText[cursor - 1] ) )
            --cursor;

        size_t numEnd = cursor;

        while( cursor > 0 && aText[cursor - 1] >= '0' && aText[cursor - 1] <= '9' )
            --cursor;

        if( cursor == numEnd || ( cursor > 0 && isRegular( aText[cursor - 1] ) ) )
            continue;

        int num = 0;

        try
        {
            num = std::stoi( std::string( aText.substr( cursor, numEnd - cursor ) ) );
        }
        catch( ... )
        {
            continue;
        }

        if( m_objects.size() >= MAX_OBJECTS )
            break;

        size_t end = pos;

        if( parseObjectAt( aText, pos, num, end ) && end > pos )
            pos = end;
    }
}


void DOCUMENT::expandObjectStreams()
{
    std::vector<int> streams;

    for( const auto& [num, object] : m_objects )
    {
        if( object.kind == OBJ::STREAM && get( object, "Type" ).IsName( "ObjStm" ) )
            streams.push_back( num );
    }

    std::sort( streams.begin(), streams.end() );

    for( const int num : streams )
    {
        const OBJ   stream = m_objects[num];
        std::string data;

        if( !decodeStream( stream, data ) )
            continue;

        const int    count = static_cast<int>( number( stream, "N", 0 ) );
        const size_t first = static_cast<size_t>( number( stream, "First", 0 ) );
        LEXER        header( data );
        std::vector<std::pair<int, size_t>> entries;

        for( int i = 0; i < count && i < 100000; ++i )
        {
            OBJ objectNumber;
            OBJ offset;

            if( !header.Next( objectNumber ) || !header.Next( offset ) || !objectNumber.IsNumber()
                || !offset.IsNumber() )
                break;

            entries.push_back( { objectNumber.Int(), first + static_cast<size_t>( offset.number ) } );
        }

        for( const auto& [objectNumber, offset] : entries )
        {
            if( offset >= data.size() || m_objects.count( objectNumber ) )
                continue;

            LEXER lexer( data, offset );
            OBJ   object;

            if( lexer.NextValue( object ) && object.kind != OBJ::OPERATOR )
                m_objects[objectNumber] = std::move( object );
        }
    }
}


void DOCUMENT::collectTrailers( std::string_view aText )
{
    // Classic trailers, last one wins.
    size_t pos = 0;

    while( ( pos = aText.find( "trailer", pos ) ) != std::string_view::npos )
    {
        LEXER lexer( aText, pos + 7 );
        OBJ   dict;

        if( lexer.Next( dict ) && dict.kind == OBJ::DIC )
        {
            if( dict.dict->count( "Root" ) )
                m_root = dict.dict->at( "Root" );

            if( dict.dict->count( "Info" ) )
                m_info = dict.dict->at( "Info" );

            if( dict.dict->count( "Encrypt" ) )
                m_root.text = "encrypted";
        }

        pos += 7;
    }

    // Cross-reference streams carry the same keys.
    for( const auto& [num, object] : m_objects )
    {
        if( object.kind == OBJ::STREAM && get( object, "Type" ).IsName( "XRef" ) )
        {
            if( object.dict->count( "Root" ) && m_root.kind != OBJ::REFERENCE )
                m_root = object.dict->at( "Root" );

            if( object.dict->count( "Info" ) && m_info.kind != OBJ::REFERENCE )
                m_info = object.dict->at( "Info" );

            if( object.dict->count( "Encrypt" ) )
                m_root.text = "encrypted";
        }
    }

    if( m_root.kind != OBJ::REFERENCE || resolve( m_root ).kind == OBJ::NUL )
    {
        for( const auto& [num, object] : m_objects )
        {
            if( object.IsDict() && get( object, "Type" ).IsName( "Catalog" ) )
            {
                OBJ ref;
                ref.kind = OBJ::REFERENCE;
                ref.refNum = num;
                ref.text = m_root.text;
                m_root = ref;
                break;
            }
        }
    }
}


void DOCUMENT::walkPages( const OBJ& aNode, DICT aInherited, std::set<int>& aVisited, int aDepth )
{
    if( aDepth > 64 )
        return;

    if( aNode.kind == OBJ::REFERENCE )
    {
        if( aVisited.count( aNode.refNum ) )
            return;

        aVisited.insert( aNode.refNum );
    }

    const OBJ node = resolve( aNode );

    if( !node.IsDict() )
        return;

    for( const char* key : { "Resources", "MediaBox", "Rotate" } )
    {
        if( node.dict->count( key ) )
            aInherited[key] = node.dict->at( key );
    }

    const OBJ kids = get( node, "Kids" );
    const OBJ type = get( node, "Type" );

    if( kids.kind == OBJ::ARR && !type.IsName( "Page" ) )
    {
        for( const OBJ& kid : *kids.array )
            walkPages( kid, aInherited, aVisited, aDepth + 1 );

        return;
    }

    if( type.IsName( "Page" ) || node.dict->count( "Contents" ) )
        m_pages.push_back( { node, std::move( aInherited ) } );
}


bool DOCUMENT::buildPageList( std::string& aError )
{
    const OBJ catalog = resolve( m_root );
    std::set<int> visited;

    if( catalog.IsDict() )
        walkPages( get( catalog, "Pages" ).kind == OBJ::NUL && catalog.dict->count( "Pages" )
                           ? catalog.dict->at( "Pages" )
                           : ( catalog.dict->count( "Pages" ) ? catalog.dict->at( "Pages" ) : OBJ() ),
                   DICT(), visited, 0 );

    if( m_pages.empty() )
    {
        // No usable page tree: take page objects in file order.
        std::vector<int> pageNumbers;

        for( const auto& [num, object] : m_objects )
        {
            if( object.IsDict() && get( object, "Type" ).IsName( "Page" ) )
                pageNumbers.push_back( num );
        }

        std::sort( pageNumbers.begin(), pageNumbers.end() );

        for( const int num : pageNumbers )
        {
            DICT inherited;
            OBJ  parent = get( m_objects[num], "Parent" );

            for( int guard = 0; guard < 32 && parent.IsDict(); ++guard )
            {
                for( const char* key : { "Resources", "MediaBox", "Rotate" } )
                {
                    if( parent.dict->count( key ) && !inherited.count( key ) )
                        inherited[key] = parent.dict->at( key );
                }

                parent = get( parent, "Parent" );
            }

            m_pages.push_back( { m_objects[num], inherited } );
        }
    }

    if( m_pages.empty() )
    {
        aError = "no pages were found in the PDF";
        return false;
    }

    return true;
}


bool DOCUMENT::Load( std::string aBytes, std::string& aError )
{
    m_bytes = std::move( aBytes );
    m_objects.clear();
    m_pages.clear();

    if( m_bytes.size() < 8 || m_bytes.compare( 0, 5, "%PDF-" ) != 0 )
    {
        const size_t header = m_bytes.find( "%PDF-" );

        if( header == std::string::npos || header > 1024 )
        {
            aError = "the file is not a PDF document";
            return false;
        }
    }

    const std::string_view text( m_bytes );
    scanObjects( text );
    expandObjectStreams();
    collectTrailers( text );

    if( m_root.text == "encrypted" )
    {
        aError = "the PDF is encrypted; KiChad reads only unencrypted documents";
        return false;
    }

    if( !buildPageList( aError ) )
        return false;

    const OBJ info = resolve( m_info );
    const OBJ title = get( info, "Title" );

    if( title.kind == OBJ::STRING )
        m_title = pdfTextToUtf8( title.text );

    return true;
}


// ---------------------------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------------------------

void DOCUMENT::parseCodespaces( const std::string& aCMap, FONT& aFont ) const
{
    LEXER            lexer( aCMap );
    OBJ              token;
    std::vector<OBJ> stack;

    while( lexer.Next( token ) )
    {
        if( token.kind == OBJ::OPERATOR && token.text == "begincodespacerange" )
        {
            while( lexer.Next( token ) && !( token.kind == OBJ::OPERATOR && token.text == "endcodespacerange" ) )
            {
                OBJ high;

                if( token.kind != OBJ::STRING || !lexer.Next( high ) || high.kind != OBJ::STRING )
                    break;

                CODESPACE range{ static_cast<int>( token.text.size() ), 0, 0 };

                for( const char c : token.text )
                    range.low = ( range.low << 8 ) | static_cast<unsigned char>( c );

                for( const char c : high.text )
                    range.high = ( range.high << 8 ) | static_cast<unsigned char>( c );

                if( range.bytes >= 1 && range.bytes <= 4 )
                    aFont.codespaces.push_back( range );
            }
        }
    }
}


void DOCUMENT::parseToUnicode( const std::string& aCMap, FONT& aFont ) const
{
    LEXER lexer( aCMap );
    OBJ   token;

    const auto codeOf = []( const std::string& aBytes )
    {
        uint32_t code = 0;

        for( const char c : aBytes )
            code = ( code << 8 ) | static_cast<unsigned char>( c );

        return code;
    };

    while( lexer.Next( token ) )
    {
        if( token.kind != OBJ::OPERATOR )
            continue;

        if( token.text == "beginbfchar" )
        {
            while( lexer.Next( token ) && !( token.kind == OBJ::OPERATOR && token.text == "endbfchar" ) )
            {
                OBJ dst;

                if( token.kind != OBJ::STRING || !lexer.Next( dst ) )
                    break;

                if( dst.kind == OBJ::STRING )
                    aFont.toUnicode[codeOf( token.text )] = utf16BeToUtf8( dst.text );
                else if( dst.kind == OBJ::NAME )
                {
                    std::string utf8;
                    appendUtf8( utf8, glyphNameToCodePoint( dst.text ) );
                    aFont.toUnicode[codeOf( token.text )] = utf8;
                }
            }

            aFont.hasToUnicode = true;
        }
        else if( token.text == "beginbfrange" )
        {
            while( lexer.Next( token ) && !( token.kind == OBJ::OPERATOR && token.text == "endbfrange" ) )
            {
                OBJ high;
                OBJ dst;

                if( token.kind != OBJ::STRING || !lexer.Next( high ) || high.kind != OBJ::STRING
                    || !lexer.Next( dst ) )
                    break;

                const uint32_t low = codeOf( token.text );
                const uint32_t top = codeOf( high.text );

                if( top < low || top - low > 65535 )
                    continue;

                if( dst.kind == OBJ::ARR )
                {
                    uint32_t code = low;

                    for( const OBJ& item : *dst.array )
                    {
                        if( item.kind == OBJ::STRING )
                            aFont.toUnicode[code] = utf16BeToUtf8( item.text );

                        if( code++ == top )
                            break;
                    }
                }
                else if( dst.kind == OBJ::STRING && !dst.text.empty() )
                {
                    if( top - low <= 256 )
                    {
                        std::string value = dst.text;

                        for( uint32_t code = low; code <= top; ++code )
                        {
                            aFont.toUnicode[code] = utf16BeToUtf8( value );

                            // Increment the last byte (with carry) as the spec describes.
                            for( size_t i = value.size(); i-- > 0; )
                            {
                                if( static_cast<unsigned char>( value[i] ) != 0xFF )
                                {
                                    value[i] = static_cast<char>( static_cast<unsigned char>( value[i] ) + 1 );
                                    break;
                                }

                                value[i] = '\0';
                            }
                        }
                    }
                    else
                    {
                        FONT::RANGE range;
                        range.low = low;
                        range.high = top;
                        range.dstPrefix = dst.text;
                        range.dstStart = 0;
                        aFont.toUnicodeRanges.push_back( range );
                    }
                }
            }

            aFont.hasToUnicode = true;
        }
    }
}


void DOCUMENT::loadSimpleEncoding( const OBJ& aFont, FONT& aOut ) const
{
    // Base: standard/WinAnsi for ASCII plus the high half from the named encoding.
    const OBJ         encoding = get( aFont, "Encoding" );
    std::string       baseName;
    const OBJ         descriptor = get( aFont, "FontDescriptor" );
    const bool        symbolic = ( static_cast<int>( number( descriptor, "Flags", 0 ) ) & 4 ) != 0
                          && ( static_cast<int>( number( descriptor, "Flags", 0 ) ) & 32 ) == 0;

    if( encoding.kind == OBJ::NAME )
        baseName = encoding.text;
    else if( encoding.IsDict() )
    {
        const OBJ base = get( encoding, "BaseEncoding" );

        if( base.kind == OBJ::NAME )
            baseName = base.text;
    }

    for( int code = 32; code < 127; ++code )
        aOut.simpleEncoding[code] = std::string( 1, static_cast<char>( code ) );

    const bool mac = baseName == "MacRomanEncoding";
    const bool useWin = !symbolic || baseName == "WinAnsiEncoding";

    if( baseName == "StandardEncoding" )
    {
        aOut.simpleEncoding[0x27] = "\xE2\x80\x99"; // quoteright
        aOut.simpleEncoding[0x60] = "\xE2\x80\x98"; // quoteleft
    }

    if( mac || useWin )
    {
        for( int code = 128; code < 256; ++code )
        {
            const uint16_t cp = mac ? MAC_ROMAN_HIGH[code - 128] : WIN_ANSI_HIGH[code - 128];

            if( cp )
            {
                std::string utf8;
                appendUtf8( utf8, cp );
                aOut.simpleEncoding[code] = utf8;
            }
        }
    }

    if( encoding.IsDict() )
    {
        const OBJ differences = get( encoding, "Differences" );

        if( differences.kind == OBJ::ARR )
        {
            int code = 0;

            for( const OBJ& raw : *differences.array )
            {
                const OBJ item = resolve( raw );

                if( item.IsNumber() )
                    code = item.Int();
                else if( item.kind == OBJ::NAME && code >= 0 && code < 256 )
                {
                    const uint32_t cp = glyphNameToCodePoint( item.text );

                    if( cp )
                    {
                        std::string utf8;
                        appendUtf8( utf8, cp );
                        aOut.simpleEncoding[code] = utf8;
                    }

                    ++code;
                }
            }
        }
    }
}


void DOCUMENT::loadWidths( const OBJ& aFont, FONT& aOut ) const
{
    if( aOut.composite )
    {
        const OBJ descendants = get( aFont, "DescendantFonts" );
        OBJ       cid;

        if( descendants.kind == OBJ::ARR && !descendants.array->empty() )
            cid = resolve( descendants.array->front() );
        else if( descendants.IsDict() )
            cid = descendants;

        aOut.defaultWidth = number( cid, "DW", 1000.0 );
        const OBJ w = get( cid, "W" );

        if( w.kind == OBJ::ARR )
        {
            const ARRAY& items = *w.array;
            size_t       i = 0;

            while( i < items.size() )
            {
                const OBJ first = resolve( items[i] );

                if( !first.IsNumber() )
                    break;

                if( i + 1 < items.size() )
                {
                    const OBJ second = resolve( items[i + 1] );

                    if( second.kind == OBJ::ARR )
                    {
                        uint32_t code = static_cast<uint32_t>( first.number );

                        for( const OBJ& width : *second.array )
                        {
                            const OBJ value = resolve( width );

                            if( value.IsNumber() )
                                aOut.widths[code] = value.number;

                            ++code;
                        }

                        i += 2;
                        continue;
                    }

                    if( second.IsNumber() && i + 2 < items.size() )
                    {
                        const OBJ width = resolve( items[i + 2] );
                        const uint32_t lo = static_cast<uint32_t>( first.number );
                        const uint32_t hi = static_cast<uint32_t>( second.number );

                        if( width.IsNumber() && hi >= lo && hi - lo < 65536 )
                        {
                            for( uint32_t code = lo; code <= hi; ++code )
                                aOut.widths[code] = width.number;
                        }

                        i += 3;
                        continue;
                    }
                }

                break;
            }
        }

        return;
    }

    const int firstChar = static_cast<int>( number( aFont, "FirstChar", 0 ) );
    const OBJ widths = get( aFont, "Widths" );
    const OBJ descriptor = get( aFont, "FontDescriptor" );
    aOut.defaultWidth = number( descriptor, "MissingWidth", 0.0 );

    if( widths.kind == OBJ::ARR && !widths.array->empty() )
    {
        uint32_t code = static_cast<uint32_t>( std::max( 0, firstChar ) );

        for( const OBJ& raw : *widths.array )
        {
            const OBJ value = resolve( raw );

            if( value.IsNumber() )
                aOut.widths[code] = value.number * ( aOut.type3 ? aOut.type3Scale * 1000.0 : 1.0 );

            ++code;
        }
    }
    else
    {
        // Standard 14 fonts without widths: Courier is monospaced, the rest are near 500.
        const OBJ base = get( aFont, "BaseFont" );
        aOut.defaultWidth = base.kind == OBJ::NAME && base.text.find( "Courier" ) != std::string::npos
                                    ? 600.0
                                    : 500.0;
    }
}


std::shared_ptr<FONT> DOCUMENT::loadFont( const OBJ& aFontRef ) const
{
    const int key = aFontRef.kind == OBJ::REFERENCE ? aFontRef.refNum : -1;

    if( key >= 0 )
    {
        const auto it = m_fontCache.find( key );

        if( it != m_fontCache.end() )
            return it->second;
    }

    const OBJ font = resolve( aFontRef );
    auto      out = std::make_shared<FONT>();

    if( font.IsDict() )
    {
        const OBJ subtype = get( font, "Subtype" );
        out->composite = subtype.IsName( "Type0" );
        out->type3 = subtype.IsName( "Type3" );

        if( out->type3 )
        {
            const OBJ matrix = get( font, "FontMatrix" );

            if( matrix.kind == OBJ::ARR && !matrix.array->empty() )
            {
                const OBJ scale = resolve( matrix.array->front() );

                if( scale.IsNumber() && scale.number != 0.0 )
                    out->type3Scale = std::abs( scale.number );
            }
        }

        const OBJ toUnicode = get( font, "ToUnicode" );
        std::string cmap;

        if( toUnicode.kind == OBJ::STREAM && decodeStream( toUnicode, cmap ) )
            parseToUnicode( cmap, *out );

        if( out->composite )
        {
            const OBJ encoding = get( font, "Encoding" );

            if( encoding.kind == OBJ::STREAM )
            {
                std::string embedded;

                if( decodeStream( encoding, embedded ) )
                    parseCodespaces( embedded, *out );
            }
            else if( encoding.kind == OBJ::NAME && encoding.text.rfind( "Identity", 0 ) != 0 )
            {
                // Predefined CJK CMaps are mixed one/two byte; two bytes is the common case.
            }

            if( out->codespaces.empty() )
                out->codespaces.push_back( { 2, 0, 0xFFFF } );
        }
        else
        {
            loadSimpleEncoding( font, *out );
        }

        loadWidths( font, *out );
    }
    else
    {
        for( int code = 32; code < 127; ++code )
            out->simpleEncoding[code] = std::string( 1, static_cast<char>( code ) );
    }

    if( key >= 0 )
        m_fontCache[key] = out;

    return out;
}


// ---------------------------------------------------------------------------------------------
// Content interpreter
// ---------------------------------------------------------------------------------------------

struct MATRIX
{
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    MATRIX Multiply( const MATRIX& m ) const
    {
        // this × m
        return { a * m.a + b * m.c,         a * m.b + b * m.d,
                 c * m.a + d * m.c,         c * m.b + d * m.d,
                 e * m.a + f * m.c + m.e,   e * m.b + f * m.d + m.f };
    }

    void Apply( double x, double y, double& ox, double& oy ) const
    {
        ox = a * x + c * y + e;
        oy = b * x + d * y + f;
    }
};


struct GLYPH_RUN
{
    double      x;       ///< device x at start
    double      xEnd;    ///< device x after advance
    double      y;       ///< device baseline
    double      size;    ///< device glyph height
    bool        rotated;
    std::string text;
};


class INTERPRETER
{
public:
    INTERPRETER( const DOCUMENT& aDocument ) : m_document( aDocument ) {}

    void Run( const std::string& aContent, const OBJ& aResources, const MATRIX& aBase, int aDepth );

    std::vector<GLYPH_RUN>& Runs() { return m_runs; }

private:
    struct STATE
    {
        MATRIX                 ctm;
        std::shared_ptr<FONT>  font;
        double                 fontSize = 0.0;
        double                 charSpacing = 0.0;
        double                 wordSpacing = 0.0;
        double                 horizontalScale = 1.0;
        double                 leading = 0.0;
        double                 rise = 0.0;
    };

    void showString( const std::string& aBytes, STATE& aState, MATRIX& aTm );
    void nextCodes( const FONT& aFont, const std::string& aBytes,
                    std::vector<std::pair<uint32_t, int>>& aCodes ) const;
    std::string unicodeFor( const FONT& aFont, uint32_t aCode, int aBytes ) const;

    const DOCUMENT&        m_document;
    std::vector<GLYPH_RUN> m_runs;
    size_t                 m_operations = 0;
};


void INTERPRETER::nextCodes( const FONT& aFont, const std::string& aBytes,
                             std::vector<std::pair<uint32_t, int>>& aCodes ) const
{
    if( !aFont.composite )
    {
        for( const char c : aBytes )
            aCodes.push_back( { static_cast<unsigned char>( c ), 1 } );

        return;
    }

    size_t pos = 0;

    while( pos < aBytes.size() )
    {
        bool matched = false;

        for( int length = 1; length <= 4 && !matched; ++length )
        {
            if( pos + length > aBytes.size() )
                break;

            uint32_t code = 0;

            for( int i = 0; i < length; ++i )
                code = ( code << 8 ) | static_cast<unsigned char>( aBytes[pos + i] );

            for( const CODESPACE& range : aFont.codespaces )
            {
                if( range.bytes == length && code >= range.low && code <= range.high )
                {
                    aCodes.push_back( { code, length } );
                    pos += length;
                    matched = true;
                    break;
                }
            }
        }

        if( !matched )
        {
            // Default to the shortest codespace length, or two bytes.
            int length = 2;

            for( const CODESPACE& range : aFont.codespaces )
                length = std::min( length, range.bytes );

            length = std::min<int>( length, static_cast<int>( aBytes.size() - pos ) );
            uint32_t code = 0;

            for( int i = 0; i < length; ++i )
                code = ( code << 8 ) | static_cast<unsigned char>( aBytes[pos + i] );

            aCodes.push_back( { code, length } );
            pos += length;
        }
    }
}


std::string INTERPRETER::unicodeFor( const FONT& aFont, uint32_t aCode, int aBytes ) const
{
    const auto it = aFont.toUnicode.find( aCode );

    if( it != aFont.toUnicode.end() )
        return it->second;

    for( const FONT::RANGE& range : aFont.toUnicodeRanges )
    {
        if( aCode >= range.low && aCode <= range.high )
        {
            std::string value = range.dstPrefix;
            uint32_t    offset = aCode - range.low;

            for( size_t i = value.size(); i-- > 0 && offset > 0; )
            {
                const uint32_t sum = static_cast<unsigned char>( value[i] ) + offset;
                value[i] = static_cast<char>( sum & 0xFF );
                offset = sum >> 8;
            }

            return utf16BeToUtf8( value );
        }
    }

    if( !aFont.composite && aCode < 256 )
        return aFont.simpleEncoding[aCode];

    if( aFont.composite && !aFont.hasToUnicode && aBytes == 2 && aCode >= 32 && aCode < 127 )
        return std::string( 1, static_cast<char>( aCode ) );

    return std::string();
}


void INTERPRETER::showString( const std::string& aBytes, STATE& aState, MATRIX& aTm )
{
    if( !aState.font || m_runs.size() >= MAX_GLYPH_RUNS )
        return;

    std::vector<std::pair<uint32_t, int>> codes;
    nextCodes( *aState.font, aBytes, codes );

    std::string text;
    double      startX = 0, startY = 0, endX = 0, endY = 0;
    bool        first = true;
    const MATRIX trmBase{ aState.fontSize * aState.horizontalScale, 0, 0, aState.fontSize, 0,
                          aState.rise };

    for( const auto& [code, bytes] : codes )
    {
        const MATRIX trm = trmBase.Multiply( aTm ).Multiply( aState.ctm );
        double       x, y;
        trm.Apply( 0, 0, x, y );

        if( first )
        {
            startX = x;
            startY = y;
            first = false;
        }

        const std::string glyph = unicodeFor( *aState.font, code, bytes );
        text += glyph;

        double width = aState.font->defaultWidth;
        const auto w = aState.font->widths.find( code );

        if( w != aState.font->widths.end() )
            width = w->second;

        if( aState.font->type3 && w == aState.font->widths.end() )
            width = 500.0;

        double advance = width / 1000.0 * aState.fontSize + aState.charSpacing;

        if( bytes == 1 && code == 32 )
            advance += aState.wordSpacing;

        advance *= aState.horizontalScale;
        aTm = MATRIX{ 1, 0, 0, 1, advance, 0 }.Multiply( aTm );
        trmBase.Multiply( aTm ).Multiply( aState.ctm ).Apply( 0, 0, endX, endY );
    }

    if( first )
        return;

    const MATRIX trm = trmBase.Multiply( aTm ).Multiply( aState.ctm );
    double       ux, uy, ox, oy;
    trm.Apply( 0, 1, ux, uy );
    trm.Apply( 0, 0, ox, oy );
    const double size = std::hypot( ux - ox, uy - oy );
    const bool   rotated = std::abs( trm.b ) > std::abs( trm.a );

    if( text.empty() )
        return;

    m_runs.push_back( { rotated ? startY : startX, rotated ? endY : endX, rotated ? startX : startY,
                        std::max( size, 0.1 ), rotated, std::move( text ) } );
}


void INTERPRETER::Run( const std::string& aContent, const OBJ& aResources, const MATRIX& aBase,
                       int aDepth )
{
    if( aDepth > MAX_FORM_DEPTH )
        return;

    LEXER              lexer( aContent );
    std::vector<OBJ>   operands;
    std::vector<STATE> stack;
    STATE              state;
    state.ctm = aBase;
    MATRIX tm;
    MATRIX tlm;
    OBJ    token;

    const OBJ fonts = m_document.get( aResources, "Font" );
    const OBJ xobjects = m_document.get( aResources, "XObject" );

    const auto num = [&]( size_t aIndex ) -> double
    {
        return aIndex < operands.size() && operands[aIndex].IsNumber() ? operands[aIndex].number : 0.0;
    };

    while( lexer.Next( token ) )
    {
        if( ++m_operations > MAX_CONTENT_OPERATIONS || m_runs.size() >= MAX_GLYPH_RUNS )
            return;

        if( token.kind != OBJ::OPERATOR )
        {
            if( operands.size() < 64 )
                operands.push_back( std::move( token ) );

            continue;
        }

        const std::string& op = token.text;

        if( op == "q" )
        {
            if( stack.size() < 256 )
                stack.push_back( state );
        }
        else if( op == "Q" )
        {
            if( !stack.empty() )
            {
                state = stack.back();
                stack.pop_back();
            }
        }
        else if( op == "cm" && operands.size() >= 6 )
        {
            const MATRIX m{ num( 0 ), num( 1 ), num( 2 ), num( 3 ), num( 4 ), num( 5 ) };
            state.ctm = m.Multiply( state.ctm );
        }
        else if( op == "BT" )
        {
            tm = MATRIX();
            tlm = MATRIX();
        }
        else if( op == "Tf" && operands.size() >= 2 )
        {
            state.fontSize = num( 1 );

            if( operands[0].kind == OBJ::NAME && fonts.IsDict() && fonts.dict->count( operands[0].text ) )
                state.font = m_document.loadFont( fonts.dict->at( operands[0].text ) );
            else
                state.font = m_document.loadFont( OBJ() );
        }
        else if( op == "Td" && operands.size() >= 2 )
        {
            tlm = MATRIX{ 1, 0, 0, 1, num( 0 ), num( 1 ) }.Multiply( tlm );
            tm = tlm;
        }
        else if( op == "TD" && operands.size() >= 2 )
        {
            state.leading = -num( 1 );
            tlm = MATRIX{ 1, 0, 0, 1, num( 0 ), num( 1 ) }.Multiply( tlm );
            tm = tlm;
        }
        else if( op == "Tm" && operands.size() >= 6 )
        {
            tlm = MATRIX{ num( 0 ), num( 1 ), num( 2 ), num( 3 ), num( 4 ), num( 5 ) };
            tm = tlm;
        }
        else if( op == "T*" )
        {
            tlm = MATRIX{ 1, 0, 0, 1, 0, -state.leading }.Multiply( tlm );
            tm = tlm;
        }
        else if( op == "TL" && !operands.empty() )
            state.leading = num( 0 );
        else if( op == "Tc" && !operands.empty() )
            state.charSpacing = num( 0 );
        else if( op == "Tw" && !operands.empty() )
            state.wordSpacing = num( 0 );
        else if( op == "Tz" && !operands.empty() )
            state.horizontalScale = num( 0 ) / 100.0;
        else if( op == "Ts" && !operands.empty() )
            state.rise = num( 0 );
        else if( op == "Tj" && !operands.empty() && operands.back().kind == OBJ::STRING )
        {
            showString( operands.back().text, state, tm );
        }
        else if( op == "'" && !operands.empty() && operands.back().kind == OBJ::STRING )
        {
            tlm = MATRIX{ 1, 0, 0, 1, 0, -state.leading }.Multiply( tlm );
            tm = tlm;
            showString( operands.back().text, state, tm );
        }
        else if( op == "\"" && operands.size() >= 3 && operands.back().kind == OBJ::STRING )
        {
            state.wordSpacing = num( 0 );
            state.charSpacing = num( 1 );
            tlm = MATRIX{ 1, 0, 0, 1, 0, -state.leading }.Multiply( tlm );
            tm = tlm;
            showString( operands.back().text, state, tm );
        }
        else if( op == "TJ" && !operands.empty() && operands.back().kind == OBJ::ARR )
        {
            for( const OBJ& item : *operands.back().array )
            {
                if( item.kind == OBJ::STRING )
                    showString( item.text, state, tm );
                else if( item.IsNumber() )
                {
                    const double shift = -item.number / 1000.0 * state.fontSize * state.horizontalScale;
                    tm = MATRIX{ 1, 0, 0, 1, shift, 0 }.Multiply( tm );
                }
            }
        }
        else if( op == "Do" && !operands.empty() && operands.back().kind == OBJ::NAME
                 && xobjects.IsDict() && xobjects.dict->count( operands.back().text ) )
        {
            const OBJ form = m_document.resolve( xobjects.dict->at( operands.back().text ) );

            if( form.kind == OBJ::STREAM && m_document.get( form, "Subtype" ).IsName( "Form" ) )
            {
                std::string content;

                if( m_document.decodeStream( form, content ) )
                {
                    MATRIX    base = state.ctm;
                    const OBJ matrix = m_document.get( form, "Matrix" );

                    if( matrix.kind == OBJ::ARR && matrix.array->size() >= 6 )
                    {
                        MATRIX m;
                        double values[6];

                        for( int i = 0; i < 6; ++i )
                        {
                            const OBJ v = m_document.resolve( ( *matrix.array )[i] );
                            values[i] = v.IsNumber() ? v.number : ( i == 0 || i == 3 ? 1.0 : 0.0 );
                        }

                        m = { values[0], values[1], values[2], values[3], values[4], values[5] };
                        base = m.Multiply( state.ctm );
                    }

                    OBJ resources = m_document.get( form, "Resources" );

                    if( !resources.IsDict() )
                        resources = aResources;

                    Run( content, resources, base, aDepth + 1 );
                }
            }
        }
        else if( op == "BI" )
        {
            // Inline image: skip to EI.
            const std::string_view text = lexer.Text();
            size_t pos = lexer.Pos();

            while( ( pos = text.find( "EI", pos ) ) != std::string_view::npos )
            {
                if( ( pos == 0 || isWhitespace( text[pos - 1] ) )
                    && ( pos + 2 >= text.size() || isWhitespace( text[pos + 2] ) ) )
                    break;

                pos += 2;
            }

            lexer.Seek( pos == std::string_view::npos ? text.size() : pos + 2 );
        }

        operands.clear();
    }
}


// ---------------------------------------------------------------------------------------------
// Line assembly
// ---------------------------------------------------------------------------------------------

std::string assembleLines( std::vector<GLYPH_RUN>& aRuns )
{
    if( aRuns.empty() )
        return std::string();

    // Rotated runs (vertical text along a margin) are gathered after the upright text.
    std::stable_partition( aRuns.begin(), aRuns.end(), []( const GLYPH_RUN& r ) { return !r.rotated; } );

    struct LINE
    {
        double                 y;
        double                 size;
        std::vector<GLYPH_RUN> runs;
    };

    std::vector<LINE> lines;

    std::vector<GLYPH_RUN> sorted = aRuns;
    std::stable_sort( sorted.begin(), sorted.end(),
                      []( const GLYPH_RUN& a, const GLYPH_RUN& b )
                      {
                          if( a.rotated != b.rotated )
                              return !a.rotated;

                          return a.y > b.y;
                      } );

    for( GLYPH_RUN& run : sorted )
    {
        if( !lines.empty() && lines.back().runs.front().rotated == run.rotated
            && std::abs( lines.back().y - run.y ) <= 0.45 * std::max( lines.back().size, run.size ) )
        {
            lines.back().runs.push_back( std::move( run ) );
            continue;
        }

        lines.push_back( { run.y, run.size, { std::move( run ) } } );
    }

    std::string out;
    double      previousY = 0.0;
    double      previousSize = 0.0;
    bool        firstLine = true;

    for( LINE& line : lines )
    {
        std::stable_sort( line.runs.begin(), line.runs.end(),
                          []( const GLYPH_RUN& a, const GLYPH_RUN& b ) { return a.x < b.x; } );

        if( !firstLine && !line.runs.front().rotated && previousY - line.y > 1.9 * std::max( previousSize, line.size ) )
            out.push_back( '\n' );

        firstLine = false;
        previousY = line.y;
        previousSize = line.size;

        std::string text;
        double      cursor = line.runs.front().x;
        bool        firstRun = true;
        const double charWidth = std::max( 0.5 * line.size, 0.5 );

        // Reproduce the column position with leading spaces so tables keep alignment.
        const int indent = static_cast<int>( std::clamp( line.runs.front().x / charWidth, 0.0, 200.0 ) );
        text.append( indent, ' ' );

        for( const GLYPH_RUN& run : line.runs )
        {
            if( !firstRun )
            {
                const double gap = run.x - cursor;

                if( gap > 0.12 * run.size )
                {
                    const int spaces = std::clamp( static_cast<int>( std::round( gap / charWidth ) ), 1, 120 );
                    text.append( spaces, ' ' );
                }
                else if( gap < -2.0 * run.size )
                {
                    // Overlapping runs (e.g. a re-drawn header) start a fresh segment.
                    text.push_back( ' ' );
                }
            }

            text += run.text;
            cursor = std::max( cursor, run.xEnd );
            firstRun = false;
        }

        // Trim trailing spaces and collapse control characters.
        while( !text.empty() && ( text.back() == ' ' || text.back() == '\t' ) )
            text.pop_back();

        for( char& c : text )
        {
            if( static_cast<unsigned char>( c ) < 32 && c != '\t' )
                c = ' ';
        }

        out += text;
        out.push_back( '\n' );

        if( out.size() > MAX_PAGE_TEXT_BYTES )
            break;
    }

    return out;
}


bool DOCUMENT::PageText( int aPage, std::string& aText, std::string& aError ) const
{
    if( aPage < 1 || aPage > PageCount() )
    {
        aError = "page out of range";
        return false;
    }

    const PAGE& page = m_pages[aPage - 1];
    std::string content;
    const OBJ   contents = get( page.dict, "Contents" );

    if( contents.kind == OBJ::STREAM )
    {
        decodeStream( contents, content );
    }
    else if( contents.kind == OBJ::ARR )
    {
        for( const OBJ& part : *contents.array )
        {
            const OBJ stream = resolve( part );
            std::string chunk;

            if( stream.kind == OBJ::STREAM && decodeStream( stream, chunk ) )
            {
                content += chunk;
                content.push_back( '\n' );
            }
        }
    }

    OBJ resources = get( page.dict, "Resources" );

    if( !resources.IsDict() && page.inherited.count( "Resources" ) )
        resources = resolve( page.inherited.at( "Resources" ) );

    INTERPRETER interpreter( *this );
    interpreter.Run( content, resources, MATRIX(), 0 );
    aText = assembleLines( interpreter.Runs() );
    return true;
}

} // namespace


class DOCUMENT_IMPL : public DOCUMENT
{
};


PDF_TEXT_DOCUMENT::PDF_TEXT_DOCUMENT() : m_impl( std::make_unique<DOCUMENT_IMPL>() )
{
}


PDF_TEXT_DOCUMENT::~PDF_TEXT_DOCUMENT() = default;


bool PDF_TEXT_DOCUMENT::Load( std::string aBytes, std::string& aError )
{
    return m_impl->Load( std::move( aBytes ), aError );
}


int PDF_TEXT_DOCUMENT::PageCount() const
{
    return m_impl->PageCount();
}


bool PDF_TEXT_DOCUMENT::PageText( int aPage, std::string& aText, std::string& aError ) const
{
    return m_impl->PageText( aPage, aText, aError );
}


std::string PDF_TEXT_DOCUMENT::Title() const
{
    return m_impl->Title();
}

} // namespace KICHAD::PDF
