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

#ifndef KICHAD_BLOCK_DIAGRAM_H
#define KICHAD_BLOCK_DIAGRAM_H

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>


/**
 * Native block-diagram support: a bounded Mermaid flowchart parser, a deterministic layered
 * layout, and a PDF renderer built on KiCad's own PDF plotter.  No external renderer, browser,
 * or JavaScript runtime is involved, so the agent's block diagrams become reviewable PDF
 * documents inside the project instead of Mermaid text the user must render elsewhere.
 */
namespace KICHAD::BLOCK_DIAGRAM
{

enum class NODE_SHAPE
{
    RECTANGLE,
    ROUNDED,
    STADIUM,
    SUBROUTINE,
    CYLINDER,
    CIRCLE,
    DIAMOND,
    HEXAGON,
    ASYMMETRIC,
    PARALLELOGRAM,
    TRAPEZOID
};

enum class LINK_STYLE
{
    SOLID,
    DOTTED,
    THICK
};

/** Optional colours as lowercase "#rrggbb" strings. */
struct STYLE
{
    std::optional<std::string> fill;
    std::optional<std::string> stroke;
    std::optional<std::string> text;
};

struct NODE
{
    std::string              id;
    std::vector<std::string> lines;
    NODE_SHAPE               shape = NODE_SHAPE::RECTANGLE;
    STYLE                    style;
    std::vector<std::string> classes;
    std::string              group;      ///< innermost subgraph id, or empty

    // Layout results in millimetres; x and y are the node centre.
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    int    rank = 0;
};

struct LINK
{
    std::string              from;
    std::string              to;
    std::vector<std::string> lines;
    LINK_STYLE               style = LINK_STYLE::SOLID;
    bool                     arrowTo = true;
    bool                     arrowFrom = false;

    // Layout results in millimetres: an open polyline from source to target.
    std::vector<std::pair<double, double>> path;
};

struct GROUP
{
    std::string              id;
    std::vector<std::string> lines;
    std::string              parent;
    STYLE                    style;

    // Layout results in millimetres; x and y are the top-left corner.
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct DIAGRAM
{
    std::string                  direction = "TB";   ///< TB, BT, LR, or RL
    std::vector<NODE>            nodes;
    std::vector<LINK>            links;
    std::vector<GROUP>           groups;
    std::map<std::string, STYLE> classDefs;

    // Layout results in millimetres.
    double width = 0.0;
    double height = 0.0;

    const NODE* FindNode( const std::string& aId ) const;
    const GROUP* FindGroup( const std::string& aId ) const;
};

constexpr size_t MAX_SOURCE_BYTES = 64 * 1024;
constexpr size_t MAX_NODES = 400;
constexpr size_t MAX_LINKS = 2000;
constexpr size_t MAX_GROUPS = 100;
constexpr size_t MAX_TEXT_LINES = 12;
constexpr size_t MAX_LINE_CHARS = 96;

/**
 * Parse the supported Mermaid flowchart subset: a `flowchart`/`graph` header with direction,
 * node declarations with every standard bracket shape, quoted and unquoted labels with
 * `<br/>` line breaks, solid/dotted/thick links with inline or piped labels and chains,
 * `&` fan-out, nested `subgraph … end`, `classDef`, `class`, `:::` and `style` colours.
 * `linkStyle`, `click`, `direction` and init directives are accepted and ignored.
 */
bool ParseMermaidFlowchart( const std::string& aSource, DIAGRAM& aDiagram,
                            std::string& aError );

/** Assign ranks, order, sizes, positions, group boxes and link paths in millimetres. */
void LayoutDiagram( DIAGRAM& aDiagram );

/** Render a laid-out diagram to a single-page PDF with KiCad's PDF plotter. */
bool RenderDiagramPdf( const DIAGRAM& aDiagram, const wxString& aOutputPath,
                       const wxString& aTitle, std::string& aError );

/** Render the same drawing with KiCad's SVG plotter. */
bool RenderDiagramSvg( const DIAGRAM& aDiagram, const wxString& aOutputPath,
                       const wxString& aTitle, std::string& aError );

/** Render to PNG in-process (SVG plot rasterized with nanosvg); no external tool involved. */
bool RenderDiagramPng( const DIAGRAM& aDiagram, const wxString& aOutputPath,
                       const wxString& aTitle, int aMaxDimension, std::string& aError );

} // namespace KICHAD::BLOCK_DIAGRAM

#endif // KICHAD_BLOCK_DIAGRAM_H
