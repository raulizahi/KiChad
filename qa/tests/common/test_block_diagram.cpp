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

#include <cmath>
#include <kiid.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/utils.h>

using namespace KICHAD::BLOCK_DIAGRAM;


namespace
{

const NODE& node( const DIAGRAM& aDiagram, const std::string& aId )
{
    const NODE* found = aDiagram.FindNode( aId );
    BOOST_REQUIRE_MESSAGE( found, "missing node " + aId );
    return *found;
}


bool overlaps( const NODE& a, const NODE& b )
{
    return std::abs( a.x - b.x ) * 2 < a.width + b.width
           && std::abs( a.y - b.y ) * 2 < a.height + b.height;
}

} // namespace


BOOST_AUTO_TEST_SUITE( BlockDiagram )


BOOST_AUTO_TEST_CASE( ParsesTheMermaidFlowchartSubset )
{
    const std::string source =
            "%% comment line\n"
            "flowchart LR\n"
            "    A[\"Sensor<br/>1080p30\"] -->|\"MIPI CSI-2\"| B(\"Processor\")\n"
            "    B -- SDIO --> C{{Wi-Fi}}\n"
            "    C -.-> D((Antenna)) ==> E[/Access point/]\n"
            "    B --> F[(Flash)] & G[[Boot ROM]]\n"
            "    H{Decision?} --- B\n"
            "    subgraph SOM [\"System on module\"]\n"
            "        direction TB\n"
            "        B\n"
            "        subgraph MEM [Memory]\n"
            "            F\n"
            "        end\n"
            "    end\n"
            "    classDef done fill:#c8e6c9,stroke:#2e7d32,color:#000\n"
            "    class B,F done\n"
            "    style C fill:lightblue\n"
            "    A:::done\n"
            "    click A \"https://example.invalid\"\n"
            "    linkStyle 0 stroke:#f00\n";

    DIAGRAM     diagram;
    std::string error;
    BOOST_REQUIRE_MESSAGE( ParseMermaidFlowchart( source, diagram, error ), error );

    BOOST_CHECK_EQUAL( diagram.direction, "LR" );
    BOOST_CHECK_EQUAL( diagram.nodes.size(), 8 );
    BOOST_CHECK_EQUAL( diagram.links.size(), 7 );
    BOOST_CHECK_EQUAL( diagram.groups.size(), 2 );

    const NODE& a = node( diagram, "A" );
    BOOST_REQUIRE_EQUAL( a.lines.size(), 2 );
    BOOST_CHECK_EQUAL( a.lines[0], "Sensor" );
    BOOST_CHECK_EQUAL( a.lines[1], "1080p30" );
    BOOST_CHECK( a.shape == NODE_SHAPE::RECTANGLE );
    BOOST_REQUIRE( a.style.fill.has_value() );
    BOOST_CHECK_EQUAL( *a.style.fill, "#c8e6c9" );

    BOOST_CHECK( node( diagram, "B" ).shape == NODE_SHAPE::ROUNDED );
    BOOST_CHECK( node( diagram, "C" ).shape == NODE_SHAPE::HEXAGON );
    BOOST_CHECK( node( diagram, "D" ).shape == NODE_SHAPE::CIRCLE );
    BOOST_CHECK( node( diagram, "E" ).shape == NODE_SHAPE::PARALLELOGRAM );
    BOOST_CHECK( node( diagram, "F" ).shape == NODE_SHAPE::CYLINDER );
    BOOST_CHECK( node( diagram, "G" ).shape == NODE_SHAPE::SUBROUTINE );
    BOOST_CHECK( node( diagram, "H" ).shape == NODE_SHAPE::DIAMOND );
    BOOST_CHECK_EQUAL( node( diagram, "E" ).lines.front(), "Access point" );
    BOOST_CHECK_EQUAL( *node( diagram, "C" ).style.fill, "#bbdefb" );
    BOOST_CHECK_EQUAL( *node( diagram, "F" ).style.stroke, "#2e7d32" );

    // Group membership is assigned where the node is first mentioned inside the subgraph.
    BOOST_CHECK_EQUAL( node( diagram, "B" ).group, "SOM" );
    BOOST_CHECK_EQUAL( node( diagram, "F" ).group, "MEM" );
    BOOST_CHECK_EQUAL( diagram.FindGroup( "MEM" )->parent, "SOM" );
    BOOST_CHECK_EQUAL( diagram.FindGroup( "SOM" )->lines.front(), "System on module" );

    const LINK& first = diagram.links[0];
    BOOST_CHECK_EQUAL( first.from, "A" );
    BOOST_CHECK_EQUAL( first.to, "B" );
    BOOST_CHECK( first.arrowTo );
    BOOST_REQUIRE_EQUAL( first.lines.size(), 1 );
    BOOST_CHECK_EQUAL( first.lines[0], "MIPI CSI-2" );

    const LINK& sdio = diagram.links[1];
    BOOST_CHECK_EQUAL( sdio.lines.front(), "SDIO" );
    BOOST_CHECK( sdio.arrowTo );

    BOOST_CHECK( diagram.links[2].style == LINK_STYLE::DOTTED );
    BOOST_CHECK( diagram.links[3].style == LINK_STYLE::THICK );
    BOOST_CHECK_EQUAL( diagram.links[4].to, "F" );
    BOOST_CHECK_EQUAL( diagram.links[5].to, "G" );
    BOOST_CHECK( !diagram.links[6].arrowTo );
}


