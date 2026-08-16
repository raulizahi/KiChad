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
#include <kicad/codex/design_script_footprint_library_generator.h>
#include <kicad/codex/codex_tool_internal.h>
#include <kicad/codex/managed_footprint_library_io.h>

#include <wx/utils.h>


namespace
{

class BUILD_CLI_ENVIRONMENT
{
public:
    BUILD_CLI_ENVIRONMENT() :
            m_hadPrevious( wxGetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ), &m_previous ) )
    {
        wxSetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ), wxS( "1" ) );
    }

    ~BUILD_CLI_ENVIRONMENT()
    {
        if( m_hadPrevious )
            wxSetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ), m_previous );
        else
            wxUnsetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ) );
    }

private:
    bool     m_hadPrevious;
    wxString m_previous;
};

} // namespace


BOOST_AUTO_TEST_SUITE( DesignScriptFootprintAuthoring )


BOOST_AUTO_TEST_CASE( LowersStandardPadsAndModelsToCurrentDeterministicFootprints )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project authored_footprints)
  (library footprint Product (table project)
    (uri "${KIPRJMOD}/libraries/Product.pretty") (managed true))
  (footprint Product:SENSOR_2P
    (reference U) (value SENSOR_2P)
    (datasheet "https://example.test/sensor.pdf")
    (description "Two-pad sensor package") (keywords "sensor smd")
    (attributes (smd true) (exclude_from_position false)
      (exclude_from_bom false) (allow_missing_courtyard true))
    (net_tie_group 1 2)
    (line silk_top (start -1.2mm -0.7mm) (end 1.2mm -0.7mm)
      (stroke 0.12mm solid) (layers F.SilkS))
    (rectangle fab_body (start -1mm -0.5mm) (end 1mm 0.5mm)
      (radius 0.1mm) (stroke 0.1mm solid) (layers F.Fab) (fill none))
    (arc pin_arc (start -1.2mm 0mm) (mid -1mm -0.2mm) (end -0.8mm 0mm)
      (stroke 0.1mm dash) (layers F.SilkS))
    (circle pin_mark (center -1mm 0mm) (radius 0.2mm)
      (stroke 0.05mm solid) (layers F.SilkS) (fill solid))
    (polygon fab_pin_one (point -1mm -0.5mm) (point -0.6mm -0.5mm)
      (point -1mm -0.1mm) (stroke 0.05mm solid) (layers F.Fab) (fill solid))
    (bezier logo_curve (start -0.5mm 0.6mm) (control1 -0.2mm 0.3mm)
      (control2 0.2mm 0.9mm) (end 0.5mm 0.6mm)
      (stroke 0.08mm dash_dot) (layers F.SilkS))
    (text body_reference "${REFERENCE}" (at 0mm 0mm) (rotation 0deg)
      (layer F.Fab)
      (font (face default) (size 0.5mm 0.5mm) (line_spacing 1)
        (thickness 0.08mm) (bold false) (italic false))
      (justify center center false) (locked false) (keep_upright true)
      (knockout false))
    (text_box pin_note "PIN 1" (box -1.9mm -1.4mm -0.6mm -0.8mm)
      (rotation 0deg) (layer F.Fab) (margins 0.05mm 0.05mm 0.05mm 0.05mm)
      (font (face default) (size 0.3mm 0.3mm) (thickness 0.05mm))
      (justify center center false) (stroke 0.05mm solid)
      (border true) (knockout false) (locked false))
    (pad input
      (number 1) (type smd) (shape roundrect) (at -0.8mm 0mm)
      (rotation 0deg) (size 0.8mm 0.8mm)
      (layers F.Cu F.Mask F.Paste) (roundrect_radius 0.2mm)
      (teardrop
        (enabled true)
        (target_length 0.5 1mm)
        (target_width 1 2mm)
        (edges curved)
        (track_width_limit 0.9)
        (allow_two_segments true)
        (prefer_zone_connections true))
      (solder_mask_margin 0.02mm) (solder_paste_margin inherit)
      (solder_paste_margin_ratio -0.1) (clearance inherit)
      (zone_connection thermal) (thermal_spoke_width 0.2mm)
      (thermal_spoke_angle 45deg) (thermal_gap 0.15mm)
      (pin_function INPUT) (pin_type signal))
    (pad output
      (number 2) (type smd) (shape rect) (at 0.8mm 0mm)
      (rotation 90deg) (size 0.8mm 1mm) (layers F.Cu F.Mask F.Paste)
      (shape_offset 0.05mm 0mm) (property testpoint) (die_length 0.25mm))
    (pad triangle
      (number 3) (type smd) (shape trapezoid) (at 0mm 1.5mm)
      (size 1mm 0.8mm) (layers F.Cu F.Mask)
      (trapezoid_delta 0.8mm 0mm))
    (pad stencil_window
      (number "") (type smd) (shape roundrect) (at 0mm -1.5mm)
      (size 0.8mm 0.8mm) (layers F.Paste) (roundrect_radius 0.2mm))
    (model "${KIPRJMOD}/models/SENSOR_2P.step"
      (visible true) (opacity 0.75)
      (offset 0mm 0mm 0.1mm) (scale 1 1 1) (rotation 0deg 0deg 90deg))
    (model "${KICAD10_3DMODEL_DIR}/Package_SON.3dshapes/WSON-8_4x4mm_P0.8mm.step"))
  (footprint Product:HEADER_1X01
    (reference J) (value HEADER_1X01)
    (attributes (through_hole true))
    (duplicate_pad_numbers_are_jumpers false)
    (pad pin1
      (number 1) (type thru_hole) (shape circle) (at 0mm 0mm)
      (rotation 0deg) (size 2mm 2mm) (layers all_copper all_mask)
      (drill round 1mm) (remove_unused_layers true) (keep_end_layers true))
    (pad mounting_hole
      (number "") (type np_thru_hole) (shape circle) (at 3mm 0mm)
      (rotation 0deg) (size 2mm 2mm) (layers all_copper all_mask)
      (drill round 2mm)))
))KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump( 2 ) );
    BOOST_REQUIRE_EQUAL( compiled.ir["authoredFootprints"].size(), 2 );
    BOOST_CHECK_EQUAL( compiled.plan["counts"]["authoredFootprints"], 2 );

    const auto& sensor = compiled.ir["authoredFootprints"][0];
    BOOST_CHECK_EQUAL( sensor["library"], "Product" );
    BOOST_CHECK_EQUAL( sensor["pads"][0]["roundrectRadiusNm"], 200000 );
    BOOST_CHECK_EQUAL( sensor["pads"][0]["teardrop"]["enabled"], true );
    BOOST_CHECK_EQUAL( sensor["pads"][0]["solderPasteMarginPpm"], -100000 );
    BOOST_CHECK_EQUAL( sensor["models"][0]["opacityPpm"], 750000 );
    BOOST_CHECK_EQUAL( sensor["models"][0]["rotationTenths"]["z"], 900 );
    BOOST_CHECK_EQUAL( sensor["graphics"].size(), 6 );
    BOOST_CHECK_EQUAL( sensor["texts"].size(), 1 );
    BOOST_CHECK_EQUAL( sensor["textBoxes"].size(), 1 );

    KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::RESULT first =
            KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::Generate( compiled.ir );
    KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::RESULT second =
            KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::Generate( compiled.ir );
    BOOST_REQUIRE_MESSAGE( first.ok, first.diagnostics.dump( 2 ) );
    BOOST_CHECK_EQUAL( first.sources, second.sources );
    BOOST_REQUIRE( first.sources.contains( "Product:SENSOR_2P" ) );
    BOOST_REQUIRE( first.sources.contains( "Product:HEADER_1X01" ) );
    const std::string smd = first.sources["Product:SENSOR_2P"].get<std::string>();
    const std::string tht = first.sources["Product:HEADER_1X01"].get<std::string>();
    BOOST_CHECK_NE( smd.find( "(version 20260206)" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(generator \"kichad_kds\")" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(net_tie_pad_groups \"1, 2\")" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(pad \"1\" smd roundrect" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(layers \"F.Cu\" \"F.Mask\" \"F.Paste\")" ),
                    std::string::npos );
    BOOST_CHECK_NE( smd.find( "(roundrect_rratio 0.25)" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(teardrops (best_length_ratio 0.5) (max_length 1) "
                              "(best_width_ratio 1) (max_width 2) (curved_edges yes) "
                              "(filter_ratio 0.9) (enabled yes) (allow_two_segments yes) "
                              "(prefer_zone_connections yes))" ),
                    std::string::npos );
    BOOST_CHECK_NE( smd.find( "(rect_delta 0.8 0)" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(pad \"\" smd roundrect" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(layers \"F.Paste\")" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(solder_paste_margin_ratio -0.1)" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(thermal_bridge_angle 45)" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(fp_rect" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(radius 0.1)" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(fp_curve" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(fp_text user \"${REFERENCE}\"" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(fp_text_box \"PIN 1\"" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(model \"${KIPRJMOD}/models/SENSOR_2P.step\"" ),
                    std::string::npos );
    BOOST_CHECK_NE( smd.find( "(opacity 0.75)" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "(rotate (xyz 0 0 90))" ), std::string::npos );
    BOOST_CHECK_NE( smd.find( "${KICAD10_3DMODEL_DIR}/Package_SON.3dshapes/"
                              "WSON-8_4x4mm_P0.8mm.step" ),
                    std::string::npos );
    BOOST_CHECK_NE( tht.find( "(pad \"1\" thru_hole circle" ), std::string::npos );
    BOOST_CHECK_NE( tht.find( "(drill 1)" ), std::string::npos );
    BOOST_CHECK_NE( tht.find( "(layers \"*.Cu\" \"*.Mask\")" ), std::string::npos );
    BOOST_CHECK_NE( tht.find( "(remove_unused_layers yes)" ), std::string::npos );
    BOOST_CHECK_NE( tht.find( "(keep_end_layers yes)" ), std::string::npos );
    BOOST_CHECK_NE( tht.find( "(pad \"\" np_thru_hole circle" ), std::string::npos );
    BOOST_CHECK_EQUAL( first.counts["libraries"], 1 );
    BOOST_CHECK_EQUAL( first.counts["footprints"], 2 );
    BOOST_CHECK_EQUAL( first.counts["pads"], 6 );
    BOOST_CHECK_EQUAL( first.counts["graphics"], 6 );
    BOOST_CHECK_EQUAL( first.counts["texts"], 1 );
    BOOST_CHECK_EQUAL( first.counts["textBoxes"], 1 );
    BOOST_CHECK_EQUAL( first.counts["models"], 2 );

    KICHAD::CODEX_TOOLS::PRIVATE_TEMPORARY_DIRECTORY temporary;
    BUILD_CLI_ENVIRONMENT buildEnvironment;
    std::string nativeError;
    BOOST_REQUIRE_MESSAGE( temporary.Create( "kichad-aperture-native", nativeError ),
                           nativeError );
    wxFileName nativeLibrary = wxFileName::DirName(
            wxString::FromUTF8( temporary.Path().string() ) );
    nativeLibrary.AppendDir( wxS( "Product.pretty" ) );
    KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::FILES nativeFiles = {
        { "SENSOR_2P.kicad_mod", smd }
    };
    BOOST_REQUIRE_MESSAGE(
            KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::InstallAtomically(
                    nativeLibrary, true, nativeFiles, nativeError ),
            nativeError );
    BOOST_REQUIRE_MESSAGE(
            KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::ValidateNative(
                    nativeLibrary, nativeError ),
            nativeError );
}


BOOST_AUTO_TEST_CASE( RejectsUnsafeOrPhysicallyInvalidFootprintSemantics )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project invalid_footprints)
  (library footprint Product (table project)
    (uri "${KIPRJMOD}/Product.pretty") (managed true))
  (footprint Product:BAD_RADIUS
    (pad p1 (number 1) (type smd) (shape roundrect) (at 0mm 0mm)
      (size 1mm 1mm) (layers F.Cu F.Mask) (roundrect_radius 0.6mm)))
  (footprint Product:DRILLED_SMD
    (pad p1 (number 1) (type smd) (shape circle) (at 0mm 0mm)
      (size 1mm 1mm) (layers F.Cu F.Mask) (drill round 0.5mm)))
  (footprint Product:PTH_WITHOUT_COPPER
    (pad p1 (number 1) (type thru_hole) (shape circle) (at 0mm 0mm)
      (size 1mm 1mm) (layers F.Cu F.Mask) (drill round 0.5mm)))
  (footprint Product:NUMBERED_NPTH
    (pad h1 (number MH1) (type np_thru_hole) (shape circle) (at 0mm 0mm)
      (size 1mm 1mm) (layers all_copper all_mask) (drill round 0.5mm)))
  (footprint Product:BAD_CIRCLE
    (pad p1 (number 1) (type smd) (shape circle) (at 0mm 0mm)
      (size 1mm 2mm) (layers F.Cu F.Mask)))
  (footprint Product:OVERSIZE_DRILL
    (pad p1 (number 1) (type thru_hole) (shape circle) (at 0mm 0mm)
      (size 1mm 1mm) (layers all_copper all_mask) (drill round 2mm)))
  (footprint Product:NUMBERED_APERTURE
    (pad stencil (number 1) (type smd) (shape rect) (at 0mm 0mm)
      (size 1mm 1mm) (layers F.Paste)))
  (footprint Product:TWO_SIDED_APERTURE
    (pad stencil (number "") (type smd) (shape rect) (at 0mm 0mm)
      (size 1mm 1mm) (layers F.Paste B.Paste)))
  (footprint Product:ELECTRICAL_APERTURE
    (pad stencil (number "") (type smd) (shape rect) (at 0mm 0mm)
      (size 1mm 1mm) (layers F.Paste)
      (pin_function STENCIL) (zone_connection solid)))
  (footprint Product:MASK_MARGIN_WITHOUT_MASK
    (pad stencil (number "") (type smd) (shape rect) (at 0mm 0mm)
      (size 1mm 1mm) (layers F.Paste) (solder_mask_margin 0.05mm)))
  (footprint Product:PASTE_MARGIN_WITHOUT_PASTE
    (pad opening (number "") (type smd) (shape rect) (at 0mm 0mm)
      (size 1mm 1mm) (layers F.Mask) (solder_paste_margin 0.05mm)))
  (footprint Product:WINDOWS_TRAVERSAL
    (pad p1 (number 1) (type smd) (shape rect) (at 0mm 0mm)
      (size 1mm 1mm) (layers F.Cu F.Mask))
    (model "${KIPRJMOD}/models\\outside.step"))
  (footprint Product:BAD_REFERENCES
    (jumper_group 1 404) (net_tie_group 1 405)
    (pad p1 (number 1) (type smd) (shape rect) (at 0mm 0mm)
      (size 1mm 1mm) (layers F.Cu F.Mask))
    (model "${KIPRJMOD}/../outside.step"))
  (footprint Product:BAD_GRAPHICS
    (line zero (start 0mm 0mm) (end 0mm 0mm)
      (stroke 0.1mm solid) (layers F.SilkS) (fill solid))
    (arc flat (start 0mm 0mm) (mid 1mm 0mm) (end 2mm 0mm)
      (stroke 0.1mm solid) (layers F.SilkS))
    (polygon empty_area (point 0mm 0mm) (point 1mm 0mm) (point 2mm 0mm)
      (stroke 0.1mm solid) (layers F.Fab) (fill none))
    (text missing_position "BAD" (layer F.SilkS)
      (font (size 1mm 1mm)) (justify center center false)))
))KDS";

    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_CHECK( !compiled.ok );
    const std::string diagnostics = compiled.diagnostics.dump();
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_footprint_pad_roundrect_radius" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_footprint_pad_drill_semantics" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_footprint_through_hole_layers" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "numbered_authored_footprint_npth_pad" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_footprint_pad_circle_size" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_footprint_pad_drill_size" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "numbered_authored_footprint_aperture_pad" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_footprint_aperture_pad_layers" ),
                    std::string::npos );
    BOOST_CHECK_NE(
            diagnostics.find( "invalid_authored_footprint_aperture_pad_electrical_metadata" ),
            std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_footprint_aperture_pad_mask_margin" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_footprint_aperture_pad_paste_margin" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "unknown_authored_footprint_jumper_pad" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "unknown_authored_footprint_net_tie_pad" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_footprint_model_path" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "degenerate_authored_footprint_graphic" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_footprint_graphic_fill" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "missing_authored_footprint_text_position" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( RequiresExplicitWholeLibraryOwnership )
{
    const std::string unmanaged = R"KDS((kichad_design
  (version 1) (project unmanaged_footprint)
  (library footprint Local (table project) (uri "${KIPRJMOD}/Local.pretty"))
  (footprint Local:X
    (pad p1 (number 1) (type smd) (shape rect) (at 0mm 0mm)
      (size 1mm 1mm) (layers F.Cu F.Mask)))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( unmanaged );
    BOOST_CHECK( !compiled.ok );
    BOOST_CHECK_NE( compiled.diagnostics.dump().find( "unmanaged_authored_footprint_library" ),
                    std::string::npos );

    const std::string empty = R"KDS((kichad_design
  (version 1) (project empty_footprint)
  (library footprint Local (table project)
    (uri "${KIPRJMOD}/Local.pretty") (managed true))
))KDS";
    compiled = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( empty );
    BOOST_CHECK( !compiled.ok );
    BOOST_CHECK_NE( compiled.diagnostics.dump().find( "empty_managed_footprint_library" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( LowersFootprintZonesGroupsAndAssemblyVariants )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project footprint_fabrication)
  (library footprint Product (table project)
    (uri "${KIPRJMOD}/Product.pretty") (managed true))
  (footprint Product:SHIELDED_MODULE
    (reference U) (value SHIELDED_MODULE)
    (attributes (smd true))
    (property manufacturer_part_number
      (name "Manufacturer Part Number") (value "MODULE-BASE")
      (at 0mm 1.5mm) (rotation 0deg) (layer F.Fab)
      (visible false) (keep_upright true) (knockout false)
      (font (size 0.8mm 0.8mm) (thickness auto))
      (justify center center false))
    (component_classes "Precision Analog" "Thermal Critical")
    (rules
      (clearance 0.1mm) (solder_mask_margin -0.02mm)
      (solder_paste_margin inherit) (solder_paste_margin_ratio -0.1)
      (zone_connection thermal))
    (stackup custom (layers F.Cu In1.Cu In2.Cu B.Cu))
    (private_layers User.1 F.Fab)
    (line shield_edge (start -3mm -2mm) (end 3mm -2mm)
      (stroke 0.15mm solid) (layers F.SilkS))
    (text assembly_note "SHIELD" (at 0mm 0mm) (rotation 0deg)
      (layer F.Fab) (font (size 0.8mm 0.8mm))
      (justify center center false))
    (pad ground (number 1) (type smd) (shape rect) (at 0mm 0mm)
      (size 2mm 2mm) (layers F.Cu F.Mask F.Paste))
    (zone paste_window
      (purpose copper) (name "Local copper window") (layers F.Cu)
      (outline (polygon
        (point -2mm -1.5mm) (point 2mm -1.5mm)
        (point 2mm 1.5mm) (point -2mm 1.5mm)
        (hole (point -0.4mm -0.4mm) (point -0.4mm 0.4mm)
          (point 0.4mm 0.4mm) (point 0.4mm -0.4mm))))
      (clearance 0.15mm) (min_thickness 0.2mm)
      (connection thermal (thermal_gap 0.2mm) (thermal_spoke_width 0.25mm))
      (islands remove_below (minimum_area 0.2mm2))
      (fill hatched (thickness 0.25mm) (gap 0.2mm) (orientation 45deg)
        (edge_smoothing fillet (amount 0.25))
        (hole_min_area_ratio 0.1) (border hatch))
      (priority 2) (border diagonal_edge (pitch 0.5mm))
      (corner_smoothing fillet (radius 0.15mm)) (locked false))
    (zone antenna_keepout
      (purpose keepout) (layers all_copper F.Mask)
      (outline (polygon
        (point -4mm -3mm) (point 4mm -3mm)
        (point 4mm 3mm) (point -4mm 3mm)))
      (prohibit (copper true) (vias true) (tracks true)
        (pads false) (footprints false))
      (border solid) (locked true))
    (group electrical_core (name "Electrical core") (locked false)
      (member pad ground) (member graphic shield_edge) (member text assembly_note)
      (member property manufacturer_part_number))
    (group complete_module (name "Complete module") (locked true)
      (member group electrical_core) (member zone antenna_keepout))
    (variant production
      (dnp false) (exclude_from_bom false) (exclude_from_position false)
      (field "Value" "SHIELDED_MODULE-PROD")
      (field "Manufacturer Part Number" "MODULE-001"))
    (variant no_shield
      (dnp true) (exclude_from_bom true) (exclude_from_position true)
      (field "Value" "SHIELDED_MODULE-DNP")))
))KDS";

    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump( 2 ) );
    const auto& footprint = compiled.ir["authoredFootprints"][0];
    BOOST_CHECK_EQUAL( footprint["zones"].size(), 2 );
    BOOST_CHECK_EQUAL( footprint["groups"].size(), 2 );
    BOOST_CHECK_EQUAL( footprint["variants"].size(), 2 );
    BOOST_CHECK_EQUAL( footprint["properties"].size(), 1 );
    BOOST_CHECK_EQUAL( footprint["componentClasses"].size(), 2 );
    BOOST_CHECK_EQUAL( footprint["stackup"].size(), 4 );
    BOOST_CHECK_EQUAL( footprint["rules"]["clearanceNm"], 100000 );
    BOOST_CHECK_EQUAL( footprint["zones"][0]["fill"]["smoothing"], "fillet" );
    BOOST_CHECK_EQUAL( footprint["zones"][0]["islands"]["minimumAreaNm2"],
                       200000000000LL );

    const KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::RESULT generated =
            KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::Generate( compiled.ir );
    BOOST_REQUIRE_MESSAGE( generated.ok, generated.diagnostics.dump( 2 ) );
    const std::string source = generated.sources["Product:SHIELDED_MODULE"].get<std::string>();
    BOOST_CHECK_NE( source.find( "(name \"Local copper window\")" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(mode hatch)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(hatch_smoothing_level 2)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(island_area_min 0.2)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(keepout" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(placement (enabled no))" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(group \"Electrical core\"" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(variant" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(field (name \"Manufacturer Part Number\")" ),
                    std::string::npos );
    BOOST_CHECK_NE( source.find( "(property \"Manufacturer Part Number\" \"MODULE-BASE\"" ),
                    std::string::npos );
    BOOST_CHECK_NE( source.find( "(component_classes (class \"Precision Analog\") "
                                 "(class \"Thermal Critical\"))" ),
                    std::string::npos );
    BOOST_CHECK_NE( source.find( "(clearance 0.1)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(solder_mask_margin -0.02)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(solder_paste_margin_ratio -0.1)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(zone_connect 1)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(stackup (layer \"F.Cu\") (layer \"B.Cu\") "
                                 "(layer \"In1.Cu\") (layer \"In2.Cu\"))" ),
                    std::string::npos );
    BOOST_CHECK_NE( source.find( "(private_layers \"F.Fab\" \"User.1\")" ),
                    std::string::npos );
    BOOST_CHECK_EQUAL( generated.counts["properties"], 1 );
    BOOST_CHECK_EQUAL( generated.counts["zones"], 2 );
    BOOST_CHECK_EQUAL( generated.counts["groups"], 2 );
    BOOST_CHECK_EQUAL( generated.counts["variants"], 2 );
}


BOOST_AUTO_TEST_SUITE_END()
