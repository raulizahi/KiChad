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

#include "block_diagram.h"
#include "kichad_remove_file.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

#include <wx/base64.h>
#include <wx/file.h>
#include <wx/filename.h>

#include <picosha2.h>


namespace
{

constexpr size_t       MAX_NAME_CHARS = 64;
constexpr size_t       MAX_TITLE_CHARS = 200;
constexpr wxFileOffset MAX_INLINE_PREVIEW_BYTES = 8 * 1024 * 1024;


bool validDiagramName( const std::string& aName )
{
    if( aName.empty() || aName.size() > MAX_NAME_CHARS )
        return false;

    if( !std::isalnum( static_cast<unsigned char>( aName.front() ) ) )
        return false;

    return std::all_of( aName.begin(), aName.end(),
                        []( char c )
                        {
                            return std::isalnum( static_cast<unsigned char>( c ) ) || c == '_'
                                   || c == '-';
                        } );
}

} // namespace


namespace KICHAD::CODEX_TOOLS
{

nlohmann::json DiagramSpec()
{
    nlohmann::json schema = { { "type", "object" },
                              { "additionalProperties", false },
                              { "required", nlohmann::json::array( { "operation", "name",
                                                                      "source" } ) } };
    schema["properties"]["operation"] =
            { { "type", "string" }, { "enum", nlohmann::json::array( { "render" } ) } };
    schema["properties"]["name"] =
            { { "type", "string" }, { "maxLength", MAX_NAME_CHARS },
              { "description",
                "Diagram file stem (letters, digits, '_' and '-'), e.g. system-block-diagram." } };
    schema["properties"]["source"] =
            { { "type", "string" }, { "maxLength", KICHAD::BLOCK_DIAGRAM::MAX_SOURCE_BYTES },
              { "description",
                "Mermaid flowchart source: 'flowchart TB|BT|LR|RL', node shapes, labelled "
                "solid/dotted/thick links, chains, '&' fan-out, nested subgraphs, classDef, "
                "class, ':::' and style colours." } };
    schema["properties"]["title"] =
            { { "type", "string" }, { "maxLength", MAX_TITLE_CHARS },
              { "description", "Optional document title drawn above the diagram." } };
    schema["properties"]["output"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description",
                "Project-relative .pdf destination; defaults to documentation/<name>.pdf. "
                "The Mermaid source is saved beside it as <stem>.mmd." } };
    schema["properties"]["preview"] =
            { { "type", "boolean" },
              { "description",
                "Attach a PNG rendering of the diagram to the response for visual review; "
                "defaults to true." } };

