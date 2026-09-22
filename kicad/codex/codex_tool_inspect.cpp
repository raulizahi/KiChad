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

#include "codex_tool_registry.h"
#include "codex_tool_internal.h"

#include "lossless_sexpr_document.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/base64.h>

#include <picosha2.h>


namespace
{

constexpr wxFileOffset MAX_INSPECTION_BYTES = 16 * 1024 * 1024;
constexpr size_t       MAX_EXPRESSION_BYTES = 32 * 1024;
constexpr size_t       MAX_RESULT_BYTES = 256 * 1024;
constexpr size_t       MAX_DISTINCT_HEADS = 512;
constexpr wxFileOffset MAX_INLINE_PREVIEW_BYTES = 8 * 1024 * 1024;
constexpr wxFileOffset MAX_PDF_DOCUMENT_BYTES = 256 * 1024 * 1024;
constexpr size_t       MAX_PDF_LAYER_SPEC_BYTES = 1024;

const char* const DEFAULT_BOARD_PDF_LAYERS =
        "F.Cu,B.Cu,F.SilkS,B.SilkS,F.Mask,B.Mask,F.Fab,B.Fab,Edge.Cuts";


bool validPdfLayerSpec( const std::string& aLayers )
{
    if( aLayers.empty() || aLayers.size() > MAX_PDF_LAYER_SPEC_BYTES )
        return false;

    // A conservative charset: KiCad layer names plus the comma separator.  Anything else
    // is rejected before it reaches the CLI argument vector.
    for( const char c : aLayers )
    {
        const bool ok = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' )
                        || ( c >= '0' && c <= '9' ) || c == '.' || c == '_' || c == ','
                        || c == '-' || c == ' ';

        if( !ok )
            return false;
    }

    return true;
}

} // namespace


