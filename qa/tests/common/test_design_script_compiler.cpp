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

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>

#include <kicad/codex/design_script_compiler.h>
#include <kicad/codex/design_script_context_builder.h>
#include <kicad/codex/design_script_layout_analyzer.h>
#include <kicad/codex/design_script_physical_synthesizer.h>
#include <kicad/codex/design_script_simulation_runner.h>
#include <qa_utils/wx_utils/unit_test_utils.h>


namespace
{

const std::string VALID_PROGRAM = R"KDS((kichad_design
  (version 1)
  (project sensor_node
    (title "Production Sensor Node")
    (company "KiChad QA")
    (revision "A")
    (date "2026-07-19")
    (comment 1 "Production release")
    (comment 9 "AI-authored KDS")
    (text_variables
      (variable PRODUCT_NAME "Sensor Node")
      (variable RELEASE_CHANNEL "production"))
    (field_templates
      (field "Manufacturer Part Number" (visible true) (url false))
      (field "Datasheet URL" (visible false) (url true))))
  (units mm)
  (library symbol Device (table global))
  (library footprint Resistor_SMD (table global))
  (library footprint LED_SMD (table global))
  (component R1
    (symbol "Device:R")
    (value "10k")
    (footprint "Resistor_SMD:R_0603_1608Metric")
    (property "Tolerance" "1%")
    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none)))
  (component LED1
    (symbol "Device:LED")
    (value "GREEN")
    (footprint "LED_SMD:LED_0603_1608Metric")
    (unit 1 (sheet root) (at 50mm 40mm) (rotation 90deg) (mirror y)))
  (net LED_A (pin R1 1 1) (pin LED1 1 1))
  (sheet root (parent none) (file "sensor_node.kicad_sch") (title "Main"))
  (board
    (stackup
      (finish "ENIG") (impedance_controlled true)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 1.53mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))))
    (outline
      (rectangle board-edge (start 0mm 0mm) (end 40mm 30mm)
        (radius 0mm) (stroke 0.05mm solid) (layers Edge.Cuts) (fill none)))
    (place R1 (at 10mm 10mm) (rotation 0deg) (side front))
    (route LED_A (id led-a-trace) (from 10mm 10mm) (to 20mm 10mm)
      (width 0.25mm) (layer F.Cu))
    (route LED_A (id led-a-arc) (from 20mm 10mm) (mid 22mm 12mm) (to 24mm 10mm)
      (width 0.25mm) (layer F.Cu))
    (via LED_A (id led-a-via) (at 24mm 10mm) (diameter 0.8mm) (drill 0.4mm))
    (zone LED_A
      (id led-a-pour)
      (name "LED copper pour")
      (layers F.Cu)
      (outline
        (polygon
          (point 1mm 1mm) (point 39mm 1mm) (point 39mm 29mm) (point 1mm 29mm)
          (hole
            (point 10mm 10mm) (point 12mm 10mm) (point 12mm 12mm) (point 10mm 12mm))))
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection thermal (thermal_gap 0.3mm) (thermal_spoke_width 0.35mm))
      (islands remove_below (minimum_area 1mm2))
      (fill hatched (thickness 0.3mm) (gap 0.4mm) (orientation 45deg)
        (smoothing 0.25) (hole_min_area_ratio 0.1) (border hatch))
      (hatch_offsets (layer F.Cu 0.1mm -0.2mm))
      (priority 3)
      (border diagonal_edge (pitch 0.5mm))
      (locked true)))
  (rules
    (minimum_clearance 0.2mm)
    (minimum_connection_width 0.2mm)
    (minimum_track_width 0.2mm)
    (minimum_via_annular_width 0.1mm)
    (minimum_via_diameter 0.6mm)
    (minimum_through_hole_diameter 0.3mm)
    (minimum_microvia_diameter 0.3mm)
    (minimum_microvia_drill 0.1mm)
    (minimum_hole_to_hole 0.25mm)
    (minimum_copper_to_hole_clearance 0.25mm)
    (minimum_silkscreen_clearance 0mm)
    (minimum_groove_width 0.25mm)
    (minimum_resolved_spokes 2)
    (minimum_silkscreen_text_height 0.8mm)
    (minimum_silkscreen_text_thickness 0.08mm)
    (minimum_copper_to_edge_clearance 0.5mm)
    (use_height_for_length_calculations true)
    (maximum_error 0.005mm)
    (allow_fillets_outside_zone_outline false))
  (source R1
    (manufacturer "Yageo")
    (mpn "RC0603FR-0710KL")
    (supplier "DigiKey")
    (quantity 1))
  (check erc)
  (check drc)
  (check sourcing)
  (output gerbers)
  (output drill)
  (output bom))
)KDS";

} // namespace


BOOST_AUTO_TEST_SUITE( DesignScriptCompiler )


BOOST_AUTO_TEST_CASE( CompilesEveryDesignFacetIntoDeterministicValidatedIr )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT first =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( VALID_PROGRAM );
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT second =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( VALID_PROGRAM );

    BOOST_REQUIRE_MESSAGE( first.ok, first.diagnostics.dump() );
    BOOST_REQUIRE( second.ok );
    BOOST_CHECK_EQUAL( first.sourceSha256, second.sourceSha256 );
    BOOST_CHECK_EQUAL( first.ir.dump(), second.ir.dump() );
    BOOST_CHECK_EQUAL( first.ir["project"]["name"].get<std::string>(), "sensor_node" );
    BOOST_REQUIRE( first.ir["project"].contains( "titleBlock" ) );
    BOOST_CHECK_EQUAL( first.ir["project"]["titleBlock"]["title"],
                       "Production Sensor Node" );
    BOOST_CHECK_EQUAL( first.ir["project"]["titleBlock"]["company"], "KiChad QA" );
    BOOST_CHECK_EQUAL( first.ir["project"]["titleBlock"]["revision"], "A" );
    BOOST_CHECK_EQUAL( first.ir["project"]["titleBlock"]["date"], "2026-07-19" );
    BOOST_REQUIRE_EQUAL( first.ir["project"]["titleBlock"]["comments"].size(), 9 );
    BOOST_CHECK_EQUAL( first.ir["project"]["titleBlock"]["comments"][0],
                       "Production release" );
    BOOST_CHECK_EQUAL( first.ir["project"]["titleBlock"]["comments"][8],
                       "AI-authored KDS" );
    BOOST_CHECK_EQUAL( first.ir["project"]["textVariables"]["PRODUCT_NAME"],
                       "Sensor Node" );
    BOOST_CHECK_EQUAL( first.ir["project"]["textVariables"].size(), 2 );
    BOOST_REQUIRE_EQUAL( first.ir["project"]["fieldTemplates"].size(), 2 );
    BOOST_CHECK_EQUAL( first.ir["project"]["fieldTemplates"][0]["name"],
                       "Manufacturer Part Number" );
    BOOST_CHECK( first.ir["project"]["fieldTemplates"][0]["visible"].get<bool>() );
    BOOST_CHECK( first.ir["project"]["fieldTemplates"][1]["url"].get<bool>() );
    BOOST_CHECK_EQUAL( first.ir["schematic"]["components"].size(), 2 );
    BOOST_CHECK_EQUAL( first.ir["schematic"]["nets"].size(), 1 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["pinConnections"].get<size_t>(), 2 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["boardStatements"].get<size_t>(), 7 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["rules"].get<size_t>(), 1 );
    BOOST_CHECK_EQUAL( first.ir["rules"]["minimumClearanceNm"].get<int64_t>(), 200000 );
    BOOST_CHECK_EQUAL( first.ir["rules"]["minimumResolvedSpokes"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( first.ir["rules"]["copperEdgeClearanceMode"].get<std::string>(),
                       "explicit" );
    BOOST_CHECK( first.ir["rules"]["useHeightForLengthCalculations"].get<bool>() );
    BOOST_CHECK_EQUAL( first.ir["rules"]["maximumErrorNm"].get<int64_t>(), 5000 );
    BOOST_CHECK( !first.ir["rules"]["allowFilletsOutsideZoneOutline"].get<bool>() );
    BOOST_CHECK( first.plan["boardFullyTyped"].get<bool>() );
    BOOST_CHECK_EQUAL( first.ir["pcb"][1]["logicalId"].get<std::string>(), "board-edge" );
    BOOST_CHECK_EQUAL( first.ir["pcb"][3]["kind"].get<std::string>(), "trace" );
    BOOST_CHECK_EQUAL( first.ir["pcb"][3]["widthNm"].get<int64_t>(), 250000 );
    BOOST_CHECK_EQUAL( first.ir["pcb"][4]["kind"].get<std::string>(), "arc" );
    BOOST_CHECK_EQUAL( first.ir["pcb"][5]["drillNm"].get<int64_t>(), 400000 );
    BOOST_CHECK_EQUAL( first.ir["pcb"][6]["kind"].get<std::string>(), "zone" );
    BOOST_CHECK_EQUAL( first.ir["pcb"][6]["polygons"][0]["holes"].size(), 1 );
    BOOST_CHECK_EQUAL( first.ir["pcb"][6]["islands"]["minimumAreaNm2"].get<int64_t>(),
                       1000000000000LL );
    BOOST_CHECK_EQUAL( first.ir["pcb"][6]["fill"]["orientationDegrees"].get<double>(),
                       45.0 );
    BOOST_REQUIRE_EQUAL( first.ir["pcb"][6]["layerProperties"].size(), 1 );
    BOOST_CHECK_EQUAL( first.ir["pcb"][6]["layerProperties"][0]["layer"].get<std::string>(),
                       "F.Cu" );
    BOOST_CHECK_EQUAL(
            first.ir["pcb"][6]["layerProperties"][0]["offset"]["yNm"].get<int64_t>(),
            -200000 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["sourcingRecords"].get<size_t>(), 1 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["checks"].get<size_t>(), 3 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["outputs"].get<size_t>(), 3 );
    BOOST_CHECK( first.plan["transactional"].get<bool>() );
}


BOOST_AUTO_TEST_CASE( CompilesExactFirmwareProgrammingPowerAndBringupHandoff )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project controller)
  (sheet root (parent none) (file "controller.kicad_sch") (title "Main"))
  (library symbol Device (table global))
  (library footprint Package_SO (table global))
  (component U1 (symbol "Device:R") (value "MCU")
    (footprint "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm")
    (unit 1 (sheet root) (at 10mm 10mm) (rotation 0deg) (mirror none)))
  (component J1 (symbol "Device:R") (value "POWER")
    (footprint "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm")
    (unit 1 (sheet root) (at 20mm 10mm) (rotation 0deg) (mirror none)))
  (component J3 (symbol "Device:R") (value "SWD")
    (footprint "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm")
    (unit 1 (sheet root) (at 30mm 10mm) (rotation 0deg) (mirror none)))
  (component TP1 (symbol "Device:R") (value "3V3")
    (footprint "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm")
    (unit 1 (sheet root) (at 40mm 10mm) (rotation 0deg) (mirror none)))
  (net +3V3 (pin U1 1 1) (pin TP1 1 1) (pin J1 1 1))
  (net GND (pin U1 1 2) (pin J1 1 2) (pin J3 1 3))
  (net SWDIO (pin U1 1 3) (pin J3 1 2))
  (net SWCLK (pin U1 1 4) (pin J3 1 4))
  (net RESET (pin U1 1 5) (pin J3 1 6))
  (production
    (assembly
      (acceptance ipc-a-610-class-2) (process lead_free_reflow)
      (solder_alloy "SAC305") (stencil top) (stencil_thickness_um 120)
      (cleaning no_clean) (coating none)
      (instruction mcu-orientation (scope component U1)
        (text "Install U1 at the marked pin-one orientation.")))
    (firmware controller
      (data_base64 "OjAxMDAwMDAwMDBGRgo6MDAwMDAwMDFGRgo=") (format ihex)
      (sha256 "c3f2ee75459ee15fe6bf5a6f5ad95766f1477ffe75fb9d867f540e1f36d39b3f")
      (bytes 26) (version "1.0.0") (target U1)
      (device_code
        (toolchain cmake) (toolchain_version "3.28.3")
        (target "fixture-mcu") (entry "src/main.c")
        (file "src/main.c" (language c)
          (sha256 "86004d65c4f387c95467c6cee92bc1f1f8cb04d6650be09fbd1e359834a56766")
          (data_base64 "aW50IG1haW4odm9pZCl7cmV0dXJuIDA7fQo="))))
    (program controller (firmware controller) (target U1) (interface swd)
      (connector J3) (device "STM32G0B1CBT6") (voltage 3.3) (speed_khz 1000)
      (erase chip) (reset run) (verify true)
      (signal swdio 2) (signal swclk 4) (signal reset 6) (signal ground 3))
    (power main (connector J1) (positive_pin 1) (return_pin 2)
      (voltage 12) (current_limit 0.25) (settle_ms 250))
    (test input-not-shorted (stage unpowered) (method resistance) (instrument dmm)
      (target net +3V3) (range 1000 1000000 ohm)
      (after_ms 0) (timeout_ms 5000) (required true))
    (test rail-3v3 (stage power_on) (method voltage) (instrument dmm)
      (target net +3V3) (range 3.20 3.40 V) (testpoint TP1) (power main)
      (after_ms 250) (timeout_ms 5000) (required true))
    (test firmware-response (stage functional) (method functional) (instrument fixture)
      (target component U1) (expected pass) (power main) (program controller)
      (after_ms 0) (timeout_ms 5000) (required true)
      (procedure "Require the deterministic firmware response."))))
)KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_CHECK_EQUAL( compiled.ir["production"]["firmware"][0]["id"], "controller" );
    BOOST_CHECK_EQUAL(
            compiled.ir["production"]["firmware"][0]["deviceCode"]["totalBytes"], 26 );
    BOOST_CHECK_EQUAL( compiled.ir["production"]["assembly"]["stencilThicknessUm"], 120 );
    BOOST_CHECK_EQUAL( compiled.ir["production"]["programming"][0]["interface"], "swd" );
    BOOST_CHECK_EQUAL( compiled.ir["production"]["power"][0]["currentLimitA"], 0.25 );
    BOOST_CHECK_EQUAL( compiled.ir["production"]["tests"][1]["range"]["unit"], "V" );
    BOOST_CHECK_EQUAL( compiled.plan["counts"]["firmwareImages"], 1 );
    BOOST_CHECK( std::find( compiled.plan["passes"].begin(), compiled.plan["passes"].end(),
                            "production_handoff" ) != compiled.plan["passes"].end() );
}


