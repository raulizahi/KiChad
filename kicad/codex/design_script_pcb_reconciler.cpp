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

#include "design_script_pcb_reconciler.h"

#include "design_script_pcb_planner.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>


namespace
{

using JSON = nlohmann::json;
using CONTEXT = KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::CONTEXT;
using RESULT = KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::RESULT;

constexpr size_t MAX_MANAGED_ITEMS = 10000;
constexpr size_t MAX_LIVE_ITEMS = 20000;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


bool boundedText( const std::string& aText, size_t aMaximum )
{
    return !aText.empty() && aText.size() <= aMaximum
           && aText.find( '\0' ) == std::string::npos;
}


bool sha256( const std::string& aText )
{
    return aText.size() == 64
           && std::all_of( aText.begin(), aText.end(),
                           []( unsigned char aCharacter )
                           {
                               return std::isdigit( aCharacter )
                                      || ( aCharacter >= 'a' && aCharacter <= 'f' );
                           } );
}


bool uuid( const std::string& aText )
{
    if( aText.size() != 36 || aText[8] != '-' || aText[13] != '-'
        || aText[18] != '-' || aText[23] != '-' )
    {
        return false;
    }

    for( size_t i = 0; i < aText.size(); ++i )
    {
        if( i == 8 || i == 13 || i == 18 || i == 23 )
            continue;

        const unsigned char character = static_cast<unsigned char>( aText[i] );

        if( !std::isdigit( character ) && !( character >= 'a' && character <= 'f' ) )
            return false;
    }

    return true;
}


bool managedType( const std::string& aType )
{
    return aType == "shape" || aType == "trace" || aType == "arc" || aType == "via"
           || aType == "zone" || aType == "rule_area" || aType == "text"
           || aType == "textbox" || aType == "table" || aType == "dimension"
           || aType == "reference_image" || aType == "barcode"
           || aType == "footprint";
}


bool componentReference( const std::string& aReference )
{
    return boundedText( aReference, 128 )
           && std::all_of( aReference.begin(), aReference.end(),
                           []( unsigned char aCharacter )
                           {
                               return std::isalnum( aCharacter ) || aCharacter == '_'
                                      || aCharacter == '-' || aCharacter == '+'
                                      || aCharacter == '.' || aCharacter == '/'
                                      || aCharacter == '#';
                           } );
}


JSON updateFields( const std::string& aType, const JSON& aItem )
{
    if( aType == "shape" )
        return JSON::array( { "shape", "layer", "locked" } );

    if( aType == "trace" )
        return JSON::array( { "start", "end", "width", "layer", "net", "locked" } );

    if( aType == "arc" )
        return JSON::array( { "start", "mid", "end", "width", "layer", "net", "locked" } );

    if( aType == "zone" )
    {
        return JSON::array( { "type", "layers", "outline", "name", "copper_settings",
                              "priority", "filled", "filled_polygons", "border", "locked",
                              "layer_properties" } );
    }

    if( aType == "rule_area" )
    {
        return JSON::array( { "type", "layers", "outline", "name", "rule_area_settings",
                              "priority", "filled", "filled_polygons", "border", "locked",
                              "layer_properties" } );
    }

    if( aType == "text" )
        return JSON::array( { "text", "layer", "knockout", "locked" } );

    if( aType == "textbox" )
    {
        return JSON::array( { "textbox", "layer", "locked", "outline", "margins",
                              "border_enabled", "knockout" } );
    }

    if( aType == "table" )
    {
        return JSON::array( { "layer", "locked", "column_count", "column_widths",
                              "row_heights", "stroke_external",
                              "stroke_header_separator", "border_stroke", "stroke_rows",
                              "stroke_columns", "separators_stroke", "cells" } );
    }

    if( aType == "dimension" )
    {
        JSON fields = JSON::array( { "locked", "layer", "text" } );

        for( const char* geometry :
             { "aligned", "orthogonal", "radial", "leader", "center" } )
        {
            if( aItem.contains( geometry ) )
            {
                fields.emplace_back( geometry );
                break;
            }
        }

        for( const char* field : { "override_text_enabled", "override_text", "prefix", "suffix",
                                   "unit", "unit_format", "arrow_direction", "precision",
                                   "suppress_trailing_zeroes", "line_thickness", "arrow_length",
                                   "extension_offset", "text_position", "keep_text_aligned" } )
        {
            fields.emplace_back( field );
        }

        return fields;
    }

    if( aType == "reference_image" )
    {
        return JSON::array( { "layer", "position", "transform_origin_offset",
                              "image_scale", "image_data", "locked" } );
    }

    if( aType == "barcode" )
    {
        return JSON::array( { "text", "kind", "error_correction", "position",
                              "orientation", "layer", "width", "height", "show_text",
                              "text_height", "knockout", "knockout_margin", "locked" } );
    }

    return JSON::array( { "position", "pad_stack", "locked", "net", "type" } );
}


struct MANAGED_ITEM
{
    std::string logicalId;
    std::string itemType;
    std::string itemId;
    std::string footprintSourceSha256;
    JSON        item = JSON::object();
};


struct PLACEMENT
{
    std::string component;
    int64_t     xNm = 0;
    int64_t     yNm = 0;
    double      rotationDegrees = 0.0;
    std::string side;
    bool        locked = false;
    std::string footprintSourceSha256;
    JSON        instance;
    JSON        presentation = JSON::object();
};


struct LIVE_FOOTPRINT
{
    std::string itemId;
    std::string libraryId;
    bool        schematicLinked = false;
};


bool readFieldPresentation( const JSON& aJson )
{
    static const std::set<std::string> allowed = {
        "visible", "layer", "position", "size", "strokeNm", "angleDegrees",
        "horizontalJustification", "verticalJustification", "fontName", "bold",
        "italic", "underlined", "mirrored", "keepUpright"
    };

    if( !aJson.is_object() || aJson.empty() || aJson.size() > allowed.size() )
        return false;

    for( auto field = aJson.begin(); field != aJson.end(); ++field )
    {
        if( !allowed.contains( field.key() ) )
            return false;
    }

    for( const char* name :
         { "visible", "bold", "italic", "underlined", "mirrored", "keepUpright" } )
    {
        if( aJson.contains( name ) && !aJson[name].is_boolean() )
            return false;
    }

    if( aJson.contains( "layer" )
        && ( !aJson["layer"].is_string()
             || ( aJson["layer"] != "F.SilkS" && aJson["layer"] != "B.SilkS"
                  && aJson["layer"] != "F.Fab" && aJson["layer"] != "B.Fab" ) ) )
    {
        return false;
    }

    const auto vector = [&]( const char* aName, int64_t aMinimum, int64_t aMaximum )
    {
        if( !aJson.contains( aName ) )
            return true;

        const JSON& value = aJson[aName];
        return value.is_object() && value.size() == 2
               && value.contains( "xNm" ) && value["xNm"].is_number_integer()
               && value.contains( "yNm" ) && value["yNm"].is_number_integer()
               && value["xNm"].get<int64_t>() >= aMinimum
               && value["xNm"].get<int64_t>() <= aMaximum
               && value["yNm"].get<int64_t>() >= aMinimum
               && value["yNm"].get<int64_t>() <= aMaximum;
    };

    if( !vector( "position", std::numeric_limits<int>::min(),
                 std::numeric_limits<int>::max() )
        || !vector( "size", 1000, 250000000 ) )
    {
        return false;
    }

    if( aJson.contains( "strokeNm" ) )
    {
        if( !aJson["strokeNm"].is_number_integer() )
            return false;

        const int64_t stroke = aJson["strokeNm"].get<int64_t>();
        int64_t maximum = 250000000;

        if( aJson.contains( "size" ) )
        {
            maximum = std::min( aJson["size"]["xNm"].get<int64_t>(),
                                aJson["size"]["yNm"].get<int64_t>() )
                      / 4;
        }

        if( stroke <= 0 || stroke > maximum )
            return false;
    }

    if( aJson.contains( "angleDegrees" )
        && ( !aJson["angleDegrees"].is_number()
             || !std::isfinite( aJson["angleDegrees"].get<double>() )
             || std::abs( aJson["angleDegrees"].get<double>() ) > 360000.0 ) )
    {
        return false;
    }

    if( aJson.contains( "horizontalJustification" )
        && ( !aJson["horizontalJustification"].is_string()
             || ( aJson["horizontalJustification"] != "left"
                  && aJson["horizontalJustification"] != "center"
                  && aJson["horizontalJustification"] != "right" ) ) )
    {
        return false;
    }

    if( aJson.contains( "verticalJustification" )
        && ( !aJson["verticalJustification"].is_string()
             || ( aJson["verticalJustification"] != "top"
                  && aJson["verticalJustification"] != "center"
                  && aJson["verticalJustification"] != "bottom" ) ) )
    {
        return false;
    }

    return !aJson.contains( "fontName" )
           || ( aJson["fontName"].is_string()
                && aJson["fontName"].get<std::string>().size() <= 256 );
}


bool readPlacement( const JSON& aJson, PLACEMENT& aPlacement, RESULT& aResult )
{
    if( !aJson.is_object() || aJson.value( "action", "" ) != "place_by_reference"
        || !aJson.contains( "component" ) || !aJson["component"].is_string()
        || !aJson.contains( "position" ) || !aJson["position"].is_object()
        || !aJson["position"].contains( "xNm" )
        || !aJson["position"]["xNm"].is_number_integer()
        || !aJson["position"].contains( "yNm" )
        || !aJson["position"]["yNm"].is_number_integer()
        || !aJson.contains( "rotationDegrees" )
        || !aJson["rotationDegrees"].is_number()
        || !aJson.contains( "side" ) || !aJson["side"].is_string()
        || !aJson.contains( "locked" ) || !aJson["locked"].is_boolean() )
    {
        diagnostic( aResult, "invalid_placement", "planned component placement is malformed" );
        return false;
    }

    aPlacement.component = aJson["component"].get<std::string>();
    aPlacement.xNm = aJson["position"]["xNm"].get<int64_t>();
    aPlacement.yNm = aJson["position"]["yNm"].get<int64_t>();
    aPlacement.rotationDegrees = aJson["rotationDegrees"].get<double>();
    aPlacement.side = aJson["side"].get<std::string>();
    aPlacement.locked = aJson["locked"].get<bool>();
    aPlacement.presentation = aJson.value( "presentation", JSON::object() );

    if( !componentReference( aPlacement.component )
        || !std::isfinite( aPlacement.rotationDegrees )
        || std::abs( aPlacement.rotationDegrees ) > 360000.0
        || ( aPlacement.side != "front" && aPlacement.side != "back" )
        || !aPlacement.presentation.is_object()
        || aPlacement.presentation.size() > 2 )
    {
        diagnostic( aResult, "invalid_placement",
                    "planned component placement contains an invalid value" );
        return false;
    }

    for( auto field = aPlacement.presentation.begin();
         field != aPlacement.presentation.end(); ++field )
    {
        if( ( field.key() != "reference" && field.key() != "value" )
            || !readFieldPresentation( field.value() ) )
        {
            diagnostic( aResult, "invalid_placement",
                        "planned footprint presentation contains an invalid value" );
            return false;
        }
    }

    if( !aJson.contains( "instance" ) )
        return true;

    const JSON& instance = aJson["instance"];

    if( !instance.is_object() || !instance.contains( "libraryId" )
        || !instance["libraryId"].is_string() || !instance.contains( "value" )
        || !instance["value"].is_string() || !instance.contains( "dnp" )
        || !instance["dnp"].is_boolean() || !instance.contains( "symbolPath" )
        || !instance["symbolPath"].is_array() || instance["symbolPath"].empty()
        || instance["symbolPath"].size() > 64
        || !instance.contains( "symbolSheetName" )
        || !instance["symbolSheetName"].is_string()
        || !instance.contains( "symbolSheetFilename" )
        || !instance["symbolSheetFilename"].is_string()
        || ( instance.contains( "fields" )
             && ( !instance["fields"].is_object() || instance["fields"].size() > 1024 ) )
        || !instance.contains( "padNets" ) || !instance["padNets"].is_object()
        || instance["padNets"].size() > 1024 )
    {
        diagnostic( aResult, "invalid_placement",
                    "planned component footprint instance is malformed" );
        return false;
    }

    const std::string libraryId = instance["libraryId"].get<std::string>();
    const size_t separator = libraryId.find( ':' );

    if( !boundedText( libraryId, 512 ) || separator == std::string::npos
        || separator == 0 || separator + 1 == libraryId.size()
        || libraryId.find( ':', separator + 1 ) != std::string::npos
        || !boundedText( instance["value"].get<std::string>(), 1024 )
        || !boundedText( instance["symbolSheetName"].get<std::string>(), 1024 )
        || !boundedText( instance["symbolSheetFilename"].get<std::string>(), 4096 ) )
    {
        diagnostic( aResult, "invalid_placement",
                    "planned component footprint instance contains an invalid value" );
        return false;
    }

    for( const JSON& id : instance["symbolPath"] )
    {
        if( !id.is_string() || !uuid( id.get<std::string>() ) )
        {
            diagnostic( aResult, "invalid_placement",
                        "planned component symbol path contains an invalid UUID" );
            return false;
        }
    }

    for( auto net = instance["padNets"].begin(); net != instance["padNets"].end(); ++net )
    {
        if( !boundedText( net.key(), 64 ) || !net.value().is_string()
            || !boundedText( net.value().get<std::string>(), 1024 ) )
        {
            diagnostic( aResult, "invalid_placement",
                        "planned component pad-to-net mapping is invalid" );
            return false;
        }
    }

    const JSON fields = instance.value( "fields", JSON::object() );

    if( instance.contains( "footprintSourceSha256" )
        && ( !instance["footprintSourceSha256"].is_string()
             || !sha256( instance["footprintSourceSha256"].get<std::string>() ) ) )
    {
        diagnostic( aResult, "invalid_placement",
                    "planned component footprint source revision is invalid" );
        return false;
    }

    for( auto field = fields.begin(); field != fields.end(); ++field )
    {
        if( !boundedText( field.key(), 128 ) || !field.value().is_string()
            || field.value().get<std::string>().size() > 4096
            || field.value().get<std::string>().find( '\0' ) != std::string::npos )
        {
            diagnostic( aResult, "invalid_placement",
                        "planned component footprint field is invalid" );
            return false;
        }
    }

    aPlacement.instance = instance;
    aPlacement.instance["fields"] = fields;
    aPlacement.footprintSourceSha256 =
            instance.value( "footprintSourceSha256", std::string() );

    return true;
}


bool validateContext( const CONTEXT& aContext, RESULT& aResult )
{
    bool valid = true;

    if( !boundedText( aContext.sourcePath, 4096 ) )
    {
        diagnostic( aResult, "invalid_reconcile_context", "sourcePath is invalid" );
        valid = false;
    }

    if( !boundedText( aContext.boardPath, 4096 ) )
    {
        diagnostic( aResult, "invalid_reconcile_context", "boardPath is invalid" );
        valid = false;
    }

    if( !boundedText( aContext.projectName, 128 ) )
    {
        diagnostic( aResult, "invalid_reconcile_context", "projectName is invalid" );
        valid = false;
    }

    if( !sha256( aContext.sourceSha256 ) )
    {
        diagnostic( aResult, "invalid_reconcile_context", "sourceSha256 is invalid" );
        valid = false;
    }

    return valid;
}


bool readManagedItem( const JSON& aJson, const std::string& aProjectName,
                      MANAGED_ITEM& aItem, bool aRequirePayload, RESULT& aResult,
                      const std::string& aCode )
{
    if( !aJson.is_object() || !aJson.contains( "logicalId" )
        || !aJson["logicalId"].is_string() || !aJson.contains( "itemType" )
        || !aJson["itemType"].is_string() || !aJson.contains( "itemId" )
        || !aJson["itemId"].is_string() )
    {
        diagnostic( aResult, aCode, "managed item identity is malformed" );
        return false;
    }

    aItem.logicalId = aJson["logicalId"].get<std::string>();
    aItem.itemType = aJson["itemType"].get<std::string>();
    aItem.itemId = aJson["itemId"].get<std::string>();

    if( aJson.contains( "footprintSourceSha256" ) )
    {
        if( aItem.itemType != "footprint" || !aJson["footprintSourceSha256"].is_string()
            || !sha256( aJson["footprintSourceSha256"].get<std::string>() ) )
        {
            diagnostic( aResult, aCode,
                        "managed footprint source revision contains an invalid value" );
            return false;
        }

        aItem.footprintSourceSha256 =
                aJson["footprintSourceSha256"].get<std::string>();
    }

    if( !boundedText( aItem.logicalId, 128 ) || !managedType( aItem.itemType )
        || ( aItem.itemType == "footprint" && !componentReference( aItem.logicalId ) )
        || !uuid( aItem.itemId )
        || KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                   aProjectName, aItem.itemType, aItem.logicalId ) != aItem.itemId )
    {
        diagnostic( aResult, aCode, "managed item identity contains an invalid value" );
        return false;
    }

