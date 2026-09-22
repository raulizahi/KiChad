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

#include <kicad/codex/design_script_compiler.h>
#include <kicad/codex/design_script_escape_analyzer.h>

#include <string>

using KICHAD::DESIGN_SCRIPT_ESCAPE_ANALYZER;


namespace
{

/** A grid array footprint: aColumns x aRows pads on aPitch mm with square aPad mm pads. */
std::string gridArray( const std::string& aName, int aColumns, int aRows, double aPitch,
                       double aPad )
{
    std::string source = "(footprint \"" + aName + "\"\n";
    int number = 1;

    for( int row = 0; row < aRows; ++row )
    {
        for( int column = 0; column < aColumns; ++column )
        {
            const double x = ( column - ( aColumns - 1 ) / 2.0 ) * aPitch;
            const double y = ( row - ( aRows - 1 ) / 2.0 ) * aPitch;
            source += "  (pad \"" + std::to_string( number++ ) + "\" smd circle (at "
                      + std::to_string( x ) + " " + std::to_string( y ) + ") (size "
                      + std::to_string( aPad ) + " " + std::to_string( aPad )
                      + ") (layers \"F.Cu\"))\n";
        }
    }

    return source + ")\n";
}


std::string design( const std::string& aRules )
{
    return "(kichad_design\n"
           "  (version 1)\n"
           "  (project escape)\n"
           "  (component U1 (symbol \"Device:R\") (value \"sensor\") "
           "(footprint \"Pkg:BGA\"))\n"
           "  (component R1 (symbol \"Device:R\") (value \"1k\") (footprint \"Pkg:R0603\"))\n"
           + aRules + ")\n";
}


const char* const FULL_RULES_TEMPLATE =
        "  (rules\n"
        "    (minimum_clearance %CLEARANCE%) (minimum_connection_width 0.2mm)\n"
        "    (minimum_track_width %TRACK%) (minimum_via_annular_width 0.1mm)\n"
        "    (minimum_via_diameter %VIA%) (minimum_through_hole_diameter 0.3mm)\n"
        "    (minimum_microvia_diameter 0.3mm) (minimum_microvia_drill 0.1mm)\n"
        "    (minimum_hole_to_hole 0.25mm) (minimum_copper_to_hole_clearance 0.25mm)\n"
        "    (minimum_silkscreen_clearance 0mm) (minimum_groove_width 0.25mm)\n"
        "    (minimum_resolved_spokes 2) (minimum_silkscreen_text_height 0.8mm)\n"
        "    (minimum_silkscreen_text_thickness 0.08mm)\n"
        "    (minimum_copper_to_edge_clearance 0.5mm)\n"
        "    (use_height_for_length_calculations true)\n"
        "    (maximum_error 0.005mm)\n"
        "    (allow_fillets_outside_zone_outline false))\n";


std::string rules( const std::string& aTrack, const std::string& aClearance,
                   const std::string& aVia )
{
    std::string out( FULL_RULES_TEMPLATE );
    const auto replace = [&]( const std::string& aToken, const std::string& aValue )
    {
        const size_t at = out.find( aToken );
        BOOST_REQUIRE_NE( at, std::string::npos );
        out.replace( at, aToken.size(), aValue );
    };
    replace( "%CLEARANCE%", aClearance );
    replace( "%TRACK%", aTrack );
    replace( "%VIA%", aVia );
    return out;
}


nlohmann::json sources()
{
    // The AR0235 sensor package that defeated the router: 9x7 balls, 0.7 mm pitch, 0.25 mm pads.
    return { { "Pkg:BGA", gridArray( "BGA", 9, 7, 0.7, 0.25 ) },
             { "Pkg:R0603",
               "(footprint \"R0603\"\n"
               "  (pad \"1\" smd roundrect (at -0.7875 0) (size 0.875 0.95) (layers \"F.Cu\"))\n"
               "  (pad \"2\" smd roundrect (at 0.7875 0) (size 0.875 0.95) (layers \"F.Cu\"))\n)\n" } };
}

} // namespace


BOOST_AUTO_TEST_SUITE( DesignScriptEscapeAnalyzer )