BOOST_AUTO_TEST_CASE( CompilesCompleteStepperControllerReleaseReference )
{
    const std::filesystem::path path =
            std::filesystem::path( KI_TEST::GetTestDataRootDir() ) / "kichad"
            / "reference_stepper_controller" / "reference_stepper_controller.kicad_kds";
    std::ifstream input( path, std::ios::binary );
    BOOST_REQUIRE_MESSAGE( input, "missing production reference KDS: " << path );
    const std::string source{ std::istreambuf_iterator<char>( input ),
                              std::istreambuf_iterator<char>() };
    BOOST_REQUIRE( !input.bad() );
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump( 2 ) );
    BOOST_CHECK_EQUAL( compiled.ir["project"]["name"], "reference_stepper_controller" );
    BOOST_CHECK_EQUAL( compiled.ir["production"]["firmware"][0]["bytes"], 9640 );
    BOOST_CHECK_EQUAL( compiled.ir["production"]["tests"][2]["power"].size(), 2 );
    BOOST_CHECK_EQUAL( compiled.ir["production"]["tests"][4]["stage"], "functional" );
    BOOST_CHECK_EQUAL( compiled.plan["counts"]["components"], 5 );
    const nlohmann::json context = KICHAD::DESIGN_SCRIPT_CONTEXT_BUILDER::Build(
            compiled.ir, compiled.plan, "manufacturing", "production", 0, 10 );
    const std::string contextText = context.dump();
    BOOST_CHECK( contextText.find( "OjEwMDAwMDAw" ) == std::string::npos );
    BOOST_CHECK( contextText.find( "dataBase64Omitted" ) != std::string::npos );
}


BOOST_AUTO_TEST_CASE( ProjectSettingsHaveOneExplicitReplaceableRepresentation )
{
    const std::string empty = R"KDS((kichad_design
  (version 1)
  (project clear_project_settings
    (text_variables)
    (field_templates))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT cleared =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( empty );
    BOOST_REQUIRE_MESSAGE( cleared.ok, cleared.diagnostics.dump() );
    BOOST_CHECK( cleared.ir["project"]["textVariables"].empty() );
    BOOST_CHECK( cleared.ir["project"]["fieldTemplates"].empty() );

    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project invalid_project_settings
    (text_variables
      (variable "BAD.NAME" value)
      (variable VALID first)
      (variable VALID second))
    (text_variables)
    (field_templates
      (field Value (visible true) (url false))
      (field MPN (visible true) (url false))
      (field mpn (visible false) (url true))
      (field MissingUrl (visible true) (visible false))))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT rejected =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !rejected.ok );
    const std::string diagnostics = rejected.diagnostics.dump();

    for( const char* code : { "invalid_project_text_variable",
                              "duplicate_project_text_variable",
                              "duplicate_project_text_variables",
                              "invalid_project_field_template",
                              "duplicate_project_field_template",
                              "incomplete_project_field_template" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( ValidatesStructuredSourcingEvidenceInKds )
{
    const std::string valid = R"KDS((kichad_design
  (version 1)
  (project sourcing)
  (component R1
    (symbol "Device:R") (value "10k") (footprint "Resistor:R_0603"))
  (source R1
    (manufacturer "Example Components")
    (mpn "EX-0603-10K")
    (datasheet "https://manufacturer.example.test/EX-0603-10K.pdf")
    (lifecycle active)
    (supplier "DigiKey")
    (sku "EX-0603-10K-ND")
    (product_url "https://distributor.example.test/EX-0603-10K")
    (available 1234)
    (verified_on 2026-07-19)
    (quantity 1)
    (unit_price "0.01 USD")
    (notes "Stock and lifecycle checked with live web search"))
  (check sourcing)
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( valid );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( compiled.ir["sourcing"].size(), 1 );
    const KICHAD::DESIGN_SCRIPT_COMPILER::JSON& evidence = compiled.ir["sourcing"][0];
    BOOST_CHECK_EQUAL( evidence["manufacturer"].get<std::string>(), "Example Components" );
    BOOST_CHECK_EQUAL( evidence["mpn"].get<std::string>(), "EX-0603-10K" );
    BOOST_CHECK_EQUAL( evidence["lifecycle"].get<std::string>(), "active" );
    BOOST_CHECK_EQUAL( evidence["available"].get<int>(), 1234 );
    BOOST_CHECK_EQUAL( evidence["verified_on"].get<std::string>(), "2026-07-19" );

    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project bad_sourcing)
  (component R1
    (symbol "Device:R") (value "10k") (footprint "Resistor:R_0603"))
  (source R1
    (manufacturer 7)
    (mpn "")
    (datasheet "http://manufacturer.example.test/part.pdf")
    (lifecycle unknown)
    (supplier "")
    (sku "")
    (product_url "https://")
    (available -1)
    (verified_on 2026-02-30)
    (quantity 0)
    (unit_price 12)
    (notes ""))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT rejected =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !rejected.ok );
    size_t invalidValues = 0;

    for( const KICHAD::DESIGN_SCRIPT_COMPILER::JSON& diagnostic : rejected.diagnostics )
    {
        if( diagnostic.value( "code", "" ) == "invalid_source_value" )
            ++invalidValues;
    }

    BOOST_CHECK_EQUAL( invalidValues, 12 );
}


