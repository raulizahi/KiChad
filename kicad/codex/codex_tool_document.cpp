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
#include "pdf_text_document.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <kicad_curl/kicad_curl_easy.h>
#include <wx/base64.h>
#include <wx/dir.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include <picosha2.h>


namespace
{

constexpr wxFileOffset MAX_DOCUMENT_BYTES = 64 * 1024 * 1024;
constexpr size_t       MAX_READ_PAGES = 20;
constexpr size_t       MAX_READ_TEXT_BYTES = 200 * 1024;
constexpr size_t       MAX_SEARCH_TEXT_BYTES = 32 * 1024 * 1024;
constexpr size_t       MAX_SEARCH_MATCHES = 100;
constexpr size_t       MAX_QUERY_CHARS = 200;
constexpr size_t       MAX_LIST_ENTRIES = 500;
constexpr wxFileOffset MAX_INLINE_PREVIEW_BYTES = 8 * 1024 * 1024;
constexpr int          MAX_PAGE_NUMBER = 5000;

const char* const DATASHEET_DIRECTORY = "datasheets";


bool readBoundedFile( const wxFileName& aPath, size_t aMaxBytes, std::string& aContent,
                      bool& aTruncated )
{
    wxFile file( aPath.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
        return false;

    const wxFileOffset length = file.Length();

    if( length == wxInvalidOffset )
        return false;

    const size_t wanted = std::min<size_t>( static_cast<size_t>( length ), aMaxBytes );
    aTruncated = static_cast<size_t>( length ) > aMaxBytes;
    aContent.assign( wanted, '\0' );

    if( wanted > 0 && file.Read( aContent.data(), wanted ) != static_cast<wxFileOffset>( wanted ) )
        return false;

    return true;
}


bool hasPdfSignature( const wxFileName& aPath )
{
    wxFile file( aPath.GetFullPath(), wxFile::read );
    char   signature[5] = { 0 };
    return file.IsOpened() && file.Read( signature, 5 ) == 5
           && std::string_view( signature, 5 ) == "%PDF-";
}


/** Open a PDF with the native reader. */
bool openDocument( const wxFileName& aPdf, KICHAD::PDF::PDF_TEXT_DOCUMENT& aDocument,
                   std::string& aError )
{
    std::string bytes;
    bool        truncated = false;

    if( !readBoundedFile( aPdf, static_cast<size_t>( MAX_DOCUMENT_BYTES ), bytes, truncated )
        || truncated )
    {
        aError = "could not read the document (limit 64 MiB)";
        return false;
    }

    if( !aDocument.Load( std::move( bytes ), aError ) )
    {
        if( aError.find( "no pages" ) != std::string::npos )
        {
            aError += "; the file is probably a truncated fragment of a larger PDF rather than a "
                      "complete document, so import the complete original instead";
        }

        return false;
    }

    return true;
}


bool pageCount( const wxFileName& aPdf, int& aPages, std::string& aError )
{
    KICHAD::PDF::PDF_TEXT_DOCUMENT document;

    if( !openDocument( aPdf, document, aError ) )
        return false;

    aPages = document.PageCount();
    return aPages > 0;
}


std::string sha256Of( const wxFileName& aPath )
{
    std::string digest;

    if( KICHAD::CODEX_TOOLS::HashFabricationFile(
                std::filesystem::path( aPath.GetFullPath().ToStdString() ), digest ) )
        return digest;

    return std::string();
}


bool parsePageRange( const std::string& aSpec, int aMaxPage, int& aFirst, int& aLast,
                     std::string& aError )
{
    const size_t dash = aSpec.find( '-' );

    try
    {
        if( dash == std::string::npos )
        {
            aFirst = aLast = std::stoi( aSpec );
        }
        else
        {
            aFirst = std::stoi( aSpec.substr( 0, dash ) );
            aLast = std::stoi( aSpec.substr( dash + 1 ) );
        }
    }
    catch( ... )
    {
        aError = "document.pages must be a page number or an inclusive range such as 12-15";
        return false;
    }

    if( aFirst < 1 || aLast < aFirst || aLast > aMaxPage )
    {
        aError = "document.pages must lie within 1-" + std::to_string( aMaxPage );
        return false;
    }

    if( static_cast<size_t>( aLast - aFirst + 1 ) > MAX_READ_PAGES )
    {
        aError = "document.pages may cover at most " + std::to_string( MAX_READ_PAGES )
                 + " pages per call";
        return false;
    }

    return true;
}


bool validDatasheetName( const std::string& aName )
{
    if( aName.empty() || aName.size() > 128 || aName.front() == '.' )
        return false;

    return std::all_of( aName.begin(), aName.end(),
                        []( char c )
                        {
                            return std::isalnum( static_cast<unsigned char>( c ) ) || c == '_'
                                   || c == '-' || c == '.' || c == ' ';
                        } );
}


std::string lowerAscii( std::string aText )
{
    for( char& c : aText )
        c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );

    return aText;
}

} // namespace