BOOST_AUTO_TEST_CASE( RejectsMalformedFlowcharts )
{
    DIAGRAM     diagram;
    std::string error;

    BOOST_CHECK( !ParseMermaidFlowchart( "", diagram, error ) );
    BOOST_CHECK( !ParseMermaidFlowchart( "sequenceDiagram\n A->>B: hi\n", diagram, error ) );
    BOOST_CHECK_NE( error.find( "flowchart" ), std::string::npos );
    BOOST_CHECK( !ParseMermaidFlowchart( "flowchart XX\n A --> B\n", diagram, error ) );
    BOOST_CHECK( !ParseMermaidFlowchart( "flowchart TB\n A -->\n", diagram, error ) );
    BOOST_CHECK_NE( error.find( "line 2" ), std::string::npos );
    BOOST_CHECK( !ParseMermaidFlowchart( "flowchart TB\n A[\"open --> B\n", diagram, error ) );
    BOOST_CHECK( !ParseMermaidFlowchart( "flowchart TB\n subgraph X\n A --> B\n", diagram,
                                         error ) );
    BOOST_CHECK_NE( error.find( "end" ), std::string::npos );
    BOOST_CHECK( !ParseMermaidFlowchart( "flowchart TB\n end\n", diagram, error ) );
    BOOST_CHECK( !ParseMermaidFlowchart( "flowchart TB\n", diagram, error ) );
    BOOST_CHECK( !ParseMermaidFlowchart( std::string( MAX_SOURCE_BYTES + 1, 'x' ), diagram,
                                         error ) );
}