BOOST_AUTO_TEST_CASE( CompilesOneExplicitLosslessStackupRepresentation )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project controlled_stackup)
  (board
    (stackup
      (finish "ENIG")
      (impedance_controlled true)
      (edge_connector bevelled)
      (edge_plating true)
      (layers
        (silkscreen F.SilkS (material "Epoxy ink") (color "White"))
        (solderpaste F.Paste)
        (soldermask F.Mask (thickness 10um) (material "LPI")
          (epsilon_r 3.5) (loss_tangent 0.025) (color "Green"))
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 0.486mm) (material "FR408HR")
          (epsilon_r 3.68) (loss_tangent 0.0092) (locked true))
        (copper In1.Cu (thickness 18um))
        (dielectric prepreg (thickness 0.486mm) (material "FR408HR 2116")
          (epsilon_r 3.66) (loss_tangent 0.0092) (locked false))
        (copper In2.Cu (thickness 18um))
        (dielectric core (thickness 0.517mm) (material "FR408HR")
          (epsilon_r 3.68) (loss_tangent 0.0092) (locked true))
        (copper B.Cu (thickness 35um))
        (soldermask B.Mask (thickness 10um) (material "LPI")
          (epsilon_r 3.5) (loss_tangent 0.025) (color "Green"))
        (solderpaste B.Paste)
        (silkscreen B.SilkS (material "Epoxy ink") (color "White"))))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( compiled.ir["pcb"].size(), 1 );
    const nlohmann::json& stackup = compiled.ir["pcb"][0];
    BOOST_CHECK_EQUAL( stackup["kind"].get<std::string>(), "stackup" );
    BOOST_CHECK_EQUAL( stackup["finish"].get<std::string>(), "ENIG" );
    BOOST_CHECK( stackup["impedanceControlled"].get<bool>() );
    BOOST_CHECK_EQUAL( stackup["edgeConnector"].get<std::string>(), "bevelled" );
    BOOST_CHECK( stackup["edgePlating"].get<bool>() );
    BOOST_CHECK_EQUAL( stackup["copperLayers"].get<int>(), 4 );
    BOOST_CHECK_EQUAL( stackup["thicknessNm"].get<int64_t>(), 1615000 );
    BOOST_REQUIRE_EQUAL( stackup["layers"].size(), 13 );
    BOOST_CHECK_EQUAL( stackup["layers"][0]["category"].get<std::string>(), "silkscreen" );
    BOOST_CHECK_EQUAL( stackup["layers"][3]["layer"].get<std::string>(), "F.Cu" );
    BOOST_CHECK_EQUAL( stackup["layers"][4]["typeName"].get<std::string>(), "core" );
    BOOST_CHECK_EQUAL( stackup["layers"][4]["dielectricIndex"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( stackup["layers"][5]["layer"].get<std::string>(), "In1.Cu" );
    BOOST_CHECK_EQUAL( stackup["layers"][8]["dielectricIndex"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( stackup["layers"][9]["layer"].get<std::string>(), "B.Cu" );
    BOOST_CHECK_EQUAL( stackup["layers"][12]["color"].get<std::string>(), "White" );
}


BOOST_AUTO_TEST_CASE( CompilesOneCanonicalCompleteNetclassRepresentation )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project controlled_netclasses)
  (net_classes
    (class Default
      (clearance 0.2mm) (track_width 0.2mm)
      (via_diameter 0.6mm) (via_drill 0.3mm)
      (microvia_diameter 0.3mm) (microvia_drill 0.1mm)
      (diff_pair_width 0.18mm) (diff_pair_gap 0.2mm) (diff_pair_via_gap 0.22mm)
      (tuning_profile none) (pcb_color default)
      (wire_width 0.15mm) (bus_width 0.3mm)
      (schematic_color default) (line_style solid))
    (class USB_HS
      (clearance inherit) (track_width 0.15mm)
      (via_diameter inherit) (via_drill inherit)
      (microvia_diameter 0.25mm) (microvia_drill inherit)
      (diff_pair_width 0.15mm) (diff_pair_gap 0.18mm) (diff_pair_via_gap inherit)
      (tuning_profile usb_hs) (pcb_color "#112233CC")
      (wire_width inherit) (bus_width inherit)
      (schematic_color "#AABBCC") (line_style dash_dot))
    (class POWER
      (clearance 0.3mm) (track_width 0.5mm)
      (via_diameter 0.8mm) (via_drill 0.4mm)
      (microvia_diameter inherit) (microvia_drill inherit)
      (diff_pair_width inherit) (diff_pair_gap inherit) (diff_pair_via_gap inherit)
      (tuning_profile inherit) (pcb_color inherit)
      (wire_width 0.2mm) (bus_width 0.4mm)
      (schematic_color inherit) (line_style inherit))
    (assign (pattern "USB_[PN]") (classes USB_HS))
    (assign (pattern "/power/VBUS[0..3]") (classes POWER USB_HS))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_REQUIRE( compiled.ir["netClasses"].is_object() );
    const nlohmann::json& netClasses = compiled.ir["netClasses"];
    BOOST_REQUIRE_EQUAL( netClasses["classes"].size(), 3 );
    BOOST_REQUIRE_EQUAL( netClasses["assignments"].size(), 2 );
    BOOST_CHECK_EQUAL( netClasses["classes"][0]["name"].get<std::string>(), "Default" );
    BOOST_CHECK_EQUAL( netClasses["classes"][0]["priority"].get<int64_t>(),
                       std::numeric_limits<int32_t>::max() );
    BOOST_CHECK_EQUAL( netClasses["classes"][1]["priority"].get<int64_t>(), 0 );
    BOOST_CHECK_EQUAL( netClasses["classes"][2]["priority"].get<int64_t>(), 1 );
    BOOST_CHECK( netClasses["classes"][1]["clearanceNm"].is_null() );
    BOOST_CHECK_EQUAL( netClasses["classes"][1]["trackWidthNm"].get<int64_t>(), 150000 );
    BOOST_CHECK_CLOSE( netClasses["classes"][1]["pcbColor"]["a"].get<double>(),
                       204.0 / 255.0, 0.0001 );
    BOOST_CHECK_EQUAL( netClasses["classes"][1]["lineStyle"].get<std::string>(),
                       "dash_dot" );
    BOOST_CHECK_EQUAL( netClasses["assignments"][1]["classes"].size(), 2 );
    BOOST_CHECK_EQUAL( compiled.plan["counts"]["netClasses"].get<size_t>(), 3 );
    BOOST_CHECK_EQUAL( compiled.plan["counts"]["netClassAssignments"].get<size_t>(), 2 );
}


BOOST_AUTO_TEST_CASE( CompilesEveryNativeCustomRuleConstraintFromOneKdsRepresentation )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project custom_rule_design)
  (custom_rules
    (rule production_constraints
      (condition always)
      (layer all)
      (severity error)
      (constraint assertion (test "A.Type != 'Footprint' || A.Orientation != 13deg"))
      (constraint clearance (min -0.01mm))
      (constraint creepage (min 1mm))
      (constraint hole_clearance (min 0.2mm))
      (constraint edge_clearance (min 0.3mm))
      (constraint hole_size (min 0.2mm) (opt 0.3mm) (max 1mm))
      (constraint hole_to_hole (min 0.25mm))
      (constraint courtyard_clearance (min 0.1mm))
      (constraint silk_clearance (min -0.05mm))
      (constraint text_height (min 0.8mm) (max 3mm))
      (constraint text_thickness (min 0.08mm) (max 0.5mm))
      (constraint track_width (min 0.15mm) (opt 0.2mm) (max 2mm))
      (constraint track_angle (min 45deg) (max 135deg))
      (constraint track_segment_length (min 0.1mm) (max 100mm))
      (constraint connection_width (min 0.15mm))
      (constraint annular_width (min 0.1mm) (max 0.5mm))
      (constraint via_diameter (min 0.4mm) (opt 0.6mm) (max 2mm))
      (constraint via_dangling)
      (constraint zone_connection (style thermal_reliefs))
      (constraint thermal_relief_gap (min 0.2mm))
      (constraint thermal_spoke_width (opt 0.3mm))
      (constraint min_resolved_spokes (count 2))
      (constraint solder_mask_expansion (opt 0.05mm))
      (constraint solder_mask_sliver (min 0.08mm))
      (constraint solder_paste_abs_margin (opt -0.03mm))
      (constraint solder_paste_rel_margin (ratio -0.1))
      (constraint disallow (items track through_via blind_via buried_via micro_via pad zone
        text graphic hole footprint))
      (constraint length (min 10ps) (opt 12ps) (max 14ps))
      (constraint skew (domain diff_pairs) (min -5ps) (opt 0ps) (max 5ps))
      (constraint via_count (min 0) (max 20))
      (constraint diff_pair_gap (min 0.15mm) (opt 0.2mm) (max 0.3mm))
      (constraint diff_pair_uncoupled (max 5mm))
      (constraint physical_clearance (min 0.2mm))
      (constraint physical_hole_clearance (min 0.25mm))
      (constraint bridged_mask))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_REQUIRE( compiled.ir["customRules"].is_object() );
    BOOST_REQUIRE_EQUAL( compiled.ir["customRules"]["rules"].size(), 1 );
    const nlohmann::json& rule = compiled.ir["customRules"]["rules"][0];
    BOOST_CHECK_EQUAL( rule["name"].get<std::string>(), "production_constraints" );
    BOOST_CHECK_EQUAL( rule["condition"].get<std::string>(), "always" );
    BOOST_CHECK_EQUAL( rule["layer"].get<std::string>(), "all" );
    BOOST_CHECK_EQUAL( rule["severity"].get<std::string>(), "error" );
    BOOST_CHECK_EQUAL( rule["constraints"].size(), 35 );
    BOOST_CHECK_EQUAL( compiled.plan["counts"]["customRules"].get<size_t>(), 1 );
    BOOST_CHECK_EQUAL( rule["constraints"][1]["values"]["min"]["value"].get<int64_t>(),
                       -10000 );
    BOOST_CHECK_EQUAL( rule["constraints"][27]["values"]["min"]["domain"].get<std::string>(),
                       "time" );
    BOOST_CHECK_EQUAL( rule["constraints"][28]["domain"].get<std::string>(), "diff_pairs" );
    BOOST_CHECK_EQUAL( rule["constraints"][25]["valuePermille"].get<int64_t>(), -100 );
}


BOOST_AUTO_TEST_CASE( RejectsAmbiguousOrUnsafeCustomRulePrograms )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project invalid_custom_rules)
  (custom_rules
    (rule duplicate
      (condition "${UNRESOLVED}")
      (layer all)
      (severity error)
      (constraint clearance (min 0.3mm) (min 0.2mm))
      (constraint clearance (min 0.1mm))
      (constraint track_angle (min 135deg) (max 45deg))
      (constraint skew (min 1mm) (max 2ps))
      (constraint disallow (items via through_via)))
    (rule duplicate
      (condition always)
      (severity warning)
      (layer F.Cu)
      (constraint solder_paste_rel_margin (ratio inf)))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_CHECK( !compiled.ok );
    const std::string diagnostics = compiled.diagnostics.dump();

    for( const char* code : { "invalid_custom_rule_condition",
                              "noncanonical_custom_constraint_values",
                              "unordered_custom_constraint_range",
                              "duplicate_custom_constraint",
                              "invalid_custom_skew_domain",
                              "mixed_custom_constraint_domains",
                              "redundant_custom_disallow", "duplicate_custom_rule",
                              "invalid_custom_rule_layer", "invalid_custom_rule_severity",
                              "invalid_custom_paste_ratio" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( RejectsAmbiguousUnsafeOrNonNativeNetclasses )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project invalid_netclasses)
  (net_classes
    (class Fast
      (clearance inherit) (track_width inherit)
      (via_diameter 0.2mm) (via_drill 0.3mm)
      (microvia_diameter inherit) (microvia_drill inherit)
      (diff_pair_width inherit) (diff_pair_gap inherit) (diff_pair_via_gap inherit)
      (tuning_profile inherit) (pcb_color inherit)
      (wire_width inherit) (bus_width inherit)
      (schematic_color inherit) (line_style inherit))
    (class Default
      (clearance inherit) (track_width 0.2mm)
      (via_diameter 0.6mm) (via_drill 0.3mm)
      (microvia_diameter 0.1mm) (microvia_drill 0.2mm)
      (diff_pair_width 0.18mm) (diff_pair_gap 0.2mm) (diff_pair_via_gap 0.22mm)
      (tuning_profile inherit) (pcb_color inherit)
      (wire_width 0.00015mm) (bus_width 0.3mm)
      (schematic_color inherit) (line_style inherit))
    (assign (pattern "BUS[0..999]") (classes Missing Fast Fast))
    (class fast
      (clearance inherit) (track_width inherit)
      (via_diameter inherit) (via_drill inherit)
      (microvia_diameter inherit) (microvia_drill inherit)
      (diff_pair_width inherit) (diff_pair_gap inherit) (diff_pair_via_gap inherit)
      (tuning_profile inherit) (pcb_color inherit)
      (wire_width inherit) (bus_width inherit)
      (schematic_color inherit) (line_style inherit))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_CHECK( !compiled.ok );
    const std::string diagnostics = compiled.diagnostics.dump();

    for( const char* code : { "invalid_default_netclass", "duplicate_netclass",
                              "noncanonical_netclass_order",
                              "invalid_default_netclass_inheritance",
                              "invalid_netclass_distance", "invalid_netclass_color",
                              "invalid_netclass_tuning_profile", "invalid_netclass_pattern",
                              "unknown_netclass_assignment",
                              "duplicate_netclass_assignment" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }

    const std::string geometrySource = R"KDS((kichad_design
  (version 1)
  (project invalid_netclass_geometry)
  (net_classes
    (class Default
      (clearance 0.2mm) (track_width 0.2mm)
      (via_diameter 0.6mm) (via_drill 0.3mm)
      (microvia_diameter 0.3mm) (microvia_drill 0.1mm)
      (diff_pair_width 0.18mm) (diff_pair_gap 0.2mm) (diff_pair_via_gap 0.22mm)
      (tuning_profile none) (pcb_color default)
      (wire_width 0.15mm) (bus_width 0.3mm)
      (schematic_color default) (line_style solid))
    (class Broken
      (clearance inherit) (track_width inherit)
      (via_diameter 0.2mm) (via_drill 0.3mm)
      (microvia_diameter 0.1mm) (microvia_drill 0.2mm)
      (diff_pair_width inherit) (diff_pair_gap inherit) (diff_pair_via_gap inherit)
      (tuning_profile inherit) (pcb_color inherit)
      (wire_width inherit) (bus_width inherit)
      (schematic_color inherit) (line_style inherit))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT geometry =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( geometrySource );
    BOOST_CHECK( !geometry.ok );
    BOOST_CHECK_NE( geometry.diagnostics.dump().find( "inconsistent_netclass_via_geometry" ),
                    std::string::npos );
    BOOST_CHECK_NE(
            geometry.diagnostics.dump().find( "inconsistent_netclass_microvia_geometry" ),
            std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectsLegacyDuplicateAndPhysicallyInconsistentGlobalRules )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project invalid_rules)
  (rule legacy_clearance (minimum 0.2mm))
  (rules
    (minimum_clearance 0.2mm)
    (minimum_clearance 0.3mm)
    (minimum_connection_width 0.2mm)
    (minimum_track_width 0.2mm)
    (minimum_via_annular_width 0.1mm)
    (minimum_via_diameter 0.4mm)
    (minimum_through_hole_diameter 0.3mm)
    (minimum_microvia_diameter 0.2mm)
    (minimum_microvia_drill 0.1mm)
    (minimum_hole_to_hole 0.25mm)
    (minimum_copper_to_hole_clearance 0.25mm)
    (minimum_silkscreen_clearance 0mm)
    (minimum_groove_width 0.25mm)
    (minimum_resolved_spokes 100)
    (minimum_silkscreen_text_height 0.8mm)
    (minimum_silkscreen_text_thickness 0.08mm)
    (minimum_copper_to_edge_clearance -0.01mm)
    (use_height_for_length_calculations true)
    (maximum_error 0.0005mm)
    (allow_fillets_outside_zone_outline maybe)
    (mystery_constraint 1mm))))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_CHECK( !compiled.ok );
    const std::string diagnostics = compiled.diagnostics.dump();

    for( const char* code : { "unknown_top_level_form", "duplicate_rules_field",
                              "unknown_rules_field", "invalid_rules_resolved_spokes",
                              "invalid_rules_edge_clearance", "inconsistent_rules_via_geometry",
                              "inconsistent_rules_microvia_geometry", "invalid_rules_distance",
                              "invalid_rules_zone_fillet_policy" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( RejectsLegacyOrStructurallyAmbiguousStackups )
{
    const std::string legacy = R"KDS((kichad_design
  (version 1)
  (project legacy_stackup)
  (board (stackup (copper_layers 4) (thickness 1.6mm))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT legacyResult =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( legacy );
    BOOST_CHECK( !legacyResult.ok );
    BOOST_CHECK_NE( legacyResult.diagnostics.dump().find( "unknown_board_field" ),
                    std::string::npos );
    BOOST_CHECK_NE( legacyResult.diagnostics.dump().find( "invalid_stackup_layers" ),
                    std::string::npos );

    const std::string ambiguous = R"KDS((kichad_design
  (version 1)
  (project ambiguous_stackup)
  (board
    (stackup
      (finish "ENIG") (impedance_controlled true)
      (edge_connector yes) (edge_plating false)
      (layers
        (soldermask F.Mask (thickness 0mm) (material "LPI")
          (epsilon_r 0.5) (loss_tangent 2) (color "Green"))
        (silkscreen F.SilkS (material "bad\nmaterial") (color "White"))
        (copper F.Cu (thickness 35um))
        (copper In2.Cu (thickness 35um))
        (dielectric foam (thickness 11mm) (material "")
          (epsilon_r 101) (loss_tangent -0.1) (locked maybe))
        (copper In1.Cu (thickness 35um))
        (dielectric core (thickness 11mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))
        (silkscreen B.SilkS (material "Ink") (color "White"))
        (soldermask B.Mask (thickness 10um) (material "LPI")
          (epsilon_r 3.5) (loss_tangent 0.02) (color "Green"))))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT rejected =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( ambiguous );
    BOOST_CHECK( !rejected.ok );
    const std::string diagnostics = rejected.diagnostics.dump();

    for( const char* code : { "invalid_stackup_order", "invalid_stackup_layers",
                              "invalid_stackup_dielectric_type",
                              "invalid_stackup_layer_thickness", "invalid_stackup_material",
                              "invalid_stackup_epsilon_r", "invalid_stackup_loss_tangent",
                              "invalid_stackup_dielectric_lock", "invalid_stackup_thickness" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( RejectsMalformedPhysicalBoardIntent )
{
    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project physical_errors)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net POWER (pin R1 1 1) (pin R2 1 1))
  (board
    (stackup
      (finish "ENIG") (impedance_controlled maybe)
      (edge_connector tapered) (edge_plating maybe)
      (layers
        (copper F.Cu (thickness -1mm))
        (dielectric core (thickness 11mm) (material "")
          (epsilon_r 0.5) (loss_tangent 2) (locked maybe))
        (copper In1.Cu (thickness 35um))
        (dielectric prepreg (thickness 11mm) (material "FR4")
          (epsilon_r 4.2) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))))
    (outline
      (rectangle same (start 0 0mm) (end -1mm 2mm)
        (radius 0mm) (stroke 0.05mm solid) (layers Edge.Cuts) (fill none)))
    (route POWER (id same) (from 0mm 0mm) (to 0mm 0mm)
      (width 0mm) (layer Edge.Cuts))
    (via POWER (id via1) (at 1mm 1mm) (diameter 0.4mm) (drill 0.5mm)
      (layers F.Cu In1.Cu) (type through))))
)KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );

    BOOST_CHECK( !result.ok );
    const std::string diagnostics = result.diagnostics.dump();

    for( const char* code : { "invalid_stackup_layers", "invalid_stackup_thickness",
                              "invalid_authored_board_outline_geometry",
                              "duplicate_board_id", "zero_length_route", "invalid_route_width",
                              "invalid_route_layer", "invalid_via_drill",
                              "invalid_through_via_layers" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( EnforcesStackupAndSinglePlacementSemantics )
{
    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project stackup_errors)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net POWER (pin R1 1 1) (pin R2 1 1))
  (board
    (stackup
      (finish "ENIG") (impedance_controlled true)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper In1.Cu (thickness 35um))
        (dielectric prepreg (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.2) (loss_tangent 0.02) (locked true))
        (copper In2.Cu (thickness 35um))
        (dielectric core (thickness 0.488mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))))
    (place R1 (at 1mm 1mm))
    (place R1 (at 2mm 2mm))
    (route POWER (id outside-route) (from 0mm 0mm) (to 1mm 1mm)
      (width 0.2mm) (layer In3.Cu))
    (route POWER (id overflow-route) (from 9223372036854775808nm 0mm) (to 1mm 1mm)
      (width 0.2mm) (layer F.Cu))
    (via POWER (id blind) (at 1mm 1mm) (diameter 0.8mm) (drill 0.4mm)
      (layers F.Cu B.Cu) (type blind))
    (via POWER (id buried) (at 2mm 2mm) (diameter 0.8mm) (drill 0.4mm)
      (layers F.Cu In1.Cu) (type buried))
    (via POWER (id micro) (at 3mm 3mm) (diameter 0.4mm) (drill 0.2mm)
      (layers F.Cu In2.Cu) (type micro))))
)KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );

    BOOST_CHECK( !result.ok );
    const std::string diagnostics = result.diagnostics.dump();

    for( const char* code : { "duplicate_board_placement", "route_layer_outside_stackup",
                              "invalid_route_start", "invalid_blind_via_span",
                              "invalid_buried_via_span", "invalid_microvia_span" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( RejectsAmbiguousOrUnsafeCopperZoneIntent )
{
    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project bad_zone)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net GND (pin R1 1 1) (pin R2 1 1))
  (board
    (stackup
      (finish "ENIG") (impedance_controlled true)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper In1.Cu (thickness 35um))
        (dielectric prepreg (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.2) (loss_tangent 0.02) (locked true))
        (copper In2.Cu (thickness 35um))
        (dielectric core (thickness 0.488mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))))
    (zone GND
      (id gnd-pour)
      (layers F.Cu F.Cu In3.Cu)
      (outline
        (polygon
          (point 0mm 0mm) (point 10mm 0mm) (point 0mm 0mm)
          (hole (point 1mm 1mm) (point 2mm 1mm))))
      (clearance -0.1mm)
      (min_thickness 0mm)
      (connection solid (thermal_gap 0.2mm))
      (islands keep_all (minimum_area 1mm2))
      (fill solid (gap 0.5mm))
      (hatch_offsets (layer F.Cu 0mm 0mm))
      (priority -1)
      (border solid (pitch 0.5mm))
      (locked maybe))
    (zone GND
      (id self-crossing)
      (layers F.Cu)
      (outline
        (polygon
          (point 0mm 0mm) (point 10mm 10mm) (point 0mm 10mm)
          (point 10mm 0mm) (point 10mm -2mm)))
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection solid)
      (islands keep_all)
      (fill solid))
    (zone GND
      (id outside-hole)
      (layers F.Cu)
      (outline
        (polygon
          (point 0mm 0mm) (point 10mm 0mm) (point 10mm 10mm) (point 0mm 10mm)
          (hole (point 20mm 20mm) (point 21mm 20mm) (point 21mm 21mm))))
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection solid)
      (islands keep_all)
      (fill solid))
    (zone GND
      (id invalid-hatch-offset)
      (layers F.Cu B.Cu)
      (outline
        (polygon
          (point 0mm 0mm) (point 10mm 0mm) (point 10mm 10mm) (point 0mm 10mm)))
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection solid)
      (islands keep_all)
      (fill hatched (thickness 0.3mm) (gap 0.4mm) (orientation 0deg))
      (hatch_offsets
        (layer F.Cu 0mm 0mm)
        (layer F.Cu 0.1mm 0.1mm)
        (layer In1.Cu 0mm 0mm)))))
)KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !result.ok );
    const std::string diagnostics = result.diagnostics.dump();

    for( const char* code : { "invalid_zone_layers", "duplicate_zone_point",
                              "invalid_zone_polygon", "invalid_zone_clearance",
                              "invalid_zone_min_thickness", "unexpected_zone_thermal_setting",
                              "unexpected_zone_minimum_island_area",
                              "unexpected_solid_zone_fill_setting", "invalid_zone_priority",
                              "unexpected_zone_hatch_offset",
                              "unexpected_zone_border_pitch", "invalid_zone_locked",
                              "zone_layer_outside_stackup",
                              "self_intersecting_zone_polygon",
                              "zone_hole_outside_polygon", "invalid_zone_hatch_offset" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( CompilesExplicitKeepoutPoliciesAndRejectsAmbiguousIntent )
{
    const std::string valid = R"KDS((kichad_design
  (version 1)
  (project keepout_board)
  (board
    (stackup
      (finish "ENIG") (impedance_controlled true)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper In1.Cu (thickness 35um))
        (dielectric prepreg (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.2) (loss_tangent 0.02) (locked true))
        (copper In2.Cu (thickness 35um))
        (dielectric core (thickness 0.488mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))))
    (keepout
      (id antenna-clearance)
      (name "Antenna clearance")
      (layers F.Cu B.Cu)
      (outline
        (polygon
          (point 1mm 1mm) (point 9mm 1mm) (point 9mm 7mm) (point 1mm 7mm)
          (hole (point 3mm 3mm) (point 4mm 3mm) (point 4mm 4mm))))
      (prohibit
        (copper true) (vias true) (tracks true) (pads false) (footprints true))
      (border diagonal_edge (pitch 0.5mm))
      (locked true))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( valid );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( compiled.ir["pcb"].size(), 2 );
    const nlohmann::json& keepout = compiled.ir["pcb"][1];
    BOOST_CHECK_EQUAL( keepout["kind"].get<std::string>(), "keepout" );
    BOOST_CHECK_EQUAL( keepout["logicalId"].get<std::string>(), "antenna-clearance" );
    BOOST_CHECK( keepout["prohibitions"]["copper"].get<bool>() );
    BOOST_CHECK( !keepout["prohibitions"]["pads"].get<bool>() );
    BOOST_CHECK_EQUAL( keepout["polygons"][0]["holes"].size(), 1 );
    BOOST_CHECK_EQUAL( keepout["border"]["pitchNm"].get<int64_t>(), 500000 );
    BOOST_CHECK( compiled.plan["boardFullyTyped"].get<bool>() );

    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project bad_keepout)
  (board
    (stackup
      (finish "ENIG") (impedance_controlled false)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 1.53mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked false))
        (copper B.Cu (thickness 35um))))
    (keepout
      (id ineffective)
      (layers F.Cu F.Cu In1.Cu)
      (outline
        (polygon (point 0mm 0mm) (point 5mm 0mm) (point 5mm 5mm)))
      (prohibit
        (copper false) (vias false) (tracks false) (pads false) (footprints false))
      (border solid (pitch 0.5mm))
      (locked maybe))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT rejected =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !rejected.ok );
    const std::string diagnostics = rejected.diagnostics.dump();

    for( const char* code : { "invalid_keepout_layers", "empty_keepout",
                              "unexpected_zone_border_pitch", "invalid_keepout_locked",
                              "keepout_layer_outside_stackup" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( CompilesBoundedBoardTextAndRejectsUnsafeTypography )
{
    const std::string valid = R"KDS((kichad_design
  (version 1)
  (project annotated_board)
  (board
    (stackup
      (finish "ENIG") (impedance_controlled true)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper In1.Cu (thickness 35um))
        (dielectric prepreg (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.2) (loss_tangent 0.02) (locked true))
        (copper In2.Cu (thickness 35um))
        (dielectric core (thickness 0.488mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))))
    (text "Assembly\nrevision A"
      (id assembly-note)
      (layer F.SilkS)
      (at 12mm 8mm)
      (size 1.5mm 2mm)
      (stroke 0.2mm)
      (angle 90deg)
      (justify left top)
      (font stroke)
      (line_spacing 1.25)
      (bold true)
      (italic true)
      (underlined true)
      (mirrored true)
      (keep_upright true)
      (hyperlink "https://example.com/revision-a")
      (knockout true)
      (locked true))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( valid );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( compiled.ir["pcb"].size(), 2 );
    const nlohmann::json& text = compiled.ir["pcb"][1];
    BOOST_CHECK_EQUAL( text["kind"].get<std::string>(), "text" );
    BOOST_CHECK_EQUAL( text["logicalId"].get<std::string>(), "assembly-note" );
    BOOST_CHECK_EQUAL( text["value"].get<std::string>(), "Assembly\nrevision A" );
    BOOST_CHECK_EQUAL( text["layer"].get<std::string>(), "F.SilkS" );
    BOOST_CHECK_EQUAL( text["size"]["xNm"].get<int64_t>(), 1500000 );
    BOOST_CHECK_EQUAL( text["strokeNm"].get<int64_t>(), 200000 );
    BOOST_CHECK_EQUAL( text["horizontalJustification"].get<std::string>(), "left" );
    BOOST_CHECK_EQUAL( text["verticalJustification"].get<std::string>(), "top" );
    BOOST_CHECK_CLOSE( text["lineSpacing"].get<double>(), 1.25, 0.0001 );
    BOOST_CHECK( text["multiline"].get<bool>() );
    BOOST_CHECK( text["bold"].get<bool>() );
    BOOST_CHECK( text["knockout"].get<bool>() );
    BOOST_CHECK( compiled.plan["boardFullyTyped"].get<bool>() );

    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project invalid_text)
  (board
    (stackup
      (finish "ENIG") (impedance_controlled false)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 1.53mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked false))
        (copper B.Cu (thickness 35um))))
    (text "Bad\rtext"
      (id unsafe-note)
      (layer In1.Cu)
      (at 3000mm 0mm)
      (size 0.5mm 0.5mm)
      (stroke 0.2mm)
      (justify sideways top)
      (font "")
      (line_spacing 0)
      (bold maybe)
      (hyperlink "line\nbreak"))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT rejected =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !rejected.ok );
    const std::string diagnostics = rejected.diagnostics.dump();

    for( const char* code : { "invalid_text_value", "invalid_text_position",
                              "invalid_text_stroke", "invalid_text_justification",
                              "invalid_text_font", "invalid_text_line_spacing",
                              "invalid_text_boolean", "invalid_text_hyperlink",
                              "text_layer_outside_stackup" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( CompilesAllNativeDimensionStylesWithoutIgnoredIntent )
{
    const std::string valid = R"KDS((kichad_design
  (version 1)
  (project dimension_board)
  (board
    (dimension aligned
      (id aligned-width) (layer Dwgs.User)
      (from 0mm 0mm) (to 20mm 0mm) (height 3mm) (extension_height 0.2mm)
      (units mm) (unit_format bare_suffix) (precision fixed_2)
      (suppress_trailing_zeroes true) (prefix "W=") (suffix " nominal")
      (line_width 0.15mm) (arrow_length 0.8mm) (extension_offset 0.1mm)
      (arrow_direction inward) (text_position manual) (keep_text_aligned false)
      (text_at 10mm 3mm) (text_size 1mm 1mm) (text_stroke 0.15mm)
      (text_angle 0deg) (text_justify center center) (font stroke) (locked true))
    (dimension orthogonal
      (id orthogonal-height) (layer Dwgs.User)
      (from 0mm 0mm) (to 0mm 10mm) (height -2mm) (extension_height 0mm) (axis y)
      (units automatic) (unit_format no_suffix) (precision scaled_in_4)
      (line_width 0.15mm) (arrow_length 0.8mm) (extension_offset 0mm)
      (arrow_direction outward) (text_position inline) (keep_text_aligned true)
      (text_size 1mm 1mm) (text_stroke 0.15mm))
    (dimension radial
      (id radius) (layer Dwgs.User)
      (center 30mm 20mm) (radius_point 35mm 20mm) (leader_length 3mm)
      (units mm) (unit_format paren_suffix) (precision fixed_3)
      (prefix "R ") (line_width 0.15mm) (arrow_length 0.8mm)
      (keep_text_aligned false) (text_at 40mm 22mm)
      (text_size 1mm 1mm) (text_stroke 0.15mm) (text_angle 30deg))
    (dimension leader
      (id callout) (layer Dwgs.User)
      (from 5mm 15mm) (to 10mm 15mm) (border roundrect) (label "Inspect joint")
      (line_width 0.15mm) (arrow_length 0.8mm) (extension_offset 0.1mm)
      (text_at 15mm 15mm) (text_size 1mm 1mm) (text_stroke 0.15mm)
      (bold true) (italic true))
    (dimension center
      (id hole-center) (layer Dwgs.User)
      (center 25mm 25mm) (to 28mm 25mm)
      (line_width 0.15mm) (arrow_length 0.8mm) (locked true))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( valid );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( compiled.ir["pcb"].size(), 5 );
    BOOST_CHECK_EQUAL( compiled.ir["pcb"][0]["dimensionStyle"].get<std::string>(), "aligned" );
    BOOST_CHECK_EQUAL( compiled.ir["pcb"][1]["geometry"]["axis"].get<std::string>(), "y" );
    BOOST_CHECK_EQUAL( compiled.ir["pcb"][2]["geometry"]["leaderLengthNm"].get<int64_t>(),
                       3000000 );
    BOOST_CHECK_EQUAL( compiled.ir["pcb"][3]["overrideText"].get<std::string>(),
                       "Inspect joint" );
    BOOST_CHECK( compiled.ir["pcb"][3]["overrideEnabled"].get<bool>() );
    BOOST_CHECK_EQUAL( compiled.ir["pcb"][4]["dimensionStyle"].get<std::string>(), "center" );
    BOOST_CHECK( compiled.ir["pcb"][4]["text"].empty() );
    BOOST_CHECK( compiled.plan["boardFullyTyped"].get<bool>() );

    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project invalid_dimension)
  (board
    (dimension center
      (id bad-center) (layer Missing.Layer)
      (center 1mm 1mm) (to 1mm 1mm)
      (line_width 0mm)
      (text_size 1mm 1mm))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT rejected =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !rejected.ok );
    const std::string diagnostics = rejected.diagnostics.dump();

    for( const char* code : { "unknown_board_field", "invalid_dimension_layer",
                              "invalid_dimension_line_width", "invalid_dimension_arrow_length",
                              "zero_length_dimension" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( BoundsCopperZoneGeometryBeforePlanning )
{
    std::string oversized = R"KDS((kichad_design
  (version 1)
  (project bounded_zone)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net GND (pin R1 1 1) (pin R2 1 1))
  (board
    (zone GND
      (id too-many-points)
      (layers F.Cu)
      (outline (polygon )KDS";

    for( int point = 0; point < 8193; ++point )
        oversized += "(point " + std::to_string( point ) + "nm 0nm)";

    oversized += R"KDS())
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection solid)
      (islands keep_all)
      (fill solid))))
)KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( oversized );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "too_many_zone_points" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectsAmbiguousAndUnsupportedFacetDeclarations )
{
    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project strict (title "First") (title "Second")
    (comment 1 "First") (comment 1 "Second") (comment 10 "Out of range")
    (comment "legacy unindexed form"))
  (library symbol Device (table project) (uri "${KIPRJMOD}/one.kicad_sym")
    (uri "${KIPRJMOD}/two.kicad_sym"))
  (library symbol Device (table global) (uri "forbidden.kicad_sym"))
  (library footprint Unsafe (table project) (uri "${KIPRJMOD}/../Unsafe.pretty"))
  (library model MissingTable)
  (sheet repeated)
  (sheet repeated)
  (component R1
    (symbol "Device:R") (value "1k") (footprint "R:R")
    (property "Tolerance" "1%") (property "Tolerance" "5%")
    (dnp false) (dnp true))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net N1 (pin R1 1 1) (pin R2 1 1))
  (net N2 (pin R1 1 1) (pin R2 1 2))
  (board
    (place MISSING (at 1mm 1mm))
    (route NO_NET (width 0.2mm))
    (teleport R1))
  (source R1 (manufacturer "Maker") (mpn "Part"))
  (source R1 (manufacturer "Maker") (mpn "Part"))
  (rules)
  (rules)
  (check erc verbose)
  (output assembly))
)KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );

    BOOST_CHECK( !result.ok );
    const std::string diagnostics = result.diagnostics.dump();

    for( const char* code : { "duplicate_project_field", "duplicate_project_comment",
                              "invalid_project_comment", "duplicate_library_field",
                              "duplicate_library", "redundant_global_library_uri",
                              "invalid_project_library_uri", "invalid_library_table",
                              "duplicate_sheet",
                              "duplicate_component_property",
                              "duplicate_component_field", "duplicate_pin_connection",
                              "unknown_board_statement", "duplicate_rules", "duplicate_source",
                              "invalid_check",
                              "invalid_output", "unresolved_component", "unresolved_net" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( CompilesAndMeasuresGeneralPhysicalLayoutContract )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project physical_contract)
  (component U1 (symbol "Connector_Generic:Conn_01x02") (value "Controller")
    (footprint "Local:Controller"))
  (component C1 (symbol "Device:C") (value "100n") (footprint "Local:C"))
  (component J1 (symbol "Connector_Generic:Conn_01x02") (value "Input")
    (footprint "Local:Input"))
  (component J2 (symbol "Connector_Generic:Conn_01x02") (value "Output")
    (footprint "Local:Output"))
  (net N1 (pin U1 1 1) (pin C1 1 1))
  (net N2 (pin J1 1 1) (pin J2 1 1))
  (board
    (outline
      (rectangle perimeter (start 0mm 0mm) (end 40mm 20mm)
        (stroke 0.05mm solid) (layers Edge.Cuts) (fill none)))
    (layout
      (board (maximum_width 40mm) (maximum_height 20mm))
      (placement
        (near C1 U1 (maximum 3mm))
        (align C1 U1 (axis y) (tolerance 0mm))
        (edge J1 right (maximum 1mm))
        (group control (members C1 U1) (maximum_span 3mm)))
      (routing
        (defaults (geometry octilinear) (maximum_vias 2))
        (net N1 (maximum_vias 1) (maximum_length 15mm))
        (bundle pair (nets N1 N2) (maximum_skew 0mm))))
    (place U1 (at 12mm 10mm))
    (place C1 (at 10mm 10mm))
    (place J1 (at 39mm 5mm))
    (place J2 (at 39mm 15mm))
    (route N1 (id n1) (from 0mm 0mm) (to 10mm 0mm)
      (width 0.2mm) (layer F.Cu))
    (route N2 (id n2) (from 0mm 1mm) (to 10mm 1mm)
      (width 0.2mm) (layer F.Cu))
    (via N1 (id n1-via) (at 5mm 0mm) (diameter 0.6mm) (drill 0.3mm)
      (layers F.Cu B.Cu) (type through)))
  (check layout)))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    auto found = std::find_if( compiled.ir["pcb"].begin(), compiled.ir["pcb"].end(),
                               []( const nlohmann::json& aStatement )
                               {
                                   return aStatement.value( "kind", "" ) == "layout";
                               } );
    BOOST_REQUIRE( found != compiled.ir["pcb"].end() );
    BOOST_CHECK_EQUAL( ( *found )["placement"].size(), 4 );
    BOOST_CHECK_EQUAL( ( *found )["routing"]["nets"].size(), 1 );
    BOOST_CHECK_EQUAL( ( *found )["routing"]["bundles"].size(), 1 );
    KICHAD::DESIGN_SCRIPT_LAYOUT_ANALYZER::RESULT analyzed =
            KICHAD::DESIGN_SCRIPT_LAYOUT_ANALYZER::Analyze( compiled.ir );
    BOOST_CHECK_MESSAGE( analyzed.clean, analyzed.issues.dump() );
    BOOST_CHECK_EQUAL( analyzed.summary["board"]["widthNm"].get<int64_t>(), 40000000 );
    BOOST_REQUIRE_EQUAL( analyzed.summary["nets"].size(), 2 );

    std::string violated = source;
    const size_t width = violated.find( "(maximum_width 40mm)" );
    BOOST_REQUIRE_NE( width, std::string::npos );
    violated.replace( width, std::string( "(maximum_width 40mm)" ).size(),
                      "(maximum_width 30mm)" );
    const size_t geometry = violated.find( "(geometry octilinear)" );
    BOOST_REQUIRE_NE( geometry, std::string::npos );
    violated.replace( geometry, std::string( "(geometry octilinear)" ).size(),
                      "(geometry orthogonal)" );
    const size_t end = violated.find( "(to 10mm 0mm)" );
    BOOST_REQUIRE_NE( end, std::string::npos );
    violated.replace( end, std::string( "(to 10mm 0mm)" ).size(), "(to 10mm 5mm)" );
    compiled = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( violated );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    analyzed = KICHAD::DESIGN_SCRIPT_LAYOUT_ANALYZER::Analyze( compiled.ir );
    BOOST_CHECK( !analyzed.clean );
    const std::string issues = analyzed.issues.dump();
    BOOST_CHECK_NE( issues.find( "board_width_exceeded" ), std::string::npos );
    BOOST_CHECK_NE( issues.find( "net_geometry_violation" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectsMalformedPhysicalLayoutContract )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project invalid_layout)
  (component U1 (symbol "Device:R") (value "1k") (footprint "Local:R"))
  (component U2 (symbol "Device:R") (value "1k") (footprint "Local:R"))
  (net N1 (pin U1 1 1) (pin U2 1 1))
  (board
    (layout
      (board (maximum_width 0mm) (maximum_height wide))
      (placement
        (near U1 U1 (maximum -1mm))
        (align U1 U2 (axis z) (tolerance invalid))
        (edge U1 diagonal (maximum 1mm))
        (group bad (members U1 U1) (maximum_span 0mm)))
      (routing
        (defaults (geometry curved) (maximum_vias -1))
        (net N1)
        (bundle bad (nets N1 N1) (maximum_skew bad))))
    (place U1 (at 1mm 1mm))
    (place U2 (at 2mm 2mm))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_CHECK( !compiled.ok );
    const std::string diagnostics = compiled.diagnostics.dump();

    for( const char* code : { "invalid_layout_board_limit", "invalid_layout_near",
                              "invalid_layout_align", "invalid_layout_edge",
                              "invalid_layout_group", "invalid_layout_routing_geometry",
                              "invalid_layout_routing_vias", "empty_layout_net_policy",
                              "invalid_layout_bundle" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( CompilesIpcD356ProductionOutput )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile(
                    "(kichad_design (version 1) (project electrical_test) "
                    "(output ipcd356))" );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( result.ir["outputs"].size(), 1 );
    BOOST_CHECK_EQUAL( result.ir["outputs"][0]["kind"].get<std::string>(), "ipcd356" );
}


BOOST_AUTO_TEST_CASE( CompilesIpc2581ProductionOutput )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile(
                    "(kichad_design (version 1) (project manufacturing_data) "
                    "(output ipc2581))" );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( result.ir["outputs"].size(), 1 );
    BOOST_CHECK_EQUAL( result.ir["outputs"][0]["kind"].get<std::string>(), "ipc2581" );
}


BOOST_AUTO_TEST_CASE( CompilesOdbppProductionOutput )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile(
                    "(kichad_design (version 1) (project odb_manufacturing_data) "
                    "(output odbpp))" );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( result.ir["outputs"].size(), 1 );
    BOOST_CHECK_EQUAL( result.ir["outputs"][0]["kind"].get<std::string>(), "odbpp" );
}


