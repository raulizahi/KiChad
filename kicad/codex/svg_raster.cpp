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

#include "svg_raster.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

#include <nanosvg.h>
#include <nanosvgrast.h>
#include <wx/file.h>


namespace KICHAD::SVG_RASTER
{

namespace
{

constexpr size_t MAX_SVG_BYTES = 64 * 1024 * 1024;

} // namespace


void CropUniformMargins( wxImage& aImage )
{
    if( !aImage.IsOk() || aImage.GetWidth() < 4 || aImage.GetHeight() < 4 || !aImage.GetData() )
        return;

    const int            width = aImage.GetWidth();
    const int            height = aImage.GetHeight();
    const unsigned char* pixels = aImage.GetData();
    const auto           channel = [&]( int aX, int aY, int aChannel ) -> int
    {
        return pixels[( static_cast<size_t>( aY ) * width + aX ) * 3 + aChannel];
    };
    int background[3];

    for( int c = 0; c < 3; ++c )
    {
        background[c] = ( channel( 0, 0, c ) + channel( width - 1, 0, c ) + channel( 0, height - 1, c )
                          + channel( width - 1, height - 1, c ) )
                        / 4;
    }

    int left = width;
    int top = height;
    int right = -1;
    int bottom = -1;

    for( int y = 0; y < height; ++y )
    {
        for( int x = 0; x < width; ++x )
        {
            const bool content = std::abs( channel( x, y, 0 ) - background[0] ) > 12
                                 || std::abs( channel( x, y, 1 ) - background[1] ) > 12
                                 || std::abs( channel( x, y, 2 ) - background[2] ) > 12;

            if( content )
            {
                left = std::min( left, x );
                top = std::min( top, y );
                right = std::max( right, x );
                bottom = std::max( bottom, y );
            }
        }
    }

    if( right < left || bottom < top )
        return;

    const int padding = std::max( 16, std::min( width, height ) / 40 );
    left = std::max( 0, left - padding );
    top = std::max( 0, top - padding );
    right = std::min( width - 1, right + padding );
    bottom = std::min( height - 1, bottom + padding );

    if( left == 0 && top == 0 && right == width - 1 && bottom == height - 1 )
        return;

    wxImage cropped = aImage.GetSubImage( wxRect( left, top, right - left + 1, bottom - top + 1 ) );

    if( cropped.IsOk() )
        aImage = cropped;
}


bool RasterizeSvg( const std::string& aSvg, int aMaxDimension, wxImage& aImage,
                   std::string& aError )
{
    if( aSvg.empty() || aSvg.size() > MAX_SVG_BYTES )
    {
        aError = "the SVG document is empty or larger than 64 MiB";
        return false;
    }

    // nanosvg mutates its input, so hand it a private copy.
    std::string                 mutableSvg = aSvg;
    NSVGimage*                  image = nsvgParse( mutableSvg.data(), "px", 96.0f );
    std::unique_ptr<NSVGimage, void ( * )( NSVGimage* )> imageGuard( image, nsvgDelete );

    if( !image || image->width <= 0.0f || image->height <= 0.0f )
    {
        aError = "the SVG document could not be parsed";
        return false;
    }

    const float  longest = std::max( image->width, image->height );
    const float  scale = static_cast<float>( std::max( 64, aMaxDimension ) ) / longest;
    const int    width = std::max( 1, static_cast<int>( std::lround( image->width * scale ) ) );
    const int    height = std::max( 1, static_cast<int>( std::lround( image->height * scale ) ) );

    NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
    std::unique_ptr<NSVGrasterizer, void ( * )( NSVGrasterizer* )> rasterizerGuard(
            rasterizer, nsvgDeleteRasterizer );

    if( !rasterizer )
    {
        aError = "could not create the SVG rasterizer";
        return false;
    }

    std::vector<unsigned char> rgba( static_cast<size_t>( width ) * height * 4, 0 );
    nsvgRasterize( rasterizer, image, 0.0f, 0.0f, scale, rgba.data(), width, height, width * 4 );

    aImage = wxImage( width, height );

    if( !aImage.IsOk() )
    {
        aError = "could not allocate the preview image";
        return false;
    }

    unsigned char* rgb = aImage.GetData();

    // Composite over white so transparent plots read like paper.
    for( size_t i = 0; i < static_cast<size_t>( width ) * height; ++i )
    {
        const unsigned alpha = rgba[i * 4 + 3];

        for( int c = 0; c < 3; ++c )
            rgb[i * 3 + c] = static_cast<unsigned char>( ( rgba[i * 4 + c] * alpha + 255 * ( 255 - alpha ) ) / 255 );
    }

    CropUniformMargins( aImage );
    return true;
}


bool RasterizeSvgFile( const wxFileName& aSvg, const wxFileName& aPng, int aMaxDimension,
                       std::string& aError )
{
    wxFile file( aSvg.GetFullPath(), wxFile::read );

    if( !file.IsOpened() || file.Length() <= 0 || file.Length() > static_cast<wxFileOffset>( MAX_SVG_BYTES ) )
    {
        aError = "could not read the SVG plot";
        return false;
    }

    std::string svg( static_cast<size_t>( file.Length() ), '\0' );

    if( file.Read( svg.data(), svg.size() ) != static_cast<wxFileOffset>( svg.size() ) )
    {
        aError = "could not read the complete SVG plot";
        return false;
    }

    wxImage image;

    if( !RasterizeSvg( svg, aMaxDimension, image, aError ) )
        return false;

    if( aPng.FileExists() && !wxRemoveFile( aPng.GetFullPath() ) )
    {
        aError = "could not replace the prior preview";
        return false;
    }

    if( !image.SaveFile( aPng.GetFullPath(), wxBITMAP_TYPE_PNG ) )
    {
        aError = "could not write the preview PNG";
        return false;
    }

    return true;
}

} // namespace KICHAD::SVG_RASTER
