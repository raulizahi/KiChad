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

#include <boost/test/unit_test.hpp>

#include <kicad/codex/codex_tool_internal.h>
#include <kicad/codex/design_script_compiler.h>
#include <kicad/codex/design_script_pcb_planner.h>
#include <kicad/codex/design_script_schematic_planner.h>
#include <kicad/codex/lossless_sexpr_document.h>

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include <algorithm>
#include <set>
#include <sstream>


namespace
{

const std::string HIERARCHY_PROGRAM = R"KDS((kichad_design
  (version 1)
  (project hierarchy)
  (sheet root
    (parent none)
    (file "hierarchy.kicad_sch")
    (title "Main"))
  (sheet power
    (parent root)
    (file "sheets/power.kicad_sch")
    (title "Power")
    (at 20mm 30mm)
    (size 40mm 20mm)
    (pin VIN input (at 20mm 35mm) (side left))
    (pin VOUT output (at 60mm 35mm) (side right)))
  (sheet monitor
    (parent power)
    (file "sheets/monitor.kicad_sch")
    (title "Monitor")
    (at 80mm 30mm)
    (size 30mm 20mm)
    (pin SENSE input (at 80mm 35mm) (side left)))
))KDS";

} // namespace


BOOST_AUTO_TEST_SUITE( DesignScriptSchematicPlanner )