BOOST_AUTO_TEST_CASE( CompilesNativeAuxiliaryManufacturingOutputs )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project auxiliary_manufacturing)
  (output netlist)
  (output stepz)
  (output brep)
  (output glb)
  (output stl)
  (output u3d)
  (output xao)
  (output 3d_pdf)
  (output board_ps)
  (output board_render)
  (output schematic_pdf)
  (output schematic_svg)
  (output schematic_dxf)
  (output schematic_ps)
  (output schematic_bom)
  (output legacy_bom_xml)
  (output assembly_svg)
  (output assembly_dxf)
  (output gencad)
  (output vrml)
  (output board_stats)
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( result.ir["outputs"].size(), 21 );

    const std::array<const char*, 21> expected = {
        "netlist", "stepz", "brep", "glb", "stl", "u3d", "xao", "3d_pdf",
        "board_ps", "board_render", "schematic_pdf", "schematic_svg", "schematic_dxf",
        "schematic_ps", "schematic_bom", "legacy_bom_xml", "assembly_svg",
        "assembly_dxf", "gencad", "vrml", "board_stats"
    };

    for( size_t index = 0; index < expected.size(); ++index )
        BOOST_CHECK_EQUAL( result.ir["outputs"][index]["kind"], expected[index] );
}


BOOST_AUTO_TEST_CASE( CompilesCanonicalGlobalAndProjectLibraryDependencies )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project canonical_libraries)
  (library symbol Device (table global))
  (library symbol LocalSymbols (table project)
    (uri "${KIPRJMOD}/libraries/local.kicad_sym"))
  (library footprint LocalFootprints (table project)
    (uri "${KIPRJMOD}/libraries/Local.pretty"))
  (library model LocalModels (table project)
    (uri "${KIPRJMOD}/libraries/models"))
  (component R1 (symbol "Device:R") (value "1k")
    (footprint "LocalFootprints:R_0603"))
))KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( result.ir["libraries"].size(), 4 );
    BOOST_CHECK( result.ir["libraries"][0]["uri"].is_null() );
    BOOST_CHECK_EQUAL( result.ir["libraries"][1]["table"].get<std::string>(), "project" );
    BOOST_CHECK_EQUAL( result.ir["libraries"][1]["uri"].get<std::string>(),
                       "${KIPRJMOD}/libraries/local.kicad_sym" );
    BOOST_CHECK_EQUAL( result.plan["counts"]["libraries"].get<size_t>(), 4 );

    const std::string unresolved = R"KDS((kichad_design
  (version 1)
  (project unresolved_library)
  (library symbol Device (table global))
  (component R1 (symbol "Missing:R") (value "1k") (footprint "Missing:R"))
))KDS";
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( unresolved );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "unresolved_component_library" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( CompilesOneExplicitResolvableSchematicHierarchy )
{
    const std::string source = R"KDS((kichad_design
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
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( result.ir["schematic"]["sheets"].size(), 2 );
    const nlohmann::json& power = result.ir["schematic"]["sheets"][1];
    BOOST_CHECK_EQUAL( power["parent"].get<std::string>(), "root" );
    BOOST_CHECK_EQUAL( power["position"]["xNm"].get<int64_t>(), 20000000 );
    BOOST_CHECK_EQUAL( power["size"]["yNm"].get<int64_t>(), 20000000 );
    BOOST_REQUIRE_EQUAL( power["pins"].size(), 2 );
    BOOST_CHECK_EQUAL( power["pins"][1]["side"].get<std::string>(), "right" );

    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project broken_hierarchy)
  (sheet root (parent none) (file "broken_hierarchy.kicad_sch") (title "Main"))
  (sheet a (parent b) (file "shared.kicad_sch") (title "Duplicate")
    (at 10mm 10mm) (size 20mm 10mm)
    (pin BAD input (at 15mm 15mm) (side left)))
  (sheet b (parent a) (file "shared.kicad_sch") (title "Cycle")
    (at 40mm 10mm) (size 20mm 10mm))
  (sheet missing (parent root) (file "../escape.kicad_sch") (title "Duplicate")
    (at 10mm 30mm) (size 20mm 10mm))
  (sheet duplicate (parent root) (file "duplicate.kicad_sch") (title "Duplicate")
    (at 40mm 30mm) (size 20mm 10mm))
))KDS";
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !result.ok );
    const std::string diagnostics = result.diagnostics.dump();

    for( const char* code : { "invalid_sheet_file", "sheet_pin_off_edge",
                              "duplicate_sibling_sheet_title", "recursive_sheet_hierarchy",
                              "recursive_sheet_file" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( CompilesOneExplicitMultiUnitComponentAndEndpointRepresentation )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project multi_unit)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (library footprint LocalFp (table project) (uri "${KIPRJMOD}/Local.pretty"))
  (sheet root (parent none) (file "multi_unit.kicad_sch") (title "Main"))
  (sheet analog (parent root) (file "analog.kicad_sch") (title "Analog")
    (at 20mm 20mm) (size 30mm 20mm))
  (component U1
    (symbol "Local:DUAL") (value "DUAL") (footprint "LocalFp:DUAL")
    (datasheet "https://example.com/dual.pdf")
    (description "AI-selected precision dual amplifier")
    (property "Manufacturer Part" "OPA2192")
    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none))
    (unit 2 (sheet analog) (at 50mm 40mm) (rotation 270deg) (mirror xy)
      (fields_autoplaced false)
      (field Value
        (at 52mm 42mm) (rotation 12.5deg) (visible true)
        (show_name true) (autoplace false) (size 1.2mm 1.5mm)
        (font "DejaVu Sans") (line_spacing 1.25) (thickness 0.2mm)
        (color #11223380) (justify right top) (bold true) (italic true)
        (hyperlink "https://example.com/value") (private false))
      (field "Manufacturer Part"
        (at 52mm 44mm) (rotation 0deg) (visible false)
        (show_name false) (autoplace true) (size 1mm 1mm)
        (font stroke) (line_spacing 1) (thickness auto)
        (color default) (justify center center) (bold false) (italic false)
        (hyperlink none) (private true))))
  (net SIGNAL (pin U1 1 1) (pin U1 2 7))
  (no_connect U1 2 8)
  (label signal-local (sheet root) (scope local) (net SIGNAL) (at 45mm 40mm)
    (rotation 0deg) (shape none) (size 1.27mm 1.27mm) (thickness auto)
    (justify left bottom) (bold false) (italic false))
  (label signal-global (sheet analog) (scope global) (net SIGNAL) (at 55mm 40mm)
    (rotation 180deg) (shape output) (size 1.5mm 1.2mm) (thickness 0.2mm)
    (justify right center) (bold true) (italic true))
  (bus_alias SIGNALS (sheet root) (members SIGNAL))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    const nlohmann::json& component = result.ir["schematic"]["components"][0];
    BOOST_REQUIRE_EQUAL( component["units"].size(), 2 );
    BOOST_CHECK_EQUAL( component["units"][1]["number"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( component["units"][1]["sheet"].get<std::string>(), "analog" );
    BOOST_CHECK_EQUAL( component["units"][1]["rotationDegrees"].get<int>(), 270 );
    BOOST_CHECK_EQUAL( component["units"][1]["mirror"].get<std::string>(), "xy" );
    BOOST_CHECK( !component["units"][1]["fieldsAutoplaced"].get<bool>() );
    BOOST_REQUIRE_EQUAL( component["units"][1]["fields"].size(), 2 );
    BOOST_CHECK_EQUAL( component["units"][1]["fields"][0]["name"], "Value" );
    BOOST_CHECK_EQUAL( component["units"][1]["fields"][0]["rotationDegrees"], 12.5 );
    BOOST_CHECK_EQUAL( component["units"][1]["fields"][0]["color"]["a"], 128 );
    BOOST_CHECK( component["units"][1]["fields"][1]["private"].get<bool>() );
    BOOST_CHECK_EQUAL( component["datasheet"], "https://example.com/dual.pdf" );
    BOOST_CHECK_EQUAL( component["description"],
                       "AI-selected precision dual amplifier" );
    BOOST_CHECK_EQUAL( result.ir["schematic"]["nets"][0]["pins"][1]["unit"].get<int>(), 2 );
    BOOST_REQUIRE_EQUAL( result.ir["schematic"]["noConnects"].size(), 1 );
    BOOST_REQUIRE_EQUAL( result.ir["schematic"]["drawings"].size(), 2 );
    BOOST_CHECK_EQUAL( result.ir["schematic"]["drawings"][0]["scope"], "local" );
    BOOST_CHECK_EQUAL( result.ir["schematic"]["drawings"][1]["shape"], "output" );
    BOOST_CHECK_EQUAL( result.ir["schematic"]["drawings"][1]["thicknessNm"], 200000 );
    BOOST_REQUIRE_EQUAL( result.ir["schematic"]["busAliases"].size(), 1 );
    BOOST_CHECK_EQUAL( result.ir["schematic"]["busAliases"][0]["members"][0], "SIGNAL" );
    BOOST_CHECK_EQUAL( result.plan["counts"]["noConnects"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( result.ir["schematic"]["nets"][0]["presentation"], "auto" );

    std::string labeledHierarchy = source;
    const std::string automaticNet = "(net SIGNAL ";
    const size_t netAt = labeledHierarchy.find( automaticNet );
    BOOST_REQUIRE_NE( netAt, std::string::npos );
    labeledHierarchy.replace( netAt, automaticNet.size(),
                              "(net SIGNAL (presentation labels) " );
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( labeledHierarchy );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_CHECK_NE( result.diagnostics.dump().find(
                            "cross_sheet_global_label_presentation" ),
                    std::string::npos );

    const std::string invalid = R"KDS((kichad_design
  (version 1) (project invalid_units)
  (sheet root (parent none) (file "invalid_units.kicad_sch") (title "Main"))
  (component U1 (symbol "Local:DUAL") (value "DUAL") (footprint "LocalFp:DUAL")
    (unit 1 (sheet missing) (at 1mm 1mm) (rotation 45deg) (mirror none)))
  (net OLD (pin U1 1) (pin U1 2))
))KDS";
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !result.ok );
    const std::string diagnostics = result.diagnostics.dump();
    BOOST_CHECK_NE( diagnostics.find( "invalid_component_unit_rotation" ), std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "unresolved_component_sheet" ), std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_pin" ), std::string::npos );

    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile(
            "(kichad_design (version 1) (project board_only) "
            "(component R1 (symbol \"Local:R\") (value \"R\") (footprint \"Local:R\")) "
            "(no_connect R1 1 2))" );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "no_connect_without_hierarchy" ),
                    std::string::npos );

    std::string invalidField = source;
    const std::string declaredField = "(field \"Manufacturer Part\"";
    const size_t fieldAt = invalidField.find( declaredField );
    BOOST_REQUIRE_NE( fieldAt, std::string::npos );
    invalidField.replace( fieldAt, declaredField.size(), "(field \"Missing Property\"" );
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalidField );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find(
                            "unresolved_component_unit_field_layout" ),
                    std::string::npos );

    invalidField = source;
    const std::string mandatoryPrivate = "(hyperlink \"https://example.com/value\") "
                                         "(private false)";
    const size_t privateAt = invalidField.find( mandatoryPrivate );
    BOOST_REQUIRE_NE( privateAt, std::string::npos );
    invalidField.replace( privateAt, mandatoryPrivate.size(),
                          "(hyperlink \"https://example.com/value\") (private true)" );
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalidField );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find(
                            "private_mandatory_component_unit_field" ),
                    std::string::npos );

    invalidField = source;
    const std::string customProperty =
            "(property \"Manufacturer Part\" \"OPA2192\")";
    const size_t propertyAt = invalidField.find( customProperty );
    BOOST_REQUIRE_NE( propertyAt, std::string::npos );
    invalidField.replace( propertyAt, customProperty.size(),
                          "(property \"reference\" \"BAD\")" );
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalidField );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "reserved_component_property" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( CompilesVirtualComponentsWithOneCanonicalFootprintNoneRepresentation )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project virtual_component)
  (library symbol Power (table project) (uri "${KIPRJMOD}/Power.kicad_sym"))
  (sheet root (parent none) (file "virtual_component.kicad_sch") (title "Main"))
  (component #PWR01
    (symbol "Power:GND") (value "GND") (footprint none)
    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none)))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( result.ir["schematic"]["components"].size(), 1 );
    BOOST_CHECK( result.ir["schematic"]["components"][0]["footprint"].is_null() );

    const std::string invalidPlacement = R"KDS((kichad_design
  (version 1)
  (project invalid_virtual_placement)
  (library symbol Power (table project) (uri "${KIPRJMOD}/Power.kicad_sym"))
  (sheet root (parent none) (file "invalid_virtual_placement.kicad_sch") (title "Main"))
  (component #PWR01
    (symbol "Power:GND") (value "GND") (footprint none)
    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none)))
  (board (place #PWR01 (at 10mm 10mm) (rotation 0deg) (side front)))
))KDS";
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalidPlacement );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "component_without_footprint_placement" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( CompilesCanonicalNativeSchematicDrawingPrimitives )
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
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    const nlohmann::json& drawings = result.ir["schematic"]["drawings"];
    BOOST_REQUIRE_EQUAL( drawings.size(), 4 );
    BOOST_CHECK_EQUAL( drawings[0]["kind"], "wire" );
    BOOST_CHECK_EQUAL( drawings[0]["stroke"]["widthNm"], 0 );
    BOOST_CHECK_EQUAL( drawings[1]["stroke"]["widthNm"], 500000 );
    BOOST_CHECK_EQUAL( drawings[1]["stroke"]["lineStyle"], "dash_dot" );
    BOOST_CHECK_EQUAL( drawings[2]["to"]["xNm"], 26270000 );
    BOOST_CHECK_EQUAL( drawings[3]["diameterNm"], 800000 );
    BOOST_CHECK_EQUAL( drawings[3]["color"]["a"], 128 );
    BOOST_CHECK_EQUAL( result.plan["counts"]["drawings"], 4 );
}


