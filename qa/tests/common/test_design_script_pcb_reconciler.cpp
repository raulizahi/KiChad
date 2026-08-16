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
#include <kicad/codex/design_script_pcb_reconciler.h>

#include <algorithm>


namespace
{

using JSON = nlohmann::json;
using RECONCILER = KICHAD::DESIGN_SCRIPT_PCB_RECONCILER;

const std::string SOURCE = R"KDS((kichad_design
  (version 1)
  (project reconcile_board)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net SIGNAL (pin R1 1 1) (pin R2 1 1))
  (board
    (outline
      (rectangle edge (start 0mm 0mm) (end 20mm 10mm)
        (radius 0mm) (stroke 0.05mm solid) (layers Edge.Cuts) (fill none)))
    (route SIGNAL (id trace-a) (from 1mm 2mm) (to 3mm 4mm)
      (width 0.25mm) (layer F.Cu))
    (via SIGNAL (id via-a) (at 3mm 4mm) (diameter 0.8mm) (drill 0.4mm))
    (zone SIGNAL
      (id signal-pour)
      (layers B.Cu)
      (outline
        (polygon
          (point 0mm 0mm) (point 20mm 0mm) (point 20mm 10mm) (point 0mm 10mm)))
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection solid)
      (islands keep_all)
      (fill solid))
    (keepout
      (id no-routing)
      (layers F.Cu)
      (outline
        (polygon
          (point 5mm 2mm) (point 8mm 2mm) (point 8mm 5mm) (point 5mm 5mm)))
      (prohibit
        (copper true) (vias true) (tracks true) (pads false) (footprints false)))
    (text "Managed note"
      (id note)
      (layer F.SilkS)
      (at 2mm 8mm)
      (size 1mm 1mm)
      (stroke 0.15mm))
    (dimension aligned
      (id board-width)
      (layer Dwgs.User)
      (from 0mm 0mm)
      (to 20mm 0mm)
      (height -2mm)
      (extension_height 0.2mm)
      (units mm)
      (unit_format bare_suffix)
      (precision fixed_2)
      (line_width 0.15mm)
      (arrow_length 0.8mm)
      (arrow_direction inward)
      (text_position outside)
      (text_size 1mm 1mm)
      (text_stroke 0.15mm)))
)
)KDS";


struct FIXTURE
{
    FIXTURE()
    {
        KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
                KICHAD::DESIGN_SCRIPT_COMPILER::Compile( SOURCE );
        BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
        sourceSha256 = compiled.sourceSha256;
        KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
                KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
        BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
        operations = std::move( plan.operations );
    }

    RECONCILER::CONTEXT Context() const
    {
        return { "reconcile_board.kicad_kds", "reconcile_board.kicad_pcb",
                 "reconcile_board", sourceSha256 };
    }

    JSON Inventory() const
    {
        JSON inventory = JSON::array();

        for( const JSON& operation : operations )
        {
            inventory.push_back( { { "itemId", operation["itemId"] },
                                   { "itemType", operation["itemType"] } } );
        }

        return inventory;
    }

    JSON        operations = JSON::array();
    std::string sourceSha256;
};

} // namespace


BOOST_AUTO_TEST_SUITE( DesignScriptPcbReconciler )


