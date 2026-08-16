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
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <wx/dir.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/base64.h>
#include <wx/image.h>

#include <picosha2.h>


namespace
{

constexpr wxFileOffset MAX_INSPECTION_BYTES = 16 * 1024 * 1024;
constexpr size_t       MAX_EXPRESSION_BYTES = 32 * 1024;
constexpr size_t       MAX_RESULT_BYTES = 256 * 1024;
constexpr size_t       MAX_DISTINCT_HEADS = 512;
constexpr wxFileOffset MAX_INLINE_PREVIEW_BYTES = 8 * 1024 * 1024;
constexpr wxFileOffset MAX_INLINE_PREVIEW_TOTAL_BYTES = 32 * 1024 * 1024;
constexpr size_t       MAX_PREVIEW_HIERARCHY_FILES = 1000;
constexpr wxFileOffset MAX_PREVIEW_HIERARCHY_BYTES = 64 * 1024 * 1024;
constexpr size_t       MAX_ATTACHED_PREVIEW_PAGES = 24;


struct PREVIEW_PAGE
{
    int         number = 1;
    std::string sheetName;
    std::string sheetPath;
    std::string sourcePath;
    double      xMm = 0.0;
    double      yMm = 0.0;
    double      widthMm = 0.0;
    double      heightMm = 0.0;
    bool        hasBounds = false;
    bool        rootChild = false;
};


struct PREVIEW_SOURCE_REVISION
{
    std::string sha256;
    size_t      files = 0;
    uint64_t    bytes = 0;
    std::vector<PREVIEW_PAGE> pages;
};


std::string projectRelativePath( const wxString& aRoot, const wxFileName& aPath )
{
    wxFileName relative = aPath;

    if( !relative.MakeRelativeTo( aRoot ) )
        return {};

    return relative.GetFullPath( wxPATH_UNIX ).ToStdString();
}


bool readPreviewSource( const wxFileName& aPath, std::string& aSource, std::string& aError )
{
    wxFile file( aPath.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
    {
        aError = "could not open hierarchy file " + aPath.GetFullPath().ToStdString();
        return false;
    }

    const wxFileOffset length = file.Length();

    if( length < 0 || length > MAX_INSPECTION_BYTES )
    {
        aError = "hierarchy file exceeds the 16 MiB inspection limit: "
                 + aPath.GetFullPath().ToStdString();
        return false;
    }

    aSource.assign( static_cast<size_t>( length ), '\0' );

    if( length > 0 && file.Read( aSource.data(), static_cast<size_t>( length ) ) != length )
    {
        aError = "could not read hierarchy file " + aPath.GetFullPath().ToStdString();
        return false;
    }

    return true;
}


std::string directProperty( const KICHAD::LOSSLESS_SEXPR_DOCUMENT& aDocument,
                            size_t aParent, const std::string& aName )
{
    const auto& nodes = aDocument.Nodes();

    for( size_t child : nodes.at( aParent ).children )
    {
        const auto& property = nodes.at( child );

        if( aDocument.ListHead( child ) == "property" && property.children.size() >= 3
            && aDocument.AtomText( property.children[1] ) == aName )
        {
            return aDocument.AtomText( property.children[2] );
        }
    }

    return {};
}


bool directPageNumber( const KICHAD::LOSSLESS_SEXPR_DOCUMENT& aDocument,
                       size_t aPathNode, int& aPage )
{
    const auto& nodes = aDocument.Nodes();

    for( size_t child : nodes.at( aPathNode ).children )
    {
        const auto& page = nodes.at( child );

        if( aDocument.ListHead( child ) != "page" || page.children.size() < 2 )
            continue;

        const std::string text = aDocument.AtomText( page.children[1] );
        int parsed = 0;
        const auto result = std::from_chars( text.data(), text.data() + text.size(), parsed );

        if( result.ec == std::errc() && result.ptr == text.data() + text.size()
            && parsed >= 1 && parsed <= 1000 )
        {
            aPage = parsed;
            return true;
        }
    }

    return false;
}


bool directPair( const KICHAD::LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aParent,
                 const std::string& aHead, double& aFirst, double& aSecond )
{
    const auto& nodes = aDocument.Nodes();

    for( size_t child : nodes.at( aParent ).children )
    {
        const auto& pair = nodes.at( child );

        if( aDocument.ListHead( child ) != aHead || pair.children.size() < 3 )
            continue;

        const std::string first = aDocument.AtomText( pair.children[1] );
        const std::string second = aDocument.AtomText( pair.children[2] );
        const auto firstResult = std::from_chars( first.data(), first.data() + first.size(),
                                                  aFirst );
        const auto secondResult = std::from_chars( second.data(), second.data() + second.size(),
                                                   aSecond );
        return firstResult.ec == std::errc()
               && firstResult.ptr == first.data() + first.size()
               && secondResult.ec == std::errc()
               && secondResult.ptr == second.data() + second.size();
    }

    return false;
}


bool addPreviewPage( std::map<int, PREVIEW_PAGE>& aPages, PREVIEW_PAGE aPage,
                     std::string& aError )
{
    auto existing = aPages.find( aPage.number );

    if( existing == aPages.end() )
    {
        aPages.emplace( aPage.number, std::move( aPage ) );
        return true;
    }

    const PREVIEW_PAGE& candidate = existing->second;

    if( candidate.sheetPath == aPage.sheetPath && candidate.sourcePath == aPage.sourcePath )
        return true;

    aError = "schematic hierarchy assigns page " + std::to_string( aPage.number )
             + " to more than one sheet instance";
    return false;
}


bool collectDocumentPreviewPages( const KICHAD::LOSSLESS_SEXPR_DOCUMENT& aDocument,
                                  const wxFileName& aDocumentPath,
                                  const wxString& aProjectRoot,
                                  const std::string& aProjectName,
                                  bool aRootDocument,
                                  std::map<int, PREVIEW_PAGE>& aPages,
                                  std::string& aError )
{
    if( aDocument.Roots().empty() )
        return true;

    const auto& nodes = aDocument.Nodes();
    const size_t root = aDocument.Roots().front();

    for( size_t sheetNode : nodes.at( root ).children )
    {
        if( aDocument.ListHead( sheetNode ) != "sheet" )
            continue;

        const std::string sheetName = directProperty( aDocument, sheetNode, "Sheetname" );
        const std::string sheetFile = directProperty( aDocument, sheetNode, "Sheetfile" );
        double xMm = 0.0;
        double yMm = 0.0;
        double widthMm = 0.0;
        double heightMm = 0.0;
        const bool hasBounds = directPair( aDocument, sheetNode, "at", xMm, yMm )
                               && directPair( aDocument, sheetNode, "size",
                                              widthMm, heightMm )
                               && widthMm > 0.0 && heightMm > 0.0;

        if( sheetFile.empty() )
            continue;

        wxFileName source( aDocumentPath.GetPathWithSep() + wxString::FromUTF8( sheetFile ) );
        source.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
        const std::string sourcePath = projectRelativePath( aProjectRoot, source );

        for( size_t instancesNode : nodes.at( sheetNode ).children )
        {
            if( aDocument.ListHead( instancesNode ) != "instances" )
                continue;

            std::vector<size_t> projects;
            std::vector<size_t> selectedProjects;

            for( size_t projectNode : nodes.at( instancesNode ).children )
            {
                const auto& project = nodes.at( projectNode );

                if( aDocument.ListHead( projectNode ) != "project"
                    || project.children.size() < 2 )
                    continue;

                projects.push_back( projectNode );

                if( aDocument.AtomText( project.children[1] ) == aProjectName )
                    selectedProjects.push_back( projectNode );
            }

            if( selectedProjects.empty() && projects.size() == 1 )
                selectedProjects = projects;

            for( size_t projectNode : selectedProjects )
            {
                const auto& project = nodes.at( projectNode );

                for( size_t pathNode : project.children )
                {
                    const auto& path = nodes.at( pathNode );

                    if( aDocument.ListHead( pathNode ) != "path" || path.children.size() < 2 )
                        continue;

                    int page = 0;

                    if( !directPageNumber( aDocument, pathNode, page ) )
                        continue;

                    if( !addPreviewPage( aPages,
                                         { page, sheetName,
                                           aDocument.AtomText( path.children[1] ), sourcePath,
                                           xMm, yMm, widthMm, heightMm, hasBounds,
                                           aRootDocument },
                                         aError ) )
                    {
                        return false;
                    }
                }
            }
        }
    }

    return true;
}


bool previewSourceRevision( const wxString& aRoot, const wxFileName& aRootPath,
                            const KICHAD::LOSSLESS_SEXPR_DOCUMENT& aRootDocument,
                            bool aFollowSchematicHierarchy, PREVIEW_SOURCE_REVISION& aRevision,
                            std::string& aError )
{
    std::map<std::string, std::string> sources;
    std::map<int, PREVIEW_PAGE>        pages;
    std::set<std::string>              visited;
    wxFileName canonicalRoot = wxFileName::DirName( aRoot );
    canonicalRoot.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    pages.emplace( 1, PREVIEW_PAGE{ 1, aRootPath.GetName().ToStdString(), "/",
                                    projectRelativePath( canonicalRoot.GetFullPath(),
                                                         aRootPath ) } );

    const auto collect = [&]( const auto& self, const wxFileName& aPath,
                              const KICHAD::LOSSLESS_SEXPR_DOCUMENT& aDocument ) -> bool
    {
        const std::string relative = projectRelativePath( canonicalRoot.GetFullPath(), aPath );

        if( relative.empty() )
        {
            aError = "preview source does not resolve inside the active project";
            return false;
        }

        if( !visited.emplace( aPath.GetFullPath().ToStdString() ).second )
            return true;

        if( visited.size() > MAX_PREVIEW_HIERARCHY_FILES )
        {
            aError = "schematic preview hierarchy exceeds 1000 files";
            return false;
        }

        if( aRevision.bytes + aDocument.Source().size()
            > static_cast<uint64_t>( MAX_PREVIEW_HIERARCHY_BYTES ) )
        {
            aError = "schematic preview hierarchy exceeds 64 MiB";
            return false;
        }

        aRevision.bytes += aDocument.Source().size();
        sources.emplace( relative, aDocument.Source() );

        if( aFollowSchematicHierarchy
            && !collectDocumentPreviewPages( aDocument, aPath,
                                             canonicalRoot.GetFullPath(),
                                             aRootPath.GetName().ToStdString(),
                                             aPath.GetFullPath()
                                                     == aRootPath.GetFullPath(),
                                             pages, aError ) )
        {
            return false;
        }

        if( !aFollowSchematicHierarchy )
            return true;

        for( size_t propertyNode : aDocument.FindLists( "property" ) )
        {
            const auto& node = aDocument.Nodes().at( propertyNode );

            if( node.children.size() < 3
                || node.parent == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
                || aDocument.ListHead( node.parent ) != "sheet"
                || aDocument.AtomText( node.children[1] ) != "Sheetfile" )
            {
                continue;
            }

            const std::string dependencyText = aDocument.AtomText( node.children[2] );
            wxFileName dependencyName( wxString::FromUTF8( dependencyText ) );

            if( dependencyText.empty() || dependencyText.find( '\0' ) != std::string::npos
                || dependencyName.IsAbsolute() )
            {
                aError = "schematic Sheetfile must be a project-contained relative path";
                return false;
            }

            wxFileName dependency( aPath.GetPathWithSep()
                                   + wxString::FromUTF8( dependencyText ) );
            dependency.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
            wxFileName dependencyRelative = dependency;

            if( !dependencyRelative.MakeRelativeTo( canonicalRoot.GetFullPath() ) )
            {
                aError = "schematic Sheetfile could not be resolved relative to its parent";
                return false;
            }

            wxFileName resolved;
            std::string resolveError;
            const std::string dependencyProjectPath =
                    dependencyRelative.GetFullPath( wxPATH_UNIX ).ToStdString();

            if( !KICHAD::CODEX_TOOLS::ResolveProjectFile(
                        canonicalRoot.GetFullPath(), dependencyProjectPath,
                        resolved, resolveError )
                || resolved.GetExt() != wxS( "kicad_sch" ) )
            {
                aError = "could not resolve schematic Sheetfile " + dependencyText + ": "
                         + ( resolveError.empty() ? "not a .kicad_sch file" : resolveError );
                return false;
            }

            if( visited.contains( resolved.GetFullPath().ToStdString() ) )
                continue;

            std::string childSource;

            if( !readPreviewSource( resolved, childSource, aError ) )
                return false;

            std::string parseError;
            std::unique_ptr<KICHAD::LOSSLESS_SEXPR_DOCUMENT> child =
                    KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( std::move( childSource ),
                                                            &parseError );

            if( !child || child->Roots().empty()
                || child->ListHead( child->Roots().front() ) != "kicad_sch" )
            {
                aError = "could not parse schematic Sheetfile " + dependencyText + ": "
                         + ( parseError.empty() ? "invalid kicad_sch root" : parseError );
                return false;
            }

            if( !self( self, resolved, *child ) )
                return false;
        }

        return true;
    };

    if( !collect( collect, aRootPath, aRootDocument ) )
        return false;

    std::string identity;

    for( const auto& [ path, source ] : sources )
    {
        identity += std::to_string( path.size() ) + ":" + path;
        identity += std::to_string( source.size() ) + ":" + source;
    }

    aRevision.sha256 = picosha2::hash256_hex_string( identity );
    aRevision.files = sources.size();

    for( auto& [number, page] : pages )
        aRevision.pages.emplace_back( std::move( page ) );

    return true;
}


struct PREVIEW_BOX
{
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};


bool isPreviewInk( const wxImage& aImage, int aX, int aY,
                   const std::array<int, 3>& aBackground )
{
    const int width = aImage.GetWidth();
    const unsigned char* pixels = aImage.GetData();
    const size_t index = ( static_cast<size_t>( aY ) * width + aX ) * 3;
    return std::abs( static_cast<int>( pixels[index] ) - aBackground[0] ) > 12
           || std::abs( static_cast<int>( pixels[index + 1] ) - aBackground[1] ) > 12
           || std::abs( static_cast<int>( pixels[index + 2] ) - aBackground[2] ) > 12;
}


double previewEdgeCoverage( const wxImage& aImage, bool aHorizontal, int aFixed,
                            int aStart, int aLength,
                            const std::array<int, 3>& aBackground )
{
    int covered = 0;

    for( int offset = 0; offset < aLength; ++offset )
    {
        bool ink = false;

        for( int radius = -2; radius <= 2 && !ink; ++radius )
        {
            const int x = aHorizontal ? aStart + offset : aFixed + radius;
            const int y = aHorizontal ? aFixed + radius : aStart + offset;

            if( x >= 0 && x < aImage.GetWidth() && y >= 0 && y < aImage.GetHeight() )
                ink = isPreviewInk( aImage, x, y, aBackground );
        }

        if( ink )
            ++covered;
    }

    return aLength > 0 ? static_cast<double>( covered ) / aLength : 0.0;
}


bool locateFirstRootSheetBox( const wxImage& aOverview, const PREVIEW_PAGE& aPage,
                              const std::array<int, 3>& aBackground,
                              PREVIEW_BOX& aBox )
{
    constexpr double pixelsPerMm = 160.0 / 25.4;
    const int expectedWidth = std::max( 1, static_cast<int>(
                                                   std::lround( aPage.widthMm
                                                                * pixelsPerMm ) ) );
    const int expectedHeight = std::max( 1, static_cast<int>(
                                                    std::lround( aPage.heightMm
                                                                 * pixelsPerMm ) ) );
    const int widthTolerance = std::max( 5, expectedWidth / 25 );
    const int heightTolerance = std::max( 5, expectedHeight / 25 );
    std::vector<std::pair<int, std::pair<int, int>>> horizontalLines;

    for( int y = 0; y < aOverview.GetHeight(); ++y )
    {
        int runStart = -1;
        int lastInk = -1;
        int gap = 0;

        for( int x = 0; x <= aOverview.GetWidth(); ++x )
        {
            const bool ink = x < aOverview.GetWidth()
                             && isPreviewInk( aOverview, x, y, aBackground );

            if( ink )
            {
                if( runStart < 0 )
                    runStart = x;

                lastInk = x;
                gap = 0;
            }
            else if( runStart >= 0 && ++gap > 2 )
            {
                const int runWidth = lastInk - runStart + 1;

                if( std::abs( runWidth - expectedWidth ) <= widthTolerance )
                    horizontalLines.emplace_back( y, std::make_pair( runStart, lastInk ) );

                runStart = -1;
                lastInk = -1;
                gap = 0;
            }
        }
    }

    bool found = false;

    for( const auto& top : horizontalLines )
    {
        for( const auto& bottom : horizontalLines )
        {
            const int height = bottom.first - top.first;

            if( height < expectedHeight - heightTolerance
                || height > expectedHeight + heightTolerance
                || std::abs( bottom.second.first - top.second.first ) > widthTolerance
                || std::abs( bottom.second.second - top.second.second ) > widthTolerance )
            {
                continue;
            }

            const int left = ( top.second.first + bottom.second.first ) / 2;
            const int right = ( top.second.second + bottom.second.second ) / 2;

            if( previewEdgeCoverage( aOverview, false, left, top.first, height + 1,
                                     aBackground ) < 0.75
                || previewEdgeCoverage( aOverview, false, right, top.first, height + 1,
                                        aBackground ) < 0.75 )
            {
                continue;
            }

            PREVIEW_BOX candidate{ left, top.first, right - left + 1, height + 1 };

            if( !found || candidate.top < aBox.top
                || ( candidate.top == aBox.top && candidate.left < aBox.left ) )
            {
                aBox = candidate;
                found = true;
            }
        }
    }

    return found;
}


bool composeRootHierarchyOverview( const std::vector<PREVIEW_PAGE>& aPages,
                                   const std::vector<wxFileName>& aPreviews,
                                   bool& aComposed, std::string& aError )
{
    aComposed = false;

    if( aPages.size() != aPreviews.size() || aPages.empty()
        || aPages.front().number != 1 )
    {
        return true;
    }

    std::vector<size_t> rootChildren;

    for( size_t i = 1; i < aPages.size(); ++i )
    {
        if( aPages[i].rootChild && aPages[i].hasBounds )
            rootChildren.push_back( i );
    }

    if( rootChildren.empty() )
        return true;

    std::sort( rootChildren.begin(), rootChildren.end(),
               [&]( size_t aLeft, size_t aRight )
               {
                   const PREVIEW_PAGE& left = aPages[aLeft];
                   const PREVIEW_PAGE& right = aPages[aRight];
                   return left.yMm != right.yMm ? left.yMm < right.yMm
                                                : left.xMm < right.xMm;
               } );

    wxImage overview;

    if( !overview.LoadFile( aPreviews.front().GetFullPath(), wxBITMAP_TYPE_PNG )
        || !overview.IsOk() || !overview.GetData() )
    {
        aError = "could not load the root schematic preview for hierarchy composition";
        return false;
    }

    const int width = overview.GetWidth();
    const int height = overview.GetHeight();
    const unsigned char* pixels = overview.GetData();
    const auto channel = [&]( int aX, int aY, int aChannel ) -> int
    {
        return pixels[( static_cast<size_t>( aY ) * width + aX ) * 3 + aChannel];
    };
    const std::array<int, 3> background = {
        ( channel( 0, 0, 0 ) + channel( width - 1, 0, 0 )
          + channel( 0, height - 1, 0 ) + channel( width - 1, height - 1, 0 ) ) / 4,
        ( channel( 0, 0, 1 ) + channel( width - 1, 0, 1 )
          + channel( 0, height - 1, 1 ) + channel( width - 1, height - 1, 1 ) ) / 4,
        ( channel( 0, 0, 2 ) + channel( width - 1, 0, 2 )
          + channel( 0, height - 1, 2 ) + channel( width - 1, height - 1, 2 ) ) / 4
    };
    const PREVIEW_PAGE& firstPage = aPages[rootChildren.front()];
    PREVIEW_BOX firstBox;

    if( !locateFirstRootSheetBox( overview, firstPage, background, firstBox ) )
    {
        aError = "native root preview did not contain the expected hierarchical sheet boxes";
        return false;
    }

    constexpr double pixelsPerMm = 160.0 / 25.4;
    const double originX = firstBox.left - firstPage.xMm * pixelsPerMm;
    const double originY = firstBox.top - firstPage.yMm * pixelsPerMm;

    for( size_t index : rootChildren )
    {
        const PREVIEW_PAGE& page = aPages[index];
        PREVIEW_BOX box{
            static_cast<int>( std::lround( originX + page.xMm * pixelsPerMm ) ),
            static_cast<int>( std::lround( originY + page.yMm * pixelsPerMm ) ),
            static_cast<int>( std::lround( page.widthMm * pixelsPerMm ) ) + 1,
            static_cast<int>( std::lround( page.heightMm * pixelsPerMm ) ) + 1
        };

        if( box.left < 0 || box.top < 0 || box.left + box.width > width
            || box.top + box.height > height
            || previewEdgeCoverage( overview, true, box.top, box.left, box.width,
                                    background ) < 0.7
            || previewEdgeCoverage( overview, true, box.top + box.height - 1,
                                    box.left, box.width, background ) < 0.7
            || previewEdgeCoverage( overview, false, box.left, box.top, box.height,
                                    background ) < 0.7
            || previewEdgeCoverage( overview, false, box.left + box.width - 1,
                                    box.top, box.height, background ) < 0.7 )
        {
            aError = "native root preview sheet geometry does not match the schematic hierarchy";
            return false;
        }

        wxImage child;

        if( !child.LoadFile( aPreviews[index].GetFullPath(), wxBITMAP_TYPE_PNG )
            || !child.IsOk() || child.GetWidth() <= 0 || child.GetHeight() <= 0 )
        {
            aError = "could not load every child schematic preview for hierarchy composition";
            return false;
        }

        const int inset = std::max( 5, std::min( box.width, box.height ) / 45 );
        const int availableWidth = box.width - inset * 2;
        const int availableHeight = box.height - inset * 2;
        const double scale = std::min(
                static_cast<double>( availableWidth ) / child.GetWidth(),
                static_cast<double>( availableHeight ) / child.GetHeight() );
        const int childWidth = std::max( 1, static_cast<int>(
                                                   std::lround( child.GetWidth() * scale ) ) );
        const int childHeight = std::max( 1, static_cast<int>(
                                                    std::lround( child.GetHeight() * scale ) ) );
        child.Rescale( childWidth, childHeight, wxIMAGE_QUALITY_HIGH );
        const int childX = box.left + ( box.width - childWidth ) / 2;
        const int childY = box.top + ( box.height - childHeight ) / 2;
        overview.Paste( child, childX, childY );
    }

    if( !overview.SaveFile( aPreviews.front().GetFullPath(), wxBITMAP_TYPE_PNG ) )
    {
        aError = "could not save the composed root schematic hierarchy preview";
        return false;
    }

    aComposed = true;
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
              { "enum", nlohmann::json::array( { "summary", "find", "render" } ) } };
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
              { "description", "One-based schematic page. Omit to render the hierarchy." } };