BOOST_AUTO_TEST_CASE( CompilesOneExplicitNetTargetedSchematicDirectiveRepresentation )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project schematic_directive)
  (sheet root (parent none) (file "schematic_directive.kicad_sch") (title "Directive"))
  (component R1 (symbol "Local:R") (value "10k") (footprint "Local:R")
    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none)))
  (component R2 (symbol "Local:R") (value "20k") (footprint "Local:R")
    (unit 1 (sheet root) (at 60mm 40mm) (rotation 0deg) (mirror none)))
  (net SIGNAL (pin R1 1 1) (pin R2 1 1))
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
    (property "Impedance Policy" "controlled"
      (at 46mm 38mm) (rotation 0deg) (size 1.27mm 1.5mm)
      (thickness 0.2mm) (justify left bottom)
      (bold true) (italic false) (visible true))
    (property "Review Note" "route as pair"
      (at 46mm 42mm) (rotation 180deg) (size 1mm 1mm)
      (thickness auto) (justify right top)
      (bold false) (italic true) (visible false)))
  (directive area-policy
    (sheet root) (target rule_area analog-policy) (at 70mm 40mm)
    (rotation 0deg) (shape rectangle) (length 1.27mm)
    (property "Component Class" "ANALOG"
      (at 72mm 40mm) (rotation 0deg) (size 1.27mm 1.27mm)
      (thickness auto) (justify left center)
      (bold false) (italic false) (visible true)))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    const nlohmann::json& ruleArea = result.ir["schematic"]["drawings"][0];
    BOOST_CHECK_EQUAL( ruleArea["kind"], "rule_area" );
    BOOST_CHECK_EQUAL( ruleArea["polygon"].size(), 4 );
    BOOST_CHECK_EQUAL( ruleArea["stroke"]["widthNm"], 200000 );
    BOOST_CHECK_EQUAL( ruleArea["stroke"]["color"]["a"], 128 );
    BOOST_CHECK_EQUAL( ruleArea["fill"]["type"], "hatch" );
    BOOST_CHECK_EQUAL( ruleArea["fill"]["color"]["a"], 153 );
    BOOST_CHECK( ruleArea["exclude_from_sim"].get<bool>() );
    BOOST_CHECK( ruleArea["exclude_from_bom"].get<bool>() );
    BOOST_CHECK( !ruleArea["exclude_from_board"].get<bool>() );
    BOOST_CHECK( ruleArea["dnp"].get<bool>() );
    const nlohmann::json& directive = result.ir["schematic"]["drawings"][1];
    BOOST_CHECK_EQUAL( directive["kind"], "directive" );
    BOOST_CHECK_EQUAL( directive["target"]["kind"], "net" );
    BOOST_CHECK_EQUAL( directive["target"]["name"], "SIGNAL" );
    BOOST_CHECK_EQUAL( directive["rotationDegrees"], 90 );
    BOOST_CHECK_EQUAL( directive["shape"], "diamond" );
    BOOST_CHECK_EQUAL( directive["lengthNm"], 2540000 );
    BOOST_REQUIRE_EQUAL( directive["properties"].size(), 2 );
    BOOST_CHECK_EQUAL( directive["properties"][0]["thicknessNm"], 200000 );
    BOOST_CHECK( directive["properties"][0]["bold"].get<bool>() );
    BOOST_CHECK( !directive["properties"][1]["visible"].get<bool>() );
    BOOST_CHECK_EQUAL( result.ir["schematic"]["drawings"][2]["target"]["kind"],
                       "rule_area" );
    BOOST_CHECK_EQUAL( result.ir["schematic"]["drawings"][2]["target"]["name"],
                       "analog-policy" );
    BOOST_CHECK_EQUAL( result.plan["counts"]["drawings"], 3 );

    std::string detached = program;
    const std::string attachedText = "(target rule_area analog-policy) (at 70mm 40mm)";
    const size_t attachedPosition = detached.find( attachedText );
    BOOST_REQUIRE_NE( attachedPosition, std::string::npos );
    detached.replace( attachedPosition, attachedText.size(),
                      "(target rule_area analog-policy) (at 80mm 40mm)" );
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( detached );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "detached_schematic_directive_rule_area" ),
                    std::string::npos );

    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile(
            R"KDS((kichad_design (version 1) (project invalid_directive)
              (sheet root (parent none) (file "invalid.kicad_sch") (title "Invalid"))
              (directive d (sheet root) (target net MISSING) (at 1mm 1mm)
                (rotation 0deg) (shape round) (length 2.54mm))))KDS" );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "missing_schematic_directive_property" ),
                    std::string::npos );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "unresolved_schematic_directive_net" ),
                    std::string::npos );

    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile(
            R"KDS((kichad_design (version 1) (project invalid_rule_area)
              (sheet root (parent none) (file "invalid.kicad_sch") (title "Invalid"))
              (rule_area crossed (sheet root)
                (polygon (point 1mm 1mm) (point 5mm 5mm)
                  (point 1mm 5mm) (point 5mm 1mm))
                (stroke default dash default) (fill none default)
                (exclude_from_sim false) (exclude_from_bom false)
                (exclude_from_board false) (dnp false))))KDS" );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "invalid_schematic_rule_area_polygon" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectsAmbiguousOrUnsafeSchematicDrawingPrimitives )
{
    const std::vector<std::string> programs = {
        R"KDS((kichad_design (version 1) (project no_hierarchy)
          (wire w (sheet root) (from 1mm 1mm) (to 2mm 1mm)
            (stroke default default))))KDS",
        R"KDS((kichad_design (version 1) (project bad_sheet)
          (sheet root (parent none) (file "bad_sheet.kicad_sch") (title "Bad"))
          (junction j (sheet missing) (at 1mm 1mm)
            (diameter auto) (color default))))KDS",
        R"KDS((kichad_design (version 1) (project duplicate_id)
          (sheet root (parent none) (file "duplicate_id.kicad_sch") (title "Bad"))
          (wire same (sheet root) (from 1mm 1mm) (to 2mm 1mm)
            (stroke default default))
          (junction same (sheet root) (at 2mm 1mm)
            (diameter auto) (color default))))KDS",
        R"KDS((kichad_design (version 1) (project zero_length)
          (sheet root (parent none) (file "zero_length.kicad_sch") (title "Bad"))
          (bus b (sheet root) (from 1mm 1mm) (to 1mm 1mm)
            (stroke default default))))KDS",
        R"KDS((kichad_design (version 1) (project flat_entry)
          (sheet root (parent none) (file "flat_entry.kicad_sch") (title "Bad"))
          (bus_entry e (sheet root) (from 1mm 1mm) (to 2mm 1mm)
            (stroke default default))))KDS",
        R"KDS((kichad_design (version 1) (project bad_stroke)
          (sheet root (parent none) (file "bad_stroke.kicad_sch") (title "Bad"))
          (wire w (sheet root) (from 1mm 1mm) (to 2mm 1mm)
            (stroke 0.001mm zigzag))))KDS",
        R"KDS((kichad_design (version 1) (project unresolved_label)
          (sheet root (parent none) (file "unresolved_label.kicad_sch") (title "Bad"))
          (label l (sheet root) (scope local) (net MISSING) (at 1mm 1mm)
            (rotation 0deg) (shape none) (size 1.27mm 1.27mm) (thickness auto)
            (justify left bottom) (bold false) (italic false))))KDS",
        R"KDS((kichad_design (version 1) (project bad_label_shape)
          (sheet root (parent none) (file "bad_label_shape.kicad_sch") (title "Bad"))
          (label l (sheet root) (scope local) (net MISSING) (at 1mm 1mm)
            (rotation 0deg) (shape input) (size 1.27mm 1.27mm) (thickness auto)
            (justify left bottom) (bold false) (italic false))))KDS",
        R"KDS((kichad_design (version 1) (project bad_bus_alias)
          (sheet root (parent none) (file "bad_bus_alias.kicad_sch") (title "Bad"))
          (bus_alias CONTROL (sheet root) (members MISSING MISSING))))KDS"
    };

    for( const std::string& program : programs )
    {
        const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
                KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
        BOOST_CHECK_MESSAGE( !result.ok, program );
    }
}