BOOST_AUTO_TEST_CASE( ReportsTheFabFeaturesAFinePitchPackageRequires )
{
    // KiCad's own defaults (0.2 mm track, 0.15 mm clearance) need 0.5 mm to escape a channel
    // that is only 0.45 mm wide, which is exactly why the board could never be routed.
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( design( rules( "0.2mm", "0.15mm",
                                                                     "0.6mm" ) ) );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );

    DESIGN_SCRIPT_ESCAPE_ANALYZER::RESULT result =
            DESIGN_SCRIPT_ESCAPE_ANALYZER::Analyze( compiled.ir, sources() );
    BOOST_CHECK( !result.feasible );
    BOOST_REQUIRE_EQUAL( result.requirements.size(), 1 );

    const nlohmann::json& requirement = result.requirements[0];
    BOOST_CHECK_EQUAL( requirement["footprint"].get<std::string>(), "Pkg:BGA" );
    BOOST_CHECK_EQUAL( requirement["pitchNm"].get<int64_t>(), 700000 );
    BOOST_CHECK_EQUAL( requirement["padNm"].get<int64_t>(), 250000 );
    // 0.70 mm pitch minus 0.25 mm pad leaves a 0.45 mm channel.
    BOOST_CHECK_EQUAL( requirement["escapeChannelNm"].get<int64_t>(), 450000 );
    // Track plus two clearances at KiCad's defaults is 0.50 mm: it does not fit.
    BOOST_CHECK_EQUAL( requirement["declaredEscapeWidthNm"].get<int64_t>(), 500000 );
    BOOST_CHECK( !requirement["escapes"].get<bool>() );
    // Splitting the channel three ways gives the floor a fab must hold: 0.15 mm.
    BOOST_CHECK_EQUAL( requirement["requiredTrackNm"].get<int64_t>(), 150000 );
    BOOST_CHECK_EQUAL( requirement["requiredClearanceNm"].get<int64_t>(), 150000 );

    BOOST_REQUIRE_GE( result.issues.size(), 1 );
    const nlohmann::json& issue = result.issues[0];
    BOOST_CHECK_EQUAL( issue["type"].get<std::string>(), "escape_infeasible" );
    BOOST_CHECK_EQUAL( issue["component"].get<std::string>(), "U1" );
    BOOST_CHECK_EQUAL( issue["severity"].get<std::string>(), "error" );
    BOOST_CHECK_NE( issue["description"].get<std::string>().find( "450 um channel" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( AcceptsRulesAFabricatorCanActuallyHold )
{
    // OSH Park's floors (0.127 mm) escape the same package.
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( design( rules( "0.127mm", "0.127mm",
                                                                     "0.5mm" ) ) );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );

    DESIGN_SCRIPT_ESCAPE_ANALYZER::RESULT result =
            DESIGN_SCRIPT_ESCAPE_ANALYZER::Analyze( compiled.ir, sources() );
    BOOST_CHECK( result.feasible );
    BOOST_CHECK( result.issues.empty() );
    BOOST_REQUIRE_EQUAL( result.requirements.size(), 1 );
    BOOST_CHECK( result.requirements[0]["escapes"].get<bool>() );
    BOOST_CHECK_EQUAL( result.requirements[0]["declaredEscapeWidthNm"].get<int64_t>(), 381000 );
    BOOST_CHECK_EQUAL( result.summary["finePitchPackages"].get<int>(), 1 );
    BOOST_CHECK( result.summary["rulesDeclared"].get<bool>() );
}


BOOST_AUTO_TEST_CASE( RejectsAViaThatCannotFitTheDiagonalPocket )
{
    // Fine enough tracks, but a via too large for the pocket between four balls.
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( design( rules( "0.1mm", "0.1mm",
                                                                     "0.9mm" ) ) );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );

    DESIGN_SCRIPT_ESCAPE_ANALYZER::RESULT result =
            DESIGN_SCRIPT_ESCAPE_ANALYZER::Analyze( compiled.ir, sources() );
    BOOST_CHECK( !result.feasible );
    BOOST_REQUIRE_EQUAL( result.issues.size(), 1 );
    BOOST_CHECK_EQUAL( result.issues[0]["type"].get<std::string>(),
                       "via_does_not_fit_pocket" );
    // 0.70 mm x sqrt(2) minus the 0.25 mm pad is a 0.7399 mm pocket.
    BOOST_CHECK_EQUAL( result.issues[0]["diagonalPocketNm"].get<int64_t>(), 739949 );
}