BOOST_FIXTURE_TEST_CASE( CreatesThenIdempotentlyUpdatesManagedItems, FIXTURE )
{
    RECONCILER::RESULT first =
            RECONCILER::Reconcile( operations, nullptr, JSON::array(), Context() );
    BOOST_REQUIRE_MESSAGE( first.ok, first.diagnostics.dump() );
    BOOST_CHECK_EQUAL( first.counts["create"].get<int>(), 7 );
    BOOST_CHECK_EQUAL( first.counts["update"].get<int>(), 0 );
    BOOST_REQUIRE_EQUAL( first.nextState["managedPcbItems"].size(), 7 );

    RECONCILER::RESULT repeated =
            RECONCILER::Reconcile( operations, first.nextState, Inventory(), Context() );
    BOOST_REQUIRE_MESSAGE( repeated.ok, repeated.diagnostics.dump() );
    BOOST_CHECK_EQUAL( repeated.counts["create"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( repeated.counts["update"].get<int>(), 7 );
    BOOST_CHECK_EQUAL( repeated.counts["delete"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( repeated.nextState.dump(), first.nextState.dump() );
    auto zoneUpdate = std::find_if(
            repeated.actions.begin(), repeated.actions.end(),
            []( const JSON& action ) { return action.value( "itemType", "" ) == "zone"; } );
    BOOST_REQUIRE( zoneUpdate != repeated.actions.end() );
    BOOST_CHECK_EQUAL( ( *zoneUpdate )["fieldMask"].dump(),
                       JSON::array( { "type", "layers", "outline", "name",
                                      "copper_settings", "priority", "filled",
                                      "filled_polygons", "border", "locked",
                                      "layer_properties" } ).dump() );
    auto keepoutUpdate = std::find_if(
            repeated.actions.begin(), repeated.actions.end(),
            []( const JSON& action ) { return action.value( "itemType", "" ) == "rule_area"; } );
    BOOST_REQUIRE( keepoutUpdate != repeated.actions.end() );
    BOOST_CHECK_EQUAL( ( *keepoutUpdate )["fieldMask"].dump(),
                       JSON::array( { "type", "layers", "outline", "name",
                                      "rule_area_settings", "priority", "filled",
                                      "filled_polygons", "border", "locked",
                                      "layer_properties" } ).dump() );
    auto textUpdate = std::find_if(
            repeated.actions.begin(), repeated.actions.end(),
            []( const JSON& action ) { return action.value( "itemType", "" ) == "text"; } );
    BOOST_REQUIRE( textUpdate != repeated.actions.end() );
    BOOST_CHECK_EQUAL( ( *textUpdate )["fieldMask"].dump(),
                       JSON::array( { "text", "layer", "knockout", "locked" } ).dump() );
    auto dimensionUpdate = std::find_if(
            repeated.actions.begin(), repeated.actions.end(),
            []( const JSON& action ) { return action.value( "itemType", "" ) == "dimension"; } );
    BOOST_REQUIRE( dimensionUpdate != repeated.actions.end() );
    BOOST_CHECK_EQUAL(
            ( *dimensionUpdate )["fieldMask"].dump(),
            JSON::array( { "locked", "layer", "text", "aligned", "override_text_enabled",
                           "override_text", "prefix", "suffix", "unit", "unit_format",
                           "arrow_direction", "precision", "suppress_trailing_zeroes",
                           "line_thickness", "arrow_length", "extension_offset",
                           "text_position", "keep_text_aligned" } ).dump() );
}


BOOST_FIXTURE_TEST_CASE( DeletesOnlyPreviouslyManagedObsoleteItems, FIXTURE )
{
    RECONCILER::RESULT first =
            RECONCILER::Reconcile( operations, nullptr, JSON::array(), Context() );
    BOOST_REQUIRE( first.ok );

    JSON reduced = operations;
    reduced.erase( reduced.begin() + 1 );
    RECONCILER::RESULT changed =
            RECONCILER::Reconcile( reduced, first.nextState, Inventory(), Context() );
    BOOST_REQUIRE_MESSAGE( changed.ok, changed.diagnostics.dump() );
    BOOST_CHECK_EQUAL( changed.counts["update"].get<int>(), 6 );
    BOOST_CHECK_EQUAL( changed.counts["delete"].get<int>(), 1 );
    BOOST_REQUIRE_EQUAL( changed.actions.size(), 7 );
    auto deleted = std::find_if(
            changed.actions.begin(), changed.actions.end(),
            []( const JSON& action ) { return action.value( "action", "" ) == "delete"; } );
    BOOST_REQUIRE( deleted != changed.actions.end() );
    BOOST_CHECK_EQUAL( ( *deleted )["logicalId"].get<std::string>(), "trace-a" );
    BOOST_REQUIRE_EQUAL( changed.nextState["managedPcbItems"].size(), 6 );
}


BOOST_FIXTURE_TEST_CASE( RefusesUnmanagedCollisionsAndManagedTypeDrift, FIXTURE )
{
    JSON collisionInventory = JSON::array( {
            { { "itemId", operations[0]["itemId"] }, { "itemType", "shape" } }
    } );
    RECONCILER::RESULT collision =
            RECONCILER::Reconcile( operations, nullptr, collisionInventory, Context() );
    BOOST_CHECK( !collision.ok );
    BOOST_CHECK( collision.actions.empty() );
    BOOST_CHECK_NE( collision.diagnostics.dump().find( "unmanaged_uuid_collision" ),
                    std::string::npos );

    RECONCILER::RESULT first =
            RECONCILER::Reconcile( operations, nullptr, JSON::array(), Context() );
    BOOST_REQUIRE( first.ok );
    JSON drift = Inventory();
    drift[0]["itemType"] = "other";
    RECONCILER::RESULT conflict =
            RECONCILER::Reconcile( operations, first.nextState, drift, Context() );
    BOOST_CHECK( !conflict.ok );
    BOOST_CHECK( conflict.actions.empty() );
    BOOST_CHECK_NE( conflict.diagnostics.dump().find( "managed_item_identity_conflict" ),
                    std::string::npos );
}


BOOST_FIXTURE_TEST_CASE( RejectsCorruptStateScopeAndMalformedOperations, FIXTURE )
{
    RECONCILER::RESULT first =
            RECONCILER::Reconcile( operations, nullptr, JSON::array(), Context() );
    BOOST_REQUIRE( first.ok );

    JSON wrongScope = first.nextState;
    wrongScope["boardPath"] = "other.kicad_pcb";
    RECONCILER::RESULT scoped =
            RECONCILER::Reconcile( operations, wrongScope, Inventory(), Context() );
    BOOST_CHECK( !scoped.ok );
    BOOST_CHECK_NE( scoped.diagnostics.dump().find( "managed_state_scope_mismatch" ),
                    std::string::npos );

    JSON forgedOwnership = first.nextState;
    forgedOwnership["managedPcbItems"][0]["itemId"] =
            "00000000-0000-8000-8000-000000000000";
    RECONCILER::RESULT forged =
            RECONCILER::Reconcile( operations, forgedOwnership, Inventory(), Context() );
    BOOST_CHECK( !forged.ok );
    BOOST_CHECK_NE( forged.diagnostics.dump().find( "invalid_managed_state" ),
                    std::string::npos );

    JSON placement = JSON::array( {
            { { "action", "place_by_reference" }, { "component", "R1" } }
    } );
    RECONCILER::RESULT unsupported =
            RECONCILER::Reconcile( placement, nullptr, JSON::array(), Context() );
    BOOST_CHECK( !unsupported.ok );
    BOOST_CHECK_NE( unsupported.diagnostics.dump().find( "invalid_placement" ),
                    std::string::npos );
}


BOOST_FIXTURE_TEST_CASE( ResolvesSchematicFootprintPlacementWithoutTakingOwnership, FIXTURE )
{
    const std::string footprintId = "11111111-1111-8111-8111-111111111111";
    JSON desired = operations;
    desired.push_back( { { "action", "place_by_reference" },
                         { "component", "R1" },
                         { "position", { { "xNm", 12500000 }, { "yNm", 7500000 } } },
                         { "rotationDegrees", 37.5 },
                         { "side", "back" },
                         { "locked", true } } );
    JSON inventory = JSON::array( {
            { { "itemId", footprintId }, { "itemType", "footprint" },
              { "reference", "R1" }, { "schematicLinked", true } }
    } );
    RECONCILER::RESULT placed =
            RECONCILER::Reconcile( desired, nullptr, inventory, Context() );
    BOOST_REQUIRE_MESSAGE( placed.ok, placed.diagnostics.dump() );
    BOOST_CHECK_EQUAL( placed.counts["create"].get<int>(), 7 );
    BOOST_CHECK_EQUAL( placed.counts["placement"].get<int>(), 1 );
    BOOST_REQUIRE_EQUAL( placed.nextState["managedPcbItems"].size(), 7 );
    BOOST_REQUIRE_EQUAL( placed.actions.size(), 8 );
    auto placementAction = std::find_if(
            placed.actions.begin(), placed.actions.end(),
            []( const JSON& candidate )
            {
                return candidate.value( "component", "" ) == "R1";
            } );
    BOOST_REQUIRE( placementAction != placed.actions.end() );
    const JSON& action = *placementAction;
    BOOST_CHECK_EQUAL( action["action"].get<std::string>(), "update" );
    BOOST_CHECK_EQUAL( action["itemType"].get<std::string>(), "footprint" );
    BOOST_CHECK_EQUAL( action["itemId"].get<std::string>(), footprintId );
    BOOST_CHECK_EQUAL( action["item"]["id"]["value"].get<std::string>(), footprintId );
    BOOST_CHECK_EQUAL( action["item"]["position"]["xNm"].get<std::string>(), "12500000" );
    BOOST_CHECK_EQUAL( action["item"]["layer"].get<std::string>(), "BL_B_Cu" );
    BOOST_CHECK_EQUAL( action["item"]["locked"].get<std::string>(), "LS_LOCKED" );
    BOOST_CHECK_EQUAL( action["fieldMask"].dump(),
                       R"(["position","orientation","layer","locked"])" );
}


BOOST_FIXTURE_TEST_CASE( RefusesMissingAmbiguousAndUnlinkedFootprintPlacements, FIXTURE )
{
    JSON placement = JSON::array( {
            { { "action", "place_by_reference" }, { "component", "R1" },
              { "position", { { "xNm", 1 }, { "yNm", 2 } } },
              { "rotationDegrees", 0.0 }, { "side", "front" }, { "locked", false } }
    } );
    RECONCILER::RESULT missing =
            RECONCILER::Reconcile( placement, nullptr, JSON::array(), Context() );
    BOOST_CHECK( !missing.ok );
    BOOST_CHECK_NE( missing.diagnostics.dump().find( "missing_component_footprint" ),
                    std::string::npos );

    JSON unlinkedInventory = JSON::array( {
            { { "itemId", "11111111-1111-8111-8111-111111111111" },
              { "itemType", "footprint" }, { "reference", "R1" },
              { "schematicLinked", false } }
    } );
    RECONCILER::RESULT unlinked =
            RECONCILER::Reconcile( placement, nullptr, unlinkedInventory, Context() );
    BOOST_CHECK( !unlinked.ok );
    BOOST_CHECK_NE( unlinked.diagnostics.dump().find( "unlinked_component_footprint" ),
                    std::string::npos );

    JSON ambiguousInventory = unlinkedInventory;
    ambiguousInventory[0]["schematicLinked"] = true;
    ambiguousInventory.push_back(
            { { "itemId", "22222222-2222-8222-8222-222222222222" },
              { "itemType", "footprint" }, { "reference", "R1" },
              { "schematicLinked", true } } );
    RECONCILER::RESULT ambiguous =
            RECONCILER::Reconcile( placement, nullptr, ambiguousInventory, Context() );
    BOOST_CHECK( !ambiguous.ok );
    BOOST_CHECK_NE( ambiguous.diagnostics.dump().find( "ambiguous_component_footprint" ),
                    std::string::npos );

    placement.push_back( placement.front() );
    RECONCILER::RESULT duplicate =
            RECONCILER::Reconcile( placement, nullptr, ambiguousInventory, Context() );
    BOOST_CHECK( !duplicate.ok );
    BOOST_CHECK_NE( duplicate.diagnostics.dump().find( "duplicate_component_placement" ),
                    std::string::npos );
}


BOOST_FIXTURE_TEST_CASE( ReconcilesFootprintFieldPresentationByReference, FIXTURE )
{
    const std::string footprintId = "11111111-1111-8111-8111-111111111111";
    JSON placement = { { "action", "place_by_reference" }, { "component", "U1" },
                       { "position", { { "xNm", 10000000 }, { "yNm", 8000000 } } },
                       { "rotationDegrees", 0.0 }, { "side", "front" },
                       { "locked", false } };
    placement["presentation"] = {
        { "reference",
          { { "visible", true }, { "layer", "F.SilkS" },
            { "position", { { "xNm", 10000000 }, { "yNm", 6000000 } } },
            { "size", { { "xNm", 1000000 }, { "yNm", 1000000 } } },
            { "strokeNm", 150000 }, { "angleDegrees", 0.0 },
            { "horizontalJustification", "center" },
            { "verticalJustification", "bottom" }, { "keepUpright", true } } },
        { "value", { { "visible", false }, { "layer", "F.Fab" } } }
    };
    JSON desired = JSON::array( { std::move( placement ) } );
    JSON inventory = JSON::array( {
            { { "itemId", footprintId }, { "itemType", "footprint" },
              { "reference", "U1" }, { "schematicLinked", true } }
    } );
    RECONCILER::RESULT reconciled =
            RECONCILER::Reconcile( desired, nullptr, inventory, Context() );
    BOOST_REQUIRE_MESSAGE( reconciled.ok, reconciled.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( reconciled.actions.size(), 2 );
    BOOST_CHECK_EQUAL( reconciled.counts["footprintPresentation"].get<int>(), 1 );
    const auto action = std::find_if(
            reconciled.actions.begin(), reconciled.actions.end(),
            []( const JSON& aCandidate )
            {
                return aCandidate.value( "action", "" )
                       == "update_footprint_presentation";
            } );
    BOOST_REQUIRE( action != reconciled.actions.end() );
    BOOST_CHECK_EQUAL( ( *action )["itemId"].get<std::string>(), footprintId );
    BOOST_CHECK_EQUAL( ( *action )["presentation"]["reference"]["strokeNm"].get<int64_t>(),
                       150000 );
    BOOST_CHECK( !( *action )["presentation"]["value"]["visible"].get<bool>() );
}


BOOST_FIXTURE_TEST_CASE( CreatesMissingFootprintFromExecutableSchematicInstance, FIXTURE )
{
    JSON placement = JSON::array( {
            { { "action", "place_by_reference" }, { "component", "R1" },
              { "position", { { "xNm", 12500000 }, { "yNm", 7500000 } } },
              { "rotationDegrees", 37.5 }, { "side", "back" }, { "locked", true },
              { "instance",
                { { "libraryId", "Resistor_SMD:R_0603_1608Metric" },
                  { "value", "1k" }, { "dnp", false },
                  { "symbolPath",
                    JSON::array( { "11111111-1111-8111-8111-111111111111" } ) },
                  { "symbolSheetName", "Root" },
                  { "symbolSheetFilename", "board.kicad_sch" },
                  { "padNets", { { "1", "SIGNAL" }, { "2", "GND" } } } } } }
    } );
    RECONCILER::RESULT created =
            RECONCILER::Reconcile( placement, nullptr, JSON::array(), Context() );
    BOOST_REQUIRE_MESSAGE( created.ok, created.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( created.actions.size(), 1 );
    BOOST_CHECK_EQUAL( created.counts["placement"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( created.counts["footprintCreate"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( created.actions[0]["action"].get<std::string>(),
                       "create_footprint" );
    BOOST_CHECK_EQUAL( created.actions[0]["component"].get<std::string>(), "R1" );
    const std::string footprintId =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                    "reconcile_board", "footprint", "R1" );
    BOOST_CHECK_EQUAL( created.actions[0]["itemId"].get<std::string>(), footprintId );
    BOOST_CHECK_EQUAL( created.actions[0]["instance"]["padNets"]["2"]
                               .get<std::string>(),
                       "GND" );
    BOOST_REQUIRE_EQUAL( created.nextState["managedPcbItems"].size(), 1 );
    BOOST_CHECK_EQUAL( created.nextState["managedPcbItems"][0]["itemType"]
                               .get<std::string>(),
                       "footprint" );
    BOOST_CHECK_EQUAL( created.nextState["managedPcbItems"][0]["itemId"]
                               .get<std::string>(),
                       footprintId );

    JSON live = JSON::array( {
            { { "itemId", footprintId }, { "itemType", "footprint" },
              { "reference", "R1" },
              { "libraryId", "Resistor_SMD:R_0603_1608Metric" },
              { "schematicLinked", true } }
    } );
    RECONCILER::RESULT repeated =
            RECONCILER::Reconcile( placement, created.nextState, live, Context() );
    BOOST_REQUIRE_MESSAGE( repeated.ok, repeated.diagnostics.dump() );
    BOOST_CHECK_EQUAL( repeated.counts["footprintCreate"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( repeated.counts["placement"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( repeated.actions[0]["action"].get<std::string>(), "update" );
    BOOST_CHECK_EQUAL( repeated.nextState.dump(), created.nextState.dump() );

    RECONCILER::RESULT removed =
            RECONCILER::Reconcile( JSON::array(), created.nextState,
                                   JSON::array( { { { "itemId", footprintId },
                                                    { "itemType", "footprint" } } } ),
                                   Context() );
    BOOST_REQUIRE_MESSAGE( removed.ok, removed.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( removed.actions.size(), 1 );
    BOOST_CHECK_EQUAL( removed.actions[0]["action"].get<std::string>(), "delete" );
    BOOST_CHECK_EQUAL( removed.actions[0]["itemId"].get<std::string>(), footprintId );
    BOOST_CHECK_EQUAL( removed.counts["delete"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( removed.counts["footprintDelete"].get<int>(), 1 );
    BOOST_CHECK( removed.nextState["managedPcbItems"].empty() );

    RECONCILER::RESULT collision =
            RECONCILER::Reconcile( placement, nullptr, live, Context() );
    BOOST_CHECK( !collision.ok );
    BOOST_CHECK_NE( collision.diagnostics.dump().find( "unmanaged_uuid_collision" ),
                    std::string::npos );

    placement[0]["instance"]["symbolPath"][0] = "not-a-uuid";
    RECONCILER::RESULT malformed =
            RECONCILER::Reconcile( placement, nullptr, JSON::array(), Context() );
    BOOST_CHECK( !malformed.ok );
    BOOST_CHECK_NE( malformed.diagnostics.dump().find( "invalid_placement" ),
                    std::string::npos );
}


BOOST_FIXTURE_TEST_CASE( ReplacesFootprintWhenTheDesiredLibraryIdentityChanges, FIXTURE )
{
    const std::string oldFootprintId = "11111111-1111-8111-8111-111111111111";
    JSON placement = JSON::array( {
            { { "action", "place_by_reference" }, { "component", "U1" },
              { "position", { { "xNm", 10000000 }, { "yNm", 8000000 } } },
              { "rotationDegrees", 90.0 }, { "side", "front" }, { "locked", false },
              { "instance",
                { { "libraryId", "Package_DFN_QFN:WSON-8-1EP_4x4mm_P0.8mm_EP1.98x3mm" },
                  { "value", "TPS62160" }, { "dnp", false },
                  { "symbolPath",
                    JSON::array( { "11111111-1111-8111-8111-111111111111" } ) },
                  { "symbolSheetName", "Root" },
                  { "symbolSheetFilename", "board.kicad_sch" },
                  { "padNets", { { "1", "VIN" }, { "2", "GND" } } } } } }
    } );
    JSON oldLive = JSON::array( {
            { { "itemId", oldFootprintId }, { "itemType", "footprint" },
              { "reference", "U1" }, { "libraryId", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm" },
              { "schematicLinked", true } }
    } );
    RECONCILER::RESULT replaced =
            RECONCILER::Reconcile( placement, nullptr, oldLive, Context() );
    BOOST_REQUIRE_MESSAGE( replaced.ok, replaced.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( replaced.actions.size(), 1 );
    BOOST_CHECK_EQUAL( replaced.actions[0]["action"].get<std::string>(),
                       "replace_footprint" );
    BOOST_CHECK_EQUAL( replaced.actions[0]["replacedItemId"].get<std::string>(),
                       oldFootprintId );
    BOOST_CHECK_EQUAL( replaced.counts["footprintReplace"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( replaced.counts["footprintDelete"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( replaced.counts["footprintCreate"].get<int>(), 1 );
    BOOST_REQUIRE_EQUAL( replaced.nextState["managedPcbItems"].size(), 1 );
    BOOST_CHECK_EQUAL( replaced.nextState["managedPcbItems"][0]["itemId"],
                       replaced.actions[0]["itemId"] );

    JSON unknownLive = oldLive;
    unknownLive[0].erase( "libraryId" );
    RECONCILER::RESULT unknown =
            RECONCILER::Reconcile( placement, nullptr, unknownLive, Context() );
    BOOST_CHECK( !unknown.ok );
    BOOST_CHECK_NE( unknown.diagnostics.dump().find( "unknown_live_footprint_library" ),
                    std::string::npos );
}


BOOST_FIXTURE_TEST_CASE( ReplacesAChangedFootprintSourceOnceAndPersistsItsRevision, FIXTURE )
{
    const std::string firstRevision( 64, 'a' );
    const std::string secondRevision( 64, 'b' );
    const std::string managedId =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                    "reconcile_board", "footprint", "U1" );
    JSON placement = JSON::array( {
            { { "action", "place_by_reference" }, { "component", "U1" },
              { "position", { { "xNm", 10000000 }, { "yNm", 8000000 } } },
              { "rotationDegrees", 90.0 }, { "side", "front" }, { "locked", false },
              { "instance",
                { { "libraryId", "Product:REGULATOR" },
                  { "footprintSourceSha256", firstRevision },
                  { "value", "TPS62160" }, { "dnp", false },
                  { "symbolPath",
                    JSON::array( { "11111111-1111-8111-8111-111111111111" } ) },
                  { "symbolSheetName", "Root" },
                  { "symbolSheetFilename", "board.kicad_sch" },
                  { "padNets", { { "1", "VIN" }, { "2", "GND" } } } } } }
    } );

    RECONCILER::RESULT created =
            RECONCILER::Reconcile( placement, nullptr, JSON::array(), Context() );
    BOOST_REQUIRE_MESSAGE( created.ok, created.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( created.actions.size(), 1 );
    BOOST_CHECK_EQUAL( created.actions[0]["action"].get<std::string>(),
                       "create_footprint" );
    BOOST_REQUIRE_EQUAL( created.nextState["managedPcbItems"].size(), 1 );
    BOOST_CHECK_EQUAL(
            created.nextState["managedPcbItems"][0]["footprintSourceSha256"],
            firstRevision );

    JSON live = JSON::array( {
            { { "itemId", managedId }, { "itemType", "footprint" },
              { "reference", "U1" }, { "libraryId", "Product:REGULATOR" },
              { "schematicLinked", true } }
    } );
    RECONCILER::RESULT unchanged = RECONCILER::Reconcile(
            placement, created.nextState, live, Context() );
    BOOST_REQUIRE_MESSAGE( unchanged.ok, unchanged.diagnostics.dump() );
    BOOST_CHECK_EQUAL( unchanged.counts["footprintReplace"].get<int>(), 0 );
    BOOST_CHECK_EQUAL(
            unchanged.nextState["managedPcbItems"][0]["footprintSourceSha256"],
            firstRevision );

    placement[0]["instance"]["footprintSourceSha256"] = secondRevision;
    RECONCILER::RESULT changed = RECONCILER::Reconcile(
            placement, unchanged.nextState, live, Context() );
    BOOST_REQUIRE_MESSAGE( changed.ok, changed.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( changed.actions.size(), 1 );
    BOOST_CHECK_EQUAL( changed.actions[0]["action"].get<std::string>(),
                       "replace_footprint" );
    BOOST_CHECK_EQUAL( changed.actions[0]["replacedItemId"].get<std::string>(),
                       managedId );
    BOOST_CHECK_EQUAL( changed.counts["footprintReplace"].get<int>(), 1 );
    BOOST_CHECK_EQUAL(
            changed.nextState["managedPcbItems"][0]["footprintSourceSha256"],
            secondRevision );
}


BOOST_FIXTURE_TEST_CASE( IsDeterministicAcrossDesiredStatementOrder, FIXTURE )
{
    JSON reversed = operations;
    std::reverse( reversed.begin(), reversed.end() );
    RECONCILER::RESULT first =
            RECONCILER::Reconcile( operations, nullptr, JSON::array(), Context() );
    RECONCILER::RESULT second =
            RECONCILER::Reconcile( reversed, nullptr, JSON::array(), Context() );
    BOOST_REQUIRE( first.ok );
    BOOST_REQUIRE( second.ok );
    BOOST_CHECK_EQUAL( first.actions.dump(), second.actions.dump() );
    BOOST_CHECK_EQUAL( first.nextState.dump(), second.nextState.dump() );
}


BOOST_AUTO_TEST_SUITE_END()