BOOST_AUTO_TEST_CASE( CompilesCompleteStableSchematicFreeText )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project schematic_text)
  (sheet root (parent none) (file "schematic_text.kicad_sch") (title "Text"))
  (text "AI note\nwith context"
    (id design-note) (sheet root) (at 25mm 35mm) (rotation 15.5deg)
    (exclude_from_sim true) (size 1.2mm 1.5mm) (font "DejaVu Sans")
    (line_spacing 1.25) (thickness 0.2mm) (color #11223380)
    (justify right top) (mirror true) (bold true) (italic true)
    (hyperlink "https://example.com/design-note"))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    const nlohmann::json& text = result.ir["schematic"]["drawings"][0];
    BOOST_CHECK_EQUAL( text["kind"], "text" );
    BOOST_CHECK_EQUAL( text["id"], "design-note" );
    BOOST_CHECK_EQUAL( text["content"], "AI note\nwith context" );
    BOOST_CHECK_CLOSE( text["rotationDegrees"].get<double>(), 15.5, 0.000001 );
    BOOST_CHECK( text["exclude_from_sim"].get<bool>() );
    BOOST_CHECK_EQUAL( text["font"], "DejaVu Sans" );
    BOOST_CHECK_CLOSE( text["lineSpacing"].get<double>(), 1.25, 0.000001 );
    BOOST_CHECK_EQUAL( text["thicknessNm"], 200000 );
    BOOST_CHECK_EQUAL( text["color"]["a"], 128 );
    BOOST_CHECK_EQUAL( text["justify"]["horizontal"], "right" );
    BOOST_CHECK( text["mirror"].get<bool>() );
    BOOST_CHECK( text["bold"].get<bool>() );
    BOOST_CHECK( text["italic"].get<bool>() );
    BOOST_CHECK_EQUAL( text["hyperlink"], "https://example.com/design-note" );

    std::string invalid = program;
    const size_t rotation = invalid.find( "15.5deg" );
    BOOST_REQUIRE_NE( rotation, std::string::npos );
    invalid.replace( rotation, 7, "360deg" );
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "invalid_schematic_text_rotation" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( CompilesCompleteStableSchematicTextBox )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project schematic_text_box)
  (sheet root (parent none) (file "schematic_text_box.kicad_sch") (title "Text Box"))
  (text_box "AI constraint summary\nwith bounded context"
    (id constraint-summary) (sheet root) (at 25mm 35mm) (rotation 22.5deg)
    (box_size 30mm 12mm) (margins 0.5mm 0.75mm 1mm 1.25mm)
    (exclude_from_sim false) (stroke none dash_dot #10203080)
    (fill cross_hatch #40506099)
    (text_size 1.2mm 1.5mm) (font "DejaVu Sans")
    (line_spacing 1.25) (thickness 0.2mm) (color #708090cc)
    (justify left top) (mirror true) (bold true) (italic true)
    (hyperlink "https://example.com/constraint-summary"))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    const nlohmann::json& textBox = result.ir["schematic"]["drawings"][0];
    BOOST_CHECK_EQUAL( textBox["kind"], "text_box" );
    BOOST_CHECK_EQUAL( textBox["id"], "constraint-summary" );
    BOOST_CHECK_EQUAL( textBox["content"], "AI constraint summary\nwith bounded context" );
    BOOST_CHECK_CLOSE( textBox["rotationDegrees"].get<double>(), 22.5, 0.000001 );
    BOOST_CHECK_EQUAL( textBox["boxSize"]["xNm"], 30000000 );
    BOOST_CHECK_EQUAL( textBox["margins"]["bottomNm"], 1250000 );
    BOOST_CHECK_EQUAL( textBox["stroke"]["widthNm"], -1 );
    BOOST_CHECK_EQUAL( textBox["stroke"]["lineStyle"], "dash_dot" );
    BOOST_CHECK_EQUAL( textBox["stroke"]["color"]["a"], 128 );
    BOOST_CHECK_EQUAL( textBox["fill"]["type"], "cross_hatch" );
    BOOST_CHECK_EQUAL( textBox["fill"]["color"]["a"], 153 );
    BOOST_CHECK_EQUAL( textBox["size"]["yNm"], 1500000 );
    BOOST_CHECK_EQUAL( textBox["color"]["a"], 204 );
    BOOST_CHECK( textBox["mirror"].get<bool>() );
    BOOST_CHECK( textBox["bold"].get<bool>() );
    BOOST_CHECK( textBox["italic"].get<bool>() );

    std::string invalid = program;
    const size_t position = invalid.find( "25mm 35mm" );
    BOOST_REQUIRE_NE( position, std::string::npos );
    invalid.replace( position, 9, "1990mm 35mm" );
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "invalid_schematic_text_box_extent" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( CompilesEveryNativeSchematicGraphicGeometry )
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
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    const nlohmann::json& drawings = result.ir["schematic"]["drawings"];
    BOOST_REQUIRE_EQUAL( drawings.size(), 5 );
    BOOST_CHECK_EQUAL( drawings[0]["kind"], "polyline" );
    BOOST_CHECK_EQUAL( drawings[0]["points"].size(), 3 );
    BOOST_CHECK_EQUAL( drawings[1]["kind"], "rectangle" );
    BOOST_CHECK_EQUAL( drawings[1]["cornerRadiusNm"], 2000000 );
    BOOST_CHECK_EQUAL( drawings[2]["kind"], "circle" );
    BOOST_CHECK_EQUAL( drawings[2]["stroke"]["widthNm"], -1 );
    BOOST_CHECK_EQUAL( drawings[2]["fill"]["type"], "hatch" );
    BOOST_CHECK_EQUAL( drawings[3]["kind"], "arc" );
    BOOST_CHECK_EQUAL( drawings[3]["fill"]["type"], "background" );
    BOOST_CHECK_EQUAL( drawings[4]["kind"], "bezier" );
    BOOST_CHECK_EQUAL( drawings[4]["points"].size(), 4 );
    BOOST_CHECK_EQUAL( drawings[4]["stroke"]["color"]["a"], 221 );

    std::string invalid = program;
    const size_t midpoint = invalid.find( "80mm 20mm" );
    BOOST_REQUIRE_NE( midpoint, std::string::npos );
    invalid.replace( midpoint, 9, "80mm 30mm" );
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "invalid_schematic_arc_geometry" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( CompilesDigestVerifiedSelfContainedSchematicImage )
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
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    const nlohmann::json& image = result.ir["schematic"]["drawings"][0];
    BOOST_CHECK_EQUAL( image["kind"], "image" );
    BOOST_CHECK_EQUAL( image["id"], "system-overview" );
    BOOST_CHECK_CLOSE( image["scale"].get<double>(), 1.25, 0.000001 );
    BOOST_CHECK_EQUAL( image["mediaType"], "image/png" );
    BOOST_CHECK_EQUAL( image["byteCount"], 68 );
    BOOST_CHECK_EQUAL( image["pixels"]["width"], 1 );
    BOOST_CHECK_EQUAL( image["pixels"]["height"], 1 );
    BOOST_CHECK_EQUAL( image["description"],
                       "AI-readable one-pixel system overview fixture" );

    std::string invalid = program;
    const size_t digest = invalid.find( "431ced6916a2a21a" );
    BOOST_REQUIRE_NE( digest, std::string::npos );
    invalid[digest] = '5';
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "schematic_image_digest_mismatch" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( CompilesCompleteAiNativeSchematicTableGrid )
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
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    const nlohmann::json& table = result.ir["schematic"]["drawings"][0];
    BOOST_CHECK_EQUAL( table["kind"], "table" );
    BOOST_CHECK_EQUAL( table["id"], "pin-summary" );
    BOOST_CHECK_EQUAL( table["rotationDegrees"], 90 );
    BOOST_CHECK_EQUAL( table["columnWidthsNm"].size(), 2 );
    BOOST_CHECK_EQUAL( table["rowHeightsNm"][1], 10000000 );
    BOOST_CHECK_EQUAL( table["cells"].size(), 4 );
    BOOST_CHECK_EQUAL( table["cells"][0]["fill"]["color"]["a"], 153 );
    BOOST_CHECK_EQUAL( table["cells"][0]["font"], "DejaVu Sans" );
    BOOST_CHECK( table["cells"][0]["exclude_from_sim"].get<bool>() );
    BOOST_CHECK_EQUAL( table["merges"].size(), 1 );
    BOOST_CHECK_EQUAL( table["merges"][0]["lastColumn"], 1 );

    std::string invalid = program;
    const size_t duplicate = invalid.find( "(cell 2 2" );
    BOOST_REQUIRE_NE( duplicate, std::string::npos );
    invalid.replace( duplicate, 9, "(cell 2 1" );
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "duplicate_schematic_table_cell" ),
                    std::string::npos );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "incomplete_schematic_table_cells" ),
                    std::string::npos );

    invalid = program;
    const std::string emptyCovered = "(cell 1 2 \"\"";
    const size_t covered = invalid.find( emptyCovered );
    BOOST_REQUIRE_NE( covered, std::string::npos );
    invalid.replace( covered, emptyCovered.size(), "(cell 1 2 \"hidden\"" );
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "covered_schematic_table_cell_content" ),
                    std::string::npos );

    invalid = program;
    const std::string oneMerge = "(merges (merge 1 1 1 2))";
    const size_t merge = invalid.find( oneMerge );
    BOOST_REQUIRE_NE( merge, std::string::npos );
    invalid.replace( merge, oneMerge.size(),
                     "(merges (merge 1 1 1 2) (merge 1 2 2 2))" );
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "overlapping_schematic_table_merge" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( CompilesTypedNestedSchematicGroupsWithNativeScreenSemantics )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project schematic_groups)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (library footprint Local (table project) (uri "${KIPRJMOD}/Local.pretty"))
  (sheet root (parent none) (file "schematic_groups.kicad_sch") (title "Groups"))
  (sheet child (parent root) (file "child.kicad_sch") (title "Child")
    (at 20mm 20mm) (size 30mm 20mm)
    (pin SIG input (at 20mm 25mm) (side left)))
  (component U1 (symbol "Local:Part") (value "Part") (footprint "Local:Part")
    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none)))
  (component U2 (symbol "Local:Part") (value "Part") (footprint "Local:Part")
    (unit 1 (sheet root) (at 60mm 40mm) (rotation 0deg) (mirror none)))
  (net SIG (pin U1 1 1) (pin U2 1 1))
  (no_connect U1 1 2)
  (wire root-wire (sheet root) (from 40mm 40mm) (to 60mm 40mm)
    (stroke default solid))
  (wire child-wire (sheet child) (from 20mm 25mm) (to 30mm 25mm)
    (stroke 0.2mm dash))
  (group signal-core (sheet root) (name "Signal core") (locked true)
    (members
      (member component U1 1)
      (member net_label SIG U1 1 1 1)))
  (group root-bundle (sheet root) (name "Root bundle") (locked false)
    (members
      (member drawing root-wire)
      (member sheet child)
      (member no_connect U1 1 2 1)
      (member group signal-core)))
  (group child-interface (sheet child) (name "Child interface") (locked true)
    (members
      (member hierarchical_label child SIG)
      (member drawing child-wire)))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    const nlohmann::json& groups = result.ir["schematic"]["groups"];
    BOOST_REQUIRE_EQUAL( groups.size(), 3 );
    BOOST_CHECK_EQUAL( groups[0]["id"], "signal-core" );
    BOOST_CHECK( groups[0]["locked"].get<bool>() );
    BOOST_CHECK_EQUAL( groups[0]["members"][0]["kind"], "component" );
    BOOST_CHECK_EQUAL( groups[0]["members"][1]["occurrence"], 1 );
    BOOST_CHECK_EQUAL( groups[1]["members"][3]["kind"], "group" );
    BOOST_CHECK_EQUAL( result.plan["counts"]["groups"], 3 );

    std::string invalid = program;
    const std::string nested = "(member group signal-core)";
    const size_t nestedAt = invalid.find( nested );
    BOOST_REQUIRE_NE( nestedAt, std::string::npos );
    invalid.replace( nestedAt, nested.size(), "(member group root-bundle)" );
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "recursive_schematic_group" ),
                    std::string::npos );

    invalid = program;
    const std::string childDrawing = "(member drawing child-wire)";
    const size_t childDrawingAt = invalid.find( childDrawing );
    BOOST_REQUIRE_NE( childDrawingAt, std::string::npos );
    invalid.replace( childDrawingAt, childDrawing.size(), "(member drawing root-wire)" );
    result = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find(
                            "mismatched_schematic_group_member_sheet" ),
                    std::string::npos );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "multiply_grouped_schematic_item" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( BoundsCanonicalProjectLibraryTablesBeforePlanning )
{
    std::string source = R"KDS((kichad_design
  (version 1)
  (project bounded_libraries)
)KDS";

    for( int index = 0; index < 257; ++index )
    {
        source += "(library symbol Local" + std::to_string( index )
                  + " (table project) (uri \"${KIPRJMOD}/Local"
                  + std::to_string( index ) + ".kicad_sym\"))\n";
    }

    source += ")";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "too_many_project_libraries" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( ReportsAllSemanticErrorsWithoutProducingAnExecutableProgram )
{
    const std::string invalid = R"KDS((kichad_design
  (version 2)
  (project broken)
  (component R1 (symbol "Device:R") (value "1k"))
  (component R1 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net N1 (pin R1 1 1) (pin MISSING 1 2))
  (mystery unsafe))
)KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );

    BOOST_CHECK( !result.ok );
    BOOST_CHECK_GE( result.diagnostics.size(), 5 );

    std::string diagnostics = result.diagnostics.dump();
    BOOST_CHECK_NE( diagnostics.find( "invalid_version" ), std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "missing_component_field" ), std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "duplicate_component" ), std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "unresolved_component" ), std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "unknown_top_level_form" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( BoundsMalformedAndHostilePrograms )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT malformed =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( "(kichad_design (version 1)" );
    BOOST_CHECK( !malformed.ok );
    BOOST_REQUIRE( !malformed.diagnostics.empty() );
    BOOST_CHECK_EQUAL( malformed.diagnostics[0]["code"].get<std::string>(), "parse_failed" );

    std::string deep = "(kichad_design (version 1) (project deep) ";

    for( int i = 0; i < 300; ++i )
        deep += "(x ";

    deep += "value";

    for( int i = 0; i < 300; ++i )
        deep += ')';

    deep += ')';

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT nested =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( deep );
    BOOST_CHECK( !nested.ok );
    BOOST_REQUIRE( !nested.diagnostics.empty() );
    BOOST_CHECK_NE( nested.diagnostics[0]["message"].get<std::string>().find( "256 levels" ),
                    std::string::npos );

    std::string oversized( 16 * 1024 * 1024 + 1, 'x' );
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT large =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( oversized );
    BOOST_CHECK( !large.ok );
    BOOST_CHECK_EQUAL( large.diagnostics[0]["code"].get<std::string>(), "invalid_source_size" );

    std::string embeddedNul = "(kichad_design";
    embeddedNul.push_back( '\0' );
    embeddedNul += "(version 1))";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT nul =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( embeddedNul );
    BOOST_CHECK( !nul.ok );
    BOOST_CHECK_EQUAL( nul.diagnostics[0]["code"].get<std::string>(), "invalid_encoding" );
    BOOST_CHECK_EQUAL( nul.diagnostics[0]["byteOffset"].get<size_t>(), 14 );
    BOOST_CHECK_EQUAL( nul.diagnostics[0]["line"].get<size_t>(), 1 );
    BOOST_CHECK_EQUAL( nul.diagnostics[0]["column"].get<size_t>(), 15 );

    const std::string invalidUtf8 = "(kichad_design (version 1) (project \"\xC3\x28\"))";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT utf8 =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalidUtf8 );
    BOOST_CHECK( !utf8.ok );
    BOOST_CHECK_EQUAL( utf8.diagnostics[0]["code"].get<std::string>(), "invalid_encoding" );
    BOOST_CHECK_EQUAL( utf8.diagnostics[0]["byteOffset"].get<size_t>(), invalidUtf8.find( '\xC3' ) );
    BOOST_CHECK_EQUAL( utf8.diagnostics[0]["line"].get<size_t>(), 1 );
    BOOST_CHECK_EQUAL( utf8.diagnostics[0]["column"].get<size_t>(),
                       invalidUtf8.find( '\xC3' ) + 1 );
}