BOOST_AUTO_TEST_CASE( ReportsRequiredFeaturesBeforeAnyRulesAreDeclared )
{
    // No rules yet: the design still learns the floors it will need, which is the point of
    // estimating before a fab profile is chosen.
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( design( "" ) );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );

    DESIGN_SCRIPT_ESCAPE_ANALYZER::RESULT result =
            DESIGN_SCRIPT_ESCAPE_ANALYZER::Analyze( compiled.ir, sources() );
    BOOST_CHECK( result.feasible );
    BOOST_REQUIRE_EQUAL( result.issues.size(), 1 );
    BOOST_CHECK_EQUAL( result.issues[0]["type"].get<std::string>(), "fab_features_required" );
    BOOST_CHECK_EQUAL( result.issues[0]["severity"].get<std::string>(), "warning" );
    BOOST_CHECK_EQUAL( result.issues[0]["requiredTrackNm"].get<int64_t>(), 150000 );
    BOOST_CHECK( !result.summary["rulesDeclared"].get<bool>() );
}


BOOST_AUTO_TEST_CASE( RecommendsACopperLayerCountFromTheDesign )
{
    // The sensor package is 9x7 balls: four rings deep, so escaping it needs three signal
    // layers, plus a reference plane and a power plane, rounded up to an even count.
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( design( rules( "0.127mm", "0.127mm",
                                                                     "0.5mm" ) ) );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );

    DESIGN_SCRIPT_ESCAPE_ANALYZER::RESULT result =
            DESIGN_SCRIPT_ESCAPE_ANALYZER::Analyze( compiled.ir, sources() );
    BOOST_CHECK_EQUAL( result.recommendedCopperLayers, 6 );
    BOOST_REQUIRE_GE( result.layerRationale.size(), 2 );
    BOOST_CHECK_NE( result.layerRationale[0].get<std::string>().find( "rings deep" ),
                    std::string::npos );
    BOOST_CHECK_EQUAL( result.summary["recommendedCopperLayers"].get<int>(), 6 );

    // A board with neither fine-pitch parts nor pairs stays at two layers.
    nlohmann::json coarse = { { "Pkg:BGA", gridArray( "QFN", 9, 7, 1.27, 0.6 ) },
                              { "Pkg:R0603", sources()["Pkg:R0603"] } };
    DESIGN_SCRIPT_ESCAPE_ANALYZER::RESULT plain =
            DESIGN_SCRIPT_ESCAPE_ANALYZER::Analyze( compiled.ir, coarse );
    BOOST_CHECK_EQUAL( plain.recommendedCopperLayers, 2 );
}


BOOST_AUTO_TEST_CASE( RecommendsFourLayersForDifferentialPairsAlone )
{
    // No fine-pitch array, but a differential pair needs a reference plane.
    const std::string source =
            "(kichad_design\n"
            "  (version 1)\n"
            "  (project pairs)\n"
            "  (component U1 (symbol \"Device:R\") (value \"driver\") "
            "(footprint \"Pkg:R0603\"))\n"
            "  (component U2 (symbol \"Device:R\") (value \"receiver\") "
            "(footprint \"Pkg:R0603\"))\n"
            "  (net LVDS_P (presentation labels) (pin U1 1 \"1\") (pin U2 1 \"1\"))\n"
            "  (net LVDS_N (presentation labels) (pin U1 1 \"2\") (pin U2 1 \"2\"))\n)\n";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );

    nlohmann::json onlyPassives = { { "Pkg:R0603", sources()["Pkg:R0603"] } };
    DESIGN_SCRIPT_ESCAPE_ANALYZER::RESULT result =
            DESIGN_SCRIPT_ESCAPE_ANALYZER::Analyze( compiled.ir, onlyPassives );
    BOOST_CHECK_EQUAL( result.summary["differentialPairs"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( result.recommendedCopperLayers, 4 );
}


BOOST_AUTO_TEST_CASE( IgnoresCoarsePackagesAndPerimeterOnlyParts )
{
    // A 0603 resistor and a perimeter-only quad package set no escape floor.
    nlohmann::json coarse = { { "Pkg:BGA", gridArray( "QFN", 9, 7, 1.27, 0.6 ) },
                              { "Pkg:R0603", sources()["Pkg:R0603"] } };
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( design( rules( "0.2mm", "0.15mm",
                                                                     "0.6mm" ) ) );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );

    DESIGN_SCRIPT_ESCAPE_ANALYZER::RESULT result =
            DESIGN_SCRIPT_ESCAPE_ANALYZER::Analyze( compiled.ir, coarse );
    BOOST_CHECK( result.feasible );
    BOOST_CHECK( result.issues.empty() );
    BOOST_CHECK_EQUAL( result.requirements.size(), 0 );
    BOOST_CHECK_EQUAL( result.summary["analyzedPackages"].get<int>(), 2 );
}


BOOST_AUTO_TEST_SUITE_END()