    if( aRequirePayload )
    {
        if( !aJson.contains( "item" ) || !aJson["item"].is_object()
            || !aJson["item"].contains( "id" ) || !aJson["item"]["id"].is_object()
            || !aJson["item"]["id"].contains( "value" )
            || !aJson["item"]["id"]["value"].is_string()
            || aJson["item"]["id"]["value"].get<std::string>() != aItem.itemId )
        {
            diagnostic( aResult, aCode, "planned item payload does not contain its itemId" );
            return false;
        }

        aItem.item = aJson["item"];
    }

    return true;
}


bool readPreviousState( const JSON& aState, const CONTEXT& aContext,
                        std::map<std::string, MANAGED_ITEM>& aItems, RESULT& aResult )
{
    if( aState.is_null() )
        return true;

    if( !aState.is_object() || aState.value( "format", "" ) != "kichad-kds-managed-state"
        || aState.value( "version", 0 ) != 1 || !aState.contains( "sourcePath" )
        || !aState["sourcePath"].is_string() || !aState.contains( "boardPath" )
        || !aState["boardPath"].is_string() || !aState.contains( "projectName" )
        || !aState["projectName"].is_string() || !aState.contains( "sourceSha256" )
        || !aState["sourceSha256"].is_string() || !aState.contains( "managedPcbItems" )
        || !aState["managedPcbItems"].is_array()
        || aState["managedPcbItems"].size() > MAX_MANAGED_ITEMS )
    {
        diagnostic( aResult, "invalid_managed_state",
                    "managed-state manifest is malformed or unsupported" );
        return false;
    }

    if( aState["sourcePath"].get<std::string>() != aContext.sourcePath
        || aState["boardPath"].get<std::string>() != aContext.boardPath
        || aState["projectName"].get<std::string>() != aContext.projectName )
    {
        diagnostic( aResult, "managed_state_scope_mismatch",
                    "managed-state manifest belongs to a different source, board, or project" );
        return false;
    }

    if( !boundedText( aState["projectName"].get<std::string>(), 128 )
        || !sha256( aState["sourceSha256"].get<std::string>() ) )
    {
        diagnostic( aResult, "invalid_managed_state",
                    "managed-state project or source revision is invalid" );
        return false;
    }

    std::set<std::pair<std::string, std::string>> logicalIds;

    for( const JSON& entry : aState["managedPcbItems"] )
    {
        MANAGED_ITEM item;

        if( !readManagedItem( entry, aState["projectName"].get<std::string>(), item, false,
                              aResult, "invalid_managed_state" ) )
            continue;

        if( !aItems.emplace( item.itemId, item ).second
            || !logicalIds.emplace( item.itemType, item.logicalId ).second )
        {
            diagnostic( aResult, "invalid_managed_state",
                        "managed-state item identities are not unique" );
        }
    }

    return aResult.diagnostics.empty();
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_PCB_RECONCILER::RESULT DESIGN_SCRIPT_PCB_RECONCILER::Reconcile(
        const JSON& aDesiredOperations, const JSON& aPreviousState,
        const JSON& aLiveInventory, const CONTEXT& aContext )
{
    RESULT result;
    result.counts = { { "create", 0 }, { "update", 0 }, { "delete", 0 },
                      { "placement", 0 }, { "footprintCreate", 0 },
                      { "footprintDelete", 0 }, { "footprintReplace", 0 },
                      { "footprintMetadata", 0 },
                      { "footprintPresentation", 0 } };
    validateContext( aContext, result );

    std::map<std::string, MANAGED_ITEM> previous;
    readPreviousState( aPreviousState, aContext, previous, result );

    std::map<std::string, MANAGED_ITEM> desired;
    std::set<std::pair<std::string, std::string>> desiredLogicalIds;
    std::map<std::string, PLACEMENT>    placements;

    if( !aDesiredOperations.is_array() || aDesiredOperations.size() > MAX_MANAGED_ITEMS )
    {
        diagnostic( result, "invalid_desired_operations",
                    "desired PCB operations must be a bounded array" );
    }
    else
    {
        for( const JSON& operation : aDesiredOperations )
        {
            const std::string action = operation.is_object()
                                               ? operation.value( "action", "" )
                                               : "";

            if( action == "place_by_reference" )
            {
                PLACEMENT placement;

                if( readPlacement( operation, placement, result )
                    && !placements.emplace( placement.component, placement ).second )
                {
                    diagnostic( result, "duplicate_component_placement",
                                "component " + placement.component
                                        + " has more than one planned placement" );
                }

                continue;
            }

            if( action != "upsert" )
            {
                diagnostic( result, "unsupported_desired_operation",
                            "apply received an unsupported lowered PCB operation" );
                continue;
            }

            MANAGED_ITEM item;

            if( !readManagedItem( operation, aContext.projectName, item, true, result,
                                  "invalid_desired_operations" ) )
            {
                continue;
            }

            if( item.itemType == "footprint" )
            {
                diagnostic( result, "invalid_desired_operations",
                            "footprint ownership must be derived from component placement" );
                continue;
            }

            if( !desired.emplace( item.itemId, item ).second
                || !desiredLogicalIds.emplace( item.itemType, item.logicalId ).second )
            {
                diagnostic( result, "invalid_desired_operations",
                            "desired PCB item identities are not unique" );
            }
        }
    }

    std::map<std::string, std::string> live;
    std::map<std::string, std::vector<LIVE_FOOTPRINT>> liveFootprints;
    std::set<std::string> liveFootprintIds;

    if( !aLiveInventory.is_array() || aLiveInventory.size() > MAX_LIVE_ITEMS )
    {
        diagnostic( result, "invalid_live_inventory",
                    "live PCB inventory must be a bounded array" );
    }
    else
    {
        for( const JSON& entry : aLiveInventory )
        {
            if( !entry.is_object() || !entry.contains( "itemId" )
                || !entry["itemId"].is_string() || !entry.contains( "itemType" )
                || !entry["itemType"].is_string()
                || !uuid( entry["itemId"].get<std::string>() )
                || !boundedText( entry["itemType"].get<std::string>(), 128 ) )
            {
                diagnostic( result, "invalid_live_inventory",
                            "live PCB inventory contains an invalid item" );
                continue;
            }

            const std::string itemId = entry["itemId"].get<std::string>();
            const std::string itemType = entry["itemType"].get<std::string>();
            auto [existing, inserted] = live.emplace( itemId, itemType );

            if( !inserted && existing->second != itemType )
            {
                diagnostic( result, "invalid_live_inventory",
                            "live PCB inventory contains conflicting item types" );
            }

            if( itemType == "footprint" )
            {
                const bool hasReference = entry.contains( "reference" );
                const bool hasLink = entry.contains( "schematicLinked" );

                // UUID-targeted inventory needs only type and identity. Reference-targeted
                // inventory supplies both fields and is used for placement resolution.
                if( hasReference != hasLink )
                {
                    diagnostic( result, "invalid_live_inventory",
                                "live footprint inventory contains an invalid identity" );
                    continue;
                }

                if( hasReference )
                {
                    if( !entry["reference"].is_string()
                        || !componentReference( entry["reference"].get<std::string>() )
                        || !entry["schematicLinked"].is_boolean()
                        || !liveFootprintIds.emplace( itemId ).second )
                    {
                        diagnostic( result, "invalid_live_inventory",
                                    "live footprint inventory contains an invalid identity" );
                        continue;
                    }

                    std::string libraryId;

                    if( entry.contains( "libraryId" ) )
                    {
                        const std::string candidate =
                                entry["libraryId"].is_string()
                                        ? entry["libraryId"].get<std::string>()
                                        : std::string();
                        const size_t separator = candidate.find( ':' );

                        if( !entry["libraryId"].is_string()
                            || !boundedText( candidate, 512 )
                            || separator == std::string::npos || separator == 0
                            || separator + 1 == candidate.size()
                            || candidate.find( ':', separator + 1 ) != std::string::npos )
                        {
                            diagnostic( result, "invalid_live_inventory",
                                        "live footprint inventory contains an invalid library ID" );
                            continue;
                        }

                        libraryId = entry["libraryId"].get<std::string>();
                    }

                    liveFootprints[entry["reference"].get<std::string>()].push_back(
                            { itemId, std::move( libraryId ),
                              entry["schematicLinked"].get<bool>() } );
                }
            }
        }
    }

    if( !result.diagnostics.empty() )
        return result;

    for( const auto& [itemId, item] : desired )
    {
        auto liveItem = live.find( itemId );

        if( liveItem == live.end() )
        {
            result.actions.push_back( { { "action", "create" },
                                        { "logicalId", item.logicalId },
                                        { "itemType", item.itemType },
                                        { "itemId", item.itemId },
                                        { "item", item.item } } );
            ++result.counts["create"].get_ref<int64_t&>();
            continue;
        }

        auto previousItem = previous.find( itemId );

        if( previousItem == previous.end() )
        {
            diagnostic( result, "unmanaged_uuid_collision",
                        "desired UUID " + itemId + " is already used by an unmanaged board item" );
            continue;
        }

        if( previousItem->second.itemType != item.itemType
            || previousItem->second.logicalId != item.logicalId
            || liveItem->second != item.itemType )
        {
            diagnostic( result, "managed_item_identity_conflict",
                        "managed UUID " + itemId + " has changed identity or live type" );
            continue;
        }

        result.actions.push_back( { { "action", "update" },
                                    { "logicalId", item.logicalId },
                                    { "itemType", item.itemType },
                                    { "itemId", item.itemId },
                                    { "fieldMask", updateFields( item.itemType, item.item ) },
                                    { "item", item.item } } );
        ++result.counts["update"].get_ref<int64_t&>();
    }

    for( const auto& [itemId, item] : previous )
    {
        if( item.itemType == "footprint" || desired.contains( itemId ) )
            continue;

        auto liveItem = live.find( itemId );

        if( liveItem == live.end() )
            continue;

        if( liveItem->second != item.itemType )
        {
            diagnostic( result, "managed_item_identity_conflict",
                        "obsolete managed UUID " + itemId + " has a different live type" );
            continue;
        }

        result.actions.push_back( { { "action", "delete" },
                                    { "logicalId", item.logicalId },
                                    { "itemType", item.itemType },
                                    { "itemId", item.itemId } } );
        ++result.counts["delete"].get_ref<int64_t&>();
    }

    std::map<std::string, MANAGED_ITEM> desiredFootprints;

    const auto retainFootprintOwnership = [&]( const std::string& aComponent,
                                                const std::string& aItemId,
                                                const std::string& aSourceSha256 )
    {
        MANAGED_ITEM item = { aComponent, "footprint", aItemId, aSourceSha256,
                              JSON::object() };

        if( !desiredFootprints.emplace( aItemId, item ).second
            || !desiredLogicalIds.emplace( item.itemType, item.logicalId ).second )
        {
            diagnostic( result, "invalid_desired_operations",
                        "desired PCB ownership identities are not unique" );
        }
    };

    for( const auto& [component, placement] : placements )
    {
        const std::string managedId = KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                aContext.projectName, "footprint", component );
        const auto previousOwned = previous.find( managedId );
        const bool ownedBefore = previousOwned != previous.end();
        const auto liveOwned = live.find( managedId );

        if( liveOwned != live.end() && liveOwned->second != "footprint" )
        {
            diagnostic( result, "managed_footprint_identity_conflict",
                        "component " + component
                                + " has a deterministic footprint UUID used by another item type" );
            continue;
        }

        auto footprints = liveFootprints.find( component );

        if( footprints == liveFootprints.end() )
        {
            if( liveOwned != live.end() )
            {
                diagnostic( result, "managed_footprint_identity_conflict",
                            "deterministic footprint for component " + component
                                    + " exists with a different reference or link" );
                continue;
            }

            if( !placement.instance.is_object() )
            {
                diagnostic( result, "missing_component_footprint",
                            "no live PCB footprint has reference " + component
                                    + " and no executable schematic instance was planned" );
                continue;
            }

            result.actions.push_back(
                    { { "action", "create_footprint" },
                      { "component", component },
                      { "logicalId", component },
                      { "itemType", "footprint" },
                      { "itemId", managedId },
                      { "instance", placement.instance },
                      { "position", { { "xNm", placement.xNm },
                                        { "yNm", placement.yNm } } },
                      { "rotationDegrees", placement.rotationDegrees },
                      { "side", placement.side },
                      { "locked", placement.locked },
                      { "presentation", placement.presentation } } );

            if( !placement.presentation.empty() )
                ++result.counts["footprintPresentation"].get_ref<int64_t&>();

            retainFootprintOwnership( component, managedId,
                                      placement.footprintSourceSha256 );
            ++result.counts["placement"].get_ref<int64_t&>();
            ++result.counts["footprintCreate"].get_ref<int64_t&>();
            continue;
        }

        if( footprints->second.size() != 1 )
        {
            diagnostic( result, "ambiguous_component_footprint",
                        "more than one live PCB footprint has reference " + component );
            continue;
        }

        const LIVE_FOOTPRINT& footprint = footprints->second.front();

        if( footprint.itemId == managedId && !ownedBefore )
        {
            diagnostic( result, "unmanaged_uuid_collision",
                        "deterministic footprint UUID for component " + component
                                + " exists without KDS ownership" );
            continue;
        }

        if( footprint.itemId != managedId && ownedBefore )
        {
            diagnostic( result, "managed_footprint_identity_conflict",
                        "owned footprint for component " + component
                                + " was removed or replaced by an unmanaged instance" );
            continue;
        }

        if( !footprint.schematicLinked )
        {
            diagnostic( result, "unlinked_component_footprint",
                        "PCB footprint " + component + " is not linked to a schematic symbol" );
            continue;
        }

        if( placement.instance.is_object() )
        {
            const std::string desiredLibraryId =
                    placement.instance["libraryId"].get<std::string>();

            if( footprint.libraryId.empty() )
            {
                diagnostic( result, "unknown_live_footprint_library",
                            "PCB footprint " + component
                                    + " does not report its source library identity, so KDS "
                                      "cannot safely determine whether its geometry must be replaced" );
                continue;
            }

            const bool sourceRevisionChanged =
                    !placement.footprintSourceSha256.empty()
                    && ( !ownedBefore
                         || previousOwned->second.footprintSourceSha256
                                    != placement.footprintSourceSha256 );

            if( footprint.libraryId != desiredLibraryId || sourceRevisionChanged )
            {
                result.actions.push_back(
                        { { "action", "replace_footprint" },
                          { "component", component },
                          { "logicalId", component },
                          { "itemType", "footprint" },
                          { "replacedItemId", footprint.itemId },
                          { "itemId", managedId },
                          { "instance", placement.instance },
                          { "position", { { "xNm", placement.xNm },
                                            { "yNm", placement.yNm } } },
                          { "rotationDegrees", placement.rotationDegrees },
                          { "side", placement.side },
                          { "locked", placement.locked },
                          { "presentation", placement.presentation } } );
                retainFootprintOwnership( component, managedId,
                                          placement.footprintSourceSha256 );
                ++result.counts["placement"].get_ref<int64_t&>();
                ++result.counts["footprintReplace"].get_ref<int64_t&>();
                ++result.counts["footprintCreate"].get_ref<int64_t&>();
                ++result.counts["footprintDelete"].get_ref<int64_t&>();

                if( !placement.presentation.empty() )
                    ++result.counts["footprintPresentation"].get_ref<int64_t&>();

                continue;
            }
        }

        if( ownedBefore )
            retainFootprintOwnership( component, managedId,
                                      placement.footprintSourceSha256 );

        JSON item = { { "id", { { "value", footprint.itemId } } },
                      { "position", { { "xNm", std::to_string( placement.xNm ) },
                                      { "yNm", std::to_string( placement.yNm ) } } },
                      { "orientation", { { "valueDegrees", placement.rotationDegrees } } },
                      { "layer", placement.side == "front" ? "BL_F_Cu" : "BL_B_Cu" },
                      { "locked", placement.locked ? "LS_LOCKED" : "LS_UNLOCKED" } };

        result.actions.push_back(
                { { "action", "update" },
                  { "component", component },
                  { "itemType", "footprint" },
                  { "itemId", footprint.itemId },
                  { "fieldMask", JSON::array( { "position", "orientation", "layer", "locked" } ) },
                  { "item", std::move( item ) } } );

        if( !placement.presentation.empty() )
        {
            result.actions.push_back(
                    { { "action", "update_footprint_presentation" },
                      { "component", component },
                      { "itemType", "footprint" },
                      { "itemId", footprint.itemId },
                      { "presentation", placement.presentation } } );
            ++result.counts["footprintPresentation"].get_ref<int64_t&>();
        }

        if( placement.instance.is_object() )
        {
            result.actions.push_back(
                    { { "action", "update_footprint_metadata" },
                      { "component", component },
                      { "itemType", "footprint" },
                      { "itemId", footprint.itemId },
                      { "instance", placement.instance } } );
            ++result.counts["footprintMetadata"].get_ref<int64_t&>();
        }

        ++result.counts["placement"].get_ref<int64_t&>();
    }

    for( const auto& [itemId, item] : previous )
    {
        if( item.itemType != "footprint" || desiredFootprints.contains( itemId ) )
            continue;

        auto liveItem = live.find( itemId );

        if( liveItem == live.end() )
            continue;

        if( liveItem->second != "footprint" )
        {
            diagnostic( result, "managed_footprint_identity_conflict",
                        "obsolete managed footprint UUID " + itemId
                                + " belongs to another item type" );
            continue;
        }

        result.actions.push_back( { { "action", "delete" },
                                    { "logicalId", item.logicalId },
                                    { "itemType", item.itemType },
                                    { "itemId", item.itemId } } );
        ++result.counts["delete"].get_ref<int64_t&>();
        ++result.counts["footprintDelete"].get_ref<int64_t&>();
    }

    if( !result.diagnostics.empty() )
    {
        result.actions = JSON::array();
        result.counts = { { "create", 0 }, { "update", 0 }, { "delete", 0 },
                          { "placement", 0 }, { "footprintCreate", 0 },
                          { "footprintDelete", 0 }, { "footprintReplace", 0 },
                          { "footprintMetadata", 0 },
                          { "footprintPresentation", 0 } };
        return result;
    }

    JSON managedItems = JSON::array();
    std::map<std::string, MANAGED_ITEM> nextOwned = desired;
    nextOwned.insert( desiredFootprints.begin(), desiredFootprints.end() );

    for( const auto& [itemId, item] : nextOwned )
    {
        JSON stateItem = { { "logicalId", item.logicalId },
                           { "itemType", item.itemType },
                           { "itemId", itemId } };

        if( item.itemType == "footprint" && !item.footprintSourceSha256.empty() )
            stateItem["footprintSourceSha256"] = item.footprintSourceSha256;

        managedItems.push_back( std::move( stateItem ) );
    }

    result.nextState = { { "format", "kichad-kds-managed-state" },
                         { "version", 1 },
                         { "sourcePath", aContext.sourcePath },
                         { "boardPath", aContext.boardPath },
                         { "projectName", aContext.projectName },
                         { "sourceSha256", aContext.sourceSha256 },
                         { "managedPcbItems", std::move( managedItems ) } };
    result.ok = true;
    return result;
}

} // namespace KICHAD
