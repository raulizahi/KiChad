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

#include <kicad/codex/svg_raster.h>

#include <wx/filename.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>


BOOST_AUTO_TEST_SUITE( SvgRaster )


BOOST_AUTO_TEST_CASE( RasterizesKiCadStyleSvgOverWhite )
{
    // Multi-line style attributes and grouped paths, as KiCad's plotter writes them.
    const std::string svg =
            "<?xml version=\"1.0\" standalone=\"no\"?>\n"
            "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\"100.0000mm\" "
            "height=\"50.0000mm\" viewBox=\"0.0000 0.0000 100.0000 50.0000\">\n"
            "<g style=\"fill:#000000; fill-opacity:1.0000;stroke:#000000; stroke-opacity:1.0000;\n"
            "stroke-width:0.5000; stroke-linecap:round; stroke-linejoin:round;\">\n"
            "<path style=\"fill:none; \n stroke:#FF0000\" d=\"M10.0 10.0\nL90.0 40.0\" />\n"
            "<rect x=\"20\" y=\"20\" width=\"10\" height=\"10\" />\n"
            "</g>\n</svg>\n";

    wxImage     image;
    std::string error;
    BOOST_REQUIRE_MESSAGE( KICHAD::SVG_RASTER::RasterizeSvg( svg, 400, image, error ), error );
    BOOST_CHECK_GT( image.GetWidth(), 100 );
    BOOST_CHECK_GT( image.GetHeight(), 40 );

    // Some pixels are dark (the rectangle) and some are red (the line); the ground is white.
    const unsigned char* data = image.GetData();
    size_t dark = 0;
    size_t red = 0;
    size_t white = 0;

    for( int i = 0; i < image.GetWidth() * image.GetHeight(); ++i )
    {
        const int r = data[i * 3], g = data[i * 3 + 1], b = data[i * 3 + 2];

        if( r < 60 && g < 60 && b < 60 )
            ++dark;
        else if( r > 200 && g < 80 && b < 80 )
            ++red;
        else if( r > 240 && g > 240 && b > 240 )
            ++white;
    }

    BOOST_CHECK_GT( dark, 50 );
    BOOST_CHECK_GT( red, 50 );
    BOOST_CHECK_GT( white, dark );

    BOOST_CHECK( !KICHAD::SVG_RASTER::RasterizeSvg( "", 400, image, error ) );
    BOOST_CHECK( !KICHAD::SVG_RASTER::RasterizeSvg( "<not-svg/>", 400, image, error ) );
}


BOOST_AUTO_TEST_CASE( RasterizesSampleSvgFilesWhenRequested )
{
    wxString inputs;

    if( !wxGetEnv( wxS( "KICHAD_QA_SVG_INPUTS" ), &inputs ) || inputs.IsEmpty() )
    {
        BOOST_TEST_MESSAGE( "Skipping opt-in SVG rasterization samples" );
        return;
    }

    wxStringTokenizer tokenizer( inputs, wxS( ":" ) );

    while( tokenizer.HasMoreTokens() )
    {
        wxFileName svg( tokenizer.GetNextToken() );
        wxFileName png( svg );
        png.SetExt( wxS( "png" ) );
        std::string error;
        BOOST_REQUIRE_MESSAGE( KICHAD::SVG_RASTER::RasterizeSvgFile( svg, png, 2000, error ),
                               svg.GetFullPath().ToStdString() + ": " + error );
        BOOST_TEST_MESSAGE( "Wrote " + png.GetFullPath().ToStdString() );
    }
}


BOOST_AUTO_TEST_SUITE_END()
