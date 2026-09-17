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

#ifndef KICHAD_SVG_RASTER_H
#define KICHAD_SVG_RASTER_H

#include <string>

#include <wx/filename.h>
#include <wx/image.h>


namespace KICHAD::SVG_RASTER
{

/**
 * Rasterize an SVG document (as produced by kicad-cli or KiCad's SVG plotter) to an RGB image
 * over a white ground with nanosvg, scaling so the longer side is aMaxDimension pixels at most,
 * then crop uniform blank margins so the drawing fills the frame.  No external tool is needed.
 */
bool RasterizeSvg( const std::string& aSvg, int aMaxDimension, wxImage& aImage,
                   std::string& aError );

bool RasterizeSvgFile( const wxFileName& aSvg, const wxFileName& aPng, int aMaxDimension,
                       std::string& aError );

/** Crop uniform-colour margins (sampled from the corners), keeping a small padding. */
void CropUniformMargins( wxImage& aImage );

} // namespace KICHAD::SVG_RASTER

#endif // KICHAD_SVG_RASTER_H