BOOST_AUTO_TEST_CASE( LaysOutRankedNonOverlappingNodesAndGroupBoxes )
{
    const std::string source =
            "flowchart TB\n"
            "    A[Start] --> B[Middle] --> C[End]\n"
            "    A --> D[Side branch with a fairly long label]\n"
            "    D --> C\n"
            "    C --> A\n"
            "    subgraph G [Group]\n"
            "        B\n"
            "        D\n"
            "    end\n"
            "    X[Loner]\n";

    DIAGRAM     diagram;
    std::string error;
    BOOST_REQUIRE_MESSAGE( ParseMermaidFlowchart( source, diagram, error ), error );
    LayoutDiagram( diagram );

    BOOST_CHECK_EQUAL( node( diagram, "A" ).rank, 0 );
    BOOST_CHECK_EQUAL( node( diagram, "B" ).rank, 1 );
    BOOST_CHECK_EQUAL( node( diagram, "D" ).rank, 1 );
    BOOST_CHECK_EQUAL( node( diagram, "C" ).rank, 2 );
    BOOST_CHECK_EQUAL( node( diagram, "X" ).rank, 0 );

    BOOST_CHECK_LT( node( diagram, "A" ).y, node( diagram, "B" ).y );
    BOOST_CHECK_LT( node( diagram, "B" ).y, node( diagram, "C" ).y );
    BOOST_CHECK_GT( diagram.width, 100.0 );
    BOOST_CHECK_GT( diagram.height, 60.0 );

    for( size_t i = 0; i < diagram.nodes.size(); ++i )
    {
        BOOST_CHECK_GT( diagram.nodes[i].width, 0.0 );
        BOOST_CHECK_GT( diagram.nodes[i].x - diagram.nodes[i].width / 2, 0.0 );
        BOOST_CHECK_LT( diagram.nodes[i].x + diagram.nodes[i].width / 2, diagram.width );
        BOOST_CHECK_LT( diagram.nodes[i].y + diagram.nodes[i].height / 2, diagram.height );

        for( size_t j = i + 1; j < diagram.nodes.size(); ++j )
        {
            BOOST_CHECK_MESSAGE( !overlaps( diagram.nodes[i], diagram.nodes[j] ),
                                 diagram.nodes[i].id + " overlaps " + diagram.nodes[j].id );
        }
    }

    // The group box encloses its members and no foreign node.
    // Qualified because <winsock2.h> declares a global GROUP on MSW.
    const KICHAD::BLOCK_DIAGRAM::GROUP& group = *diagram.FindGroup( "G" );
    BOOST_CHECK_GT( group.width, 0.0 );

    for( const NODE& n : diagram.nodes )
    {
        const bool inside = n.x - n.width / 2 >= group.x - 0.01
                            && n.x + n.width / 2 <= group.x + group.width + 0.01
                            && n.y - n.height / 2 >= group.y - 0.01
                            && n.y + n.height / 2 <= group.y + group.height + 0.01;
        const bool crosses = n.x - n.width / 2 < group.x + group.width
                             && n.x + n.width / 2 > group.x
                             && n.y - n.height / 2 < group.y + group.height
                             && n.y + n.height / 2 > group.y;

        if( n.group == "G" )
            BOOST_CHECK_MESSAGE( inside, n.id + " should be inside the group box" );
        else
            BOOST_CHECK_MESSAGE( !crosses, n.id + " must not intersect the group box" );
    }

    for( const LINK& link : diagram.links )
        BOOST_CHECK_GE( link.path.size(), 2 );

    // Horizontal layouts progress along x instead.
    DIAGRAM horizontal;
    BOOST_REQUIRE( ParseMermaidFlowchart( "flowchart LR\n A --> B --> C\n", horizontal, error ) );
    LayoutDiagram( horizontal );
    BOOST_CHECK_LT( node( horizontal, "A" ).x, node( horizontal, "B" ).x );
    BOOST_CHECK_LT( node( horizontal, "B" ).x, node( horizontal, "C" ).x );
    BOOST_CHECK_CLOSE( node( horizontal, "A" ).y, node( horizontal, "C" ).y, 1e-6 );

    DIAGRAM reversed;
    BOOST_REQUIRE( ParseMermaidFlowchart( "flowchart BT\n A --> B\n", reversed, error ) );
    LayoutDiagram( reversed );
    BOOST_CHECK_GT( node( reversed, "A" ).y, node( reversed, "B" ).y );
}


BOOST_AUTO_TEST_CASE( RendersAWellFormedPdf )
{
    DIAGRAM     diagram;
    std::string error;
    BOOST_REQUIRE( ParseMermaidFlowchart(
            "flowchart TB\n A[\"Power in\"] --> B{Regulator OK?}\n B -- yes --> C([Load])\n"
            " B -. no .-> D[(Log)]\n",
            diagram, error ) );
    LayoutDiagram( diagram );

    wxFileName output( wxFileName::GetTempDir(),
                       wxS( "kichad-block-diagram-" ) + KIID().AsString() + wxS( ".pdf" ) );
    BOOST_REQUIRE_MESSAGE( RenderDiagramPdf( diagram, output.GetFullPath(), wxS( "Power path" ),
                                             error ),
                           error );

    wxFile file( output.GetFullPath(), wxFile::read );
    BOOST_REQUIRE( file.IsOpened() );
    std::string bytes( static_cast<size_t>( file.Length() ), '\0' );
    BOOST_REQUIRE_EQUAL( file.Read( bytes.data(), bytes.size() ),
                         static_cast<wxFileOffset>( bytes.size() ) );
    BOOST_CHECK( bytes.starts_with( "%PDF-" ) );
    BOOST_CHECK_NE( bytes.find( "%%EOF" ), std::string::npos );
    BOOST_CHECK_NE( bytes.find( "/Type /Page" ), std::string::npos );
    BOOST_CHECK_GT( bytes.size(), 2048 );
    wxRemoveFile( output.GetFullPath() );
}


