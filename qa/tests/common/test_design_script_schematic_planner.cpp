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

#include <kicad/codex/design_script_compiler.h>
#include <kicad/codex/design_script_pcb_planner.h>
#include <kicad/codex/design_script_schematic_planner.h>
#include <kicad/codex/lossless_sexpr_document.h>

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/utils.h>


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
  (net SIGNAL (pin R1 1 1) (pin R2 1 1))
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
                                 "      (at 35 40 0)" ),
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
    BOOST_CHECK_NE( native.find( "(at 40 31.11 270)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(at 51.11 40 180)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(wire\n    (pts\n      (xy 40 36.19)\n"
                                 "      (xy 40 31.11)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(at 40 43.81)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(at 80 40 270)" ), std::string::npos );
    BOOST_CHECK_EQUAL( native.find( "(mirror x y)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(label \"SIGNAL\"" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(global_label \"SIGNAL\"\n    (shape output)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(size 1.2 1.5)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(thickness 0.2)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(bold yes)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(italic yes)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(netclass_flag \"\"\n    (length 2.54)\n"
                                 "    (shape diamond)\n    (at 45 40 90)" ),
                    std::string::npos );
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
  (net SIG (pin R1 1 1) (pin R2 1 1))
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
    BOOST_CHECK_EQUAL( first.counts["generatedWires"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( first.counts["generatedJunctions"].get<int>(), 0 );
    const std::string native = first.operations[0]["files"][0]["newDocumentSource"];
    BOOST_CHECK_NE( native.find( "(wire\n    (pts\n      (xy 42.54 40)\n"
                                 "      (xy 46.35 40)" ),
                    std::string::npos );
    BOOST_CHECK_NE( native.find( "(wire\n    (pts\n      (xy 46.35 40)\n"
                                 "      (xy 53.65 40)" ),
                    std::string::npos );
    const size_t firstLabel = native.find( "(global_label \"SIGNAL\"" );
    BOOST_REQUIRE_NE( firstLabel, std::string::npos );
    BOOST_CHECK_NE( native.find( "(global_label \"SIGNAL\"", firstLabel + 1 ),
                    std::string::npos );
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