BOOST_AUTO_TEST_CASE( MinimalNewProjectSidecarIsAValidReusableProgram )
{
    const std::string source =
            "(kichad_design\n  (version 1)\n  (project \"My Project\"))\n";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_CHECK_EQUAL( result.ir["project"]["name"].get<std::string>(), "My Project" );
    BOOST_CHECK_EQUAL( result.plan["counts"]["components"].get<size_t>(), 0 );
}


BOOST_AUTO_TEST_CASE( CompilesAndExecutesTypedElectricalSimulationContracts )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project divider)
  (library symbol Device (table global))
  (component R1 (symbol "Device:R") (value "10k") (footprint none))
  (component R2 (symbol "Device:R") (value "10k") (footprint none))
  (component R3 (symbol "Device:R") (value "test fixture") (footprint none))
  (net VIN (pin R1 1 1) (pin R3 1 1))
  (net OUT (pin R1 1 2) (pin R2 1 1))
  (net GND (pin R2 1 2) (pin R3 1 2))
  (electrical
    (simulation divider_dc
      (ground GND)
      (device upper (component R1) (unit 1) (kind resistor)
        (nodes VIN OUT) (pins 1 2) (value 10kohm))
      (device lower (component R2) (unit 1) (kind resistor)
        (nodes OUT GND) (pins 1 2) (value 10kohm))
      (source supply (kind voltage) (positive VIN) (negative GND) (dc 5V))
      (analysis nominal (kind operating_point))
      (assert output_voltage (analysis nominal) (probe voltage OUT)
        (minimum 2.49V) (maximum 2.51V) (scope all))))
  (check electrical)
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_CHECK_EQUAL( compiled.plan["counts"]["electricalSimulations"].get<size_t>(), 1 );
    KICHAD::DESIGN_SCRIPT_SIMULATION_RUNNER::RESULT simulated =
            KICHAD::DESIGN_SCRIPT_SIMULATION_RUNNER::Run( compiled.ir );
    BOOST_REQUIRE_MESSAGE( simulated.ok, simulated.error );
    BOOST_CHECK_MESSAGE( simulated.clean, simulated.issues.dump() );
    BOOST_CHECK_EQUAL( simulated.summary["analyses"].get<size_t>(), 1 );
    BOOST_CHECK_EQUAL( simulated.summary["assertions"].get<size_t>(), 1 );
}