BOOST_AUTO_TEST_CASE( RendersSampleDiagramsForInspectionWhenRequested )
{
    wxString directory;

    if( !wxGetEnv( wxS( "KICHAD_QA_DIAGRAM_DIR" ), &directory ) || directory.IsEmpty() )
    {
        BOOST_TEST_MESSAGE( "Skipping opt-in sample diagram rendering" );
        return;
    }

    const std::pair<const char*, const char*> samples[] = {
        { "camera-tb.pdf",
          "flowchart TB\n"
          "    CAM[\"1080p30 camera + lens<br/>Sensor/module and connector TBD\"]\n"
          "    MPU[\"STMicroelectronics STM32MP257FAI3<br/>Dual Cortex-A35 · OpenSTLinux<br/>"
          "TFBGA-436 · 0.8 mm pitch<br/>Camera capture → ISP → H.264 → Streaming\"]\n"
          "    RAM[\"1 GiB DDR memory target<br/>Exact MPN and configuration TBD\"]\n"
          "    FLASH[\"8 GB eMMC target<br/>Linux, application, A/B updates<br/>Exact MPN TBD\"]\n"
          "    WIFI[\"Dual-band Wi-Fi module<br/>2.4 / 5 GHz · Linux driver required<br/>Exact MPN TBD\"]\n"
          "    ANT[\"Antenna + RF connector<br/>Exact parts TBD\"]\n"
          "    AP[\"External Wi-Fi access point\"]\n"
          "    CLIENT[\"Video receiver<br/>1080p30 H.264<br/>RTSP/RTP baseline\"]\n"
          "    DEBUG[\"USB recovery + UART console<br/>JTAG/SWD + boot/reset controls<br/>"
          "Connectors and protection TBD\"]\n"
          "    PWR[\"5 V USB-C input<br/>PMIC + rails\"]\n"
          "    CAM -->|\"2-lane MIPI CSI-2\"| MPU\n"
          "    RAM <-->|\"DDR4\"| MPU\n"
          "    FLASH <-->|\"eMMC HS200\"| MPU\n"
          "    MPU -->|\"SDIO 3.0\"| WIFI\n"
          "    WIFI --> ANT\n"
          "    ANT -. \"802.11ac\" .-> AP\n"
          "    AP ==> CLIENT\n"
          "    DEBUG --- MPU\n"
          "    PWR --> MPU\n"
          "    PWR --> WIFI\n"
          "    classDef done fill:#c8e6c9,stroke:#2e7d32\n"
          "    classDef todo fill:#fff9c4,stroke:#f9a825\n"
          "    class MPU done\n"
          "    class CAM,RAM,FLASH,WIFI,ANT,DEBUG,PWR todo\n" },
        { "nested-lr.pdf",
          "flowchart LR\n"
          "    IN([Input]) --> P\n"
          "    subgraph BOARD [\"Main board\"]\n"
          "        subgraph SOM [\"System on module\"]\n"
          "            P{{\"Processor\"}} --> M[(Memory)]\n"
          "            P --> S[[Storage]]\n"
          "        end\n"
          "        P --> R{\"Regulator OK?\"}\n"
          "        R -- yes --> O[/Output/]\n"
          "        R -- no --> F[\\Fault\\]\n"
          "    end\n"
          "    O --> OUT>Display]\n"
          "    F -.-> IN\n" }
    };

    for( const auto& [file, source] : samples )
    {
        DIAGRAM     diagram;
        std::string error;
        BOOST_REQUIRE_MESSAGE( ParseMermaidFlowchart( source, diagram, error ), error );
        LayoutDiagram( diagram );
        wxFileName output( directory, wxString::FromUTF8( file ) );
        BOOST_REQUIRE_MESSAGE(
                RenderDiagramPdf( diagram, output.GetFullPath(), wxS( "Sample diagram" ), error ),
                error );
        wxFileName png( output );
        png.SetExt( wxS( "png" ) );
        BOOST_REQUIRE_MESSAGE(
                RenderDiagramPng( diagram, png.GetFullPath(), wxS( "Sample diagram" ), 1800, error ),
                error );
        BOOST_TEST_MESSAGE( "Wrote " + output.GetFullPath().ToStdString() );
    }
}


BOOST_AUTO_TEST_SUITE_END()
