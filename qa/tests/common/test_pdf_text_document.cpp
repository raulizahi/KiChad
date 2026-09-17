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

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <kicad/codex/block_diagram.h>
#include <kicad/codex/pdf_text_document.h>

#include <kiid.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>

using KICHAD::PDF::PDF_TEXT_DOCUMENT;


namespace
{

std::string readFile( const wxString& aPath )
{
    wxFile file( aPath, wxFile::read );
    BOOST_REQUIRE_MESSAGE( file.IsOpened(), "cannot open " + aPath.ToStdString() );
    std::string bytes( static_cast<size_t>( file.Length() ), '\0' );
    BOOST_REQUIRE_EQUAL( file.Read( bytes.data(), bytes.size() ),
                         static_cast<wxFileOffset>( bytes.size() ) );
    return bytes;
}

} // namespace


BOOST_AUTO_TEST_SUITE( PdfTextDocument )


BOOST_AUTO_TEST_CASE( ReadsTextFromNativelyPlottedPdf )
{
    KICHAD::BLOCK_DIAGRAM::DIAGRAM diagram;
    std::string                    error;
    BOOST_REQUIRE( KICHAD::BLOCK_DIAGRAM::ParseMermaidFlowchart(
            "flowchart TB\n A[\"Absolute maximum ratings\"] --> B[Pinout table]\n"
            " B -->|\"VDD = 3.3 V\"| C[(Storage)]\n",
            diagram, error ) );
    KICHAD::BLOCK_DIAGRAM::LayoutDiagram( diagram );

    wxFileName output( wxFileName::GetTempDir(),
                       wxS( "kichad-pdf-text-" ) + KIID().AsString() + wxS( ".pdf" ) );
    BOOST_REQUIRE_MESSAGE( KICHAD::BLOCK_DIAGRAM::RenderDiagramPdf(
                                   diagram, output.GetFullPath(), wxS( "Regulator notes" ), error ),
                           error );

    PDF_TEXT_DOCUMENT document;
    BOOST_REQUIRE_MESSAGE( document.Load( readFile( output.GetFullPath() ), error ), error );
    BOOST_CHECK_EQUAL( document.PageCount(), 1 );
    BOOST_CHECK_EQUAL( document.Title(), "Regulator notes" );

    std::string text;
    BOOST_REQUIRE_MESSAGE( document.PageText( 1, text, error ), error );
    BOOST_TEST_MESSAGE( text );
    BOOST_CHECK_NE( text.find( "Absolute maximum ratings" ), std::string::npos );
    BOOST_CHECK_NE( text.find( "Pinout table" ), std::string::npos );
    BOOST_CHECK_NE( text.find( "VDD = 3.3 V" ), std::string::npos );
    BOOST_CHECK_NE( text.find( "Regulator notes" ), std::string::npos );
    BOOST_CHECK( !document.PageText( 2, text, error ) );

    wxRemoveFile( output.GetFullPath() );
}


BOOST_AUTO_TEST_CASE( RejectsNonPdfAndEmptyInput )
{
    PDF_TEXT_DOCUMENT document;
    std::string       error;
    BOOST_CHECK( !document.Load( "", error ) );
    BOOST_CHECK( !document.Load( "(kicad_pcb (version 20260206))", error ) );
    BOOST_CHECK( !document.Load( "%PDF-1.4\n%%EOF\n", error ) );
    BOOST_CHECK_NE( error.find( "pages" ), std::string::npos );
    BOOST_CHECK( !document.Load( "%PDF-1.4\n1 0 obj << /Type /Catalog >> endobj\n"
                                 "trailer << /Root 1 0 R /Encrypt 2 0 R >>\n%%EOF\n",
                                 error ) );
    BOOST_CHECK_NE( error.find( "encrypted" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( ReadsRealDatasheetsWhenRequested )
{
    wxString paths;

    if( !wxGetEnv( wxS( "KICHAD_QA_PDF_PATHS" ), &paths ) || paths.IsEmpty() )
    {
        BOOST_TEST_MESSAGE( "Skipping opt-in real datasheet reading" );
        return;
    }

    wxStringTokenizer tokenizer( paths, wxS( ":" ) );

    while( tokenizer.HasMoreTokens() )
    {
        const wxString path = tokenizer.GetNextToken();
        PDF_TEXT_DOCUMENT document;
        std::string       error;
        BOOST_REQUIRE_MESSAGE( document.Load( readFile( path ), error ), path.ToStdString() + ": " + error );
        BOOST_TEST_MESSAGE( path.ToStdString() + ": " + std::to_string( document.PageCount() )
                            + " pages, title '" + document.Title() + "'" );
        BOOST_CHECK_GT( document.PageCount(), 0 );

        for( int page : { 1, 2, std::min( 5, document.PageCount() ) } )
        {
            std::string text;
            BOOST_REQUIRE_MESSAGE( document.PageText( page, text, error ), error );
            BOOST_TEST_MESSAGE( "--- page " + std::to_string( page ) + " ---\n" + text.substr( 0, 1500 ) );
            BOOST_CHECK_GT( text.size(), 40 );
        }
    }
}


BOOST_AUTO_TEST_SUITE_END()