namespace KICHAD::CODEX_TOOLS
{

nlohmann::json DocumentSpec()
{
    nlohmann::json schema = { { "type", "object" },
                              { "additionalProperties", false },
                              { "required", nlohmann::json::array( { "operation" } ) } };
    schema["properties"]["operation"] =
            { { "type", "string" },
              { "enum", nlohmann::json::array(
                                { "list", "import", "fetch", "read", "search", "render" } ) } };
    schema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description",
                "Project-relative PDF path for read, search, and render, e.g. "
                "datasheets/stm32mp257f.pdf." } };
    schema["properties"]["source"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description",
                "For import: an absolute PDF path the user named (below their home directory) "
                "or a project-relative path." } };
    schema["properties"]["url"] =
            { { "type", "string" }, { "maxLength", 2048 },
              { "description", "For fetch: an https:// URL of a PDF up to 64 MiB." } };
    schema["properties"]["name"] =
            { { "type", "string" }, { "maxLength", 128 },
              { "description",
                "For import and fetch: file name to store under datasheets/; defaults to the "
                "source or URL basename." } };
    schema["properties"]["pages"] =
            { { "type", "string" }, { "maxLength", 16 },
              { "description",
                "For read: a page number or inclusive range such as 12-15, at most 20 pages; "
                "defaults to 1-20." } };
    schema["properties"]["query"] =
            { { "type", "string" }, { "maxLength", MAX_QUERY_CHARS },
              { "description", "For search: case-insensitive text to find across every page." } };
    schema["properties"]["limit"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", MAX_SEARCH_MATCHES },
              { "description", "For search: maximum matches returned; defaults to 40." } };
    schema["properties"]["page"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", MAX_PAGE_NUMBER },
              { "description", "For render: the one-based page to attach as a PNG." } };

    return { { "type", "function" },
             { "name", "document" },
             { "description",
               "Read PDF datasheets and other documents inside the project. 'fetch' downloads an "
               "https PDF into datasheets/, 'import' copies a PDF the user named by absolute path "
               "into datasheets/, 'list' shows the project's PDFs, 'read' returns the text of a "
               "page range, 'search' finds text across all pages with page numbers, and 'render' "
               "attaches one page as an image when the optional poppler utility is installed. "
               "Reading and searching are native and need no external software. KiChad has no "
               "chat attachments: use this tool instead of asking the user to upload files." },
             { "inputSchema", std::move( schema ) } };
}

} // namespace KICHAD::CODEX_TOOLS


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleDocument(
        const JSON& aArguments, const wxString& aProjectPath ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string() )
    {
        return failure( "invalid_arguments", "document.operation must be a string" );
    }

    const std::string operation = aArguments["operation"].get<std::string>();
    static const std::vector<std::string> operations = { "list", "import", "fetch", "read",
                                                         "search", "render" };

    if( std::find( operations.begin(), operations.end(), operation ) == operations.end() )
    {
        return failure( "invalid_arguments",
                        "document.operation must be one of list, import, fetch, read, search, "
                        "render" );
    }

    if( !wxFileName::DirExists( aProjectPath ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !KICHAD::CODEX_TOOLS::CanonicalizeExisting( root, true ) )
        return failure( "project_unavailable", "active project path could not be resolved" );

    wxString rootPath = root.GetPathWithSep();
#ifdef __WXMSW__
    rootPath.MakeLower();
#endif

    const auto insideRoot = [&]( const wxFileName& aPath )
    {
        wxString candidate = aPath.GetFullPath();
#ifdef __WXMSW__
        candidate.MakeLower();
#endif
        return candidate.StartsWith( rootPath );
    };

    const auto relativeTo = [&]( const wxFileName& aPath )
    {
        wxFileName relative( aPath );
        relative.MakeRelativeTo( root.GetFullPath() );
        return relative.GetFullPath( wxPATH_UNIX ).ToStdString();
    };

    // Resolve an existing project-confined PDF named by a project-relative path.
    const auto resolveProjectPdf = [&]( const std::string& aRelative, wxFileName& aResolved,
                                        std::string& aError )
    {
        wxFileName candidate( wxString::FromUTF8( aRelative ) );

        if( aRelative.empty() || aRelative.size() > 4096
            || aRelative.find( '\0' ) != std::string::npos || candidate.IsAbsolute() )
        {
            aError = "document.path must be project-relative";
            return false;
        }

        candidate.MakeAbsolute( root.GetFullPath() );
        candidate.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

        if( !candidate.FileExists() )
        {
            aError = "document does not exist: " + aRelative;
            return false;
        }

        if( !KICHAD::CODEX_TOOLS::CanonicalizeExisting( candidate ) || !insideRoot( candidate ) )
        {
            aError = "document.path resolves outside the active project";
            return false;
        }

        if( candidate.GetExt().Lower() != wxS( "pdf" ) || !hasPdfSignature( candidate ) )
        {
            aError = "document.path is not a PDF document";
            return false;
        }

        wxFile file( candidate.GetFullPath(), wxFile::read );

        if( !file.IsOpened() || file.Length() > MAX_DOCUMENT_BYTES )
        {
            aError = "documents are limited to 64 MiB";
            return false;
        }

        aResolved = candidate;
        return true;
    };

    // Install a validated PDF into datasheets/<name> and describe it.
    const auto installDatasheet = [&]( const wxFileName& aStaged, const std::string& aName,
                                       const std::string& aOrigin, JSON& aPayload,
                                       std::string& aError )
    {
        if( !validDatasheetName( aName ) || lowerAscii( aName ).rfind( ".pdf" ) != aName.size() - 4 )
        {
            aError = "document.name must be a plain file name ending in .pdf";
            return false;
        }

        wxFileName directory = wxFileName::DirName( root.GetFullPath() );
        directory.AppendDir( wxString::FromUTF8( DATASHEET_DIRECTORY ) );

        if( !directory.DirExists()
            && !wxFileName::Mkdir( directory.GetFullPath(), 0755, wxPATH_MKDIR_FULL ) )
        {
            aError = "could not create the datasheets directory";
            return false;
        }

        wxFileName destination( directory.GetFullPath(), wxString::FromUTF8( aName ) );

        if( destination.FileExists() && !hasPdfSignature( destination ) )
        {
            aError = "datasheets/" + aName + " exists and is not a PDF; choose another name";
            return false;
        }

        if( destination.FileExists() && !wxRemoveFile( destination.GetFullPath() ) )
        {
            aError = "could not replace the existing datasheet";
            return false;
        }

        if( !wxCopyFile( aStaged.GetFullPath(), destination.GetFullPath(), true ) )
        {
            aError = "could not copy the document into the project";
            return false;
        }

        wxFile file( destination.GetFullPath(), wxFile::read );
        int    pages = 0;
        std::string pageError;
        const bool counted = pageCount( destination, pages, pageError );

        aPayload = { { "operation", operation },
                     { "path", std::string( DATASHEET_DIRECTORY ) + "/" + aName },
                     { "bytes", static_cast<uint64_t>( file.IsOpened() ? file.Length() : 0 ) },
                     { "sha256", sha256Of( destination ) },
                     { "origin", aOrigin } };

        if( counted )
            aPayload["pages"] = pages;
        else
            aPayload["pagesUnavailable"] = pageError;

        return true;
    };

    if( operation == "list" )
    {
        JSON documents = JSON::array();
        wxArrayString files;
        wxDir::GetAllFiles( root.GetFullPath(), &files, wxS( "*.pdf" ), wxDIR_FILES | wxDIR_DIRS );
        files.Sort();
        size_t seen = 0;

        for( const wxString& path : files )
        {
            wxFileName file( path );
            const std::string relative = relativeTo( file );

            if( relative.rfind( ".kichad/", 0 ) == 0 || relative.rfind( "fabrication/", 0 ) == 0 )
                continue;

            if( seen++ >= MAX_LIST_ENTRIES )
                break;

            wxFile handle( path, wxFile::read );
            documents.push_back(
                    { { "path", relative },
                      { "bytes", static_cast<uint64_t>( handle.IsOpened() ? handle.Length() : 0 ) },
                      { "pdf", hasPdfSignature( file ) } } );
        }

        return success( { { "operation", "list" },
                          { "documents", std::move( documents ) },
                          { "truncated", seen > MAX_LIST_ENTRIES },
                          { "datasheetDirectory", DATASHEET_DIRECTORY } } );
    }

    if( operation == "import" )
    {
        if( !aArguments.contains( "source" ) || !aArguments["source"].is_string() )
            return failure( "invalid_arguments", "document.source is required for import" );

        const std::string source = aArguments["source"].get<std::string>();

        if( source.empty() || source.size() > 4096 || source.find( '\0' ) != std::string::npos )
            return failure( "invalid_arguments", "document.source must be a file path" );

        wxFileName candidate( wxString::FromUTF8( source ) );

        if( !candidate.IsAbsolute() )
            candidate.MakeAbsolute( root.GetFullPath() );

        candidate.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

        if( !candidate.FileExists() )
            return failure( "invalid_path", "the source document does not exist: " + source );

        if( !KICHAD::CODEX_TOOLS::CanonicalizeExisting( candidate ) )
            return failure( "invalid_path", "the source document could not be resolved" );

        // A user-named absolute path is honoured only below their home directory or the
        // project itself, so the agent cannot roam the rest of the filesystem.
        wxFileName home = wxFileName::DirName( wxGetHomeDir() );
        home.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
        KICHAD::CODEX_TOOLS::CanonicalizeExisting( home, true );
        wxString homePath = home.GetPathWithSep();
        wxString candidatePath = candidate.GetFullPath();
#ifdef __WXMSW__
        homePath.MakeLower();
        candidatePath.MakeLower();
#endif

        if( !candidatePath.StartsWith( homePath ) && !insideRoot( candidate ) )
        {
            return failure( "invalid_path",
                            "document.source must lie below the user's home directory or "
                            "inside the project" );
        }

        if( candidate.GetExt().Lower() != wxS( "pdf" ) || !hasPdfSignature( candidate ) )
            return failure( "invalid_source", "the source document is not a PDF" );

        wxFile file( candidate.GetFullPath(), wxFile::read );

        if( !file.IsOpened() || file.Length() > MAX_DOCUMENT_BYTES )
            return failure( "file_too_large", "documents are limited to 64 MiB" );

        const std::string name = aArguments.contains( "name" ) && aArguments["name"].is_string()
                                         ? aArguments["name"].get<std::string>()
                                         : candidate.GetFullName().ToStdString();
        JSON        payload;
        std::string error;

        if( !installDatasheet( candidate, name, candidate.GetFullPath().ToStdString(), payload,
                               error ) )
            return failure( "import_failed", error );

        return success( payload );
    }

    if( operation == "fetch" )
    {
        if( !aArguments.contains( "url" ) || !aArguments["url"].is_string() )
            return failure( "invalid_arguments", "document.url is required for fetch" );

        const std::string url = aArguments["url"].get<std::string>();

        if( url.size() > 2048 || lowerAscii( url ).rfind( "https://", 0 ) != 0 )
            return failure( "invalid_arguments", "document.url must be an https:// URL" );

        std::string name;

        if( aArguments.contains( "name" ) && aArguments["name"].is_string() )
        {
            name = aArguments["name"].get<std::string>();
        }
        else
        {
            std::string basename = url.substr( url.find_last_of( '/' ) + 1 );
            basename = basename.substr( 0, basename.find_first_of( "?#" ) );

            if( lowerAscii( basename ).rfind( ".pdf" ) != basename.size() - 4 || basename.size() < 5 )
                basename = "download.pdf";

            name = basename;
        }

        if( !validDatasheetName( name ) )
            return failure( "invalid_arguments", "document.name must be a plain .pdf file name" );

        KICHAD::CODEX_TOOLS::PRIVATE_TEMPORARY_DIRECTORY temporary;
        std::string                                      error;

        if( !temporary.Create( "kichad-fetch", error ) )
            return failure( "fetch_failed", error );

        wxFileName staged( wxString::FromUTF8( temporary.Path().string() ), wxS( "download.pdf" ) );
        uint64_t   received = 0;
        bool       oversized = false;

        {
            std::ofstream stream( temporary.Path() / "download.pdf", std::ios::binary );

            if( !stream )
                return failure( "fetch_failed", "could not stage the download" );

            KICAD_CURL_EASY curl;
            curl.SetURL( url );
            curl.SetFollowRedirects( true );
            curl.SetUserAgent( "KiChad/1.0 (datasheet fetch)" );
            curl.SetConnectTimeout( 30 );
            curl.SetTimeout( 300 );
            curl.SetStallTimeout( 1024, 60 );
            curl.SetOutputStream( &stream );
            curl.SetTransferCallback(
                    [&]( size_t aDownloadTotal, size_t aDownloaded, size_t, size_t )
                    {
                        received = aDownloaded;

                        if( aDownloadTotal > static_cast<size_t>( MAX_DOCUMENT_BYTES )
                            || aDownloaded > static_cast<size_t>( MAX_DOCUMENT_BYTES ) )
                        {
                            oversized = true;
                            return 1;
                        }

                        return 0;
                    },
                    250000 );

            const int code = curl.Perform();
            const int status = curl.GetResponseStatusCode();

            if( oversized )
                return failure( "file_too_large", "the document exceeds the 64 MiB download limit" );

            const std::string hint =
                    "; distributor and manufacturer sites often block automated downloads, so "
                    "ask the user to download the PDF in their browser and give you its path, "
                    "then use document.import";

            if( code != 0 )
                return failure( "fetch_failed", "download failed: " + curl.GetErrorText( code ) + hint );

            if( status != 200 )
            {
                return failure( "fetch_failed",
                                "the server answered HTTP " + std::to_string( status ) + hint );
            }
        }

        if( !hasPdfSignature( staged ) )
        {
            return failure( "invalid_source",
                            "the server returned something other than a PDF (usually a bot "
                            "challenge page); ask the user to download the PDF in their browser "
                            "and give you its path, then use document.import" );
        }

        JSON payload;

        if( !installDatasheet( staged, name, url, payload, error ) )
            return failure( "fetch_failed", error );

        payload["downloadedBytes"] = received;
        return success( payload );
    }

    // read, search, render: an existing project PDF.
    if( !aArguments.contains( "path" ) || !aArguments["path"].is_string() )
        return failure( "invalid_arguments", "document.path is required for " + operation );

    const std::string relativePath = aArguments["path"].get<std::string>();
    wxFileName        pdf;
    std::string       error;

    if( !resolveProjectPdf( relativePath, pdf, error ) )
        return failure( "invalid_path", error );

    KICHAD::PDF::PDF_TEXT_DOCUMENT document;

    if( !openDocument( pdf, document, error ) )
        return failure( "invalid_source", error );

    const int pages = document.PageCount();
    JSON      payload = { { "operation", operation }, { "path", relativePath }, { "pages", pages } };

    if( !document.Title().empty() )
        payload["title"] = document.Title();

    if( operation == "read" )
    {
        int first = 1;
        int last = std::min<int>( pages, static_cast<int>( MAX_READ_PAGES ) );

        if( aArguments.contains( "pages" ) )
        {
            if( !aArguments["pages"].is_string() )
                return failure( "invalid_arguments", "document.pages must be a string" );

            if( !parsePageRange( aArguments["pages"].get<std::string>(), pages, first, last, error ) )
                return failure( "invalid_arguments", error );
        }

        std::string labelled;
        bool        truncated = false;

        for( int page = first; page <= last; ++page )
        {
            std::string text;

            if( !document.PageText( page, text, error ) )
                return failure( "read_failed", error );

            labelled += "=== page " + std::to_string( page ) + " ===\n" + text + "\n";

            if( labelled.size() > MAX_READ_TEXT_BYTES )
            {
                labelled.resize( MAX_READ_TEXT_BYTES );
                truncated = true;
                break;
            }
        }

        payload["firstPage"] = first;
        payload["lastPage"] = last;
        payload["text"] = std::move( labelled );
        payload["truncated"] = truncated;
        return success( payload );
    }

    if( operation == "search" )
    {
        if( !aArguments.contains( "query" ) || !aArguments["query"].is_string() )
            return failure( "invalid_arguments", "document.query is required for search" );

        const std::string query = aArguments["query"].get<std::string>();

        if( query.empty() || query.size() > MAX_QUERY_CHARS )
            return failure( "invalid_arguments", "document.query must contain 1 to 200 characters" );

        size_t limit = 40;

        if( aArguments.contains( "limit" ) )
        {
            if( !aArguments["limit"].is_number_integer() || aArguments["limit"].get<int>() < 1
                || aArguments["limit"].get<int>() > static_cast<int>( MAX_SEARCH_MATCHES ) )
                return failure( "invalid_arguments", "document.limit must be between 1 and 100" );

            limit = static_cast<size_t>( aArguments["limit"].get<int>() );
        }

        const std::string needle = lowerAscii( query );
        JSON              matches = JSON::array();
        size_t            total = 0;
        size_t            scanned = 0;
        bool              truncated = false;

        for( int page = 1; page <= pages; ++page )
        {
            std::string text;

            if( !document.PageText( page, text, error ) )
                continue;

            scanned += text.size();

            if( scanned > MAX_SEARCH_TEXT_BYTES )
            {
                truncated = true;
                break;
            }

            std::istringstream stream( text );
            std::string        line;

            while( std::getline( stream, line ) )
            {
                if( lowerAscii( line ).find( needle ) == std::string::npos )
                    continue;

                ++total;

                if( matches.size() < limit )
                {
                    size_t begin = line.find_first_not_of( ' ' );
                    std::string trimmed = begin == std::string::npos ? std::string() : line.substr( begin );

                    if( trimmed.size() > 400 )
                        trimmed.resize( 400 );

                    matches.push_back( { { "page", page }, { "text", trimmed } } );
                }
            }
        }

        payload["query"] = query;
        payload["totalMatches"] = total;
        payload["matches"] = std::move( matches );
        payload["textTruncated"] = truncated;
        return success( payload );
    }

    // render
    if( !aArguments.contains( "page" ) || !aArguments["page"].is_number_integer() )
        return failure( "invalid_arguments", "document.page is required for render" );

    const int page = aArguments["page"].get<int>();

    if( page < 1 || page > pages )
        return failure( "invalid_arguments", "document.page must lie within 1-" + std::to_string( pages ) );

    wxFileName previewDirectory = wxFileName::DirName( root.GetFullPath() );
    previewDirectory.AppendDir( wxS( ".kichad" ) );
    previewDirectory.AppendDir( wxS( "previews" ) );

    if( !previewDirectory.DirExists()
        && !wxFileName::Mkdir( previewDirectory.GetFullPath(), 0700, wxPATH_MKDIR_FULL ) )
        return failure( "preview_failed", "could not create the derived preview directory" );

    const std::string digest =
            picosha2::hash256_hex_string( relativePath + ":" + std::to_string( page ) ).substr( 0, 16 );
    const wxString filename = wxString::FromUTF8( "document-" + digest + "-p" + std::to_string( page )
                                                  + ".png" );
    wxFileName preview( previewDirectory.GetFullPath(), filename );

    if( !KICHAD::CODEX_TOOLS::RasterizePdfPreview( pdf, preview, error, page ) )
    {
        return failure( "dependency_unavailable",
                        "page images need the optional poppler pdftoppm utility, which is not "
                        "installed; read or search the page text instead (" + error + ")" );
    }

    wxFile png( preview.GetFullPath(), wxFile::read );
    const wxFileOffset pngBytes = png.IsOpened() ? png.Length() : wxInvalidOffset;

    if( pngBytes <= 0 || pngBytes > MAX_INLINE_PREVIEW_BYTES )
        return failure( "preview_failed", "the page image must contain 1 byte through 8 MiB" );

    std::string image( static_cast<size_t>( pngBytes ), '\0' );

    if( png.Read( image.data(), image.size() ) != pngBytes )
        return failure( "preview_failed", "could not read the complete page image" );

    payload["page"] = page;
    payload["previewPath"] = ".kichad/previews/" + filename.ToStdString();
    payload["previewBytes"] = static_cast<uint64_t>( pngBytes );
    JSON result = success( payload );
    const std::string encoded = wxBase64Encode( image.data(), image.size() ).ToStdString();
    result["contentItems"].push_back(
            { { "type", "inputImage" }, { "imageUrl", "data:image/png;base64," + encoded } } );
    return result;
}