BOOST_AUTO_TEST_CASE( LowersHierarchyIntoStableNativeExpressionsAndPaths )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( HIERARCHY_PROGRAM );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );

    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT first =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT second =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( first.fullyLowered, first.diagnostics.dump() );
    BOOST_CHECK_EQUAL( first.operations.dump(), second.operations.dump() );
    BOOST_REQUIRE_EQUAL( first.operations.size(), 1 );
    BOOST_CHECK_EQUAL( first.counts["files"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( first.counts["sheets"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( first.counts["pins"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( first.counts["managedItems"].get<int>(), 5 );

    const nlohmann::json& operation = first.operations[0];
    BOOST_CHECK_EQUAL( operation["action"].get<std::string>(),
                       "reconcile_schematic_hierarchy" );
    BOOST_CHECK_EQUAL( operation["rootFile"].get<std::string>(), "hierarchy.kicad_sch" );
    BOOST_REQUIRE_EQUAL( operation["files"].size(), 3 );
    BOOST_CHECK( operation["files"][0]["root"].get<bool>() );
    BOOST_CHECK_EQUAL( operation["files"][1]["path"].get<std::string>(),
                       "sheets/power.kicad_sch" );
    BOOST_CHECK_EQUAL( operation["files"][2]["page"].get<int>(), 3 );

    const std::string rootSource = operation["files"][0]["newDocumentSource"];
    const std::string powerSource = operation["files"][1]["newDocumentSource"];
    BOOST_CHECK_NE( rootSource.find( "(version 20260306)" ), std::string::npos );
    BOOST_CHECK_NE( rootSource.find( "(title \"Main\")" ), std::string::npos );
    BOOST_CHECK_NE( rootSource.find( "(property \"Sheetfile\" \"sheets/power.kicad_sch\"" ),
                    std::string::npos );
    BOOST_CHECK_NE( powerSource.find( "(hierarchical_label \"VIN\"" ), std::string::npos );
    BOOST_CHECK_NE( powerSource.find( "(hierarchical_label \"VOUT\"" ), std::string::npos );
    const std::string expectedNestedParentPath =
            "(path \"/" + operation["files"][0]["screenUuid"].get<std::string>() + "/"
            + operation["managedItems"][0]["uuid"].get<std::string>() + "\"";
    BOOST_CHECK_NE( powerSource.find( expectedNestedParentPath ), std::string::npos );

    for( const nlohmann::json& file : operation["files"] )
    {
        std::string parseError;
        BOOST_CHECK_MESSAGE(
                KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse(
                        file["newDocumentSource"].get<std::string>(), &parseError ),
                parseError );
    }
}


BOOST_AUTO_TEST_CASE( LowersOneCompleteProjectTitleBlockToCurrentNativeSchematic )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project titled
    (title "Production Controller")
    (date "2026-07-19")
    (revision "C")
    (company "KiChad QA")
    (comment 1 "Release candidate")
    (comment 9 "Generated from KDS"))
  (sheet root (parent none) (file "titled.kicad_sch") (title "Root sheet"))
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    const nlohmann::json& file = plan.operations[0]["files"][0];
    BOOST_CHECK( file["rootTitleBlockOwned"].get<bool>() );
    const std::string source = file["newDocumentSource"];
    BOOST_CHECK_NE( source.find( "(title \"Production Controller\")" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(date \"2026-07-19\")" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(rev \"C\")" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(company \"KiChad QA\")" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(comment 1 \"Release candidate\")" ),
                    std::string::npos );
    BOOST_CHECK_NE( source.find( "(comment 9 \"Generated from KDS\")" ),
                    std::string::npos );
    BOOST_CHECK_EQUAL( source.find( "(title \"Root sheet\")" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectsMalformedOrAliasedScreenIrWithoutPartialOperations )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( HIERARCHY_PROGRAM );
    BOOST_REQUIRE( compiled.ok );
    compiled.ir["schematic"]["sheets"][2]["file"] = "sheets/power.kicad_sch";

    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT result =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_CHECK( !result.fullyLowered );
    BOOST_CHECK( result.operations.empty() );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "shared_sheet_file_not_supported" ),
                    std::string::npos );

    compiled.ir["schematic"]["sheets"][2].erase( "position" );
    result = KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_CHECK( !result.fullyLowered );
    BOOST_CHECK( result.operations.empty() );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "invalid_schematic_ir" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( PreservesExistingScreenIdentityInNestedInstancePaths )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( HIERARCHY_PROGRAM );
    BOOST_REQUIRE( compiled.ok );
    const nlohmann::json existing = {
        { "hierarchy.kicad_sch", "11111111-2222-4333-8444-555555555555" },
        { "sheets/power.kicad_sch", "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee" }
    };
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT result =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir, existing );
    BOOST_REQUIRE_MESSAGE( result.fullyLowered, result.diagnostics.dump() );
    const nlohmann::json& files = result.operations[0]["files"];
    BOOST_CHECK_EQUAL( files[0]["screenUuid"].get<std::string>(),
                       "11111111-2222-4333-8444-555555555555" );
    BOOST_CHECK_EQUAL( files[1]["screenUuid"].get<std::string>(),
                       "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee" );
    BOOST_CHECK_NE( files[1]["newDocumentSource"].get<std::string>().find(
                            "(uuid \"aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee\")" ),
                    std::string::npos );
    BOOST_CHECK_NE( files[1]["newDocumentSource"].get<std::string>().find(
                            "(path \"/11111111-2222-4333-8444-555555555555/" ),
                    std::string::npos );

    result = KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
            compiled.ir, { { "hierarchy.kicad_sch", "not-a-uuid" } } );
    BOOST_CHECK( !result.fullyLowered );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "invalid_screen_identity" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( LowersResolvedComponentsGlobalNetsAndNoConnectsWithoutPlaceholders )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project connected)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (library footprint LocalFp (table project) (uri "${KIPRJMOD}/Local.pretty"))
  (sheet root (parent none) (file "connected.kicad_sch") (title "Connected"))
  (component R1 (symbol "Local:R") (value "10k") (footprint "LocalFp:R")
    (datasheet "https://example.com/r1.pdf")
    (description "AI-controlled resistor instance")
    (property "Manufacturer Part" "RC0603FR-0710KL")
    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none)
      (fields_autoplaced false)
      (field Value
        (at 43mm 42mm) (rotation 12.5deg) (visible true)
        (show_name true) (autoplace false) (size 1.2mm 1.5mm)
        (font "DejaVu Sans") (line_spacing 1.25) (thickness 0.2mm)
        (color #11223380) (justify right top) (bold true) (italic true)
        (hyperlink "https://example.com/value") (private false))
      (field "Manufacturer Part"
        (at 43mm 44mm) (rotation 0deg) (visible false)
        (show_name false) (autoplace true) (size 1mm 1mm)
        (font stroke) (line_spacing 1) (thickness auto)
        (color default) (justify center center) (bold false) (italic false)
        (hyperlink none) (private true))))
  (component R2 (symbol "Local:R") (value "20k") (footprint "LocalFp:R")
    (unit 1 (sheet root) (at 60mm 40mm) (rotation 90deg) (mirror x)))
  (component R3 (symbol "Local:R") (value "30k") (footprint "LocalFp:R")
    (unit 1 (sheet root) (at 80mm 40mm) (rotation 90deg) (mirror xy)))
  (net SIGNAL (presentation labels) (pin R1 1 1) (pin R2 1 1))
  (no_connect R1 1 2)
  (label signal-local (sheet root) (scope local) (net SIGNAL) (at 45mm 40mm)
    (rotation 0deg) (shape none) (size 1.27mm 1.27mm) (thickness auto)
    (justify left bottom) (bold false) (italic false))
  (label signal-global (sheet root) (scope global) (net SIGNAL) (at 55mm 40mm)
    (rotation 180deg) (shape output) (size 1.5mm 1.2mm) (thickness 0.2mm)
    (justify right center) (bold true) (italic true))
  (rule_area analog-policy
    (sheet root)
    (polygon (point 70mm 30mm) (point 90mm 30mm)
      (point 90mm 50mm) (point 70mm 50mm))
    (stroke 0.2mm dash_dot #11223380)
    (fill hatch #44556699)
    (exclude_from_sim true) (exclude_from_bom true)
    (exclude_from_board false) (dnp true))
  (directive signal-policy
    (sheet root) (target net SIGNAL) (at 45mm 40mm)
    (rotation 90deg) (shape diamond) (length 2.54mm)
    (property "Netclass" "HighSpeed"
      (at 46mm 38mm) (rotation 0deg) (size 1.27mm 1.27mm)
      (thickness auto) (justify left bottom)
      (bold false) (italic false) (visible true))
    (property "Review Note" "route as pair"
      (at 46mm 42mm) (rotation 180deg) (size 1mm 1mm)
      (thickness 0.2mm) (justify right top)
      (bold true) (italic true) (visible false)))
  (directive area-policy
    (sheet root) (target rule_area analog-policy) (at 70mm 40mm)
    (rotation 0deg) (shape rectangle) (length 1.27mm)
    (property "Component Class" "ANALOG"
      (at 72mm 40mm) (rotation 0deg) (size 1.27mm 1.27mm)
      (thickness auto) (justify left center)
      (bold false) (italic false) (visible true)))
  (text "AI note\nwith context"
    (id design-note) (sheet root) (at 25mm 35mm) (rotation 15.5deg)
    (exclude_from_sim true) (size 1.2mm 1.5mm) (font "DejaVu Sans")
    (line_spacing 1.25) (thickness 0.2mm) (color #11223380)
    (justify right top) (mirror true) (bold true) (italic true)
    (hyperlink "https://example.com/design-note"))
  (text_box "AI constraint summary\nwith bounded context"
    (id constraint-summary) (sheet root) (at 100mm 60mm) (rotation 22.5deg)
    (box_size 30mm 12mm) (margins 0.5mm 0.75mm 1mm 1.25mm)
    (exclude_from_sim false) (stroke 0.3mm dash_dot #10203080)
    (fill cross_hatch #40506099)
    (text_size 1.3mm 1.7mm) (font "DejaVu Sans")
    (line_spacing 1.25) (thickness 0.2mm) (color #708090cc)
    (justify left top) (mirror true) (bold true) (italic true)
    (hyperlink "https://example.com/constraint-summary"))
  (bus_alias SIGNALS (sheet root) (members SIGNAL))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const std::string cache = R"SYM((symbol "Local:R"
  (property "Reference" "R" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (property "Value" "R" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (symbol "R_1_1"
    (pin passive line (at 0 3.81 270) (length 1.27)
      (name "" (effects (font (size 1.27 1.27))))
      (number "1" (effects (font (size 1.27 1.27)))))
    (pin passive line (at 0 -3.81 90) (length 1.27)
      (name "" (effects (font (size 1.27 1.27))))
      (number "2" (effects (font (size 1.27 1.27)))))))
)SYM";
    const nlohmann::json resolved = {
        { "Local:R",
          { { "libraryId", "Local:R" },
            { "cacheSource", cache },
            { "flags",
              { { "excludeFromSim", true }, { "inBom", false }, { "onBoard", false },
                { "inPosFiles", false } } },
            { "properties", { { "Description", "Resistor" } } },
            { "propertyLayouts",
              { { "Reference",
                  { { "position", { { "xNm", -5000000 }, { "yNm", 0 } } },
                    { "rotationDegrees", 0.0 }, { "visible", true },
                    { "showName", false }, { "autoplace", false } } } } },
            { "units",
              { { "1", nlohmann::json::array(
                               { { { "number", "1" }, { "xNm", 0 }, { "yNm", 3810000 },
                                   { "rotationDegrees", 270 } },
                                 { { "number", "2" }, { "xNm", 0 }, { "yNm", -3810000 },
                                   { "rotationDegrees", 90 } } } ) } } } } }
    };
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, nlohmann::json::object(), resolved );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_CHECK_EQUAL( plan.counts["components"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( plan.counts["netEndpoints"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( plan.counts["noConnects"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( plan.counts["drawings"].get<int>(), 7 );
    BOOST_CHECK_EQUAL( plan.counts["busAliases"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( plan.counts["librarySymbols"].get<int>(), 1 );
    const std::string native = plan.operations[0]["files"][0]["newDocumentSource"];
    BOOST_CHECK_NE( native.find( "(symbol \"Local:R\"" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(lib_id \"Local:R\")" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(exclude_from_sim yes)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(in_bom no)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(on_board no)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(in_pos_files no)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(property \"Datasheet\" \"https://example.com/r1.pdf\"" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(property \"Description\" "
                                 "\"AI-controlled resistor instance\"" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(property private \"Manufacturer Part\" "
                                 "\"RC0603FR-0710KL\"" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(property \"Reference\" \"R1\"\n"
                                 "      (at 34.37 39.37 0)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(at 43 42 12.5)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(show_name yes)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(do_not_autoplace yes)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(face \"DejaVu Sans\")" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(href \"https://example.com/value\")" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(global_label \"SIGNAL\"" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(no_connect" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(mirror x)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(at 39.37 30.48 270)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(at 50.8 39.37 180)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(wire\n    (pts\n      (xy 39.37 35.56)\n"
                                 "      (xy 39.37 30.48)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(at 39.37 43.18)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(at 80.01 39.37 270)" ), std::string::npos );
    BOOST_CHECK_EQUAL( native.find( "(mirror x y)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(label \"SIGNAL\"" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(global_label \"SIGNAL\"\n    (shape output)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(size 1.2 1.5)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(thickness 0.2)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(bold yes)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(italic yes)" ), std::string::npos );
    BOOST_CHECK_MESSAGE( native.find( "(netclass_flag \"\"\n    (length 2.54)\n"
                                      "    (shape diamond)\n    (at 44.45 39.37 90)" )
                                 != std::string::npos,
                         native );
    BOOST_CHECK_NE( native.find( "(property \"Netclass\" \"HighSpeed\"" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(property \"Review Note\" \"route as pair\"" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(hide yes)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(justify right top)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(rule_area\n    (exclude_from_sim yes)\n"
                                 "    (in_bom no)\n    (on_board yes)\n    (dnp yes)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(type dash_dot)\n        (color 17 34 51 0.50196078)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(type hatch)\n        (color 68 85 102 0.6)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(property \"Component Class\" \"ANALOG\"" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(text \"AI note\\nwith context\"" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(exclude_from_sim yes)\n    (at 25 35 15.5)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(face \"DejaVu Sans\")" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(size 1.5 1.2)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(line_spacing 1.25)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(color 17 34 51 0.50196078)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(justify right top mirror)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(href \"https://example.com/design-note\")" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(text_box \"AI constraint summary\\nwith bounded context\"" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(at 100 60 22.5)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(size 30 12)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(margins 0.5 0.75 1 1.25)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(size 1.7 1.3)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(width 0.3)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(type cross_hatch)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(color 112 128 144 0.8)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(href \"https://example.com/constraint-summary\")" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(bus_alias \"SIGNALS\"\n    (members \"SIGNAL\")" ),
                    std::string::npos );

    nlohmann::json malformed = compiled.ir;
    malformed["schematic"]["components"][0]["units"][0]["fields"][0]["private"] = true;
    plan = KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
            malformed, nlohmann::json::object(), resolved );
    BOOST_CHECK( !plan.fullyLowered );
    BOOST_CHECK_NE( plan.diagnostics.dump().find( "malformed component placement" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( LowersTypedNestedGroupsAndRejectsMissingNativeOccurrences )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project grouped_native)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (sheet root (parent none) (file "grouped_native.kicad_sch") (title "Groups"))
  (sheet child (parent root) (file "child.kicad_sch") (title "Child")
    (at 20mm 20mm) (size 30mm 20mm)
    (pin SIG input (at 20mm 25mm) (side left)))
  (component R1 (symbol "Local:R") (value "1k") (footprint none)
    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none)))
  (component R2 (symbol "Local:R") (value "2k") (footprint none)
    (unit 1 (sheet root) (at 60mm 40mm) (rotation 0deg) (mirror none)))
  (net SIG (presentation labels) (pin R1 1 1) (pin R2 1 1))
  (no_connect R1 1 2)
  (wire root-wire (sheet root) (from 40mm 40mm) (to 60mm 40mm)
    (stroke default solid))
  (wire child-wire (sheet child) (from 20mm 25mm) (to 30mm 25mm)
    (stroke default solid))
  (group signal-core (sheet root) (name "Signal core") (locked true)
    (members (member component R1 1) (member net_label SIG R1 1 1 1)))
  (group root-bundle (sheet root) (name "Root bundle") (locked false)
    (members (member drawing root-wire) (member sheet child)
      (member no_connect R1 1 2 1) (member group signal-core)))
  (group child-interface (sheet child) (name "Child interface") (locked true)
    (members (member hierarchical_label child SIG) (member drawing child-wire)))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const std::string cache = R"SYM((symbol "Local:R"
  (property "Reference" "R" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (property "Value" "R" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (symbol "R_1_1"
    (pin passive line (at 0 3.81 270) (length 1.27)
      (name "" (effects (font (size 1.27 1.27))))
      (number "1" (effects (font (size 1.27 1.27)))))
    (pin passive line (at 0 -3.81 90) (length 1.27)
      (name "" (effects (font (size 1.27 1.27))))
      (number "2" (effects (font (size 1.27 1.27)))))))
)SYM";
    const nlohmann::json resolved = {
        { "Local:R",
          { { "libraryId", "Local:R" }, { "cacheSource", cache },
            { "flags",
              { { "excludeFromSim", false }, { "inBom", true }, { "onBoard", false },
                { "inPosFiles", false } } },
            { "properties", nlohmann::json::object() },
            { "units",
              { { "1", nlohmann::json::array(
                               { { { "number", "1" }, { "xNm", 0 }, { "yNm", 3810000 },
                                   { "rotationDegrees", 270 } },
                                 { { "number", "2" }, { "xNm", 0 },
                                   { "yNm", -3810000 },
                                   { "rotationDegrees", 90 } } } ) } } } } }
    };
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, nlohmann::json::object(), resolved );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_CHECK_EQUAL( plan.counts["groups"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( plan.operations[0]["managedItems"].size(), 15 );
    const std::string rootNative = plan.operations[0]["files"][0]["newDocumentSource"];
    const std::string childNative = plan.operations[0]["files"][1]["newDocumentSource"];
    BOOST_CHECK_NE( rootNative.find( "(group \"Signal core\"" ), std::string::npos );
    BOOST_CHECK_NE( rootNative.find( "(group \"Root bundle\"" ), std::string::npos );
    BOOST_CHECK_NE( childNative.find( "(group \"Child interface\"" ), std::string::npos );
    const std::string nestedUuid = KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
            "grouped_native", "schematic_group", "signal-core" );
    BOOST_CHECK_NE( rootNative.find( nestedUuid ), std::string::npos );
    BOOST_CHECK_NE( rootNative.find( "(locked yes)" ), std::string::npos );
    const size_t rootBundle = rootNative.find( "(group \"Root bundle\"" );
    const size_t instances = rootNative.find( "(sheet_instances", rootBundle );
    BOOST_REQUIRE_NE( rootBundle, std::string::npos );
    BOOST_REQUIRE_NE( instances, std::string::npos );
    BOOST_CHECK_EQUAL( rootNative.substr( rootBundle, instances - rootBundle )
                               .find( "(locked yes)" ),
                       std::string::npos );
    BOOST_CHECK_NE( childNative.find( "(locked yes)" ), std::string::npos );

    std::string invalid = program;
    const std::string occurrence = "(member net_label SIG R1 1 1 1)";
    const size_t occurrenceAt = invalid.find( occurrence );
    BOOST_REQUIRE_NE( occurrenceAt, std::string::npos );
    invalid.replace( occurrenceAt, occurrence.size(),
                     "(member net_label SIG R1 1 1 2)" );
    compiled = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    plan = KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
            compiled.ir, nlohmann::json::object(), resolved );
    BOOST_CHECK( !plan.fullyLowered );
    BOOST_CHECK_NE( plan.diagnostics.dump().find( "unresolved_schematic_group_member" ),
                    std::string::npos );

    compiled = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE( compiled.ok );
    compiled.ir["schematic"]["groups"][0]["members"].push_back(
            { { "kind", "group" }, { "id", "root-bundle" } } );
    plan = KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
            compiled.ir, nlohmann::json::object(), resolved );
    BOOST_CHECK( !plan.fullyLowered );
    BOOST_CHECK_NE( plan.diagnostics.dump().find( "recursive_schematic_group" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( TreatsAnUndeclaredHierarchyAsACompleteNoOp )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile(
                    "(kichad_design (version 1) (project board_only))" );
    BOOST_REQUIRE( compiled.ok );
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT result =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_CHECK( result.fullyLowered );
    BOOST_CHECK( result.operations.empty() );
    BOOST_CHECK_EQUAL( result.counts["files"].get<int>(), 0 );
}


BOOST_AUTO_TEST_CASE( BuildsExportAwareValidationPlanForSchematicOnlySymbols )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project netlist_eligibility)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (sheet root (parent none) (file "netlist_eligibility.kicad_sch") (title "Main"))
  (component U1 (symbol "Local:Multi") (value "Multi") (footprint none)
    (unit 1 (sheet root) (at 20mm 20mm) (rotation 0deg) (mirror none))
    (unit 2 (sheet root) (at 30mm 20mm) (rotation 0deg) (mirror none)))
  (component FLG1 (symbol "Local:PWR_FLAG") (value "PWR_FLAG") (footprint none)
    (unit 1 (sheet root) (at 40mm 20mm) (rotation 0deg) (mirror none)))
  (component FLG2 (symbol "Local:PWR_FLAG") (value "PWR_FLAG") (footprint none)
    (unit 1 (sheet root) (at 50mm 20mm) (rotation 0deg) (mirror none)))
  (net VCC (pin U1 1 1) (pin FLG1 1 1))
  (no_connect FLG2 1 1)
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const nlohmann::json resolved = {
        { "Local:Multi",
          { { "flags",
              { { "excludeFromSim", false }, { "inBom", true },
                { "onBoard", true }, { "inPosFiles", true } } } } },
        { "Local:PWR_FLAG",
          { { "flags",
              { { "excludeFromSim", true }, { "inBom", false },
                { "onBoard", false }, { "inPosFiles", false } } } } }
    };
    nlohmann::json validationPlan;
    std::string error;
    BOOST_REQUIRE_MESSAGE(
            KICHAD::CODEX_TOOLS::BuildNativeNetlistValidationPlan(
                    compiled.ir, resolved, "netlist_eligibility",
                    validationPlan, error ),
            error );
    BOOST_REQUIRE_EQUAL(
            validationPlan["expectedNetlistReferences"].size(), 1 );
    BOOST_CHECK_EQUAL(
            validationPlan["expectedNetlistReferences"][0].get<std::string>(), "U1" );
    BOOST_REQUIRE_EQUAL( validationPlan["expectedNetlistNets"].size(), 1 );
    BOOST_REQUIRE_EQUAL(
            validationPlan["expectedNetlistNets"][0]["nodes"].size(), 1 );
    BOOST_CHECK_EQUAL(
            validationPlan["expectedNetlistNets"][0]["nodes"][0]["reference"]
                    .get<std::string>(),
            "U1" );
    BOOST_CHECK( validationPlan["expectedNetlistNoConnects"].empty() );

    nlohmann::json malformed = resolved;
    malformed["Local:PWR_FLAG"]["flags"].erase( "onBoard" );
    error.clear();
    BOOST_CHECK( !KICHAD::CODEX_TOOLS::BuildNativeNetlistValidationPlan(
            compiled.ir, malformed, "netlist_eligibility", validationPlan, error ) );
    BOOST_CHECK_NE( error.find( "on-board eligibility" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( LowersCanonicalWiresBusesEntriesAndJunctionsToNativeItems )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project schematic_drawings)
  (sheet root (parent none) (file "schematic_drawings.kicad_sch") (title "Drawings"))
  (wire signal-leg (sheet root) (from 20mm 20mm) (to 30mm 20mm)
    (stroke default default))
  (bus control-bus (sheet root) (from 20mm 30mm) (to 40mm 30mm)
    (stroke 0.5mm dash_dot))
  (bus_entry control-entry (sheet root) (from 25mm 28.73mm) (to 26.27mm 30mm)
    (stroke default solid))
  (junction signal-junction (sheet root) (at 30mm 20mm)
    (diameter 0.8mm) (color #11223380))
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_CHECK_EQUAL( plan.counts["drawings"].get<int>(), 4 );
    const std::string native = plan.operations[0]["files"][0]["newDocumentSource"];
    BOOST_CHECK_NE( native.find( "(wire\n" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(bus\n" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(bus_entry\n" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(size 1.27 1.27)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(width 0.5)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(type dash_dot)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(diameter 0.8)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(color 17 34 51 0.50196078)" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( GeneratesOneStableReviewableWireDirectlyBetweenResolvedNetPins )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project wired_net)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (library footprint LocalFp (table project) (uri "${KIPRJMOD}/Local.pretty"))
  (sheet root (parent none) (file "wired_net.kicad_sch") (title "Wired net"))
  (component R1 (symbol "Local:R") (value "10k") (footprint "LocalFp:R")
    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none)))
  (component R2 (symbol "Local:R") (value "10k") (footprint "LocalFp:R")
    (unit 1 (sheet root) (at 60mm 40mm) (rotation 0deg) (mirror none)))
  (net SIGNAL (presentation wired) (pin R1 1 2) (pin R2 1 1))
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_CHECK_EQUAL( compiled.ir["schematic"]["nets"][0]["presentation"], "wired" );
    BOOST_CHECK( compiled.ir["schematic"]["nets"][0]["presentationExplicit"].get<bool>() );
    const std::string cache = R"SYM((symbol "Local:R"
  (property "Reference" "R" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (property "Value" "R" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (symbol "R_1_1"
    (pin passive line (at -2.54 0 0) (length 1.27)
      (name "" (effects (font (size 1.27 1.27))))
      (number "1" (effects (font (size 1.27 1.27)))))
    (pin passive line (at 2.54 0 180) (length 1.27)
      (name "" (effects (font (size 1.27 1.27))))
      (number "2" (effects (font (size 1.27 1.27)))))))
)SYM";
    const nlohmann::json resolved = {
        { "Local:R",
          { { "libraryId", "Local:R" }, { "cacheSource", cache },
            { "flags",
              { { "excludeFromSim", false }, { "inBom", true }, { "onBoard", true },
                { "inPosFiles", true } } },
            { "properties", nlohmann::json::object() },
            { "propertyLayouts", nlohmann::json::object() },
            { "units",
              { { "1", nlohmann::json::array(
                               { { { "number", "1" }, { "xNm", -2'540'000 },
                                   { "yNm", 0 }, { "rotationDegrees", 0 } },
                                 { { "number", "2" }, { "xNm", 2'540'000 },
                                   { "yNm", 0 }, { "rotationDegrees", 180 } } } ) } } } } }
    };
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT first =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, nlohmann::json::object(), resolved );
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT second =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, nlohmann::json::object(), resolved );
    BOOST_REQUIRE_MESSAGE( first.fullyLowered, first.diagnostics.dump() );
    BOOST_CHECK_EQUAL( first.operations.dump(), second.operations.dump() );
    BOOST_CHECK_EQUAL( first.counts["generatedWires"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( first.counts["generatedJunctions"].get<int>(), 0 );
    const std::string native = first.operations[0]["files"][0]["newDocumentSource"];
    BOOST_CHECK_NE( native.find( "(wire\n    (pts\n      (xy 41.91 39.37)\n"
                                 "      (xy 57.15 39.37)" ),
                    std::string::npos );
    const size_t firstLabel = native.find( "(global_label \"SIGNAL\"" );
    BOOST_REQUIRE_NE( firstLabel, std::string::npos );
    BOOST_CHECK_EQUAL( native.find( "(global_label \"SIGNAL\"", firstLabel + 1 ),
                       std::string::npos );
}


BOOST_AUTO_TEST_CASE( RoutesAroundEndpointTrapsWithBoundedOrthogonalSearch )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project routed_detour)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (sheet root (parent none) (file "routed_detour.kicad_sch") (title "Detour"))
  (component HA1 (symbol "Local:One") (value "HA1") (footprint none)
    (unit 1 (sheet root) (at 38.1mm 48.26mm) (rotation 0deg) (mirror none)))
  (component HA2 (symbol "Local:One") (value "HA2") (footprint none)
    (unit 1 (sheet root) (at 43.18mm 48.26mm) (rotation 180deg) (mirror none)))
  (component HB1 (symbol "Local:One") (value "HB1") (footprint none)
    (unit 1 (sheet root) (at 38.1mm 53.34mm) (rotation 0deg) (mirror none)))
  (component HB2 (symbol "Local:One") (value "HB2") (footprint none)
    (unit 1 (sheet root) (at 43.18mm 53.34mm) (rotation 180deg) (mirror none)))
  (component V1 (symbol "Local:One") (value "V1") (footprint none)
    (unit 1 (sheet root) (at 60.96mm 45.72mm) (rotation 90deg) (mirror none)))
  (component V2 (symbol "Local:One") (value "V2") (footprint none)
    (unit 1 (sheet root) (at 60.96mm 55.88mm) (rotation 270deg) (mirror none)))
  (component S1 (symbol "Local:One") (value "S1") (footprint none)
    (unit 1 (sheet root) (at 40.64mm 50.8mm) (rotation 0deg) (mirror none)))
  (component S2 (symbol "Local:One") (value "S2") (footprint none)
    (unit 1 (sheet root) (at 81.28mm 50.8mm) (rotation 180deg) (mirror none)))
  (net BLOCK_ABOVE (pin HA1 1 1) (pin HA2 1 1))
  (net BLOCK_BELOW (pin HB1 1 1) (pin HB2 1 1))
  (net BLOCK_VERTICAL (pin V1 1 1) (pin V2 1 1))
  (net SIGNAL (pin S1 1 1) (pin S2 1 1))
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const std::string cache = R"SYM((symbol "Local:One"
  (property "Reference" "U" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (property "Value" "One" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (symbol "One_1_1"
    (pin passive line (at 0 0 180) (length 1.27)
      (name "" (effects (font (size 1.27 1.27))))
      (number "1" (effects (font (size 1.27 1.27)))))))
)SYM";
    const nlohmann::json resolved = {
        { "Local:One",
          { { "libraryId", "Local:One" }, { "cacheSource", cache },
            { "flags",
              { { "excludeFromSim", false }, { "inBom", true }, { "onBoard", false },
                { "inPosFiles", false } } },
            { "properties", nlohmann::json::object() },
            { "propertyLayouts", nlohmann::json::object() },
            { "units",
              { { "1", nlohmann::json::array(
                               { { { "number", "1" }, { "xNm", 0 },
                                   { "yNm", 0 }, { "rotationDegrees", 180 } } } ) } } } } }
    };
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, nlohmann::json::object(), resolved );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_CHECK_EQUAL( plan.counts["isolatedLabelFallbacks"].get<int>(), 0 );
    size_t signalWires = 0;

    for( const nlohmann::json& item : plan.operations[0]["files"][0]["items"] )
    {
        if( item.value( "logicalId", "" ).starts_with( "net_wire/SIGNAL/" ) )
            ++signalWires;
    }

    BOOST_CHECK_GE( signalWires, 5 );
}


BOOST_AUTO_TEST_CASE( SynthesizesReadableHierarchyForAutomaticCrossSheetNets )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project automatic_hierarchy)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (sheet root (parent none) (file "automatic_hierarchy.kicad_sch") (title "Main"))
  (sheet control (parent root) (file "control.kicad_sch") (title "Control")
    (at 20mm 20mm) (size 30mm 20mm))
  (component R1 (symbol "Local:R") (value "10k") (footprint none)
    (unit 1 (sheet root) (at 60mm 22.54mm) (rotation 0deg) (mirror none)))
  (component R2 (symbol "Local:R") (value "10k") (footprint none)
    (unit 1 (sheet control) (at 40mm 20mm) (rotation 0deg) (mirror none)))
  (net SIGNAL (pin R1 1 1) (pin R2 1 1))
  (net RETURN (pin R1 1 2) (pin R2 1 2))
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_CHECK_EQUAL( compiled.ir["schematic"]["nets"][0]["presentation"], "auto" );
    BOOST_CHECK( !compiled.ir["schematic"]["nets"][0]["presentationExplicit"].get<bool>() );

    const std::string cache = R"SYM((symbol "Local:R"
  (property "Reference" "R" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (property "Value" "R" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (symbol "R_1_1"
    (pin passive line (at -2.54 0 0) (length 1.27)
      (name "" (effects (font (size 1.27 1.27))))
      (number "1" (effects (font (size 1.27 1.27)))))
    (pin passive line (at 2.54 0 180) (length 1.27)
      (name "" (effects (font (size 1.27 1.27))))
      (number "2" (effects (font (size 1.27 1.27)))))))
)SYM";
    const nlohmann::json resolved = {
        { "Local:R",
          { { "libraryId", "Local:R" }, { "cacheSource", cache },
            { "flags",
              { { "excludeFromSim", false }, { "inBom", true }, { "onBoard", true },
                { "inPosFiles", true } } },
            { "properties", nlohmann::json::object() },
            { "propertyLayouts", nlohmann::json::object() },
            { "units",
              { { "1", nlohmann::json::array(
                               { { { "number", "1" }, { "xNm", -2'540'000 },
                                   { "yNm", 0 }, { "rotationDegrees", 0 } },
                                 { { "number", "2" }, { "xNm", 2'540'000 },
                                   { "yNm", 0 }, { "rotationDegrees", 180 } } } ) } } } } }
    };
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT first =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, nlohmann::json::object(), resolved );
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT second =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, nlohmann::json::object(), resolved );
    BOOST_REQUIRE_MESSAGE( first.fullyLowered, first.diagnostics.dump() );
    BOOST_CHECK_EQUAL( first.operations.dump(), second.operations.dump() );
    BOOST_CHECK_EQUAL( first.counts["pins"].get<int>(), 2 );
    BOOST_CHECK_GT( first.counts["generatedWires"].get<int>(), 4 );

    const std::string root = first.operations[0]["files"][0]["newDocumentSource"];
    const std::string child = first.operations[0]["files"][1]["newDocumentSource"];
    BOOST_CHECK_NE( root.find( "(pin \"SIGNAL\" passive" ), std::string::npos );
    BOOST_CHECK_NE( root.find( "(pin \"RETURN\" passive" ), std::string::npos );
    BOOST_CHECK_NE( root.find( "(wire\n" ), std::string::npos );
    const size_t rootName = root.find( "(global_label \"SIGNAL\"" );
    BOOST_REQUIRE_NE( rootName, std::string::npos );
    BOOST_CHECK_EQUAL( root.find( "(global_label \"SIGNAL\"", rootName + 1 ),
                       std::string::npos );
    BOOST_CHECK_NE( child.find( "(hierarchical_label \"SIGNAL\"" ), std::string::npos );
    BOOST_CHECK_NE( child.find( "(hierarchical_label \"RETURN\"" ), std::string::npos );
    BOOST_CHECK_NE( child.find( "(wire\n" ), std::string::npos );
    BOOST_CHECK_EQUAL( child.find( "(global_label \"SIGNAL\"" ), std::string::npos );

    nlohmann::json explicitWiredIr = compiled.ir;
    for( nlohmann::json& net : explicitWiredIr["schematic"]["nets"] )
    {
        net["presentation"] = "wired";
        net["presentationExplicit"] = true;
    }
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT explicitWired =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    explicitWiredIr, nlohmann::json::object(), resolved );
    BOOST_REQUIRE_MESSAGE( explicitWired.fullyLowered, explicitWired.diagnostics.dump() );
    BOOST_CHECK_EQUAL( explicitWired.counts["pins"].get<int>(), 2 );
    BOOST_CHECK_GT( explicitWired.counts["generatedWires"].get<int>(), 4 );

    wxString exportDirectory;

    if( wxGetEnv( wxS( "KICHAD_QA_EXPORT_AUTOMATIC_HIERARCHY_DIR" ),
                  &exportDirectory ) )
    {
        BOOST_REQUIRE( wxFileName::Mkdir( exportDirectory, wxS_DIR_DEFAULT,
                                         wxPATH_MKDIR_FULL )
                       || wxDirExists( exportDirectory ) );

        for( const nlohmann::json& file : first.operations[0]["files"] )
        {
            wxFileName target( wxString::FromUTF8( file["path"].get<std::string>() ) );
            target.MakeAbsolute( exportDirectory );
            BOOST_REQUIRE( wxFileName::Mkdir( target.GetPath(), wxS_DIR_DEFAULT,
                                             wxPATH_MKDIR_FULL )
                           || wxDirExists( target.GetPath() ) );
            const std::string source = file["newDocumentSource"].get<std::string>();
            wxFile output;
            BOOST_REQUIRE( output.Create( target.GetFullPath(), true ) );
            BOOST_REQUIRE_EQUAL( output.Write( source.data(), source.size() ), source.size() );
            BOOST_REQUIRE( output.Flush() );
        }

        const auto writeSupportFile = [&]( const wxString& aName,
                                           const std::string& aSource )
        {
            wxFile output;
            const wxFileName target( exportDirectory, aName );
            BOOST_REQUIRE( output.Create( target.GetFullPath(), true ) );
            BOOST_REQUIRE_EQUAL( output.Write( aSource.data(), aSource.size() ),
                                 aSource.size() );
            BOOST_REQUIRE( output.Flush() );
        };
        writeSupportFile(
                wxS( "Local.kicad_sym" ),
                "(kicad_symbol_lib\n  (version 20251024)\n"
                "  (generator \"kicad_symbol_editor\")\n"
                "  (generator_version \"10.0\")\n  " + cache + "\n)\n" );
        writeSupportFile(
                wxS( "sym-lib-table" ),
                "(sym_lib_table\n  (version 7)\n"
                "  (lib (name \"Local\") (type \"KiCad\") "
                "(uri \"${KIPRJMOD}/Local.kicad_sym\") (options \"\") (descr \"\"))\n)\n" );
        writeSupportFile( wxS( "automatic_hierarchy.kicad_pro" ), "{}\n" );
    }
}


BOOST_AUTO_TEST_CASE( KeepsAdjacentMultiPinWiredNetsElectricallyIsolated )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project isolated_wired_nets)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (sheet root (parent none) (file "isolated_wired_nets.kicad_sch") (title "USB"))
  (component J1 (symbol "Local:USB") (value "USB") (footprint none)
    (unit 1 (sheet root) (at 50.8mm 88.9mm) (rotation 0deg) (mirror none)))
  (component U1 (symbol "Local:USB") (value "Protection") (footprint none)
    (unit 1 (sheet root) (at 114.3mm 88.9mm) (rotation 0deg) (mirror none)))
  (net "USB_D-_RAW" (presentation wired) (pin J1 1 1) (pin J1 1 2) (pin U1 1 5))
  (net "USB_D+_RAW" (presentation wired) (pin J1 1 3) (pin J1 1 4) (pin U1 1 6))
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const std::string cache = R"SYM((symbol "Local:USB"
  (property "Reference" "U" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (property "Value" "USB" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (symbol "USB_1_1"
    (pin passive line (at -25.4 0 0) (length 1.27)
      (name "D-1" (effects (font (size 1.27 1.27))))
      (number "1" (effects (font (size 1.27 1.27)))))
    (pin passive line (at 0 0 180) (length 1.27)
      (name "D-2" (effects (font (size 1.27 1.27))))
      (number "2" (effects (font (size 1.27 1.27)))))
    (pin passive line (at -25.4 2.54 0) (length 1.27)
      (name "D+1" (effects (font (size 1.27 1.27))))
      (number "3" (effects (font (size 1.27 1.27)))))
    (pin passive line (at 0 2.54 180) (length 1.27)
      (name "D+2" (effects (font (size 1.27 1.27))))
      (number "4" (effects (font (size 1.27 1.27)))))
    (pin passive line (at -5.08 0 0) (length 1.27)
      (name "D-OUT" (effects (font (size 1.27 1.27))))
      (number "5" (effects (font (size 1.27 1.27)))))
    (pin passive line (at -5.08 -2.54 0) (length 1.27)
      (name "D+OUT" (effects (font (size 1.27 1.27))))
      (number "6" (effects (font (size 1.27 1.27)))))))
)SYM";
    const nlohmann::json pins = nlohmann::json::array(
            { { { "number", "1" }, { "xNm", -25'400'000 }, { "yNm", 0 },
                { "rotationDegrees", 0 } },
              { { "number", "2" }, { "xNm", 0 }, { "yNm", 0 },
                { "rotationDegrees", 180 } },
              { { "number", "3" }, { "xNm", -25'400'000 }, { "yNm", 2'540'000 },
                { "rotationDegrees", 0 } },
              { { "number", "4" }, { "xNm", 0 }, { "yNm", 2'540'000 },
                { "rotationDegrees", 180 } },
              { { "number", "5" }, { "xNm", -5'080'000 }, { "yNm", 0 },
                { "rotationDegrees", 0 } },
              { { "number", "6" }, { "xNm", -5'080'000 }, { "yNm", -2'540'000 },
                { "rotationDegrees", 0 } } } );
    const nlohmann::json resolved = {
        { "Local:USB",
          { { "libraryId", "Local:USB" }, { "cacheSource", cache },
            { "flags", { { "excludeFromSim", false }, { "inBom", true },
                           { "onBoard", true }, { "inPosFiles", true } } },
            { "properties", nlohmann::json::object() },
            { "propertyLayouts", nlohmann::json::object() },
            { "units", { { "1", pins } } } } }
    };
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, nlohmann::json::object(), resolved );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_CHECK_EQUAL( plan.counts["isolatedLabelFallbacks"].get<int>(), 0 );

    std::set<std::string> routedNets;

    for( const nlohmann::json& item : plan.operations[0]["files"][0]["items"] )
    {
        const std::string logicalId = item.value( "logicalId", "" );

        if( logicalId.starts_with( "net_wire/USB_D-_RAW/" ) )
            routedNets.emplace( "USB_D-_RAW" );
        else if( logicalId.starts_with( "net_wire/USB_D+_RAW/" ) )
            routedNets.emplace( "USB_D+_RAW" );
    }

    BOOST_CHECK_EQUAL( routedNets.size(), 2 );

    // A crowded page can force physical routing to fall back to labels.  Presentation may
    // change in that case, but a PCB-facing global net name must never become sheet-qualified.
    nlohmann::json congestedIr = compiled.ir;
    nlohmann::json blocker = congestedIr["schematic"]["nets"][0];
    blocker["name"] = "BLOCKER";
    blocker["pins"] = nlohmann::json::array(
            { { { "component", "J1" }, { "unit", 1 }, { "number", "5" } },
              { { "component", "U1" }, { "unit", 1 }, { "number", "1" } } } );
    congestedIr["schematic"]["nets"].push_back( std::move( blocker ) );
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT congestedPlan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    congestedIr, nlohmann::json::object(), resolved );
    BOOST_REQUIRE_MESSAGE( congestedPlan.fullyLowered, congestedPlan.diagnostics.dump() );
    BOOST_CHECK_GT( congestedPlan.counts["isolatedLabelFallbacks"].get<int>(), 0 );
    size_t isolatedMinusLabels = 0;

    for( const nlohmann::json& item : congestedPlan.operations[0]["files"][0]["items"] )
    {
        if( !item.value( "logicalId", "" ).starts_with(
                    "net_isolated_label/USB_D-_RAW/" ) )
        {
            continue;
        }

        ++isolatedMinusLabels;
        BOOST_CHECK_EQUAL( item["kind"].get<std::string>(), "global_label" );
        BOOST_CHECK_NE( item["source"].get<std::string>().find(
                                "(global_label \"USB_D-_RAW\"" ),
                        std::string::npos );
    }

    BOOST_CHECK_EQUAL( isolatedMinusLabels, 3 );

    wxString exportDirectory;

    if( wxGetEnv( wxS( "KICHAD_QA_EXPORT_USB_WIRED_DIR" ), &exportDirectory ) )
    {
        BOOST_REQUIRE( wxFileName::Mkdir( exportDirectory, wxS_DIR_DEFAULT,
                                         wxPATH_MKDIR_FULL )
                       || wxDirExists( exportDirectory ) );
        const wxFileName schematicPath( exportDirectory,
                                        wxS( "isolated_wired_nets.kicad_sch" ) );
        const std::string source =
                congestedPlan.operations[0]["files"][0]["newDocumentSource"].get<std::string>();
        wxFile schematic;
        BOOST_REQUIRE( schematic.Create( schematicPath.GetFullPath(), true ) );
        BOOST_REQUIRE_EQUAL( schematic.Write( source.data(), source.size() ), source.size() );
        BOOST_REQUIRE( schematic.Flush() );

        wxFile project;
        BOOST_REQUIRE( project.Create(
                wxFileName( exportDirectory, wxS( "isolated_wired_nets.kicad_pro" ) )
                        .GetFullPath(), true ) );
        BOOST_REQUIRE_EQUAL( project.Write( "{}\n", 3 ), 3 );
        BOOST_REQUIRE( project.Flush() );
    }
}


BOOST_AUTO_TEST_CASE( RoutesBulkCrossSheetWiredNetsWithoutMergingInterfaces )
{
    constexpr int NET_COUNT = 24;
    std::ostringstream program;
    std::ostringstream cache;
    nlohmann::json pins = nlohmann::json::array();
    program << "(kichad_design\n  (version 1)\n  (project bulk_hierarchy)\n"
               "  (library symbol Local (table project) (uri \"${KIPRJMOD}/Local.kicad_sym\"))\n"
               "  (sheet root (parent none) (file \"bulk_hierarchy.kicad_sch\") (title \"Main\"))\n"
               "  (sheet left (parent root) (file \"left.kicad_sch\") (title \"Left\")"
               " (at 25.4mm 25.4mm) (size 50.8mm 76.2mm))\n"
               "  (sheet right (parent root) (file \"right.kicad_sch\") (title \"Right\")"
               " (at 101.6mm 25.4mm) (size 50.8mm 76.2mm))\n"
               "  (component U1 (symbol \"Local:Hub\") (value \"Hub\") (footprint none)"
               " (unit 1 (sheet left) (at 76.2mm 76.2mm) (rotation 0deg) (mirror none)))\n"
               "  (component U2 (symbol \"Local:Hub\") (value \"Hub\") (footprint none)"
               " (unit 1 (sheet right) (at 76.2mm 76.2mm) (rotation 0deg) (mirror none)))\n";
    cache << "(symbol \"Local:Hub\"\n"
             "  (property \"Reference\" \"U\" (at 0 0 0) (effects (font (size 1.27 1.27))))\n"
             "  (property \"Value\" \"Hub\" (at 0 0 0) (effects (font (size 1.27 1.27))))\n"
             "  (symbol \"Hub_1_1\"\n";

    for( int index = 0; index < NET_COUNT; ++index )
    {
        const std::string number = std::to_string( index + 1 );
        const std::string netName = index < 10 ? "N0" + std::to_string( index )
                                               : "N" + std::to_string( index );
        const int64_t yNm = static_cast<int64_t>( NET_COUNT / 2 - index ) * 2'540'000;
        const double yMm = static_cast<double>( yNm ) / 1'000'000.0;
        program << "  (net " << netName
                << " (presentation wired) (pin U1 1 " << number << ") (pin U2 1 "
                << number << "))\n";
        cache << "    (pin passive line (at 0 " << yMm << " 180) (length 1.27)"
                 " (name \"" << netName
              << "\" (effects (font (size 1.27 1.27))))"
                 " (number \"" << number
              << "\" (effects (font (size 1.27 1.27)))))\n";
        pins.push_back( { { "number", number }, { "xNm", 0 }, { "yNm", yNm },
                          { "rotationDegrees", 180 } } );
    }

    program << ")";
    cache << "  ))";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program.str() );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const nlohmann::json resolved = {
        { "Local:Hub",
          { { "libraryId", "Local:Hub" }, { "cacheSource", cache.str() },
            { "flags", { { "excludeFromSim", false }, { "inBom", true },
                           { "onBoard", true }, { "inPosFiles", true } } },
            { "properties", nlohmann::json::object() },
            { "propertyLayouts", nlohmann::json::object() },
            { "units", { { "1", pins } } } } }
    };
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, nlohmann::json::object(), resolved );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_CHECK_EQUAL( plan.counts["pins"].get<int>(), NET_COUNT * 2 );
    BOOST_CHECK_EQUAL( plan.counts["isolatedLabelFallbacks"].get<int>(), 0 );
    BOOST_CHECK_GT( plan.counts["generatedWires"].get<int>(), NET_COUNT * 2 );

    wxString exportDirectory;

    if( wxGetEnv( wxS( "KICHAD_QA_EXPORT_BULK_HIERARCHY_DIR" ), &exportDirectory ) )
    {
        BOOST_REQUIRE( wxFileName::Mkdir( exportDirectory, wxS_DIR_DEFAULT,
                                         wxPATH_MKDIR_FULL )
                       || wxDirExists( exportDirectory ) );

        for( const nlohmann::json& file : plan.operations[0]["files"] )
        {
            wxFileName target( wxString::FromUTF8( file["path"].get<std::string>() ) );
            target.MakeAbsolute( exportDirectory );
            const std::string source = file["newDocumentSource"].get<std::string>();
            wxFile output;
            BOOST_REQUIRE( output.Create( target.GetFullPath(), true ) );
            BOOST_REQUIRE_EQUAL( output.Write( source.data(), source.size() ), source.size() );
            BOOST_REQUIRE( output.Flush() );
        }

        const auto writeSupportFile = [&]( const wxString& aName,
                                           const std::string& aSource )
        {
            wxFile output;
            const wxFileName target( exportDirectory, aName );
            BOOST_REQUIRE( output.Create( target.GetFullPath(), true ) );
            BOOST_REQUIRE_EQUAL( output.Write( aSource.data(), aSource.size() ),
                                 aSource.size() );
            BOOST_REQUIRE( output.Flush() );
        };
        writeSupportFile(
                wxS( "Local.kicad_sym" ),
                "(kicad_symbol_lib\n  (version 20251024)\n"
                "  (generator \"kicad_symbol_editor\")\n"
                "  (generator_version \"10.0\")\n  " + cache.str() + "\n)\n" );
        writeSupportFile(
                wxS( "sym-lib-table" ),
                "(sym_lib_table\n  (version 7)\n"
                "  (lib (name \"Local\") (type \"KiCad\") "
                "(uri \"${KIPRJMOD}/Local.kicad_sym\") (options \"\") (descr \"\"))\n)\n" );
        writeSupportFile( wxS( "bulk_hierarchy.kicad_pro" ), "{}\n" );

        std::string nativeError;
        const wxFileName root( exportDirectory, wxS( "bulk_hierarchy.kicad_sch" ) );
        BOOST_REQUIRE_MESSAGE(
                KICHAD::CODEX_TOOLS::ValidateNativeSchematicHierarchy(
                        root, compiled.ir, resolved, nativeError ),
                nativeError );

        const auto leftFile = std::find_if(
                plan.operations[0]["files"].begin(), plan.operations[0]["files"].end(),
                []( const nlohmann::json& aFile )
                {
                    return aFile.value( "path", "" ) == "left.kicad_sch";
                } );
        BOOST_REQUIRE( leftFile != plan.operations[0]["files"].end() );
        const std::string leftSource =
                leftFile->at( "newDocumentSource" ).get<std::string>();
        std::string corruptedLeft = leftSource;
        const std::string distinctLabel = "(hierarchical_label \"N01\"";
        const size_t labelPosition = corruptedLeft.find( distinctLabel );
        BOOST_REQUIRE_NE( labelPosition, std::string::npos );
        corruptedLeft.replace( labelPosition, distinctLabel.size(),
                               "(hierarchical_label \"N00\"" );
        writeSupportFile( wxS( "left.kicad_sch" ), corruptedLeft );

        nativeError.clear();
        BOOST_CHECK( !KICHAD::CODEX_TOOLS::ValidateNativeSchematicHierarchy(
                root, compiled.ir, resolved, nativeError ) );
        BOOST_CHECK_NE( nativeError.find(
                                "native schematic connectivity does not match compiled KDS" ),
                        std::string::npos );
        writeSupportFile( wxS( "left.kicad_sch" ), leftSource );
    }
}


BOOST_AUTO_TEST_CASE( RejectsSymbolAnchorsPlacedCloserThanTheReadabilityFloor )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project crowded)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (library footprint LocalFp (table project) (uri "${KIPRJMOD}/Local.pretty"))
  (sheet root (parent none) (file "crowded.kicad_sch") (title "Crowded"))
  (component R1 (symbol "Local:R") (value "10k") (footprint "LocalFp:R")
    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none)))
  (component R2 (symbol "Local:R") (value "10k") (footprint "LocalFp:R")
    (unit 1 (sheet root) (at 40mm 45mm) (rotation 0deg) (mirror none)))
  (net SIGNAL (pin R1 1 2) (pin R2 1 1))
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const std::string cache = R"SYM((symbol "Local:R"
  (property "Reference" "R" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (property "Value" "R" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (symbol "R_1_1"
    (pin passive line (at -2.54 0 0) (length 1.27)
      (name "" (effects (font (size 1.27 1.27))))
      (number "1" (effects (font (size 1.27 1.27)))))
    (pin passive line (at 2.54 0 180) (length 1.27)
      (name "" (effects (font (size 1.27 1.27))))
      (number "2" (effects (font (size 1.27 1.27)))))))
)SYM";
    const nlohmann::json resolved = {
        { "Local:R",
          { { "libraryId", "Local:R" }, { "cacheSource", cache },
            { "flags",
              { { "excludeFromSim", false }, { "inBom", true }, { "onBoard", true },
                { "inPosFiles", true } } },
            { "properties", nlohmann::json::object() },
            { "propertyLayouts", nlohmann::json::object() },
            { "units",
              { { "1", nlohmann::json::array(
                               { { { "number", "1" }, { "xNm", -2'540'000 },
                                   { "yNm", 0 }, { "rotationDegrees", 0 } },
                                 { { "number", "2" }, { "xNm", 2'540'000 },
                                   { "yNm", 0 }, { "rotationDegrees", 180 } } } ) } } } } }
    };
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, nlohmann::json::object(), resolved );
    BOOST_CHECK( !plan.fullyLowered );
    BOOST_REQUIRE( !plan.diagnostics.empty() );
    BOOST_CHECK_EQUAL( plan.diagnostics.front().value( "code", "" ),
                       "crowded_schematic_placement" );
    BOOST_CHECK_NE( plan.diagnostics.front().value( "message", "" ).find( "R1" ),
                    std::string::npos );
    BOOST_CHECK_NE( plan.diagnostics.front().value( "message", "" ).find( "7.62" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( LowersEveryNativeSchematicGraphicGeometryWithStableIdentity )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project schematic_graphics)
  (sheet root (parent none) (file "schematic_graphics.kicad_sch") (title "Graphics"))
  (polyline signal-flow (sheet root)
    (points (point 10mm 10mm) (point 20mm 15mm) (point 30mm 10mm))
    (stroke 0.2mm dash #10203080) (fill none default))
  (rectangle controller-boundary (sheet root)
    (from 10mm 25mm) (to 35mm 40mm) (radius 2mm)
    (stroke default solid default) (fill color #40506099))
  (circle inspection-window (sheet root) (center 55mm 30mm) (radius 8mm)
    (stroke none dot #708090cc) (fill hatch #10203080))
  (arc current-path (sheet root)
    (start 70mm 30mm) (mid 80mm 20mm) (end 90mm 30mm)
    (stroke 0.3mm dash_dot default) (fill background default))
  (bezier response-curve (sheet root)
    (points (point 100mm 30mm) (point 110mm 20mm)
      (point 120mm 40mm) (point 130mm 30mm))
    (stroke 0.25mm dash_dot_dot #a0b0c0dd) (fill reverse_hatch #11223344))
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT first =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT second =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( first.fullyLowered, first.diagnostics.dump() );
    BOOST_CHECK_EQUAL( first.operations.dump(), second.operations.dump() );
    BOOST_CHECK_EQUAL( first.counts["drawings"].get<int>(), 5 );
    const std::string native = first.operations[0]["files"][0]["newDocumentSource"];
    BOOST_CHECK_NE( native.find( "(polyline\n" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(pts\n    (xy 10 10)\n    (xy 20 15)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(rectangle\n    (start 10 25)\n    (end 35 40)\n"
                                 "    (radius 2)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(circle\n    (center 55 30)\n    (radius 8)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(width -0.000001)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(arc\n    (start 70 30)\n    (mid 80 20)\n"
                                 "    (end 90 30)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(bezier\n" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(type reverse_hatch)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(color 160 176 192 0.86666667)" ),
                    std::string::npos );

    std::string parseError;
    BOOST_CHECK_MESSAGE( KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( native, &parseError ),
                         parseError );
}


BOOST_AUTO_TEST_CASE( LowersVerifiedSchematicImageToChunkedNativeData )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project schematic_image)
  (sheet root (parent none) (file "schematic_image.kicad_sch") (title "Image"))
  (image system-overview (sheet root) (at 80mm 60mm) (scale 1.25)
    (media_type image/png)
    (sha256 431ced6916a2a21a156e38701afe55bbd7f88969fbbfc56d7fe099d47f265460)
    (description "AI-readable one-pixel system overview fixture")
    (data_base64 "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="))
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT first =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT second =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( first.fullyLowered, first.diagnostics.dump() );
    BOOST_CHECK_EQUAL( first.operations.dump(), second.operations.dump() );
    BOOST_CHECK_EQUAL( first.counts["drawings"].get<int>(), 1 );
    const std::string native = first.operations[0]["files"][0]["newDocumentSource"];
    BOOST_CHECK_NE( native.find( "(image\n    (at 80 60)\n    (scale 1.25)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(data\n      \"iVBORw0KGgoAAAANSUhEUgAAAAEAAAAB" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "ASUVORK5CYII=\"" ), std::string::npos );

    std::string parseError;
    BOOST_CHECK_MESSAGE( KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( native, &parseError ),
                         parseError );
}


BOOST_AUTO_TEST_CASE( LowersAiNativeTableGridAndMergeToCurrentNativeFormat )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project schematic_table)
  (sheet root (parent none) (file "schematic_table.kicad_sch") (title "Table"))
  (table pin-summary
    (sheet root) (at 80mm 80mm) (rotation 90deg)
    (columns 20mm 30mm) (rows 8mm 10mm)
    (border (external true) (header true) (stroke 0.3mm dash #10203080))
    (separators (rows true) (columns false) (stroke default solid default))
    (cells
      (cell 1 1 "Pin summary"
        (margins 0.5mm 0.6mm 0.7mm 0.8mm) (exclude_from_sim true)
        (fill color #40506099) (text_size 1.2mm 1.5mm) (font "DejaVu Sans")
        (line_spacing 1.25) (thickness 0.2mm) (color #708090cc)
        (justify left top) (mirror true) (bold true) (italic true)
        (hyperlink "https://example.com/pin-summary"))
      (cell 1 2 ""
        (margins 0mm 0mm 0mm 0mm) (exclude_from_sim false)
        (fill none default) (text_size 1.27mm 1.27mm) (font stroke)
        (line_spacing 1) (thickness auto) (color default)
        (justify center center) (mirror false) (bold false) (italic false)
        (hyperlink none))
      (cell 2 1 "VCC"
        (margins 0.4mm 0.4mm 0.4mm 0.4mm) (exclude_from_sim false)
        (fill background default) (text_size 1mm 1mm) (font stroke)
        (line_spacing 1) (thickness auto) (color default)
        (justify left center) (mirror false) (bold true) (italic false)
        (hyperlink none))
      (cell 2 2 "3V3 supply"
        (margins 0.4mm 0.4mm 0.4mm 0.4mm) (exclude_from_sim false)
        (fill none default) (text_size 1mm 1mm) (font stroke)
        (line_spacing 1) (thickness auto) (color default)
        (justify left center) (mirror false) (bold false) (italic false)
        (hyperlink none)))
    (merges (merge 1 1 1 2)))
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT first =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT second =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( first.fullyLowered, first.diagnostics.dump() );
    BOOST_CHECK_EQUAL( first.operations.dump(), second.operations.dump() );
    BOOST_CHECK_EQUAL( first.counts["drawings"].get<int>(), 1 );
    const std::string native = first.operations[0]["files"][0]["newDocumentSource"];
    BOOST_CHECK_NE( native.find( "(table\n    (column_count 2)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(column_widths 20 30)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(row_heights 8 10)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(table_cell \"Pin summary\"" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(at 80 80 90)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(size 8 -50)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(span 2 1)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(span 0 0)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(face \"DejaVu Sans\")" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(href \"https://example.com/pin-summary\")" ),
                    std::string::npos );

    std::string parseError;
    BOOST_CHECK_MESSAGE( KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( native, &parseError ),
                         parseError );
}


BOOST_AUTO_TEST_CASE( ExportsGeneratedHierarchyForOptInNativeValidation )
{
    wxString exportDirectory;

    if( !wxGetEnv( wxS( "KICHAD_QA_EXPORT_SCHEMATIC_DIR" ), &exportDirectory ) )
    {
        BOOST_TEST_MESSAGE( "Skipping opt-in native schematic fixture export" );
        return;
    }

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( HIERARCHY_PROGRAM );
    BOOST_REQUIRE( compiled.ok );
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_REQUIRE( wxFileName::Mkdir( exportDirectory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL )
                   || wxDirExists( exportDirectory ) );

    for( const nlohmann::json& file : plan.operations[0]["files"] )
    {
        wxFileName target( wxString::FromUTF8( file["path"].get<std::string>() ) );
        target.MakeAbsolute( exportDirectory );
        BOOST_REQUIRE( wxFileName::Mkdir( target.GetPath(), wxS_DIR_DEFAULT,
                                         wxPATH_MKDIR_FULL )
                       || wxDirExists( target.GetPath() ) );
        const std::string source = file["newDocumentSource"].get<std::string>();
        wxFile output;
        BOOST_REQUIRE( output.Create( target.GetFullPath(), true ) );
        BOOST_REQUIRE_EQUAL( output.Write( source.data(), source.size() ), source.size() );
        BOOST_REQUIRE( output.Flush() );
    }
}


BOOST_AUTO_TEST_SUITE_END()