namespace KICHAD::CODEX_TOOLS
{

nlohmann::json InspectSpec()
{
    nlohmann::json schema = { { "type", "object" },
                              { "additionalProperties", false },
                              { "required", nlohmann::json::array( { "operation", "path" } ) } };
    schema["properties"]["operation"] =
            { { "type", "string" },
              { "enum", nlohmann::json::array( { "summary", "find", "render", "pdf" } ) } };
    schema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative KiCad design file path." } };
    schema["properties"]["head"] =
            { { "type", "string" }, { "maxLength", 128 },
              { "description", "List head required by operation 'find'." } };
    schema["properties"]["limit"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 50 },
              { "description", "Maximum matches returned; defaults to 20." } };
    schema["properties"]["view"] =
            { { "type", "string" },
              { "enum", nlohmann::json::array(
                                { "schematic", "pcb2d", "pcblayout", "pcb3d" } ) },
              { "description", "Native PNG view required by operation 'render'." } };
    schema["properties"]["page"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 1000 },
              { "description", "One-based schematic page; defaults to 1." } };
    schema["properties"]["output"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description",
                "Project-relative .pdf destination for operation 'pdf'; defaults to "
                "documentation/<stem>.pdf for a schematic and "
                "documentation/<stem>-board.pdf for a board." } };
    schema["properties"]["layers"] =
            { { "type", "string" }, { "maxLength", 1024 },
              { "description",
                "Comma-separated KiCad board layers for a board 'pdf', one page each; "
                "defaults to " + std::string( DEFAULT_BOARD_PDF_LAYERS ) + "." } };

    return { { "type", "function" },
             { "name", "inspect" },
             { "description",
               "Inspect a KiCad 10 schematic, board, symbol library, or footprint without "
               "changing it. Use 'summary' for structural counts, 'find' for bounded raw "
               "expressions, 'render' to attach a native schematic, production-layer PCB, "
               "assembly-layout PCB, or 3D PCB PNG directly to the model for visual review, or "
               "'pdf' to write a complete schematic hierarchy or multipage board layer PDF "
               "document into the project for the user." },
             { "inputSchema", std::move( schema ) } };
}

} // namespace KICHAD::CODEX_TOOLS


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleInspect(
        const JSON& aArguments, const wxString& aProjectPath ) const
{
    if( !aArguments.is_object() )
        return failure( "invalid_arguments", "inspect arguments must be an object" );

    if( !aArguments.contains( "operation" ) || !aArguments["operation"].is_string()
        || !aArguments.contains( "path" ) || !aArguments["path"].is_string() )
    {
        return failure( "invalid_arguments", "inspect.operation and inspect.path must be strings" );
    }

    const std::string operation = aArguments["operation"].get<std::string>();
    const std::string relativePath = aArguments["path"].get<std::string>();

    if( operation != "summary" && operation != "find" && operation != "render"
        && operation != "pdf" )
    {
        return failure( "invalid_arguments",
                        "inspect.operation must be 'summary', 'find', 'render', or 'pdf'" );
    }

    wxString root = aProjectPath;

    if( !wxFileName::DirExists( root ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    wxFileName resolved;
    std::string pathError;

    if( !KICHAD::CODEX_TOOLS::ResolveProjectFile( root, relativePath, resolved, pathError ) )
        return failure( "invalid_path", pathError );

    wxFile file( resolved.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
        return failure( "read_failed", "Could not open the requested project file" );

    const wxFileOffset length = file.Length();

    if( length == wxInvalidOffset || length > MAX_INSPECTION_BYTES )
        return failure( "file_too_large", "Inspection is limited to 16 MiB per file" );

    std::string source( static_cast<size_t>( length ), '\0' );

    if( length > 0 && file.Read( source.data(), static_cast<size_t>( length ) ) != length )
        return failure( "read_failed", "Could not read the complete project file" );

    std::string parseError;
    std::unique_ptr<KICHAD::LOSSLESS_SEXPR_DOCUMENT> document =
            KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( std::move( source ), &parseError );

    if( !document )
        return failure( "parse_failed", parseError );

    if( document->Roots().empty() )
        return failure( "parse_failed", "The requested file has no root expression" );

    const std::string rootHead = document->ListHead( document->Roots().front() );

    if( rootHead != KICHAD::CODEX_TOOLS::ExpectedRootHead( resolved.GetExt() ) )
        return failure( "format_mismatch", "The file root does not match its KiCad extension" );

    JSON payload = { { "operation", operation },
                     { "path", relativePath },
                     { "bytes", static_cast<uint64_t>( length ) },
                     { "rootHead", rootHead } };

    if( operation == "pdf" )
    {
        const bool schematic = resolved.GetExt() == wxS( "kicad_sch" );
        const bool board = resolved.GetExt() == wxS( "kicad_pcb" );

        if( !schematic && !board )
        {
            return failure( "invalid_path",
                            "inspect.pdf requires a .kicad_sch or .kicad_pcb file" );
        }

        std::string layers;

        if( aArguments.contains( "layers" ) )
        {
            if( schematic )
            {
                return failure( "invalid_arguments",
                                "inspect.layers applies only to a board PDF" );
            }

            if( !aArguments["layers"].is_string() )
                return failure( "invalid_arguments", "inspect.layers must be a string" );

            layers = aArguments["layers"].get<std::string>();

            if( !validPdfLayerSpec( layers ) )
            {
                return failure( "invalid_arguments",
                                "inspect.layers must be a comma-separated list of KiCad "
                                "layer names" );
            }
        }
        else if( board )
        {
            layers = DEFAULT_BOARD_PDF_LAYERS;
        }

        std::string outputRelative;

        if( aArguments.contains( "output" ) )
        {
            if( !aArguments["output"].is_string() )
                return failure( "invalid_arguments", "inspect.output must be a string" );

            outputRelative = aArguments["output"].get<std::string>();
        }
        else
        {
            outputRelative = "documentation/" + resolved.GetName().ToStdString()
                             + ( board ? "-board.pdf" : ".pdf" );
        }

        wxFileName output;
        std::string outputResolved;

        if( !KICHAD::CODEX_TOOLS::ResolveProjectPdfDestination( root, outputRelative, output,
                                                                 outputResolved, pathError ) )
        {
            return failure( "invalid_path", pathError );
        }

        if( !output.DirExists()
            && !wxFileName::Mkdir( output.GetPath(), 0755, wxPATH_MKDIR_FULL ) )
        {
            return failure( "pdf_failed", "could not create the PDF output directory" );
        }

        const std::string kind = schematic ? "schematic" : "pcb";
        const bool written = m_nativePdfRunner
                                     ? m_nativePdfRunner( kind, resolved, output, layers,
                                                          pathError )
                                     : KICHAD::CODEX_TOOLS::RunNativeKiCadPdf(
                                               kind, resolved, output, layers, pathError );

        if( !written )
            return failure( "pdf_failed", pathError );

        wxFile pdfFile( output.GetFullPath(), wxFile::read );
        const wxFileOffset pdfBytes = pdfFile.IsOpened() ? pdfFile.Length() : wxInvalidOffset;

        if( pdfBytes <= 0 || pdfBytes > MAX_PDF_DOCUMENT_BYTES )
            return failure( "pdf_failed", "the PDF document must contain 1 byte through 256 MiB" );

        std::string pdf( static_cast<size_t>( pdfBytes ), '\0' );

        if( pdfFile.Read( pdf.data(), pdf.size() ) != pdfBytes )
            return failure( "pdf_failed", "could not read back the complete PDF document" );

        const size_t tail = std::min<size_t>( pdf.size(), 2048 );
        const std::string_view prefix( pdf.data(), std::min<size_t>( pdf.size(), 8 ) );
        const std::string_view suffix( pdf.data() + pdf.size() - tail, tail );

        if( !prefix.starts_with( "%PDF-" ) || suffix.find( "%%EOF" ) == std::string_view::npos )
        {
            wxRemoveFile( output.GetFullPath() );
            return failure( "pdf_failed",
                            "the native export did not produce a well-formed PDF document" );
        }

        payload["kind"] = kind;
        payload["outputPath"] = outputResolved;
        payload["outputBytes"] = static_cast<uint64_t>( pdfBytes );
        payload["sha256"] = picosha2::hash256_hex_string( pdf );

        if( board )
            payload["layers"] = layers;

        return success( payload );
    }

    if( operation == "render" )
    {
        if( !aArguments.contains( "view" ) || !aArguments["view"].is_string() )
            return failure( "invalid_arguments", "inspect.view is required for render" );

        const std::string view = aArguments["view"].get<std::string>();
        int page = 1;

        if( aArguments.contains( "page" ) )
        {
            if( !aArguments["page"].is_number_integer() )
                return failure( "invalid_arguments", "inspect.page must be an integer" );

            page = aArguments["page"].get<int>();

            if( page < 1 || page > 1000 )
                return failure( "invalid_arguments", "inspect.page must be between 1 and 1000" );
        }

        const bool schematic = resolved.GetExt() == wxS( "kicad_sch" );
        const bool board = resolved.GetExt() == wxS( "kicad_pcb" );

        if( ( view == "schematic" && !schematic )
            || ( ( view == "pcb2d" || view == "pcblayout" || view == "pcb3d" )
                 && !board ) )
        {
            return failure( "invalid_path",
                            "the requested preview view does not match the KiCad file type" );
        }

        if( view != "schematic" && view != "pcb2d" && view != "pcblayout"
            && view != "pcb3d" )
            return failure( "invalid_arguments", "inspect.view is not supported" );

        wxFileName previewDirectory = wxFileName::DirName( root );
        previewDirectory.AppendDir( wxS( "kichad" ) );
        previewDirectory.AppendDir( wxS( "previews" ) );

        if( !previewDirectory.DirExists()
            && !wxFileName::Mkdir( previewDirectory.GetFullPath(), 0700,
                                  wxPATH_MKDIR_FULL ) )
        {
            return failure( "preview_failed", "could not create the derived preview directory" );
        }

        const std::string identity = relativePath + ":" + view + ":" + std::to_string( page );
        const std::string digest = picosha2::hash256_hex_string( identity ).substr( 0, 16 );
        const wxString filename = wxString::FromUTF8(
                "preview-" + digest + "-" + view + ".png" );
        wxFileName preview( previewDirectory.GetFullPath(), filename );

        const bool rendered = m_nativePreviewRunner
                                      ? m_nativePreviewRunner(
                                                view, resolved, preview, page, pathError )
                                      : KICHAD::CODEX_TOOLS::RunNativeKiCadPreview(
                                                view, resolved, preview, page, pathError );

        if( !rendered )
        {
            return failure( "preview_failed", pathError );
        }

        wxFile previewFile( preview.GetFullPath(), wxFile::read );
        const wxFileOffset previewBytes = previewFile.IsOpened()
                                                  ? previewFile.Length()
                                                  : wxInvalidOffset;

        if( previewBytes <= 0 || previewBytes > MAX_INLINE_PREVIEW_BYTES )
        {
            return failure( "preview_failed",
                            "native preview must contain 1 byte through 8 MiB" );
        }

        std::string png( static_cast<size_t>( previewBytes ), '\0' );

        if( previewFile.Read( png.data(), png.size() ) != previewBytes )
            return failure( "preview_failed", "could not read the complete native preview" );

        const std::string previewRelative = "kichad/previews/"
                                            + filename.ToStdString();
        payload["view"] = view;
        payload["page"] = page;
        payload["previewPath"] = previewRelative;
        payload["previewBytes"] = static_cast<uint64_t>( previewBytes );
        payload["derived"] = true;
        JSON result = success( payload );
        const std::string encoded = wxBase64Encode( png.data(), png.size() ).ToStdString();
        result["contentItems"].push_back(
                { { "type", "inputImage" },
                  { "imageUrl", "data:image/png;base64," + encoded } } );
        return result;
    }

    if( operation == "summary" )
    {
        std::map<std::string, size_t> counts;

        for( size_t i = 0; i < document->Nodes().size(); ++i )
        {
            std::string head = document->ListHead( i );

            if( !head.empty() )
                ++counts[head];
        }

        std::vector<std::pair<std::string, size_t>> ordered( counts.begin(), counts.end() );
        std::sort( ordered.begin(), ordered.end(),
                   []( const auto& aLeft, const auto& aRight )
                   {
                       if( aLeft.second != aRight.second )
                           return aLeft.second > aRight.second;

                       return aLeft.first < aRight.first;
                   } );

        JSON listCounts = JSON::array();

        for( size_t i = 0; i < ordered.size() && i < MAX_DISTINCT_HEADS; ++i )
        {
            const auto& [head, count] = ordered[i];
            listCounts.push_back( { { "head", head }, { "count", count } } );
        }

        payload["roots"] = document->Roots().size();
        payload["nodes"] = document->Nodes().size();
        payload["distinctHeads"] = ordered.size();
        payload["listCounts"] = std::move( listCounts );
        payload["resultTruncated"] = ordered.size() > MAX_DISTINCT_HEADS;
        return success( payload );
    }

    if( !aArguments.contains( "head" ) || !aArguments["head"].is_string() )
        return failure( "invalid_arguments", "inspect.head must be a string for operation 'find'" );

    const std::string head = aArguments["head"].get<std::string>();

    if( head.empty() || head.size() > 128 )
        return failure( "invalid_arguments", "inspect.head must contain 1 to 128 bytes" );

    int limit = 20;

    if( aArguments.contains( "limit" ) )
    {
        if( !aArguments["limit"].is_number_integer() )
            return failure( "invalid_arguments", "inspect.limit must be an integer" );

        limit = aArguments["limit"].get<int>();

        if( limit < 1 || limit > 50 )
            return failure( "invalid_arguments", "inspect.limit must be between 1 and 50" );
    }

    const std::vector<size_t> matches = document->FindLists( head );
    JSON expressions = JSON::array();
    size_t resultBytes = 0;

    for( size_t i = 0; i < matches.size() && expressions.size() < static_cast<size_t>( limit ); ++i )
    {
        std::string raw = document->RawText( matches[i] );
        bool truncated = raw.size() > MAX_EXPRESSION_BYTES;

        if( truncated )
        {
            size_t boundary = MAX_EXPRESSION_BYTES;

            while( boundary > 0 && boundary < raw.size()
                   && ( static_cast<unsigned char>( raw[boundary] ) & 0xC0 ) == 0x80 )
            {
                --boundary;
            }

            raw.resize( boundary );
        }

        if( resultBytes + raw.size() > MAX_RESULT_BYTES )
            break;

        resultBytes += raw.size();
        expressions.push_back( { { "index", i }, { "text", std::move( raw ) },
                                 { "truncated", truncated } } );
    }

    payload["head"] = head;
    payload["totalMatches"] = matches.size();
    payload["expressions"] = std::move( expressions );
    payload["resultTruncated"] = payload["expressions"].size() < matches.size();
    return success( payload );
}