BOOST_AUTO_TEST_CASE( SynthesizesDeterministicCourtyardAwarePlacementAndOrthogonalCopper )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project synthesized_board)
  (library symbol Device (table global))
  (library footprint Resistor_SMD (table global))
  (component R1 (symbol "Device:R") (value "10k")
    (footprint "Resistor_SMD:R_0603_1608Metric"))
  (component R2 (symbol "Device:R") (value "10k")
    (footprint "Resistor_SMD:R_0603_1608Metric"))
  (net N1 (pin R1 1 1) (pin R2 1 1))
  (net N2 (pin R1 1 2) (pin R2 1 2))
  (board
    (outline
      (rectangle edge (start 0mm 0mm) (end 20mm 12mm)
        (stroke 0.05mm solid) (layers Edge.Cuts) (fill none)))
    (synthesize
      (placement (grid 1mm) (clearance 0.5mm) (edge_clearance 1mm)
        (rotations 0deg 90deg))
      (routing (grid 0.5mm) (clearance 0.2mm) (width 0.25mm) (layer F.Cu)))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );

    const std::string footprint = R"FP((footprint "R_0603_1608Metric"
  (version 20260206)
  (generator "pcbnew")
  (layer "F.Cu")
  (fp_rect (start -1.5 -1) (end 1.5 1)
    (stroke (width 0.05) (type default)) (fill none) (layer "F.CrtYd"))
  (pad "1" smd roundrect (at -0.75 0) (size 0.8 0.9)
    (layers "F.Cu" "F.Paste" "F.Mask") (roundrect_rratio 0.25))
  (pad "2" smd roundrect (at 0.75 0) (size 0.8 0.9)
    (layers "F.Cu" "F.Paste" "F.Mask") (roundrect_rratio 0.25))))FP";
    const nlohmann::json sources = {
        { "Resistor_SMD:R_0603_1608Metric", footprint }
    };
    KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIZER::RESULT synthesized =
            KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIZER::Synthesize( compiled.ir, sources );
    BOOST_REQUIRE_MESSAGE( synthesized.ok, synthesized.error );
    BOOST_CHECK_EQUAL( synthesized.summary["placements"].get<size_t>(), 2 );
    BOOST_CHECK_GT( synthesized.summary["routes"].get<size_t>(), 0 );

    size_t placements = 0;
    size_t traces = 0;

    for( const nlohmann::json& statement : synthesized.ir["pcb"] )
    {
        if( statement.value( "kind", "" ) == "place" )
            ++placements;
        else if( statement.value( "kind", "" ) == "trace" )
        {
            ++traces;
            BOOST_CHECK( statement["start"]["xNm"] == statement["end"]["xNm"]
                         || statement["start"]["yNm"] == statement["end"]["yNm"] );
        }
    }

    BOOST_CHECK_EQUAL( placements, 2 );
    BOOST_CHECK_EQUAL( traces, synthesized.summary["routes"].get<size_t>() );
}


BOOST_AUTO_TEST_CASE( DescribesAStableVersionedLanguageWithoutHostExecution )
{
    nlohmann::json description = KICHAD::DESIGN_SCRIPT_COMPILER::Describe();
    BOOST_CHECK_EQUAL( description["language"].get<std::string>(), "kichad-design" );
    BOOST_CHECK_EQUAL( description["version"].get<int>(), 1 );
    BOOST_CHECK( description["deterministic"].get<bool>() );
    BOOST_CHECK( !description["hostCodeExecution"].get<bool>() );
    BOOST_CHECK_GE( description["topLevelForms"].size(), 10 );
    BOOST_CHECK_NE( description.dump().find( "thermal_spoke_width" ), std::string::npos );
    BOOST_CHECK_NE( description.dump().find( "hatch_offsets" ), std::string::npos );
    BOOST_CHECK_NE( description.dump().find( "prohibit" ), std::string::npos );
    BOOST_CHECK_NE( description.dump().find( "edge_connector none|yes|bevelled" ),
                    std::string::npos );
    BOOST_CHECK_NE( description.dump().find( "dielectric core|prepreg" ),
                    std::string::npos );
    BOOST_CHECK_NE( description.dump().find( "font stroke|NAME" ), std::string::npos );
    BOOST_CHECK_NE( description.dump().find( "comment 1..9 TEXT" ), std::string::npos );
    BOOST_CHECK_NE( description.dump().find(
                            "dimension aligned|orthogonal|radial|leader|center" ),
                    std::string::npos );
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT example =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile(
                    description["example"].get<std::string>() );
    BOOST_CHECK_MESSAGE( example.ok, example.diagnostics.dump() );
}


BOOST_AUTO_TEST_CASE( DescribesAuthoritativeExhaustiveCapabilityCoverageWithoutASecondFormat )
{
    const nlohmann::json description = KICHAD::DESIGN_SCRIPT_COMPILER::Describe();
    BOOST_REQUIRE( description.contains( "capabilityCoverage" ) );
    const nlohmann::json& coverage = description["capabilityCoverage"];
    BOOST_CHECK( coverage["authoritative"].get<bool>() );
    BOOST_CHECK_EQUAL( coverage["target"]["kicad"].get<std::string>(), "10.0.4" );
    BOOST_CHECK_EQUAL( coverage["target"]["kds"].get<int>(), 1 );
    BOOST_CHECK( !coverage["complete"].get<bool>() );
    BOOST_CHECK_NE( coverage["representation"].get<std::string>().find(
                            ".kicad_kds remains the single authored design representation" ),
                    std::string::npos );
    BOOST_REQUIRE( coverage["qualification"].is_array() );
    BOOST_CHECK_EQUAL( coverage["qualification"].size(), 9 );
    BOOST_REQUIRE( coverage["facets"].is_array() );
    BOOST_CHECK_GE( coverage["facets"].size(), 80 );

    const std::set<std::string> expectedDomains = {
        "language", "project", "schematic", "symbols", "simulation", "pcb",
        "footprints", "manufacturing", "interchange", "editor", "auxiliary"
    };
    const std::set<std::string> allowedStates = {
        "qualified", "partial", "unrepresented"
    };
    std::set<std::string> ids;
    std::set<std::string> domains;
    std::map<std::string, size_t> counts;
    bool foundQualifiedTitleBlock = false;
    bool foundQualifiedDirectives = false;
    bool foundQualifiedTextGraphics = false;
    bool foundQualifiedGroups = false;
    bool foundQualifiedFields = false;
    bool foundQualifiedTextVariables = false;
    bool foundQualifiedFieldTemplates = false;

    for( const nlohmann::json& facet : coverage["facets"] )
    {
        BOOST_REQUIRE( facet.is_object() );
        const std::string id = facet["id"].get<std::string>();
        const std::string domain = facet["domain"].get<std::string>();
        const std::string state = facet["state"].get<std::string>();
        BOOST_CHECK( ids.emplace( id ).second );
        BOOST_CHECK( expectedDomains.contains( domain ) );
        BOOST_CHECK( allowedStates.contains( state ) );
        BOOST_CHECK( facet["kdsForms"].is_array() );
        BOOST_CHECK( !facet["controls"].get<std::string>().empty() );
        BOOST_CHECK( facet["gaps"].is_array() );

        if( state == "qualified" )
            BOOST_CHECK( facet["gaps"].empty() );
        else
            BOOST_CHECK( !facet["gaps"].empty() );

        domains.emplace( domain );
        ++counts[state];

        if( id == "project.title_block" )
            foundQualifiedTitleBlock = state == "qualified" && facet["gaps"].empty();

        if( id == "schematic.directive_labels" )
        {
            foundQualifiedDirectives =
                    state == "qualified" && facet["gaps"].empty()
                    && facet["kdsForms"] == nlohmann::json::array( { "directive",
                                                                     "rule_area" } );
        }

        if( id == "schematic.text_graphics" )
        {
            foundQualifiedTextGraphics =
                    state == "qualified" && facet["gaps"].empty()
                    && std::find( facet["kdsForms"].begin(), facet["kdsForms"].end(), "table" )
                               != facet["kdsForms"].end();
        }

        if( id == "schematic.groups" )
        {
            foundQualifiedGroups = state == "qualified" && facet["gaps"].empty()
                                   && facet["kdsForms"]
                                              == nlohmann::json::array( { "group" } );
        }

        if( id == "schematic.fields" )
        {
            foundQualifiedFields = state == "qualified" && facet["gaps"].empty()
                                   && facet["kdsForms"]
                                              == nlohmann::json::array( { "component" } );
        }

        if( id == "project.text_variables" )
        {
            foundQualifiedTextVariables = state == "qualified" && facet["gaps"].empty()
                                          && facet["kdsForms"]
                                                     == nlohmann::json::array(
                                                             { "text_variables" } );
        }

        if( id == "project.field_templates" )
        {
            foundQualifiedFieldTemplates = state == "qualified" && facet["gaps"].empty()
                                           && facet["kdsForms"]
                                                      == nlohmann::json::array(
                                                              { "field_templates" } );
        }
    }

    BOOST_CHECK( foundQualifiedTitleBlock );
    BOOST_CHECK( foundQualifiedDirectives );
    BOOST_CHECK( foundQualifiedTextGraphics );
    BOOST_CHECK( foundQualifiedGroups );
    BOOST_CHECK( foundQualifiedFields );
    BOOST_CHECK( foundQualifiedTextVariables );
    BOOST_CHECK( foundQualifiedFieldTemplates );
    BOOST_CHECK( domains == expectedDomains );

    for( const std::string& state : allowedStates )
        BOOST_CHECK_EQUAL( coverage["counts"][state].get<size_t>(), counts[state] );

    BOOST_CHECK_EQUAL( coverage["complete"].get<bool>(),
                       counts["partial"] == 0 && counts["unrepresented"] == 0 );
}


BOOST_AUTO_TEST_SUITE_END()