    return { { "type", "function" },
             { "name", "inspect" },
             { "description",
               "Inspect a KiCad 10 schematic, board, symbol library, or footprint without "
               "changing it. Use 'summary' for structural counts, 'find' for bounded raw "
               "expressions, or 'render' to directly attach native schematic hierarchy pages, a "
               "production-layer PCB, an assembly-layout PCB, or a 3D PCB view." },
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

    if( operation != "summary" && operation != "find" && operation != "render" )
    {
        return failure( "invalid_arguments",
                        "inspect.operation must be 'summary', 'find', or 'render'" );
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

    if( operation == "render" )
    {
        if( !aArguments.contains( "view" ) || !aArguments["view"].is_string() )
            return failure( "invalid_arguments", "inspect.view is required for render" );

        const std::string view = aArguments["view"].get<std::string>();
        int page = 1;
        const bool pageSpecified = aArguments.contains( "page" );

        if( pageSpecified )
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
        previewDirectory.AppendDir( wxS( ".kichad" ) );
        previewDirectory.AppendDir( wxS( "previews" ) );

        if( !previewDirectory.DirExists()
            && !wxFileName::Mkdir( previewDirectory.GetFullPath(), 0700,
                                  wxPATH_MKDIR_FULL ) )
        {
            return failure( "preview_failed", "could not create the derived preview directory" );
        }

        PREVIEW_SOURCE_REVISION revision;

        if( !previewSourceRevision( root, resolved, *document, schematic,
                                    revision, pathError ) )
        {
            return failure( "preview_failed", pathError );
        }

        std::vector<PREVIEW_PAGE> requestedPages;

        if( schematic && !pageSpecified )
        {
            const size_t count = std::min( revision.pages.size(),
                                           MAX_ATTACHED_PREVIEW_PAGES );
            requestedPages.assign( revision.pages.begin(), revision.pages.begin() + count );
        }
        else
        {
            auto metadata = std::find_if(
                    revision.pages.begin(), revision.pages.end(),
                    [&]( const PREVIEW_PAGE& aCandidate )
                    {
                        return aCandidate.number == page;
                    } );

            if( metadata != revision.pages.end() )
                requestedPages.push_back( *metadata );
            else
                requestedPages.push_back( { page, {}, {}, relativePath } );
        }

        if( requestedPages.empty() )
            return failure( "preview_failed", "schematic hierarchy has no renderable pages" );

        const std::string selection = schematic && !pageSpecified
                                              ? "hierarchy"
                                              : std::to_string( page );
        const std::string identity = relativePath + ":" + view + ":" + selection;
        const std::string requestDigest =
                picosha2::hash256_hex_string( identity ).substr( 0, 16 );
        const std::string sourceDigest = revision.sha256.substr( 0, 16 );
        std::vector<int> pages;
        std::vector<wxFileName> previews;
        std::set<wxString> currentNames;

        for( const PREVIEW_PAGE& requested : requestedPages )
        {
            const wxString filename = wxString::FromUTF8(
                    "preview-" + requestDigest + "-" + sourceDigest + "-p"
                    + std::to_string( requested.number ) + "-" + view + ".png" );
            pages.push_back( requested.number );
            previews.emplace_back( previewDirectory.GetFullPath(), filename );
            currentNames.emplace( filename );
        }

        const bool rendered = m_nativePreviewRunner
                                      ? m_nativePreviewRunner(
                                                view, resolved, pages, previews, pathError )
                                      : KICHAD::CODEX_TOOLS::RunNativeKiCadPreview(
                                                view, resolved, pages, previews, pathError );

        if( !rendered )
        {
            return failure( "preview_failed", pathError );
        }

        bool hierarchyOverviewComposed = false;

        if( schematic && !pageSpecified
            && !composeRootHierarchyOverview( requestedPages, previews,
                                              hierarchyOverviewComposed, pathError ) )
        {
            return failure( "preview_failed", pathError );
        }

        std::vector<std::string> pngPages;
        std::vector<wxFileOffset> previewSizes;
        wxFileOffset totalPreviewBytes = 0;

        for( const wxFileName& preview : previews )
        {
            wxFile previewFile( preview.GetFullPath(), wxFile::read );
            const wxFileOffset previewBytes = previewFile.IsOpened()
                                                      ? previewFile.Length()
                                                      : wxInvalidOffset;

            if( previewBytes <= 0 || previewBytes > MAX_INLINE_PREVIEW_BYTES
                || totalPreviewBytes + previewBytes > MAX_INLINE_PREVIEW_TOTAL_BYTES )
            {
                return failure( "preview_failed",
                                "native preview pages must each be at most 8 MiB and "
                                "at most 32 MiB in total" );
            }

            std::string png( static_cast<size_t>( previewBytes ), '\0' );

            if( previewFile.Read( png.data(), png.size() ) != previewBytes )
                return failure( "preview_failed", "could not read every native preview page" );

            totalPreviewBytes += previewBytes;
            previewSizes.push_back( previewBytes );
            pngPages.emplace_back( std::move( png ) );
        }

        payload["view"] = view;
        payload["sourceSha256"] = revision.sha256;
        payload["sourceFiles"] = revision.files;
        payload["sourceBytes"] = revision.bytes;
        payload["pageCount"] = schematic ? revision.pages.size() : 1;
        payload["renderedPages"] = requestedPages.size();
        payload["pagesTruncated"] = schematic && !pageSpecified
                                    && revision.pages.size() > requestedPages.size();
        payload["hierarchyOverviewComposed"] = hierarchyOverviewComposed;
        payload["previewBytes"] = static_cast<uint64_t>( totalPreviewBytes );
        payload["derived"] = true;
        payload["pages"] = JSON::array();

        for( size_t i = 0; i < requestedPages.size(); ++i )
        {
            const PREVIEW_PAGE& requested = requestedPages[i];
            JSON pageData = { { "page", requested.number },
                              { "contentItemIndex", i + 1 },
                              { "previewBytes", static_cast<uint64_t>( previewSizes[i] ) } };

            if( !requested.sheetName.empty() )
                pageData["sheetName"] = requested.sheetName;

            if( !requested.sheetPath.empty() )
                pageData["sheetPath"] = requested.sheetPath;

            if( !requested.sourcePath.empty() )
                pageData["sourcePath"] = requested.sourcePath;

            if( requested.hasBounds )
            {
                pageData["boundsMm"] = {
                    { "x", requested.xMm }, { "y", requested.yMm },
                    { "width", requested.widthMm }, { "height", requested.heightMm }
                };
            }

            payload["pages"].push_back( std::move( pageData ) );
        }

        if( requestedPages.size() == 1 )
            payload["page"] = requestedPages.front().number;

        wxDir derivedDirectory( previewDirectory.GetFullPath() );
        wxString staleName;
        size_t removed = 0;
        const wxString stalePattern = wxString::FromUTF8(
                "preview-" + requestDigest + "-*-" + view + ".png" );
        bool hasStale = derivedDirectory.IsOpened()
                        && derivedDirectory.GetFirst( &staleName, stalePattern, wxDIR_FILES );

        while( hasStale )
        {
            if( !currentNames.contains( staleName )
                && wxRemoveFile( wxFileName( previewDirectory.GetFullPath(),
                                             staleName ).GetFullPath() ) )
            {
                ++removed;
            }

            hasStale = derivedDirectory.GetNext( &staleName );
        }

        payload["supersededPreviewsRemoved"] = removed;
        JSON result = success( payload );

        for( const std::string& png : pngPages )
        {
            const std::string encoded = wxBase64Encode( png.data(), png.size() ).ToStdString();
            result["contentItems"].push_back(
                    { { "type", "inputImage" },
                      { "imageUrl", "data:image/png;base64," + encoded } } );
        }

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
