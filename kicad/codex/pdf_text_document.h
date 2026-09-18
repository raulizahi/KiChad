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

#ifndef KICHAD_PDF_TEXT_DOCUMENT_H
#define KICHAD_PDF_TEXT_DOCUMENT_H

#include <memory>
#include <string>
#include <vector>


namespace KICHAD::PDF
{

class DOCUMENT_IMPL;

/**
 * A bounded, dependency-free PDF text reader.
 *
 * Objects are indexed by scanning the file for `N G obj` definitions rather than trusting the
 * cross-reference table, so documents with damaged or truncated xref data (a common result of
 * splitting datasheets) still open.  Streams are decoded with Flate, LZW, ASCIIHex, ASCII85 and
 * RunLength filters plus PNG/TIFF predictors; object streams are expanded.  Text is recovered
 * by interpreting page content streams (including form XObjects) with the text matrix, font
 * widths, ToUnicode CMaps and simple-font encodings, then assembling glyph runs into lines by
 * baseline and horizontal gap so tables keep their column separation.
 *
 * Encrypted documents are reported as unsupported rather than mis-read.
 */
class PDF_TEXT_DOCUMENT
{
public:
    PDF_TEXT_DOCUMENT();
    ~PDF_TEXT_DOCUMENT();

    /// Parse the document bytes; returns false with a diagnostic when it cannot be read.
    bool Load( std::string aBytes, std::string& aError );

    int PageCount() const;

    /// Text of one one-based page.  Missing fonts or undecodable streams degrade gracefully.
    bool PageText( int aPage, std::string& aText, std::string& aError ) const;

    /// Document /Info title when present.
    std::string Title() const;

private:
    std::unique_ptr<DOCUMENT_IMPL> m_impl;
};

} // namespace KICHAD::PDF

#endif // KICHAD_PDF_TEXT_DOCUMENT_H