    return { { "type", "function" },
             { "name", "diagram" },
             { "description",
               "Render a block diagram natively inside KiChad. Give Mermaid flowchart source and "
               "receive a PDF document in the project plus the saved .mmd source; the PNG "
               "preview is attached so the layout can be reviewed. Use this for every "
               "architecture or block diagram instead of pasting Mermaid text for the user to "
               "render elsewhere." },
             { "inputSchema", std::move( schema ) } };
}

} // namespace KICHAD::CODEX_TOOLS


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleDiagram(
        const JSON& aArguments, const wxString& aProjectPath ) const
{
    using namespace KICHAD::BLOCK_DIAGRAM;

    if( !aArguments.is_object() )
        return failure( "invalid_arguments", "diagram arguments must be an object" );

    if( !aArguments.contains( "operation" ) || !aArguments["operation"].is_string()
        || aArguments["operation"].get<std::string>() != "render" )
    {
        return failure( "invalid_arguments", "diagram.operation must be 'render'" );
    }

    if( !aArguments.contains( "name" ) || !aArguments["name"].is_string()
        || !validDiagramName( aArguments["name"].get<std::string>() ) )
    {
        return failure( "invalid_arguments",
                        "diagram.name must be 1 to 64 letters, digits, '_' or '-' and start "
                        "with a letter or digit" );
    }

    if( !aArguments.contains( "source" ) || !aArguments["source"].is_string() )
        return failure( "invalid_arguments", "diagram.source must be a Mermaid flowchart string" );

    const std::string name = aArguments["name"].get<std::string>();
    const std::string source = aArguments["source"].get<std::string>();
    std::string       title;

    if( aArguments.contains( "title" ) )
    {
        if( !aArguments["title"].is_string()
            || aArguments["title"].get<std::string>().size() > MAX_TITLE_CHARS )
            return failure( "invalid_arguments", "diagram.title must be a short string" );

        title = aArguments["title"].get<std::string>();
    }

    bool preview = true;

    if( aArguments.contains( "preview" ) )
    {
        if( !aArguments["preview"].is_boolean() )
            return failure( "invalid_arguments", "diagram.preview must be a boolean" );

        preview = aArguments["preview"].get<bool>();
    }

    wxString root = aProjectPath;

    if( !wxFileName::DirExists( root ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    DIAGRAM     diagram;
    std::string error;

    if( !ParseMermaidFlowchart( source, diagram, error ) )
        return failure( "invalid_source", "Mermaid flowchart rejected: " + error );

    LayoutDiagram( diagram );

    std::string outputRelative = "documentation/" + name + ".pdf";

    if( aArguments.contains( "output" ) )
    {
        if( !aArguments["output"].is_string() )
            return failure( "invalid_arguments", "diagram.output must be a string" );

        outputRelative = aArguments["output"].get<std::string>();
    }

    wxFileName  output;
    std::string outputResolved;

    if( !KICHAD::CODEX_TOOLS::ResolveProjectPdfDestination( root, outputRelative, output,
                                                             outputResolved, error ) )
    {
        return failure( "invalid_path", error );
    }

    if( !output.DirExists() && !wxFileName::Mkdir( output.GetPath(), 0755, wxPATH_MKDIR_FULL ) )
        return failure( "diagram_failed", "could not create the diagram output directory" );

    // Render into a private directory first so a failed plot never leaves a partial document.
    KICHAD::CODEX_TOOLS::PRIVATE_TEMPORARY_DIRECTORY temporary;

    if( !temporary.Create( "kichad-diagram", error ) )
        return failure( "diagram_failed", error );

    wxFileName staged( wxString::FromUTF8( temporary.Path().string() ), wxS( "diagram.pdf" ) );

    if( !RenderDiagramPdf( diagram, staged.GetFullPath(), wxString::FromUTF8( title ), error ) )
        return failure( "diagram_failed", error );

    wxFile pdfFile( staged.GetFullPath(), wxFile::read );
    const wxFileOffset pdfBytes = pdfFile.IsOpened() ? pdfFile.Length() : wxInvalidOffset;

    if( pdfBytes <= 0 )
        return failure( "diagram_failed", "the native plotter produced no PDF document" );

    std::string pdf( static_cast<size_t>( pdfBytes ), '\0' );

    if( pdfFile.Read( pdf.data(), pdf.size() ) != pdfBytes )
        return failure( "diagram_failed", "could not read back the PDF document" );

    const size_t           tail = std::min<size_t>( pdf.size(), 2048 );
    const std::string_view prefix( pdf.data(), std::min<size_t>( pdf.size(), 8 ) );
    const std::string_view suffix( pdf.data() + pdf.size() - tail, tail );

    if( !prefix.starts_with( "%PDF-" ) || suffix.find( "%%EOF" ) == std::string_view::npos )
        return failure( "diagram_failed", "the native plotter did not produce a well-formed PDF" );

    pdfFile.Close();

    if( output.FileExists() && !KICHAD::RemoveFileWithRetry( output.GetFullPath() ) )
        return failure( "diagram_failed", "could not replace the prior diagram PDF" );

    if( !wxCopyFile( staged.GetFullPath(), output.GetFullPath(), true ) )
        return failure( "diagram_failed", "could not install the diagram PDF into the project" );

    // Keep the Mermaid text beside the PDF as the editable source of the drawing.
    wxFileName sourceFile( output );
    sourceFile.SetExt( wxS( "mmd" ) );
    std::string sourceRelative = outputResolved.substr( 0, outputResolved.size() - 3 ) + "mmd";

    if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically( sourceFile, true, source, error ) )
    {
        return failure( "diagram_failed", "could not save the Mermaid source: " + error );
    }

    size_t groupCount = 0;

    // Qualified because <winsock2.h> declares a global GROUP on MSW.
    for( const KICHAD::BLOCK_DIAGRAM::GROUP& group : diagram.groups )
    {
        if( group.width > 0 )
            ++groupCount;
    }

    JSON payload = { { "operation", "render" },
                     { "name", name },
                     { "outputPath", outputResolved },
                     { "sourcePath", sourceRelative },
                     { "outputBytes", static_cast<uint64_t>( pdfBytes ) },
                     { "sha256", picosha2::hash256_hex_string( pdf ) },
                     { "direction", diagram.direction },
                     { "nodes", diagram.nodes.size() },
                     { "links", diagram.links.size() },
                     { "subgraphs", groupCount },
                     { "widthMm", std::round( diagram.width * 10.0 ) / 10.0 },
                     { "heightMm", std::round( diagram.height * 10.0 ) / 10.0 },
                     { "previewAttached", false } };

    JSON result = success( payload );

    if( !preview )
        return result;

    wxFileName previewDirectory = wxFileName::DirName( root );
    previewDirectory.AppendDir( wxS( ".kichad" ) );
    previewDirectory.AppendDir( wxS( "previews" ) );

    if( !previewDirectory.DirExists()
        && !wxFileName::Mkdir( previewDirectory.GetFullPath(), 0700, wxPATH_MKDIR_FULL ) )
    {
        payload["previewError"] = "could not create the derived preview directory";
        return success( payload );
    }

    const wxString filename = wxString::FromUTF8( "diagram-" + name + ".png" );
    wxFileName     previewFile( previewDirectory.GetFullPath(), filename );

    if( !RenderDiagramPng( diagram, previewFile.GetFullPath(), wxString::FromUTF8( title ), 1800,
                           error ) )
    {
        payload["previewError"] = error;
        return success( payload );
    }

    wxFile png( previewFile.GetFullPath(), wxFile::read );
    const wxFileOffset pngBytes = png.IsOpened() ? png.Length() : wxInvalidOffset;

    if( pngBytes <= 0 || pngBytes > MAX_INLINE_PREVIEW_BYTES )
    {
        payload["previewError"] = "preview must contain 1 byte through 8 MiB";
        return success( payload );
    }

    std::string image( static_cast<size_t>( pngBytes ), '\0' );

    if( png.Read( image.data(), image.size() ) != pngBytes )
    {
        payload["previewError"] = "could not read the complete preview";
        return success( payload );
    }

    payload["previewAttached"] = true;
    payload["previewPath"] = ".kichad/previews/" + filename.ToStdString();
    result = success( payload );
    const std::string encoded = wxBase64Encode( image.data(), image.size() ).ToStdString();
    result["contentItems"].push_back(
            { { "type", "inputImage" }, { "imageUrl", "data:image/png;base64," + encoded } } );
    return result;
}
