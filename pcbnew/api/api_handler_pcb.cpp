/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <magic_enum.hpp>
#include <properties/property.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>

#include <common.h>
#include <api/api_handler_pcb.h>
#include <api/api_pcb_utils.h>
#include <api/api_enums.h>
#include <api/api_utils.h>
#include <board_commit.h>
#include <board_connected_item.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <kicad_clipboard.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_edit_frame.h>
#include <pcb_group.h>
#include <pcb_field.h>
#include <pcb_reference_image.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <pcb_textbox.h>
#include <pcb_table.h>
#include <pcb_track.h>
#include <pcbnew_id.h>
#include <pcb_marker.h>
#include <drc/drc_item.h>
#include <drc/drc_engine.h>
#include <drc/drc_rule_parser.h>
#include <layer_ids.h>
#include <project.h>
#include <tool/tool_manager.h>
#include <tools/pcb_actions.h>
#include <tools/pcb_selection_tool.h>
#include <zone.h>

#include <api/common/types/base_types.pb.h>
#include <connectivity/connectivity_data.h>
#include <google/protobuf/util/field_mask_util.h>
#include <widgets/appearance_controls.h>
#include <widgets/report_severity.h>
#include <wx/file.h>

using namespace kiapi::common::commands;
using types::CommandStatus;
using types::DocumentType;
using types::ItemRequestStatus;


namespace
{

constexpr size_t MAX_CUSTOM_RULES_BYTES = 1024 * 1024;
constexpr size_t MAX_CUSTOM_RULE_COUNT = 512;
constexpr size_t MAX_CUSTOM_CONSTRAINTS_PER_RULE = 64;
constexpr size_t MAX_CUSTOM_CONSTRAINT_COUNT = 4096;
constexpr size_t MAX_PARSED_ITEMS_BYTES = 4 * 1024 * 1024;


bool readCustomRulesFile( const wxFileName& aPath, bool& aPresent, std::string& aSource,
                          std::string& aError )
{
    aPresent = aPath.FileExists();
    aSource.clear();

    if( !aPresent )
        return true;

    wxFile file( aPath.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
    {
        aError = "could not open the board custom-rules file";
        return false;
    }

    const wxFileOffset length = file.Length();

    if( length < 0 || length > static_cast<wxFileOffset>( MAX_CUSTOM_RULES_BYTES ) )
    {
        aError = "board custom rules are limited to 1 MiB";
        return false;
    }

    aSource.assign( static_cast<size_t>( length ), '\0' );

    if( length > 0 && file.Read( aSource.data(), static_cast<size_t>( length ) ) != length )
    {
        aError = "could not read the complete board custom-rules file";
        aSource.clear();
        return false;
    }

    return true;
}


bool writeCustomRulesAtomically( const wxFileName& aPath, const std::string& aSource,
                                 std::string& aError )
{
    const wxString temporaryPath =
            aPath.GetFullPath() + wxS( ".tmp-" ) + KIID().AsString();
    wxFile temporary;

    if( !temporary.Create( temporaryPath, true )
        || temporary.Write( aSource.data(), aSource.size() ) != aSource.size()
        || !temporary.Flush() )
    {
        temporary.Close();
        wxRemoveFile( temporaryPath );
        aError = "could not durably write the custom-rules temporary file";
        return false;
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, aPath.GetFullPath(), true ) )
    {
        wxRemoveFile( temporaryPath );
        aError = "could not atomically install the custom-rules file";
        return false;
    }

    return true;
}


bool validateCustomRulesSource( const std::string& aSource, std::string& aError )
{
    if( aSource.empty() || aSource.size() > MAX_CUSTOM_RULES_BYTES
        || aSource.find( '\0' ) != std::string::npos )
    {
        aError = "board custom rules must contain 1 byte to 1 MiB of UTF-8 text";
        return false;
    }

    const wxString decoded = wxString::FromUTF8( aSource.data(), aSource.size() );
    const wxScopedCharBuffer reencoded = decoded.ToUTF8();

    if( reencoded.length() != aSource.size()
        || std::memcmp( reencoded.data(), aSource.data(), aSource.size() ) != 0 )
    {
        aError = "board custom rules must be valid UTF-8";
        return false;
    }

    try
    {
        std::vector<std::shared_ptr<DRC_RULE>> rules;
        DRC_RULES_PARSER parser( decoded, wxS( "API custom rules" ) );
        parser.Parse( rules, nullptr );

        if( rules.size() > MAX_CUSTOM_RULE_COUNT )
        {
            aError = "board custom rules support at most 512 rules";
            return false;
        }

        size_t constraintCount = 0;

        for( const std::shared_ptr<DRC_RULE>& rule : rules )
        {
            if( rule->m_Constraints.size() > MAX_CUSTOM_CONSTRAINTS_PER_RULE )
            {
                aError = "each board custom rule supports at most 64 constraints";
                return false;
            }

            constraintCount += rule->m_Constraints.size();

            if( constraintCount > MAX_CUSTOM_CONSTRAINT_COUNT )
            {
                aError = "board custom rules support at most 4096 constraints";
                return false;
            }
        }
    }
    catch( const PARSE_ERROR& error )
    {
        aError = error.what();
        return false;
    }

    return true;
}


std::unique_ptr<google::protobuf::Message> unpackAnyMessage(
        const google::protobuf::Any& aAny )
{
    const std::string& typeUrl = aAny.type_url();
    size_t             separator = typeUrl.rfind( '/' );
    std::string        typeName = separator == std::string::npos
                                          ? typeUrl
                                          : typeUrl.substr( separator + 1 );
    const google::protobuf::Descriptor* descriptor =
            google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName( typeName );

    if( !descriptor )
        return {};

    const google::protobuf::Message* prototype =
            google::protobuf::MessageFactory::generated_factory()->GetPrototype( descriptor );

    if( !prototype )
        return {};

    std::unique_ptr<google::protobuf::Message> message( prototype->New() );

    if( !aAny.UnpackTo( message.get() ) )
        return {};

    return message;
}


std::optional<KIID> itemIdFromAny( const google::protobuf::Any& aAny )
{
    std::unique_ptr<google::protobuf::Message> message = unpackAnyMessage( aAny );

    if( !message )
        return std::nullopt;

    const google::protobuf::FieldDescriptor* idField =
            message->GetDescriptor()->FindFieldByName( "id" );

    if( !idField || idField->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE )
        return std::nullopt;

    const google::protobuf::Message& idMessage =
            message->GetReflection()->GetMessage( *message, idField );
    const google::protobuf::FieldDescriptor* valueField =
            idMessage.GetDescriptor()->FindFieldByName( "value" );

    if( !valueField || valueField->cpp_type()
                              != google::protobuf::FieldDescriptor::CPPTYPE_STRING )
    {
        return std::nullopt;
    }

    std::string value = idMessage.GetReflection()->GetString( idMessage, valueField );

    if( !KIID::SniffTest( wxString::FromUTF8( value ) ) )
        return std::nullopt;

    return KIID( value );
}


bool mergeItemUpdate( const google::protobuf::Any& aUpdate,
                      const google::protobuf::Any& aExisting,
                      const google::protobuf::FieldMask& aMask,
                      google::protobuf::Any& aMerged, std::string& aError )
{
    if( aUpdate.type_url() != aExisting.type_url() )
    {
        aError = "the update protobuf type does not match the existing board item";
        return false;
    }

    std::unique_ptr<google::protobuf::Message> update = unpackAnyMessage( aUpdate );
    std::unique_ptr<google::protobuf::Message> existing = unpackAnyMessage( aExisting );

    if( !update || !existing )
    {
        aError = "the update protobuf message could not be decoded";
        return false;
    }

    for( const std::string& path : aMask.paths() )
    {
        if( path == "id" || path.starts_with( "id." ) )
        {
            aError = "an item's UUID cannot be changed by an update";
            return false;
        }

        if( !google::protobuf::util::FieldMaskUtil::GetFieldDescriptors(
                    update->GetDescriptor(), path, nullptr ) )
        {
            aError = fmt::format( "the field-mask path {} is invalid for {}", path,
                                  update->GetDescriptor()->full_name() );
            return false;
        }
    }

    google::protobuf::util::FieldMaskUtil::MergeOptions options;
    options.set_replace_message_fields( true );
    options.set_replace_repeated_fields( true );
    google::protobuf::util::FieldMaskUtil::MergeMessageTo(
            *update, aMask, options, existing.get() );
    aMerged.PackFrom( *existing );
    return true;
}


bool isManagedFootprintUpdateMask( const google::protobuf::FieldMask& aMask )
{
    static const std::set<std::string> allowed = {
        "position", "orientation", "layer", "locked", "value_field",
        "datasheet_field", "description_field", "attributes.do_not_populate",
        "definition.items",
        "reference_field.visible", "reference_field.text.layer",
        "reference_field.text.text.position",
        "reference_field.text.text.attributes.size",
        "reference_field.text.text.attributes.stroke_width",
        "reference_field.text.text.attributes.angle",
        "reference_field.text.text.attributes.horizontal_alignment",
        "reference_field.text.text.attributes.vertical_alignment",
        "reference_field.text.text.attributes.font_name",
        "reference_field.text.text.attributes.bold",
        "reference_field.text.text.attributes.italic",
        "reference_field.text.text.attributes.underlined",
        "reference_field.text.text.attributes.mirrored",
        "reference_field.text.text.attributes.keep_upright",
        "value_field.visible", "value_field.text.layer",
        "value_field.text.text.position",
        "value_field.text.text.attributes.size",
        "value_field.text.text.attributes.stroke_width",
        "value_field.text.text.attributes.angle",
        "value_field.text.text.attributes.horizontal_alignment",
        "value_field.text.text.attributes.vertical_alignment",
        "value_field.text.text.attributes.font_name",
        "value_field.text.text.attributes.bold",
        "value_field.text.text.attributes.italic",
        "value_field.text.text.attributes.underlined",
        "value_field.text.text.attributes.mirrored",
        "value_field.text.text.attributes.keep_upright"
    };

    if( aMask.paths().empty() )
        return false;

    for( const std::string& path : aMask.paths() )
    {
        if( !allowed.contains( path ) )
            return false;
    }

    return true;
}


bool isFootprintPresentationPath( const std::string& aPath )
{
    return aPath.starts_with( "reference_field." )
           || aPath.starts_with( "value_field." );
}


bool validFootprintPresentationField( const board::types::Field& aField )
{
    using namespace common::types;

    if( !aField.has_text() || !aField.text().has_text()
        || aField.text().text().text().empty()
        || aField.text().text().text().size() > 1024 )
    {
        return false;
    }

    const board::types::BoardLayer layer = aField.text().layer();

    if( layer != board::types::BL_F_SilkS && layer != board::types::BL_B_SilkS
        && layer != board::types::BL_F_Fab && layer != board::types::BL_B_Fab )
    {
        return false;
    }

    const Text& text = aField.text().text();
    const TextAttributes& attributes = text.attributes();
    const int64_t width = attributes.size().x_nm();
    const int64_t height = attributes.size().y_nm();
    const int64_t stroke = attributes.stroke_width().value_nm();

    return text.position().x_nm() >= std::numeric_limits<int>::min()
           && text.position().x_nm() <= std::numeric_limits<int>::max()
           && text.position().y_nm() >= std::numeric_limits<int>::min()
           && text.position().y_nm() <= std::numeric_limits<int>::max()
           && width >= 1000 && width <= 250000000
           && height >= 1000 && height <= 250000000
           && stroke >= 0
           && ( stroke == 0 || stroke <= std::min( width, height ) / 4 )
           && std::isfinite( attributes.angle().value_degrees() )
           && std::abs( attributes.angle().value_degrees() ) <= 360000.0
           && attributes.horizontal_alignment() != HorizontalAlignment::HA_UNKNOWN
           && attributes.vertical_alignment() != VerticalAlignment::VA_UNKNOWN
           && attributes.font_name().size() <= 256;
}


struct NATIVE_BOARD_RULES
{
    int  minimumClearance;
    int  minimumConnectionWidth;
    int  minimumTrackWidth;
    int  minimumViaAnnularWidth;
    int  minimumViaDiameter;
    int  minimumThroughHoleDiameter;
    int  minimumMicroviaDiameter;
    int  minimumMicroviaDrill;
    int  minimumHoleToHole;
    int  minimumCopperToHoleClearance;
    int  minimumSilkscreenClearance;
    int  minimumGrooveWidth;
    int  minimumResolvedSpokes;
    int  minimumSilkscreenTextHeight;
    int  minimumSilkscreenTextThickness;
    int  minimumCopperToEdgeClearance;
    bool useHeightForLengthCalculations;
    int  maximumError;
    bool allowFilletsOutsideZoneOutline;
    std::map<int, SEVERITY> ruleSeverities;
};


kiapi::board::BoardRuleSeverity encodeBoardRuleSeverity( SEVERITY aSeverity )
{
    if( aSeverity == RPT_SEVERITY_WARNING )
        return kiapi::board::BRS_WARNING;

    if( aSeverity == RPT_SEVERITY_IGNORE )
        return kiapi::board::BRS_IGNORE;

    return kiapi::board::BRS_ERROR;
}


bool decodeBoardRuleSeverity( kiapi::board::BoardRuleSeverity aSeverity,
                              SEVERITY& aDecoded )
{
    if( aSeverity == kiapi::board::BRS_ERROR )
        aDecoded = RPT_SEVERITY_ERROR;
    else if( aSeverity == kiapi::board::BRS_WARNING )
        aDecoded = RPT_SEVERITY_WARNING;
    else if( aSeverity == kiapi::board::BRS_IGNORE )
        aDecoded = RPT_SEVERITY_IGNORE;
    else
        return false;

    return true;
}


kiapi::board::BoardDesignRules encodeBoardDesignRules( const BOARD_DESIGN_SETTINGS& aSettings )
{
    kiapi::board::BoardDesignRules rules;
    rules.mutable_minimum_clearance()->set_value_nm( aSettings.m_MinClearance );
    rules.mutable_minimum_connection_width()->set_value_nm( aSettings.m_MinConn );
    rules.mutable_minimum_track_width()->set_value_nm( aSettings.m_TrackMinWidth );
    rules.mutable_minimum_via_annular_width()->set_value_nm( aSettings.m_ViasMinAnnularWidth );
    rules.mutable_minimum_via_diameter()->set_value_nm( aSettings.m_ViasMinSize );
    rules.mutable_minimum_through_hole_diameter()->set_value_nm( aSettings.m_MinThroughDrill );
    rules.mutable_minimum_microvia_diameter()->set_value_nm( aSettings.m_MicroViasMinSize );
    rules.mutable_minimum_microvia_drill()->set_value_nm( aSettings.m_MicroViasMinDrill );
    rules.mutable_minimum_hole_to_hole()->set_value_nm( aSettings.m_HoleToHoleMin );
    rules.mutable_minimum_copper_to_hole_clearance()->set_value_nm( aSettings.m_HoleClearance );
    rules.mutable_minimum_silkscreen_clearance()->set_value_nm( aSettings.m_SilkClearance );
    rules.mutable_minimum_groove_width()->set_value_nm( aSettings.m_MinGrooveWidth );
    rules.set_minimum_resolved_spokes( aSettings.m_MinResolvedSpokes );
    rules.mutable_minimum_silkscreen_text_height()->set_value_nm(
            aSettings.m_MinSilkTextHeight );
    rules.mutable_minimum_silkscreen_text_thickness()->set_value_nm(
            aSettings.m_MinSilkTextThickness );

    if( aSettings.m_CopperEdgeClearance < 0 )
    {
        rules.set_copper_edge_clearance_mode( kiapi::board::BCECM_LEGACY );
        rules.mutable_minimum_copper_to_edge_clearance()->set_value_nm( 0 );
    }
    else
    {
        rules.set_copper_edge_clearance_mode( kiapi::board::BCECM_EXPLICIT );
        rules.mutable_minimum_copper_to_edge_clearance()->set_value_nm(
                aSettings.m_CopperEdgeClearance );
    }

    rules.set_use_height_for_length_calculations( aSettings.m_UseHeightForLengthCalcs );
    rules.mutable_maximum_error()->set_value_nm( aSettings.m_MaxError );
    rules.set_allow_fillets_outside_zone_outline( aSettings.m_ZoneKeepExternalFillets );

    for( const RC_ITEM& item : DRC_ITEM::GetItemsWithSeverities() )
    {
        const wxString key = item.GetSettingsKey();

        if( key.IsEmpty() || !aSettings.m_DRCSeverities.contains( item.GetErrorCode() ) )
            continue;

        ( *rules.mutable_rule_severities() )[key.ToStdString()] =
                encodeBoardRuleSeverity(
                        aSettings.m_DRCSeverities.at( item.GetErrorCode() ) );
    }

    return rules;
}


bool decodeBoardDesignRules( const kiapi::board::BoardDesignRules& aRules,
                             NATIVE_BOARD_RULES& aDecoded, std::string& aError )
{
    auto distance = [&]( bool aPresent, const kiapi::common::types::Distance& aDistance,
                         int64_t aMinimum, int64_t aMaximum, const char* aName, int& aValue )
    {
        const int64_t value = aDistance.value_nm();

        if( !aPresent || value < aMinimum || value > aMaximum )
        {
            aError = fmt::format( "{} must be explicitly set from {}nm through {}nm", aName,
                                  aMinimum, aMaximum );
            return false;
        }

        aValue = static_cast<int>( value );
        return true;
    };

    if( !distance( aRules.has_minimum_clearance(), aRules.minimum_clearance(), 0, 25000000,
                   "minimum_clearance", aDecoded.minimumClearance )
        || !distance( aRules.has_minimum_connection_width(),
                      aRules.minimum_connection_width(), 0, 100000000,
                      "minimum_connection_width", aDecoded.minimumConnectionWidth )
        || !distance( aRules.has_minimum_track_width(), aRules.minimum_track_width(), 0,
                      25000000, "minimum_track_width", aDecoded.minimumTrackWidth )
        || !distance( aRules.has_minimum_via_annular_width(),
                      aRules.minimum_via_annular_width(), 0, 25000000,
                      "minimum_via_annular_width", aDecoded.minimumViaAnnularWidth )
        || !distance( aRules.has_minimum_via_diameter(), aRules.minimum_via_diameter(), 0,
                      25000000, "minimum_via_diameter", aDecoded.minimumViaDiameter )
        || !distance( aRules.has_minimum_through_hole_diameter(),
                      aRules.minimum_through_hole_diameter(), 0, 25000000,
                      "minimum_through_hole_diameter", aDecoded.minimumThroughHoleDiameter )
        || !distance( aRules.has_minimum_microvia_diameter(),
                      aRules.minimum_microvia_diameter(), 0, 10000000,
                      "minimum_microvia_diameter", aDecoded.minimumMicroviaDiameter )
        || !distance( aRules.has_minimum_microvia_drill(), aRules.minimum_microvia_drill(), 0,
                      10000000, "minimum_microvia_drill", aDecoded.minimumMicroviaDrill )
        || !distance( aRules.has_minimum_hole_to_hole(), aRules.minimum_hole_to_hole(), 0,
                      10000000, "minimum_hole_to_hole", aDecoded.minimumHoleToHole )
        || !distance( aRules.has_minimum_copper_to_hole_clearance(),
                      aRules.minimum_copper_to_hole_clearance(), 0, 100000000,
                      "minimum_copper_to_hole_clearance",
                      aDecoded.minimumCopperToHoleClearance )
        || !distance( aRules.has_minimum_silkscreen_clearance(),
                      aRules.minimum_silkscreen_clearance(), -10000000, 100000000,
                      "minimum_silkscreen_clearance", aDecoded.minimumSilkscreenClearance )
        || !distance( aRules.has_minimum_groove_width(), aRules.minimum_groove_width(), 0,
                      25000000, "minimum_groove_width", aDecoded.minimumGrooveWidth )
        || !distance( aRules.has_minimum_silkscreen_text_height(),
                      aRules.minimum_silkscreen_text_height(), 0, 100000000,
                      "minimum_silkscreen_text_height", aDecoded.minimumSilkscreenTextHeight )
        || !distance( aRules.has_minimum_silkscreen_text_thickness(),
                      aRules.minimum_silkscreen_text_thickness(), 0, 25000000,
                      "minimum_silkscreen_text_thickness",
                      aDecoded.minimumSilkscreenTextThickness )
        || !distance( aRules.has_maximum_error(), aRules.maximum_error(), 1000, 100000,
                      "maximum_error", aDecoded.maximumError ) )
    {
        return false;
    }

    if( !aRules.has_minimum_resolved_spokes() || aRules.minimum_resolved_spokes() > 99 )
    {
        aError = "minimum_resolved_spokes must be explicitly set from 0 through 99";
        return false;
    }

    aDecoded.minimumResolvedSpokes = static_cast<int>( aRules.minimum_resolved_spokes() );

    if( !aRules.has_use_height_for_length_calculations() )
    {
        aError = "use_height_for_length_calculations must be explicitly set";
        return false;
    }

    aDecoded.useHeightForLengthCalculations =
            aRules.use_height_for_length_calculations();

    if( !aRules.has_allow_fillets_outside_zone_outline() )
    {
        aError = "allow_fillets_outside_zone_outline must be explicitly set";
        return false;
    }

    aDecoded.allowFilletsOutsideZoneOutline =
            aRules.allow_fillets_outside_zone_outline();

    std::map<std::string, int> knownRuleKeys;

    for( const RC_ITEM& item : DRC_ITEM::GetItemsWithSeverities() )
    {
        const wxString key = item.GetSettingsKey();

        if( !key.IsEmpty() )
            knownRuleKeys.emplace( key.ToStdString(), item.GetErrorCode() );
    }

    if( aRules.has_default_rule_severity() )
    {
        SEVERITY defaultSeverity;

        if( !decodeBoardRuleSeverity( aRules.default_rule_severity(), defaultSeverity ) )
        {
            aError = "default_rule_severity must be error, warning, or ignore";
            return false;
        }

        for( const auto& [key, code] : knownRuleKeys )
            aDecoded.ruleSeverities[code] = defaultSeverity;
    }
    else if( static_cast<size_t>( aRules.rule_severities_size() )
             != knownRuleKeys.size() )
    {
        aError = "rule_severities must be complete when default_rule_severity is absent";
        return false;
    }

    for( const auto& [key, severity] : aRules.rule_severities() )
    {
        auto known = knownRuleKeys.find( key );
        SEVERITY decodedSeverity;

        if( known == knownRuleKeys.end()
            || !decodeBoardRuleSeverity( severity, decodedSeverity ) )
        {
            aError = "rule_severities contains an unknown check or invalid severity: " + key;
            return false;
        }

        aDecoded.ruleSeverities[known->second] = decodedSeverity;
    }

    if( aDecoded.ruleSeverities.size() != knownRuleKeys.size() )
    {
        aError = "rule_severities did not resolve every registered DRC check";
        return false;
    }

    if( !aRules.has_minimum_copper_to_edge_clearance() )
    {
        aError = "minimum_copper_to_edge_clearance must be explicitly set";
        return false;
    }

    if( aRules.copper_edge_clearance_mode() == kiapi::board::BCECM_LEGACY )
    {
        if( aRules.minimum_copper_to_edge_clearance().value_nm() != 0 )
        {
            aError = "legacy copper-edge clearance requires a normalized zero distance";
            return false;
        }

        aDecoded.minimumCopperToEdgeClearance = -10000;
    }
    else if( aRules.copper_edge_clearance_mode() == kiapi::board::BCECM_EXPLICIT )
    {
        if( !distance( true, aRules.minimum_copper_to_edge_clearance(), 0, 25000000,
                       "minimum_copper_to_edge_clearance",
                       aDecoded.minimumCopperToEdgeClearance ) )
        {
            return false;
        }
    }
    else
    {
        aError = "copper_edge_clearance_mode must be explicit or legacy";
        return false;
    }

    const int64_t minimumViaDiameter =
            static_cast<int64_t>( aDecoded.minimumThroughHoleDiameter )
            + 2LL * aDecoded.minimumViaAnnularWidth;
    if( aDecoded.minimumViaDiameter < minimumViaDiameter )
    {
        aError = "minimum_via_diameter cannot satisfy the drill and annular-width constraints";
        return false;
    }

    return true;
}

} // namespace


API_HANDLER_PCB::API_HANDLER_PCB( PCB_EDIT_FRAME* aFrame ) :
        API_HANDLER_EDITOR( aFrame )
{
    registerHandler<RunAction, RunActionResponse>( &API_HANDLER_PCB::handleRunAction );
    registerHandler<GetOpenDocuments, GetOpenDocumentsResponse>(
            &API_HANDLER_PCB::handleGetOpenDocuments );
    registerHandler<SaveDocument, Empty>( &API_HANDLER_PCB::handleSaveDocument );
    registerHandler<SaveCopyOfDocument, Empty>( &API_HANDLER_PCB::handleSaveCopyOfDocument );
    registerHandler<RevertDocument, Empty>( &API_HANDLER_PCB::handleRevertDocument );

    registerHandler<GetItems, GetItemsResponse>( &API_HANDLER_PCB::handleGetItems );
    registerHandler<GetItemsById, GetItemsResponse>( &API_HANDLER_PCB::handleGetItemsById );

    registerHandler<GetSelection, SelectionResponse>( &API_HANDLER_PCB::handleGetSelection );
    registerHandler<ClearSelection, Empty>( &API_HANDLER_PCB::handleClearSelection );
    registerHandler<AddToSelection, SelectionResponse>( &API_HANDLER_PCB::handleAddToSelection );
    registerHandler<RemoveFromSelection, SelectionResponse>(
            &API_HANDLER_PCB::handleRemoveFromSelection );

    registerHandler<GetBoardStackup, BoardStackupResponse>( &API_HANDLER_PCB::handleGetStackup );
    registerHandler<UpdateBoardStackup, BoardStackupResponse>(
            &API_HANDLER_PCB::handleUpdateStackup );
    registerHandler<GetBoardDesignRules, BoardDesignRulesResponse>(
            &API_HANDLER_PCB::handleGetBoardDesignRules );
    registerHandler<UpdateBoardDesignRules, BoardDesignRulesResponse>(
            &API_HANDLER_PCB::handleUpdateBoardDesignRules );
    registerHandler<GetBoardCustomRules, BoardCustomRulesResponse>(
            &API_HANDLER_PCB::handleGetBoardCustomRules );
    registerHandler<UpdateBoardCustomRules, BoardCustomRulesResponse>(
            &API_HANDLER_PCB::handleUpdateBoardCustomRules );
    registerHandler<GetBoardEnabledLayers, BoardEnabledLayersResponse>(
        &API_HANDLER_PCB::handleGetBoardEnabledLayers );
    registerHandler<SetBoardEnabledLayers, BoardEnabledLayersResponse>(
        &API_HANDLER_PCB::handleSetBoardEnabledLayers );
    registerHandler<GetGraphicsDefaults, GraphicsDefaultsResponse>(
            &API_HANDLER_PCB::handleGetGraphicsDefaults );
    registerHandler<GetBoundingBox, GetBoundingBoxResponse>(
            &API_HANDLER_PCB::handleGetBoundingBox );
    registerHandler<GetPadShapeAsPolygon, PadShapeAsPolygonResponse>(
            &API_HANDLER_PCB::handleGetPadShapeAsPolygon );
    registerHandler<CheckPadstackPresenceOnLayers, PadstackPresenceResponse>(
            &API_HANDLER_PCB::handleCheckPadstackPresenceOnLayers );
    registerHandler<GetTitleBlockInfo, types::TitleBlockInfo>(
            &API_HANDLER_PCB::handleGetTitleBlockInfo );
    registerHandler<SetTitleBlockInfo, Empty>( &API_HANDLER_PCB::handleSetTitleBlockInfo );
    registerHandler<ExpandTextVariables, ExpandTextVariablesResponse>(
            &API_HANDLER_PCB::handleExpandTextVariables );
    registerHandler<GetBoardOrigin, types::Vector2>( &API_HANDLER_PCB::handleGetBoardOrigin );
    registerHandler<SetBoardOrigin, Empty>( &API_HANDLER_PCB::handleSetBoardOrigin );
    registerHandler<GetBoardLayerName, BoardLayerNameResponse>( &API_HANDLER_PCB::handleGetBoardLayerName );

    registerHandler<InteractiveMoveItems, Empty>( &API_HANDLER_PCB::handleInteractiveMoveItems );
    registerHandler<GetNets, NetsResponse>( &API_HANDLER_PCB::handleGetNets );
    registerHandler<GetConnectedItems, GetItemsResponse>( &API_HANDLER_PCB::handleGetConnectedItems );
    registerHandler<GetItemsByNet, GetItemsResponse>( &API_HANDLER_PCB::handleGetItemsByNet );
    registerHandler<GetItemsByNetClass, GetItemsResponse>( &API_HANDLER_PCB::handleGetItemsByNetClass );
    registerHandler<GetNetClassForNets, NetClassForNetsResponse>(
            &API_HANDLER_PCB::handleGetNetClassForNets );
    registerHandler<RefillZones, Empty>( &API_HANDLER_PCB::handleRefillZones );

    registerHandler<SaveDocumentToString, SavedDocumentResponse>(
            &API_HANDLER_PCB::handleSaveDocumentToString );
    registerHandler<SaveSelectionToString, SavedSelectionResponse>(
            &API_HANDLER_PCB::handleSaveSelectionToString );
    registerHandler<ParseAndCreateItemsFromString, CreateItemsResponse>(
            &API_HANDLER_PCB::handleParseAndCreateItemsFromString );
    registerHandler<GetVisibleLayers, BoardLayers>( &API_HANDLER_PCB::handleGetVisibleLayers );
    registerHandler<SetVisibleLayers, Empty>( &API_HANDLER_PCB::handleSetVisibleLayers );
    registerHandler<GetActiveLayer, BoardLayerResponse>( &API_HANDLER_PCB::handleGetActiveLayer );
    registerHandler<SetActiveLayer, Empty>( &API_HANDLER_PCB::handleSetActiveLayer );
    registerHandler<GetBoardEditorAppearanceSettings, BoardEditorAppearanceSettings>(
            &API_HANDLER_PCB::handleGetBoardEditorAppearanceSettings );
    registerHandler<SetBoardEditorAppearanceSettings, Empty>(
            &API_HANDLER_PCB::handleSetBoardEditorAppearanceSettings );
    registerHandler<InjectDrcError, InjectDrcErrorResponse>(
            &API_HANDLER_PCB::handleInjectDrcError );
}


PCB_EDIT_FRAME* API_HANDLER_PCB::frame() const
{
    return static_cast<PCB_EDIT_FRAME*>( m_frame );
}


HANDLER_RESULT<RunActionResponse> API_HANDLER_PCB::handleRunAction(
        const HANDLER_CONTEXT<RunAction>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    RunActionResponse response;

    if( frame()->GetToolManager()->RunAction( aCtx.Request.action(), true ) )
        response.set_status( RunActionStatus::RAS_OK );
    else
        response.set_status( RunActionStatus::RAS_INVALID );

    return response;
}


HANDLER_RESULT<GetOpenDocumentsResponse> API_HANDLER_PCB::handleGetOpenDocuments(
        const HANDLER_CONTEXT<GetOpenDocuments>& aCtx )
{
    if( aCtx.Request.type() != DocumentType::DOCTYPE_PCB )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetOpenDocumentsResponse response;
    common::types::DocumentSpecifier doc;

    wxFileName fn( frame()->GetCurrentFileName() );

    doc.set_type( DocumentType::DOCTYPE_PCB );
    doc.set_board_filename( fn.GetFullName() );

    doc.mutable_project()->set_name( frame()->Prj().GetProjectName().ToStdString() );
    doc.mutable_project()->set_path( frame()->Prj().GetProjectDirectory().ToStdString() );

    response.mutable_documents()->Add( std::move( doc ) );
    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSaveDocument(
        const HANDLER_CONTEXT<SaveDocument>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    frame()->SaveBoard();
    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSaveCopyOfDocument(
        const HANDLER_CONTEXT<SaveCopyOfDocument>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    wxFileName boardPath( frame()->Prj().AbsolutePath( wxString::FromUTF8( aCtx.Request.path() ) ) );

    if( !boardPath.IsOk() || !boardPath.IsDirWritable() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' could not be opened",
                                          boardPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    if( boardPath.FileExists()
        && ( !boardPath.IsFileWritable() || !aCtx.Request.options().overwrite() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' exists and cannot be overwritten",
                                          boardPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    if( boardPath.GetExt() != FILEEXT::KiCadPcbFileExtension )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' must have a kicad_pcb extension",
                                          boardPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    BOARD* board = frame()->GetBoard();

    if( board->GetFileName().Matches( boardPath.GetFullPath() ) )
    {
        frame()->SaveBoard();
        return Empty();
    }

    bool includeProject = true;

    if( aCtx.Request.has_options() )
        includeProject = aCtx.Request.options().include_project();

    frame()->SavePcbCopy( boardPath.GetFullPath(), includeProject, /* aHeadless = */ true );

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleRevertDocument(
        const HANDLER_CONTEXT<RevertDocument>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    wxFileName fn = frame()->Prj().AbsolutePath( frame()->GetBoard()->GetFileName() );

    frame()->GetScreen()->SetContentModified( false );
    frame()->ReleaseFile();
    frame()->OpenProjectFiles( std::vector<wxString>( 1, fn.GetFullPath() ), KICTL_REVERT );

    return Empty();
}


void API_HANDLER_PCB::pushCurrentCommit( const std::string& aClientName, const wxString& aMessage )
{
    API_HANDLER_EDITOR::pushCurrentCommit( aClientName, aMessage );
    frame()->Refresh();
}


std::unique_ptr<COMMIT> API_HANDLER_PCB::createCommit()
{
    return std::make_unique<BOARD_COMMIT>( frame() );
}


std::optional<BOARD_ITEM*> API_HANDLER_PCB::getItemById( const KIID& aId ) const
{
    BOARD_ITEM* item = frame()->GetBoard()->ResolveItem( aId, true );

    if( !item )
        return std::nullopt;

    return item;
}


bool API_HANDLER_PCB::validateDocumentInternal( const DocumentSpecifier& aDocument ) const
{
    if( aDocument.type() != DocumentType::DOCTYPE_PCB )
        return false;

    wxFileName fn( frame()->GetCurrentFileName() );
    return 0 == aDocument.board_filename().compare( fn.GetFullName() );
}


HANDLER_RESULT<std::unique_ptr<BOARD_ITEM>> API_HANDLER_PCB::createItemForType( KICAD_T aType,
        BOARD_ITEM_CONTAINER* aContainer )
{
    if( !aContainer )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Tried to create an item in a null container" );
        return tl::unexpected( e );
    }

    if( aType == PCB_PAD_T && !dynamic_cast<FOOTPRINT*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a pad in {}, which is not a footprint",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }
    else if( aType == PCB_FOOTPRINT_T && !dynamic_cast<BOARD*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a footprint in {}, which is not a board",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }

    std::unique_ptr<BOARD_ITEM> created = CreateItemForType( aType, aContainer );

    if( !created )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create an item of type {}, which is unhandled",
                                          magic_enum::enum_name( aType ) ) );
        return tl::unexpected( e );
    }

    return created;
}


HANDLER_RESULT<ItemRequestStatus> API_HANDLER_PCB::handleCreateUpdateItemsInternal( bool aCreate,
        const std::string& aClientName,
        const types::ItemHeader &aHeader,
        const google::protobuf::RepeatedPtrField<google::protobuf::Any>& aItems,
        std::function<void( ItemStatus, google::protobuf::Any )> aItemHandler )
{
    ApiResponseStatus e;

    auto containerResult = validateItemHeaderDocument( aHeader );

    if( !containerResult && containerResult.error().status() == ApiStatusCode::AS_UNHANDLED )
    {
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }
    else if( !containerResult )
    {
        e.CopyFrom( containerResult.error() );
        return tl::unexpected( e );
    }

    BOARD* board = frame()->GetBoard();
    BOARD_ITEM_CONTAINER* container = board;

    if( containerResult->has_value() )
    {
        const KIID& containerId = **containerResult;
        std::optional<BOARD_ITEM*> optItem = getItemById( containerId );

        if( optItem )
        {
            container = dynamic_cast<BOARD_ITEM_CONTAINER*>( *optItem );

            if( !container )
            {
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format(
                        "The requested container {} is not a valid board item container",
                        containerId.AsStdString() ) );
                return tl::unexpected( e );
            }
        }
        else
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format(
                    "The requested container {} does not exist in this document",
                    containerId.AsStdString() ) );
            return tl::unexpected( e );
        }
    }

    BOARD_COMMIT* commit = static_cast<BOARD_COMMIT*>( getCurrentCommit( aClientName ) );

    for( const google::protobuf::Any& anyItem : aItems )
    {
        ItemStatus status;
        std::optional<KICAD_T> type = TypeNameFromAny( anyItem );

        if( !type )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( fmt::format( "Could not decode a valid type from {}",
                                                   anyItem.type_url() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        if( type == PCB_DIMENSION_T )
        {
            board::types::Dimension dimension;
            anyItem.UnpackTo( &dimension );

            switch( dimension.dimension_style_case() )
            {
            case board::types::Dimension::kAligned:    type = PCB_DIM_ALIGNED_T;    break;
            case board::types::Dimension::kOrthogonal: type = PCB_DIM_ORTHOGONAL_T; break;
            case board::types::Dimension::kRadial:     type = PCB_DIM_RADIAL_T;     break;
            case board::types::Dimension::kLeader:     type = PCB_DIM_LEADER_T;     break;
            case board::types::Dimension::kCenter:     type = PCB_DIM_CENTER_T;     break;
            case board::types::Dimension::DIMENSION_STYLE_NOT_SET: break;
            }
        }

        google::protobuf::Any itemToDeserialize = anyItem;
        std::optional<BOARD_ITEM*> optItem;

        if( !aCreate )
        {
            std::optional<KIID> id = itemIdFromAny( anyItem );

            if( !id )
            {
                status.set_code( ItemStatusCode::ISC_INVALID_DATA );
                status.set_error_message( "an update item must contain a valid UUID" );
                aItemHandler( status, anyItem );
                continue;
            }

            optItem = getItemById( *id );

            if( !optItem )
            {
                status.set_code( ItemStatusCode::ISC_NONEXISTENT );
                status.set_error_message( fmt::format( "an item with UUID {} does not exist",
                                                       id->AsStdString() ) );
                aItemHandler( status, anyItem );
                continue;
            }

            if( *type == PCB_FOOTPRINT_T
                && isManagedFootprintUpdateMask( aHeader.field_mask() ) )
            {
                if( ( *optItem )->Type() != PCB_FOOTPRINT_T )
                {
                    status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
                    status.set_error_message(
                            "the footprint update UUID belongs to a different board item type" );
                    aItemHandler( status, anyItem );
                    continue;
                }

                board::types::FootprintInstance update;

                if( !anyItem.UnpackTo( &update ) )
                {
                    status.set_code( ItemStatusCode::ISC_INVALID_DATA );
                    status.set_error_message( "the footprint transform could not be decoded" );
                    aItemHandler( status, anyItem );
                    continue;
                }

                board::types::FootprintInstance mergedUpdate = update;
                const bool presentationRequested = std::any_of(
                        aHeader.field_mask().paths().begin(),
                        aHeader.field_mask().paths().end(),
                        []( const std::string& aPath )
                        {
                            return isFootprintPresentationPath( aPath );
                        } );

                if( presentationRequested )
                {
                    google::protobuf::Any existingItem;
                    ( *optItem )->Serialize( existingItem );
                    google::protobuf::Any mergedItem;
                    std::string mergeError;

                    if( !mergeItemUpdate( anyItem, existingItem, aHeader.field_mask(),
                                          mergedItem, mergeError )
                        || !mergedItem.UnpackTo( &mergedUpdate ) )
                    {
                        status.set_code( ItemStatusCode::ISC_INVALID_DATA );
                        status.set_error_message(
                                mergeError.empty()
                                        ? "the footprint presentation could not be merged"
                                        : mergeError );
                        aItemHandler( status, anyItem );
                        continue;
                    }
                }

                bool valid = true;
                std::string invalidPath;
                std::map<std::string, std::string> requestedFields;

                for( const std::string& path : aHeader.field_mask().paths() )
                {
                    if( path == "position" )
                    {
                        valid = update.has_position()
                                && update.position().x_nm() >= std::numeric_limits<int>::min()
                                && update.position().x_nm() <= std::numeric_limits<int>::max()
                                && update.position().y_nm() >= std::numeric_limits<int>::min()
                                && update.position().y_nm() <= std::numeric_limits<int>::max();
                    }
                    else if( path == "orientation" )
                    {
                        valid = update.has_orientation()
                                && std::isfinite( update.orientation().value_degrees() )
                                && std::abs( update.orientation().value_degrees() ) <= 360000.0;
                    }
                    else if( path == "layer" )
                    {
                        valid = update.layer() == board::types::BoardLayer::BL_F_Cu
                                || update.layer() == board::types::BoardLayer::BL_B_Cu;
                    }
                    else if( path == "locked" )
                    {
                        valid = update.locked() == common::types::LockedState::LS_LOCKED
                                || update.locked() == common::types::LockedState::LS_UNLOCKED;
                    }
                    else if( path == "value_field" )
                    {
                        valid = update.has_value_field()
                                && !update.value_field().text().text().text().empty()
                                && update.value_field().text().text().text().size() <= 1024;
                    }
                    else if( path == "datasheet_field" )
                    {
                        valid = update.has_datasheet_field()
                                && update.datasheet_field().text().text().text().size() <= 4096;
                    }
                    else if( path == "description_field" )
                    {
                        valid = update.has_description_field()
                                && update.description_field().text().text().text().size() <= 4096;
                    }
                    else if( path == "attributes.do_not_populate" )
                    {
                        valid = update.has_attributes();
                    }
                    else if( path == "definition.items" )
                    {
                        valid = update.has_definition()
                                && update.definition().items_size() <= 1024;
                        static const std::set<std::string> mandatory = {
                            "Reference", "Value", "Footprint", "Datasheet", "Description"
                        };

                        for( const google::protobuf::Any& packed : update.definition().items() )
                        {
                            board::types::Field field;

                            if( !valid || !packed.UnpackTo( &field ) || field.name().empty()
                                || field.name().size() > 128 || mandatory.contains( field.name() )
                                || !field.has_text()
                                || field.text().text().text().size() > 4096
                                || !requestedFields.emplace(
                                           field.name(), field.text().text().text() ).second )
                            {
                                valid = false;
                                break;
                            }
                        }
                    }
                    else if( isFootprintPresentationPath( path ) )
                    {
                        valid = true;
                    }
                    else
                    {
                        valid = false;
                    }

                    if( !valid )
                    {
                        invalidPath = path;
                        break;
                    }
                }

                if( valid && presentationRequested )
                {
                    const bool referenceRequested = std::any_of(
                            aHeader.field_mask().paths().begin(),
                            aHeader.field_mask().paths().end(),
                            []( const std::string& aPath )
                            {
                                return aPath.starts_with( "reference_field." );
                            } );
                    const bool valueRequested = std::any_of(
                            aHeader.field_mask().paths().begin(),
                            aHeader.field_mask().paths().end(),
                            []( const std::string& aPath )
                            {
                                return aPath.starts_with( "value_field." );
                            } );

                    if( referenceRequested
                        && !validFootprintPresentationField(
                                mergedUpdate.reference_field() ) )
                    {
                        valid = false;
                        invalidPath = "reference_field";
                    }
                    else if( valueRequested
                             && !validFootprintPresentationField(
                                     mergedUpdate.value_field() ) )
                    {
                        valid = false;
                        invalidPath = "value_field";
                    }
                }

                if( !valid )
                {
                    status.set_code( ItemStatusCode::ISC_INVALID_DATA );
                    status.set_error_message(
                            fmt::format(
                                    "managed footprint {} contains a missing or invalid field: {}",
                                    update.id().value(),
                                    invalidPath.empty() ? "unknown" : invalidPath ) );
                    aItemHandler( status, anyItem );
                    continue;
                }

                FOOTPRINT* footprint = static_cast<FOOTPRINT*>( *optItem );
                commit->Modify( footprint );

                if( google::protobuf::util::FieldMaskUtil::IsPathInFieldMask(
                            "layer", aHeader.field_mask() ) )
                {
                    footprint->SetLayerAndFlip(
                            FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>(
                                    update.layer() ) );
                }

                if( google::protobuf::util::FieldMaskUtil::IsPathInFieldMask(
                            "position", aHeader.field_mask() ) )
                {
                    footprint->SetPosition( VECTOR2I( update.position().x_nm(),
                                                      update.position().y_nm() ) );
                }

                if( google::protobuf::util::FieldMaskUtil::IsPathInFieldMask(
                            "orientation", aHeader.field_mask() ) )
                {
                    footprint->SetOrientationDegrees( update.orientation().value_degrees() );
                }

                if( google::protobuf::util::FieldMaskUtil::IsPathInFieldMask(
                            "locked", aHeader.field_mask() ) )
                {
                    footprint->SetLocked(
                            update.locked() == common::types::LockedState::LS_LOCKED );
                }

                if( google::protobuf::util::FieldMaskUtil::IsPathInFieldMask(
                            "value_field", aHeader.field_mask() ) )
                {
                    footprint->SetValue( wxString::FromUTF8(
                            update.value_field().text().text().text() ) );
                }

                if( google::protobuf::util::FieldMaskUtil::IsPathInFieldMask(
                            "datasheet_field", aHeader.field_mask() ) )
                {
                    footprint->GetField( FIELD_T::DATASHEET )->SetText(
                            wxString::FromUTF8(
                                    update.datasheet_field().text().text().text() ) );
                }

                if( google::protobuf::util::FieldMaskUtil::IsPathInFieldMask(
                            "description_field", aHeader.field_mask() ) )
                {
                    footprint->GetField( FIELD_T::DESCRIPTION )->SetText(
                            wxString::FromUTF8(
                                    update.description_field().text().text().text() ) );
                }

                if( google::protobuf::util::FieldMaskUtil::IsPathInFieldMask(
                            "attributes.do_not_populate", aHeader.field_mask() ) )
                {
                    footprint->SetDNP( update.attributes().do_not_populate() );
                }

                if( google::protobuf::util::FieldMaskUtil::IsPathInFieldMask(
                            "definition.items", aHeader.field_mask() ) )
                {
                    std::vector<PCB_FIELD*> existingFields;
                    footprint->GetFields( existingFields, false );

                    for( PCB_FIELD* field : existingFields )
                    {
                        if( field->IsMandatory()
                            || requestedFields.contains(
                                    std::string( field->GetName().ToUTF8() ) ) )
                        {
                            continue;
                        }

                        footprint->Remove( field );
                        delete field;
                    }

                    for( const auto& [name, value] : requestedFields )
                    {
                        const wxString nativeName = wxString::FromUTF8( name );
                        PCB_FIELD* field = footprint->HasField( nativeName )
                                                   ? footprint->GetField( nativeName )
                                                   : new PCB_FIELD( footprint, FIELD_T::USER,
                                                                    nativeName );

                        if( !footprint->HasField( nativeName ) )
                        {
                            footprint->Add( field );
                            field->StyleFromSettings( frame()->GetDesignSettings(), true );
                        }

                        field->SetText( wxString::FromUTF8( value ) );
                        field->SetVisible( false );
                        field->SetLayer( footprint->GetLayer() == F_Cu ? F_Fab : B_Fab );
                        field->SetPosition( footprint->GetPosition() );
                    }
                }

                const auto applyPresentation = [&]( const char* aPrefix,
                                                     const board::types::Field& aField,
                                                     PCB_FIELD& aNative )
                {
                    const bool requested = std::any_of(
                            aHeader.field_mask().paths().begin(),
                            aHeader.field_mask().paths().end(),
                            [&]( const std::string& aPath )
                            {
                                return aPath.starts_with( std::string( aPrefix ) + "." );
                            } );

                    if( !requested )
                        return true;

                    google::protobuf::Any packed;
                    packed.PackFrom( aField );
                    return aNative.Deserialize( packed );
                };

                if( !applyPresentation( "reference_field", mergedUpdate.reference_field(),
                                        footprint->Reference() )
                    || !applyPresentation( "value_field", mergedUpdate.value_field(),
                                           footprint->Value() ) )
                {
                    status.set_code( ItemStatusCode::ISC_INVALID_DATA );
                    status.set_error_message(
                            "the footprint presentation could not be applied" );
                    aItemHandler( status, anyItem );
                    continue;
                }

                status.set_code( ItemStatusCode::ISC_OK );
                google::protobuf::Any updatedItem;
                footprint->Serialize( updatedItem );
                aItemHandler( status, std::move( updatedItem ) );
                continue;
            }

            if( aHeader.field_mask().paths_size() > 0 )
            {
                google::protobuf::Any existingItem;
                ( *optItem )->Serialize( existingItem );
                std::string mergeError;

                if( !mergeItemUpdate( anyItem, existingItem, aHeader.field_mask(),
                                      itemToDeserialize, mergeError ) )
                {
                    status.set_code( ItemStatusCode::ISC_INVALID_DATA );
                    status.set_error_message( mergeError );
                    aItemHandler( status, anyItem );
                    continue;
                }
            }
        }

        HANDLER_RESULT<std::unique_ptr<BOARD_ITEM>> creationResult =
                createItemForType( *type, container );

        if( !creationResult )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( creationResult.error().error_message() );
            aItemHandler( status, anyItem );
            continue;
        }

        std::unique_ptr<BOARD_ITEM> item( std::move( *creationResult ) );

        if( !item->Deserialize( itemToDeserialize ) )
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "could not unpack {} from request",
                                              item->GetClass().ToStdString() ) );
            return tl::unexpected( e );
        }

        if( aCreate )
            optItem = getItemById( item->m_Uuid );

        if( aCreate && optItem )
        {
            status.set_code( ItemStatusCode::ISC_EXISTING );
            status.set_error_message( fmt::format( "an item with UUID {} already exists",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }
        if( aCreate && !( board->GetEnabledLayers() & item->GetLayerSet() ).any() )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_DATA );
            status.set_error_message(
                "attempted to add item with no overlapping layers with the board" );
            aItemHandler( status, anyItem );
            continue;
        }

        status.set_code( ItemStatusCode::ISC_OK );
        google::protobuf::Any newItem;

        if( aCreate )
        {
            if( item->Type() == PCB_FOOTPRINT_T )
            {
                // Ensure children have unique identifiers; in case the API client created this new
                // footprint by cloning an existing one and only changing the parent UUID.
                item->RunOnChildren(
                        []( BOARD_ITEM* aChild )
                        {
                            const_cast<KIID&>( aChild->m_Uuid ) = KIID();
                        },
                        RECURSE );
            }

            item->Serialize( newItem );
            commit->Add( item.release() );
        }
        else
        {
            BOARD_ITEM* boardItem = *optItem;

            // Footprints can't be modified by CopyFrom at the moment because the commit system
            // doesn't currently know what to do with a footprint that has had its children
            // replaced with other children; which results in things like the view not having its
            // cached geometry for footprint children updated when you move a footprint around.
            // And also, groups are special because they can contain any item type, so we
            // can't use CopyFrom on them either.
            if( boardItem->Type() == PCB_FOOTPRINT_T  || boardItem->Type() == PCB_GROUP_T )
            {
                // Save group membership before removal, since Remove() severs the relationship
                PCB_GROUP* parentGroup = dynamic_cast<PCB_GROUP*>( boardItem->GetParentGroup() );

                commit->Remove( boardItem );
                item->Serialize( newItem );

                BOARD_ITEM* newBoardItem = item.release();
                commit->Add( newBoardItem );

                // Restore group membership for the newly added item
                if( parentGroup )
                    parentGroup->AddItem( newBoardItem );
            }
            else
            {
                commit->Modify( boardItem );
                boardItem->CopyFrom( item.get() );
                boardItem->Serialize( newItem );
            }
        }

        aItemHandler( status, newItem );
    }

    if( !m_activeClients.count( aClientName ) )
    {
        pushCurrentCommit( aClientName, aCreate ? _( "Created items via API" )
                                                : _( "Modified items via API" ) );
    }


    return ItemRequestStatus::IRS_OK;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetItems( const HANDLER_CONTEXT<GetItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetItemsResponse response;

    BOARD* board = frame()->GetBoard();
    std::vector<BOARD_ITEM*> items;
    std::set<KICAD_T> typesRequested, typesInserted;
    bool handledAnything = false;

    for( KICAD_T type : parseRequestedItemTypes( aCtx.Request.types() ) )
    {
        typesRequested.emplace( type );

        if( typesInserted.count( type ) )
            continue;

        switch( type )
        {
        case PCB_TRACE_T:
        case PCB_ARC_T:
        case PCB_VIA_T:
            handledAnything = true;
            std::copy( board->Tracks().begin(), board->Tracks().end(),
                       std::back_inserter( items ) );
            typesInserted.insert( { PCB_TRACE_T, PCB_ARC_T, PCB_VIA_T } );
            break;

        case PCB_PAD_T:
        {
            handledAnything = true;

            for( FOOTPRINT* fp : board->Footprints() )
            {
                std::copy( fp->Pads().begin(), fp->Pads().end(),
                           std::back_inserter( items ) );
            }

            typesInserted.insert( PCB_PAD_T );
            break;
        }

        case PCB_FOOTPRINT_T:
        {
            handledAnything = true;

            std::copy( board->Footprints().begin(), board->Footprints().end(),
                       std::back_inserter( items ) );

            typesInserted.insert( PCB_FOOTPRINT_T );
            break;
        }

        case PCB_SHAPE_T:
        case PCB_TEXT_T:
        case PCB_TEXTBOX_T:
        case PCB_TABLE_T:
        case PCB_BARCODE_T:
        case PCB_REFERENCE_IMAGE_T:
        {
            handledAnything = true;
            bool inserted = false;

            for( BOARD_ITEM* item : board->Drawings() )
            {
                if( item->Type() == type )
                {
                    items.emplace_back( item );
                    inserted = true;
                }
            }

            if( inserted )
                typesInserted.insert( type );

            break;
        }

        case PCB_TABLECELL_T:
        {
            handledAnything = true;

            for( BOARD_ITEM* item : board->Drawings() )
            {
                if( PCB_TABLE* table = dynamic_cast<PCB_TABLE*>( item ) )
                {
                    const std::vector<PCB_TABLECELL*> cells = table->GetCells();
                    std::copy( cells.begin(), cells.end(), std::back_inserter( items ) );
                }
            }

            typesInserted.insert( PCB_TABLECELL_T );
            break;
        }

        case PCB_DIMENSION_T:
        {
            handledAnything = true;
            bool inserted = false;

            for( BOARD_ITEM* item : board->Drawings() )
            {
                switch (item->Type()) {
                    case PCB_DIM_ALIGNED_T:
                    case PCB_DIM_CENTER_T:
                    case PCB_DIM_RADIAL_T:
                    case PCB_DIM_ORTHOGONAL_T:
                    case PCB_DIM_LEADER_T:
                        items.emplace_back( item );
                        inserted = true;
                        break;
                    default:
                        break;
                }
            }
            // we have to add the dimension subtypes to the requested to get them out
            typesRequested.insert( {PCB_DIM_ALIGNED_T, PCB_DIM_CENTER_T, PCB_DIM_RADIAL_T, PCB_DIM_ORTHOGONAL_T, PCB_DIM_LEADER_T } );

            if( inserted )
                typesInserted.insert( {PCB_DIM_ALIGNED_T, PCB_DIM_CENTER_T, PCB_DIM_RADIAL_T, PCB_DIM_ORTHOGONAL_T, PCB_DIM_LEADER_T } );

            break;
        }

        case PCB_ZONE_T:
        {
            handledAnything = true;

            std::copy( board->Zones().begin(), board->Zones().end(),
                       std::back_inserter( items ) );

            typesInserted.insert( PCB_ZONE_T );
            break;
        }

        case PCB_GROUP_T:
        {
            handledAnything = true;

            std::copy( board->Groups().begin(), board->Groups().end(),
                       std::back_inserter( items ) );

            typesInserted.insert( PCB_GROUP_T );
            break;
        }
        default:
            break;
        }
    }

    if( !handledAnything )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Board object" );
        return tl::unexpected( e );
    }

    for( const BOARD_ITEM* item : items )
    {
        if( !typesRequested.count( item->Type() ) )
            continue;

        google::protobuf::Any itemBuf;
        item->Serialize( itemBuf );
        response.mutable_items()->Add( std::move( itemBuf ) );
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetItemsById(
        const HANDLER_CONTEXT<GetItemsById>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetItemsResponse response;

    std::vector<BOARD_ITEM*> items;

    for( const kiapi::common::types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
            items.emplace_back( *item );
    }

    for( const BOARD_ITEM* item : items )
    {
        google::protobuf::Any itemBuf;
        item->Serialize( itemBuf );
        response.mutable_items()->Add( std::move( itemBuf ) );
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}

void API_HANDLER_PCB::deleteItemsInternal( std::map<KIID, ItemDeletionStatus>& aItemsToDelete,
                                           const std::string& aClientName )
{
    BOARD* board = frame()->GetBoard();
    std::vector<BOARD_ITEM*> validatedItems;

    for( std::pair<const KIID, ItemDeletionStatus> pair : aItemsToDelete )
    {
        if( BOARD_ITEM* item = board->ResolveItem( pair.first, true ) )
        {
            validatedItems.push_back( item );
            aItemsToDelete[pair.first] = ItemDeletionStatus::IDS_OK;
        }

        // Note: we don't currently support locking items from API modification, but here is where
        // to add it in the future (and return IDS_IMMUTABLE)
    }

    COMMIT* commit = getCurrentCommit( aClientName );

    for( BOARD_ITEM* item : validatedItems )
        commit->Remove( item );

    if( !m_activeClients.count( aClientName ) )
        pushCurrentCommit( aClientName, _( "Deleted items via API" ) );
}


std::optional<EDA_ITEM*> API_HANDLER_PCB::getItemFromDocument( const DocumentSpecifier& aDocument,
                                                               const KIID& aId )
{
    if( !validateDocument( aDocument ) )
        return std::nullopt;

    return getItemById( aId );
}


HANDLER_RESULT<SelectionResponse> API_HANDLER_PCB::handleGetSelection(
            const HANDLER_CONTEXT<GetSelection>& aCtx )
{
    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    std::set<KICAD_T> filter;

    for( KICAD_T type : parseRequestedItemTypes( aCtx.Request.types() ) )
        filter.insert( type );

    TOOL_MANAGER* mgr = frame()->GetToolManager();
    PCB_SELECTION_TOOL* selectionTool = mgr->GetTool<PCB_SELECTION_TOOL>();

    SelectionResponse response;

    for( EDA_ITEM* item : selectionTool->GetSelection() )
    {
        if( filter.empty() || filter.contains( item->Type() ) )
            item->Serialize( *response.add_items() );
    }

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleClearSelection(
        const HANDLER_CONTEXT<ClearSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    TOOL_MANAGER* mgr = frame()->GetToolManager();
    mgr->RunAction( ACTIONS::selectionClear );
    frame()->Refresh();

    return Empty();
}


HANDLER_RESULT<SelectionResponse> API_HANDLER_PCB::handleAddToSelection(
        const HANDLER_CONTEXT<AddToSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    TOOL_MANAGER* mgr = frame()->GetToolManager();
    PCB_SELECTION_TOOL* selectionTool = mgr->GetTool<PCB_SELECTION_TOOL>();

    std::vector<EDA_ITEM*> toAdd;

    for( const types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
            toAdd.emplace_back( *item );
    }

    selectionTool->AddItemsToSel( &toAdd );
    frame()->Refresh();

    SelectionResponse response;

    for( EDA_ITEM* item : selectionTool->GetSelection() )
        item->Serialize( *response.add_items() );

    return response;
}


HANDLER_RESULT<SelectionResponse> API_HANDLER_PCB::handleRemoveFromSelection(
        const HANDLER_CONTEXT<RemoveFromSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    TOOL_MANAGER* mgr = frame()->GetToolManager();
    PCB_SELECTION_TOOL* selectionTool = mgr->GetTool<PCB_SELECTION_TOOL>();

    std::vector<EDA_ITEM*> toRemove;

    for( const types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
            toRemove.emplace_back( *item );
    }

    selectionTool->RemoveItemsFromSel( &toRemove );
    frame()->Refresh();

    SelectionResponse response;

    for( EDA_ITEM* item : selectionTool->GetSelection() )
        item->Serialize( *response.add_items() );

    return response;
}


HANDLER_RESULT<BoardStackupResponse> API_HANDLER_PCB::handleGetStackup(
        const HANDLER_CONTEXT<GetBoardStackup>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BoardStackupResponse  response;
    google::protobuf::Any any;

    frame()->GetBoard()->GetStackupOrDefault().Serialize( any );

    any.UnpackTo( response.mutable_stackup() );

    // User-settable layer names are not stored in BOARD_STACKUP at the moment
    for( board::BoardStackupLayer& layer : *response.mutable_stackup()->mutable_layers() )
    {
        if( layer.type() == board::BoardStackupLayerType::BSLT_DIELECTRIC )
            continue;

        PCB_LAYER_ID id = FromProtoEnum<PCB_LAYER_ID>( layer.layer() );
        wxCHECK2( id != UNDEFINED_LAYER, continue );

        layer.set_user_name( frame()->GetBoard()->GetLayerName( id ) );
    }

    return response;
}


HANDLER_RESULT<BoardStackupResponse> API_HANDLER_PCB::handleUpdateStackup(
        const HANDLER_CONTEXT<UpdateBoardStackup>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    google::protobuf::Any serialized;
    serialized.PackFrom( aCtx.Request.stackup() );
    BOARD_STACKUP requested;

    if( !requested.Deserialize( serialized ) )
    {
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_BAD_REQUEST );
        error.set_error_message( "stackup is incomplete, inconsistent, or outside native bounds" );
        return tl::unexpected( error );
    }

    BOARD* pcbBoard = frame()->GetBoard();
    LSET   previousEnabled = pcbBoard->GetEnabledLayers();
    LSET   enabled = previousEnabled & ~BOARD_STACKUP::StackupAllowedBrdLayers();
    int    copperLayerCount = 0;

    // user_name is authoritative response context, not stackup mutation input.  Ignore it here so
    // an exact GetBoardStackup response can always be used as a transactional rollback snapshot;
    // the normalized response below exposes the board's actual names after the update.

    for( const BOARD_STACKUP_ITEM* item : requested.GetList() )
    {
        if( item->GetBrdLayerId() != UNDEFINED_LAYER )
        {
            enabled.set( item->GetBrdLayerId() );

            if( IsCopperLayer( item->GetBrdLayerId() ) )
                ++copperLayerCount;
        }
    }

    LSET removed = previousEnabled & ~enabled;

    for( PCB_LAYER_ID layer : removed )
    {
        if( IsCopperLayer( layer ) && pcbBoard->HasItemsOnLayer( layer ) )
        {
            ApiResponseStatus error;
            error.set_status( ApiStatusCode::AS_BAD_REQUEST );
            error.set_error_message(
                    fmt::format( "stackup cannot disable non-empty layer {}",
                                 pcbBoard->GetLayerName( layer ).ToStdString() ) );
            return tl::unexpected( error );
        }
    }

    BOARD_DESIGN_SETTINGS& settings = pcbBoard->GetDesignSettings();
    const BOARD_STACKUP    previousStackup = pcbBoard->GetStackupOrDefault();
    const int              thickness = requested.BuildBoardThicknessFromStackup();
    const bool             modified = requested != previousStackup
                                      || enabled != previousEnabled
                                      || thickness != settings.GetBoardThickness()
                                      || !settings.m_HasStackup;

    if( modified )
    {
        settings.GetStackupDescriptor() = requested;
        settings.SetBoardThickness( thickness );
        settings.m_HasStackup = true;
        pcbBoard->SetEnabledLayers( enabled );
        pcbBoard->SetCopperLayerCount( copperLayerCount );
        pcbBoard->SetVisibleLayers( pcbBoard->GetVisibleLayers() | ( enabled ^ previousEnabled ) );
        frame()->UpdateUserInterface();
        frame()->OnModify();
        frame()->Refresh();
    }

    BoardStackupResponse response;
    google::protobuf::Any normalized;
    pcbBoard->GetStackupOrDefault().Serialize( normalized );
    normalized.UnpackTo( response.mutable_stackup() );

    for( board::BoardStackupLayer& layer : *response.mutable_stackup()->mutable_layers() )
    {
        if( layer.type() == board::BoardStackupLayerType::BSLT_DIELECTRIC )
            continue;

        PCB_LAYER_ID id = FromProtoEnum<PCB_LAYER_ID>( layer.layer() );
        wxCHECK2( id != UNDEFINED_LAYER, continue );
        layer.set_user_name( pcbBoard->GetLayerName( id ).ToUTF8() );
    }

    return response;
}


HANDLER_RESULT<BoardDesignRulesResponse> API_HANDLER_PCB::handleGetBoardDesignRules(
        const HANDLER_CONTEXT<GetBoardDesignRules>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BoardDesignRulesResponse response;
    response.mutable_rules()->CopyFrom(
            encodeBoardDesignRules( frame()->GetBoard()->GetDesignSettings() ) );
    return response;
}


HANDLER_RESULT<BoardDesignRulesResponse> API_HANDLER_PCB::handleUpdateBoardDesignRules(
        const HANDLER_CONTEXT<UpdateBoardDesignRules>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    NATIVE_BOARD_RULES decoded = {};
    std::string        decodeError;

    if( !aCtx.Request.has_rules()
        || !decodeBoardDesignRules( aCtx.Request.rules(), decoded, decodeError ) )
    {
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_BAD_REQUEST );
        error.set_error_message( decodeError.empty() ? "board design rules are required"
                                                     : decodeError );
        return tl::unexpected( error );
    }

    BOARD_DESIGN_SETTINGS& settings = frame()->GetBoard()->GetDesignSettings();
    const bool modified =
            settings.m_MinClearance != decoded.minimumClearance
            || settings.m_MinConn != decoded.minimumConnectionWidth
            || settings.m_TrackMinWidth != decoded.minimumTrackWidth
            || settings.m_ViasMinAnnularWidth != decoded.minimumViaAnnularWidth
            || settings.m_ViasMinSize != decoded.minimumViaDiameter
            || settings.m_MinThroughDrill != decoded.minimumThroughHoleDiameter
            || settings.m_MicroViasMinSize != decoded.minimumMicroviaDiameter
            || settings.m_MicroViasMinDrill != decoded.minimumMicroviaDrill
            || settings.m_HoleToHoleMin != decoded.minimumHoleToHole
            || settings.m_HoleClearance != decoded.minimumCopperToHoleClearance
            || settings.m_SilkClearance != decoded.minimumSilkscreenClearance
            || settings.m_MinGrooveWidth != decoded.minimumGrooveWidth
            || settings.m_MinResolvedSpokes != decoded.minimumResolvedSpokes
            || settings.m_MinSilkTextHeight != decoded.minimumSilkscreenTextHeight
            || settings.m_MinSilkTextThickness != decoded.minimumSilkscreenTextThickness
            || settings.m_CopperEdgeClearance != decoded.minimumCopperToEdgeClearance
            || settings.m_UseHeightForLengthCalcs != decoded.useHeightForLengthCalculations
            || settings.m_MaxError != decoded.maximumError
            || settings.m_ZoneKeepExternalFillets != decoded.allowFilletsOutsideZoneOutline
            || settings.m_DRCSeverities != decoded.ruleSeverities;

    if( modified )
    {
        settings.m_MinClearance = decoded.minimumClearance;
        settings.m_MinConn = decoded.minimumConnectionWidth;
        settings.m_TrackMinWidth = decoded.minimumTrackWidth;
        settings.m_ViasMinAnnularWidth = decoded.minimumViaAnnularWidth;
        settings.m_ViasMinSize = decoded.minimumViaDiameter;
        settings.m_MinThroughDrill = decoded.minimumThroughHoleDiameter;
        settings.m_MicroViasMinSize = decoded.minimumMicroviaDiameter;
        settings.m_MicroViasMinDrill = decoded.minimumMicroviaDrill;
        settings.m_HoleToHoleMin = decoded.minimumHoleToHole;
        settings.m_HoleClearance = decoded.minimumCopperToHoleClearance;
        settings.m_SilkClearance = decoded.minimumSilkscreenClearance;
        settings.m_MinGrooveWidth = decoded.minimumGrooveWidth;
        settings.m_MinResolvedSpokes = decoded.minimumResolvedSpokes;
        settings.m_MinSilkTextHeight = decoded.minimumSilkscreenTextHeight;
        settings.m_MinSilkTextThickness = decoded.minimumSilkscreenTextThickness;
        settings.m_CopperEdgeClearance = decoded.minimumCopperToEdgeClearance;
        settings.m_UseHeightForLengthCalcs = decoded.useHeightForLengthCalculations;
        settings.m_MaxError = decoded.maximumError;
        settings.m_ZoneKeepExternalFillets = decoded.allowFilletsOutsideZoneOutline;
        settings.m_DRCSeverities = decoded.ruleSeverities;
        frame()->UpdateUserInterface();
        frame()->OnModify();
        frame()->Refresh();
    }

    BoardDesignRulesResponse response;
    response.mutable_rules()->CopyFrom( encodeBoardDesignRules( settings ) );
    return response;
}


HANDLER_RESULT<BoardCustomRulesResponse> API_HANDLER_PCB::handleGetBoardCustomRules(
        const HANDLER_CONTEXT<GetBoardCustomRules>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const wxFileName rulesPath( frame()->GetDesignRulesPath() );
    bool             present = false;
    std::string      source;
    std::string      readError;

    if( !readCustomRulesFile( rulesPath, present, source, readError ) )
    {
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_UNHANDLED );
        error.set_error_message( readError );
        return tl::unexpected( error );
    }

    BoardCustomRulesResponse response;
    response.set_present( present );
    response.set_source( source );
    return response;
}


HANDLER_RESULT<BoardCustomRulesResponse> API_HANDLER_PCB::handleUpdateBoardCustomRules(
        const HANDLER_CONTEXT<UpdateBoardCustomRules>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( ( aCtx.Request.present() && aCtx.Request.source().empty() )
        || ( !aCtx.Request.present() && !aCtx.Request.source().empty() ) )
    {
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_BAD_REQUEST );
        error.set_error_message(
                "custom-rules source must be non-empty exactly when present is true" );
        return tl::unexpected( error );
    }

    std::string validationError;

    if( aCtx.Request.present()
        && !validateCustomRulesSource( aCtx.Request.source(), validationError ) )
    {
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_BAD_REQUEST );
        error.set_error_message( validationError );
        return tl::unexpected( error );
    }

    const wxFileName rulesPath( frame()->GetDesignRulesPath() );
    bool             previousPresent = false;
    std::string      previousSource;
    std::string      fileError;

    if( !readCustomRulesFile( rulesPath, previousPresent, previousSource, fileError ) )
    {
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_UNHANDLED );
        error.set_error_message( fileError );
        return tl::unexpected( error );
    }

    if( previousPresent == aCtx.Request.present()
        && previousSource == aCtx.Request.source() )
    {
        BoardCustomRulesResponse response;
        response.set_present( previousPresent );
        response.set_source( previousSource );
        return response;
    }

    auto restorePrevious = [&]() -> std::string
    {
        std::string restoreError;

        if( previousPresent )
        {
            if( !writeCustomRulesAtomically( rulesPath, previousSource, restoreError ) )
                return restoreError;
        }
        else if( rulesPath.FileExists() && !wxRemoveFile( rulesPath.GetFullPath() ) )
        {
            return "could not remove the custom-rules file while restoring prior state";
        }

        try
        {
            frame()->GetBoard()->GetDesignSettings().m_DRCEngine->InitEngine( rulesPath );
        }
        catch( const PARSE_ERROR& error )
        {
            return std::string( "could not restore the prior DRC engine: " ) + error.what();
        }

        return {};
    };

    if( aCtx.Request.present() )
    {
        if( !writeCustomRulesAtomically( rulesPath, aCtx.Request.source(), fileError ) )
        {
            ApiResponseStatus error;
            error.set_status( ApiStatusCode::AS_UNHANDLED );
            error.set_error_message( fileError );
            return tl::unexpected( error );
        }
    }
    else if( rulesPath.FileExists() && !wxRemoveFile( rulesPath.GetFullPath() ) )
    {
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_UNHANDLED );
        error.set_error_message( "could not remove the board custom-rules file" );
        return tl::unexpected( error );
    }

    try
    {
        frame()->GetBoard()->GetDesignSettings().m_DRCEngine->InitEngine( rulesPath );
    }
    catch( const PARSE_ERROR& error )
    {
        const std::string restoreError = restorePrevious();
        ApiResponseStatus responseError;
        responseError.set_status( ApiStatusCode::AS_BAD_REQUEST );
        responseError.set_error_message( std::string( "custom rules did not compile: " )
                                         + error.what()
                                         + ( restoreError.empty()
                                                     ? ""
                                                     : "; rollback failed: " + restoreError ) );
        return tl::unexpected( responseError );
    }

    bool        installedPresent = false;
    std::string installedSource;

    if( !readCustomRulesFile( rulesPath, installedPresent, installedSource, fileError )
        || installedPresent != aCtx.Request.present()
        || installedSource != aCtx.Request.source() )
    {
        const std::string restoreError = restorePrevious();
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_UNHANDLED );
        error.set_error_message(
                "custom-rules verification failed after installation"
                + ( restoreError.empty() ? std::string() : "; rollback failed: " + restoreError ) );
        return tl::unexpected( error );
    }

    BoardCustomRulesResponse response;
    response.set_present( installedPresent );
    response.set_source( installedSource );
    return response;
}


HANDLER_RESULT<BoardEnabledLayersResponse> API_HANDLER_PCB::handleGetBoardEnabledLayers(
        const HANDLER_CONTEXT<GetBoardEnabledLayers>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BoardEnabledLayersResponse response;

    BOARD* board = frame()->GetBoard();
    int copperLayerCount = board->GetCopperLayerCount();

    response.set_copper_layer_count( copperLayerCount );

    LSET enabled = board->GetEnabledLayers();

    // The Rescue layer is an internal detail and should be hidden from the API
    enabled.reset( Rescue );

    // Just in case this is out of sync; the API should always return the expected copper layers
    enabled |= LSET::AllCuMask( copperLayerCount );

    board::PackLayerSet( *response.mutable_layers(), enabled );

    return response;
}


HANDLER_RESULT<BoardEnabledLayersResponse> API_HANDLER_PCB::handleSetBoardEnabledLayers(
        const HANDLER_CONTEXT<SetBoardEnabledLayers>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( aCtx.Request.copper_layer_count() < 2
        || aCtx.Request.copper_layer_count() % 2 != 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "copper_layer_count must be an even number of at least 2" );
        return tl::unexpected( e );
    }

    if( aCtx.Request.copper_layer_count() > MAX_CU_LAYERS )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "copper_layer_count must be below %d", MAX_CU_LAYERS ) );
        return tl::unexpected( e );
    }

    int copperLayerCount = static_cast<int>( aCtx.Request.copper_layer_count() );
    LSET enabled = board::UnpackLayerSet( aCtx.Request.layers() );

    // Sanitize the input
    enabled |= LSET( { Edge_Cuts, Margin, F_CrtYd, B_CrtYd } );
    enabled &= ~LSET::AllCuMask();
    enabled |= LSET::AllCuMask( copperLayerCount );

    BOARD* board = frame()->GetBoard();

    LSET previousEnabled = board->GetEnabledLayers();
    LSEQ removedLayers;

    for( PCB_LAYER_ID layer_id : previousEnabled )
    {
        if( !enabled[layer_id] && IsCopperLayer( layer_id )
            && board->HasItemsOnLayer( layer_id ) )
            removedLayers.push_back( layer_id );
    }

    if( !removedLayers.empty() )
    {
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_BAD_REQUEST );
        error.set_error_message(
                fmt::format( "cannot disable non-empty layer {}",
                             board->GetLayerName( removedLayers.front() ).ToStdString() ) );
        return tl::unexpected( error );
    }

    if( enabled != previousEnabled )
    {
        LSET changedLayers = enabled ^ previousEnabled;
        board->SetEnabledLayers( enabled );
        board->SetCopperLayerCount( copperLayerCount );
        board->SetVisibleLayers( board->GetVisibleLayers() | changedLayers );
        frame()->UpdateUserInterface();
        frame()->OnModify();
        frame()->Refresh();
    }

    BoardEnabledLayersResponse response;

    response.set_copper_layer_count( copperLayerCount );
    board::PackLayerSet( *response.mutable_layers(), enabled );

    return response;
}


HANDLER_RESULT<GraphicsDefaultsResponse> API_HANDLER_PCB::handleGetGraphicsDefaults(
        const HANDLER_CONTEXT<GetGraphicsDefaults>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const BOARD_DESIGN_SETTINGS& bds = frame()->GetBoard()->GetDesignSettings();
    GraphicsDefaultsResponse response;

    // TODO: This should change to be an enum class
    constexpr std::array<kiapi::board::BoardLayerClass, LAYER_CLASS_COUNT> classOrder = {
        kiapi::board::BLC_SILKSCREEN,
        kiapi::board::BLC_COPPER,
        kiapi::board::BLC_EDGES,
        kiapi::board::BLC_COURTYARD,
        kiapi::board::BLC_FABRICATION,
        kiapi::board::BLC_OTHER
    };

    for( int i = 0; i < LAYER_CLASS_COUNT; ++i )
    {
        kiapi::board::BoardLayerGraphicsDefaults* l = response.mutable_defaults()->add_layers();

        l->set_layer( classOrder[i] );
        l->mutable_line_thickness()->set_value_nm( bds.m_LineThickness[i] );

        kiapi::common::types::TextAttributes* text = l->mutable_text();
        text->mutable_size()->set_x_nm( bds.m_TextSize[i].x );
        text->mutable_size()->set_y_nm( bds.m_TextSize[i].y );
        text->mutable_stroke_width()->set_value_nm( bds.m_TextThickness[i] );
        text->set_italic( bds.m_TextItalic[i] );
        text->set_keep_upright( bds.m_TextUpright[i] );
    }

    return response;
}


HANDLER_RESULT<types::Vector2> API_HANDLER_PCB::handleGetBoardOrigin(
        const HANDLER_CONTEXT<GetBoardOrigin>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    VECTOR2I origin;
    const BOARD_DESIGN_SETTINGS& settings = frame()->GetBoard()->GetDesignSettings();

    switch( aCtx.Request.type() )
    {
    case BOT_GRID:
        origin = settings.GetGridOrigin();
        break;

    case BOT_DRILL:
        origin = settings.GetAuxOrigin();
        break;

    default:
    case BOT_UNKNOWN:
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Unexpected origin type" );
        return tl::unexpected( e );
    }
    }

    types::Vector2 reply;
    PackVector2( reply, origin );
    return reply;
}

HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetBoardOrigin(
        const HANDLER_CONTEXT<SetBoardOrigin>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    VECTOR2I origin = UnpackVector2( aCtx.Request.origin() );

    switch( aCtx.Request.type() )
    {
    case BOT_GRID:
    {
        PCB_EDIT_FRAME* f = frame();

        frame()->CallAfter( [f, origin]()
                            {
                                // gridSetOrigin takes ownership and frees this
                                VECTOR2D* dorigin = new VECTOR2D( origin );
                                TOOL_MANAGER* mgr = f->GetToolManager();
                                mgr->RunAction( PCB_ACTIONS::gridSetOrigin, dorigin );
                                f->Refresh();
                            } );
        break;
    }

    case BOT_DRILL:
    {
        PCB_EDIT_FRAME* f = frame();

        frame()->CallAfter( [f, origin]()
                            {
                                TOOL_MANAGER* mgr = f->GetToolManager();
                                mgr->RunAction( PCB_ACTIONS::drillSetOrigin, origin );
                                f->Refresh();
                            } );
        break;
    }

    default:
    case BOT_UNKNOWN:
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Unexpected origin type" );
        return tl::unexpected( e );
    }
    }

    return Empty();
}


HANDLER_RESULT<BoardLayerNameResponse> API_HANDLER_PCB::handleGetBoardLayerName(
            const HANDLER_CONTEXT<GetBoardLayerName>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    BoardLayerNameResponse response;

    PCB_LAYER_ID id = FromProtoEnum<PCB_LAYER_ID>( aCtx.Request.layer() );

    response.set_name( frame()->GetBoard()->GetLayerName( id ) );

    return response;
}


HANDLER_RESULT<GetBoundingBoxResponse> API_HANDLER_PCB::handleGetBoundingBox(
        const HANDLER_CONTEXT<GetBoundingBox>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetBoundingBoxResponse response;
    bool includeText = aCtx.Request.mode() == BoundingBoxMode::BBM_ITEM_AND_CHILD_TEXT;

    for( const types::KIID& idMsg : aCtx.Request.items() )
    {
        KIID id( idMsg.value() );
        std::optional<BOARD_ITEM*> optItem = getItemById( id );

        if( !optItem )
            continue;

        BOARD_ITEM* item = *optItem;
        BOX2I bbox;

        if( item->Type() == PCB_FOOTPRINT_T )
            bbox = static_cast<FOOTPRINT*>( item )->GetBoundingBox( includeText );
        else
            bbox = item->GetBoundingBox();

        response.add_items()->set_value( idMsg.value() );
        PackBox2( *response.add_boxes(), bbox );
    }

    return response;
}


HANDLER_RESULT<PadShapeAsPolygonResponse> API_HANDLER_PCB::handleGetPadShapeAsPolygon(
        const HANDLER_CONTEXT<GetPadShapeAsPolygon>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    PadShapeAsPolygonResponse response;
    PCB_LAYER_ID layer = FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( aCtx.Request.layer() );

    for( const types::KIID& padRequest : aCtx.Request.pads() )
    {
        KIID id( padRequest.value() );
        std::optional<BOARD_ITEM*> optPad = getItemById( id );

        if( !optPad || ( *optPad )->Type() != PCB_PAD_T )
            continue;

        response.add_pads()->set_value( padRequest.value() );

        PAD* pad = static_cast<PAD*>( *optPad );
        SHAPE_POLY_SET poly;
        pad->TransformShapeToPolygon( poly, pad->Padstack().EffectiveLayerFor( layer ), 0,
                                      pad->GetMaxError(), ERROR_INSIDE );

        types::PolygonWithHoles* polyMsg = response.mutable_polygons()->Add();
        PackPolyLine( *polyMsg->mutable_outline(), poly.COutline( 0 ) );
    }

    return response;
}


HANDLER_RESULT<PadstackPresenceResponse> API_HANDLER_PCB::handleCheckPadstackPresenceOnLayers(
        const HANDLER_CONTEXT<CheckPadstackPresenceOnLayers>& aCtx )
{
    using board::types::BoardLayer;

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    PadstackPresenceResponse response;

    LSET layers;

    for( const int layer : aCtx.Request.layers() )
        layers.set( FromProtoEnum<PCB_LAYER_ID, BoardLayer>( static_cast<BoardLayer>( layer ) ) );

    for( const types::KIID& padRequest : aCtx.Request.items() )
    {
        KIID id( padRequest.value() );
        std::optional<BOARD_ITEM*> optItem = getItemById( id );

        if( !optItem )
            continue;

        switch( ( *optItem )->Type() )
        {
        case PCB_PAD_T:
        {
            PAD* pad = static_cast<PAD*>( *optItem );

            for( PCB_LAYER_ID layer : layers )
            {
                PadstackPresenceEntry* entry = response.add_entries();
                entry->mutable_item()->set_value( pad->m_Uuid.AsStdString() );
                entry->set_layer( ToProtoEnum<PCB_LAYER_ID, BoardLayer>( layer ) );
                entry->set_presence( pad->FlashLayer( layer ) ? PSP_PRESENT : PSP_NOT_PRESENT );
            }

            break;
        }

        case PCB_VIA_T:
        {
            PCB_VIA* via = static_cast<PCB_VIA*>( *optItem );

            for( PCB_LAYER_ID layer : layers )
            {
                PadstackPresenceEntry* entry = response.add_entries();
                entry->mutable_item()->set_value( via->m_Uuid.AsStdString() );
                entry->set_layer( ToProtoEnum<PCB_LAYER_ID, BoardLayer>( layer ) );
                entry->set_presence( via->FlashLayer( layer ) ? PSP_PRESENT : PSP_NOT_PRESENT );
            }

            break;
        }

        default:
            break;
        }
    }

    return response;
}


HANDLER_RESULT<types::TitleBlockInfo> API_HANDLER_PCB::handleGetTitleBlockInfo(
        const HANDLER_CONTEXT<GetTitleBlockInfo>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = frame()->GetBoard();
    const TITLE_BLOCK& block = board->GetTitleBlock();

    types::TitleBlockInfo response;

    response.set_title( block.GetTitle().ToUTF8() );
    response.set_date( block.GetDate().ToUTF8() );
    response.set_revision( block.GetRevision().ToUTF8() );
    response.set_company( block.GetCompany().ToUTF8() );
    response.set_comment1( block.GetComment( 0 ).ToUTF8() );
    response.set_comment2( block.GetComment( 1 ).ToUTF8() );
    response.set_comment3( block.GetComment( 2 ).ToUTF8() );
    response.set_comment4( block.GetComment( 3 ).ToUTF8() );
    response.set_comment5( block.GetComment( 4 ).ToUTF8() );
    response.set_comment6( block.GetComment( 5 ).ToUTF8() );
    response.set_comment7( block.GetComment( 6 ).ToUTF8() );
    response.set_comment8( block.GetComment( 7 ).ToUTF8() );
    response.set_comment9( block.GetComment( 8 ).ToUTF8() );

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetTitleBlockInfo( const HANDLER_CONTEXT<SetTitleBlockInfo>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( !aCtx.Request.has_title_block() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "SetTitleBlockInfo requires title_block" );
        return tl::unexpected( e );
    }

    TITLE_BLOCK& block = frame()->GetBoard()->GetTitleBlock();

    const types::TitleBlockInfo& request = aCtx.Request.title_block();

    block.SetTitle( wxString::FromUTF8( request.title() ) );
    block.SetDate( wxString::FromUTF8( request.date() ) );
    block.SetRevision( wxString::FromUTF8( request.revision() ) );
    block.SetCompany( wxString::FromUTF8( request.company() ) );
    block.SetComment( 0, wxString::FromUTF8( request.comment1() ) );
    block.SetComment( 1, wxString::FromUTF8( request.comment2() ) );
    block.SetComment( 2, wxString::FromUTF8( request.comment3() ) );
    block.SetComment( 3, wxString::FromUTF8( request.comment4() ) );
    block.SetComment( 4, wxString::FromUTF8( request.comment5() ) );
    block.SetComment( 5, wxString::FromUTF8( request.comment6() ) );
    block.SetComment( 6, wxString::FromUTF8( request.comment7() ) );
    block.SetComment( 7, wxString::FromUTF8( request.comment8() ) );
    block.SetComment( 8, wxString::FromUTF8( request.comment9() ) );

    if( frame() )
    {
        frame()->OnModify();
        frame()->UpdateUserInterface();
    }

    return Empty();
}


HANDLER_RESULT<ExpandTextVariablesResponse> API_HANDLER_PCB::handleExpandTextVariables(
    const HANDLER_CONTEXT<ExpandTextVariables>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    ExpandTextVariablesResponse reply;
    BOARD* board = frame()->GetBoard();

    std::function<bool( wxString* )> textResolver =
            [&]( wxString* token ) -> bool
            {
                // Handles m_board->GetTitleBlock() *and* m_board->GetProject()
                return board->ResolveTextVar( token, 0 );
            };

    for( const std::string& textMsg : aCtx.Request.text() )
    {
        wxString text = ExpandTextVars( wxString::FromUTF8( textMsg ), &textResolver );
        reply.add_text( text.ToUTF8() );
    }

    return reply;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleInteractiveMoveItems(
        const HANDLER_CONTEXT<InteractiveMoveItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    TOOL_MANAGER* mgr = frame()->GetToolManager();
    std::vector<EDA_ITEM*> toSelect;

    for( const kiapi::common::types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
            toSelect.emplace_back( static_cast<EDA_ITEM*>( *item ) );
    }

    if( toSelect.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "None of the given items exist on the board",
                                          aCtx.Request.board().board_filename() ) );
        return tl::unexpected( e );
    }

    PCB_SELECTION_TOOL* selectionTool = mgr->GetTool<PCB_SELECTION_TOOL>();
    selectionTool->GetSelection().SetReferencePoint( toSelect[0]->GetPosition() );

    mgr->RunAction( ACTIONS::selectionClear );
    mgr->RunAction<EDA_ITEMS*>( ACTIONS::selectItems, &toSelect );

    COMMIT* commit = getCurrentCommit( aCtx.ClientName );
    mgr->PostAPIAction( PCB_ACTIONS::move, commit );

    return Empty();
}


HANDLER_RESULT<NetsResponse> API_HANDLER_PCB::handleGetNets( const HANDLER_CONTEXT<GetNets>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    NetsResponse response;
    BOARD* board = frame()->GetBoard();

    std::set<wxString> netclassFilter;

    for( const std::string& nc : aCtx.Request.netclass_filter() )
        netclassFilter.insert( wxString( nc.c_str(), wxConvUTF8 ) );

    for( NETINFO_ITEM* net : board->GetNetInfo() )
    {
        NETCLASS* nc = net->GetNetClass();

        if( !netclassFilter.empty() && nc )
        {
            bool inClass = false;

            for( const wxString& filter : netclassFilter )
            {
                if( nc->ContainsNetclassWithName( filter ) )
                {
                    inClass = true;
                    break;
                }
            }

            if( !inClass )
                continue;
        }

        board::types::Net* netProto = response.add_nets();
        netProto->set_name( net->GetNetname() );
        netProto->mutable_code()->set_value( net->GetNetCode() );
    }

    return response;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetConnectedItems(
        const HANDLER_CONTEXT<GetConnectedItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    std::vector<KICAD_T> types = parseRequestedItemTypes( aCtx.Request.types() );
    const bool filterByType = aCtx.Request.types_size() > 0;

    if( filterByType && types.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Board object" );
        return tl::unexpected( e );
    }

    std::set<KICAD_T> typeFilter( types.begin(), types.end() );
    std::vector<BOARD_CONNECTED_ITEM*> sourceItems;

    for( const types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
        {
            if( BOARD_CONNECTED_ITEM* connected = dynamic_cast<BOARD_CONNECTED_ITEM*>( *item ) )
                sourceItems.emplace_back( connected );
        }
    }

    if( sourceItems.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested IDs were found or valid connected items" );
        return tl::unexpected( e );
    }

    GetItemsResponse response;
    std::shared_ptr<CONNECTIVITY_DATA> conn = frame()->GetBoard()->GetConnectivity();
    std::set<KIID> insertedItems;

    for( BOARD_CONNECTED_ITEM* source : sourceItems )
    {
        for( BOARD_CONNECTED_ITEM* connected : conn->GetConnectedItems( source ) )
        {
            if( filterByType && !typeFilter.contains( connected->Type() ) )
                continue;

            if( !insertedItems.insert( connected->m_Uuid ).second )
                continue;

            connected->Serialize( *response.add_items() );
        }
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


std::vector<KICAD_T> API_HANDLER_PCB::parseRequestedItemTypes( const google::protobuf::RepeatedField<int>& aTypes )
{
    std::vector<KICAD_T> types;

    for( int typeRaw : aTypes )
    {
        auto typeMessage = static_cast<common::types::KiCadObjectType>( typeRaw );
        KICAD_T type = FromProtoEnum<KICAD_T>( typeMessage );

        if( type != TYPE_NOT_INIT )
            types.emplace_back( type );
    }

    return types;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetItemsByNet(
        const HANDLER_CONTEXT<GetItemsByNet>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    std::vector<KICAD_T> types = parseRequestedItemTypes( aCtx.Request.types() );
    const bool filterByType = aCtx.Request.types_size() > 0;

    if( filterByType && types.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Board object" );
        return tl::unexpected( e );
    }

    if( !filterByType )
        types.assign( { PCB_PAD_T, PCB_VIA_T, PCB_TRACE_T, PCB_ARC_T, PCB_SHAPE_T, PCB_ZONE_T } );

    GetItemsResponse response;
    BOARD* board = frame()->GetBoard();
    std::shared_ptr<CONNECTIVITY_DATA> conn = board->GetConnectivity();
    std::set<KIID> insertedItems;

    const NETINFO_LIST& nets = board->GetNetInfo();

    for( const board::types::Net& net : aCtx.Request.nets() )
    {
        NETINFO_ITEM* netInfo = nets.GetNetItem( wxString::FromUTF8( net.name() ) );

        if( !netInfo )
            continue;

        for( BOARD_CONNECTED_ITEM* item : conn->GetNetItems( netInfo->GetNetCode(), types ) )
        {
            if( !insertedItems.insert( item->m_Uuid ).second )
                continue;

            item->Serialize( *response.add_items() );
        }
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetItemsByNetClass(
        const HANDLER_CONTEXT<GetItemsByNetClass>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    std::vector<KICAD_T> types = parseRequestedItemTypes( aCtx.Request.types() );
    const bool filterByType = aCtx.Request.types_size() > 0;

    if( filterByType && types.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Board object" );
        return tl::unexpected( e );
    }

    if( !filterByType )
        types.assign( { PCB_PAD_T, PCB_VIA_T, PCB_TRACE_T, PCB_ARC_T, PCB_SHAPE_T, PCB_ZONE_T } );

    std::set<wxString> requestedClasses;

    for( const std::string& netClass : aCtx.Request.net_classes() )
        requestedClasses.insert( wxString( netClass.c_str(), wxConvUTF8 ) );

    GetItemsResponse response;
    BOARD* board = frame()->GetBoard();
    std::shared_ptr<CONNECTIVITY_DATA> conn = board->GetConnectivity();
    std::set<KIID> insertedItems;

    for( NETINFO_ITEM* net : board->GetNetInfo() )
    {
        if( !net )
            continue;

        NETCLASS* nc = net->GetNetClass();

        if( !requestedClasses.empty() )
        {
            if( !nc )
                continue;

            bool inClass = false;

            for( const wxString& filter : requestedClasses )
            {
                if( nc->ContainsNetclassWithName( filter ) )
                {
                    inClass = true;
                    break;
                }
            }

            if( !inClass )
                continue;
        }

        for( BOARD_CONNECTED_ITEM* item : conn->GetNetItems( net->GetNetCode(), types ) )
        {
            if( !insertedItems.insert( item->m_Uuid ).second )
                continue;

            item->Serialize( *response.add_items() );
        }
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<NetClassForNetsResponse> API_HANDLER_PCB::handleGetNetClassForNets(
            const HANDLER_CONTEXT<GetNetClassForNets>& aCtx )
{
    NetClassForNetsResponse response;

    BOARD* board = frame()->GetBoard();
    const NETINFO_LIST& nets = board->GetNetInfo();
    google::protobuf::Any any;

    for( const board::types::Net& net : aCtx.Request.net() )
    {
        NETINFO_ITEM* netInfo = nets.GetNetItem( wxString::FromUTF8( net.name() ) );

        if( !netInfo )
            continue;

        netInfo->GetNetClass()->Serialize( any );
        auto [pair, rc] = response.mutable_classes()->insert( { net.name(), {} } );
        any.UnpackTo( &pair->second );
    }

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleRefillZones( const HANDLER_CONTEXT<RefillZones>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( aCtx.Request.zones().empty() )
    {
        TOOL_MANAGER* mgr = frame()->GetToolManager();

        // The fill action is queued because API requests already run on the editor thread.  Mark
        // copper zones pending before returning so clients cannot observe an old filled=true and
        // enqueue another fill while this one has not started yet.
        for( ZONE* zone : frame()->GetBoard()->Zones() )
        {
            if( !zone->GetIsRuleArea() )
                zone->SetIsFilled( false );
        }

        frame()->CallAfter( [mgr]()
                            {
                                mgr->RunAction( PCB_ACTIONS::zoneFillAll );
                            } );
    }
    else
    {
        // TODO
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        return tl::unexpected( e );
    }

    return Empty();
}


HANDLER_RESULT<SavedDocumentResponse> API_HANDLER_PCB::handleSaveDocumentToString(
        const HANDLER_CONTEXT<SaveDocumentToString>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SavedDocumentResponse response;
    response.mutable_document()->CopyFrom( aCtx.Request.document() );

    CLIPBOARD_IO io;
    io.SetWriter(
        [&]( const wxString& aData )
        {
            response.set_contents( aData.ToUTF8() );
        } );

    io.SaveBoard( wxEmptyString, frame()->GetBoard(), nullptr );

    return response;
}


HANDLER_RESULT<SavedSelectionResponse> API_HANDLER_PCB::handleSaveSelectionToString(
        const HANDLER_CONTEXT<SaveSelectionToString>& aCtx )
{
    SavedSelectionResponse response;

    TOOL_MANAGER* mgr = frame()->GetToolManager();
    PCB_SELECTION_TOOL* selectionTool = mgr->GetTool<PCB_SELECTION_TOOL>();
    PCB_SELECTION& selection = selectionTool->GetSelection();

    CLIPBOARD_IO io;
    io.SetWriter(
        [&]( const wxString& aData )
        {
            response.set_contents( aData.ToUTF8() );
        } );

    io.SetBoard( frame()->GetBoard() );
    io.SaveSelection( selection, false );

    return response;
}


HANDLER_RESULT<CreateItemsResponse> API_HANDLER_PCB::handleParseAndCreateItemsFromString(
        const HANDLER_CONTEXT<ParseAndCreateItemsFromString>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( aCtx.Request.contents().empty()
        || aCtx.Request.contents().size() > MAX_PARSED_ITEMS_BYTES
        || aCtx.Request.contents().find( '\0' ) != std::string::npos )
    {
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_BAD_REQUEST );
        error.set_error_message(
                "parsed PCB items must contain 1 byte to 4 MiB of UTF-8 text" );
        return tl::unexpected( error );
    }

    const wxString decoded = wxString::FromUTF8( aCtx.Request.contents().data(),
                                                  aCtx.Request.contents().size() );
    const wxScopedCharBuffer reencoded = decoded.ToUTF8();

    if( reencoded.length() != aCtx.Request.contents().size()
        || std::memcmp( reencoded.data(), aCtx.Request.contents().data(),
                        aCtx.Request.contents().size() ) != 0 )
    {
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_BAD_REQUEST );
        error.set_error_message( "parsed PCB items must be valid UTF-8" );
        return tl::unexpected( error );
    }

    std::unique_ptr<BOARD_ITEM> parsed;

    try
    {
        PCB_IO_KICAD_SEXPR parser;
        parsed.reset( parser.Parse( decoded ) );
    }
    catch( const std::exception& error )
    {
        ApiResponseStatus responseError;
        responseError.set_status( ApiStatusCode::AS_BAD_REQUEST );
        responseError.set_error_message( std::string( "could not parse PCB items: " )
                                         + error.what() );
        return tl::unexpected( responseError );
    }

    if( !parsed || parsed->Type() != PCB_FOOTPRINT_T )
    {
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_BAD_REQUEST );
        error.set_error_message(
                "PCB item parsing currently requires exactly one KiCad footprint" );
        return tl::unexpected( error );
    }

    BOARD* board = frame()->GetBoard();
    FOOTPRINT* footprint = static_cast<FOOTPRINT*>( parsed.get() );
    footprint->SetParent( board );
    footprint->ResolveComponentClassNames( board,
                                           footprint->GetTransientComponentClassNames() );
    footprint->ClearTransientComponentClassNames();
    std::string requestedId;
    std::optional<board::types::FootprintInstance> requestedMetadata;
    std::map<wxString, wxString> padNets;
    std::map<wxString, wxString> footprintFields;

    if( aCtx.Request.has_item() )
    {
        requestedMetadata.emplace();
        board::types::FootprintInstance& requested = *requestedMetadata;

        if( !aCtx.Request.item().UnpackTo( &requested )
            || ( !requested.id().value().empty()
                 && !KIID::SniffTest( wxString::FromUTF8( requested.id().value() ) ) )
            || !requested.has_definition() || !requested.definition().has_id()
            || requested.definition().id().library_nickname().empty()
            || requested.definition().id().entry_name().empty()
            || !requested.has_reference_field() || !requested.has_value_field()
            || requested.reference_field().text().text().text().empty()
            || requested.value_field().text().text().text().empty()
            || !requested.has_symbol_path() || requested.symbol_path().path_size() == 0
            || requested.symbol_path().path_size() > 64 || !requested.has_position()
            || requested.position().x_nm() < std::numeric_limits<int>::min()
            || requested.position().x_nm() > std::numeric_limits<int>::max()
            || requested.position().y_nm() < std::numeric_limits<int>::min()
            || requested.position().y_nm() > std::numeric_limits<int>::max()
            || !requested.has_orientation()
            || !std::isfinite( requested.orientation().value_degrees() )
            || std::abs( requested.orientation().value_degrees() ) > 360000.0
            || ( requested.layer() != board::types::BoardLayer::BL_F_Cu
                 && requested.layer() != board::types::BoardLayer::BL_B_Cu )
            || ( requested.locked() != common::types::LockedState::LS_LOCKED
                 && requested.locked() != common::types::LockedState::LS_UNLOCKED ) )
        {
            ApiResponseStatus error;
            error.set_status( ApiStatusCode::AS_BAD_REQUEST );
            error.set_error_message( "parsed footprint instance metadata is malformed" );
            return tl::unexpected( error );
        }

        if( aCtx.Request.field_mask().paths_size() > 0
            && ( !isManagedFootprintUpdateMask( aCtx.Request.field_mask() )
                 || std::any_of( aCtx.Request.field_mask().paths().begin(),
                                 aCtx.Request.field_mask().paths().end(),
                                 []( const std::string& aPath )
                                 {
                                     return !isFootprintPresentationPath( aPath );
                                 } ) ) )
        {
            ApiResponseStatus error;
            error.set_status( ApiStatusCode::AS_BAD_REQUEST );
            error.set_error_message(
                    "parsed footprint field mask accepts only Reference/Value presentation" );
            return tl::unexpected( error );
        }

        for( const common::types::KIID& id : requested.symbol_path().path() )
        {
            if( !KIID::SniffTest( wxString::FromUTF8( id.value() ) ) )
            {
                ApiResponseStatus error;
                error.set_status( ApiStatusCode::AS_BAD_REQUEST );
                error.set_error_message( "parsed footprint symbol path contains an invalid UUID" );
                return tl::unexpected( error );
            }
        }

        for( const google::protobuf::Any& packed : requested.definition().items() )
        {
            board::types::Pad pad;
            board::types::Field field;

            if( packed.UnpackTo( &pad ) )
            {
                if( pad.number().empty() || pad.number().size() > 64
                    || pad.net().name().empty() || pad.net().name().size() > 1024
                    || !pad.id().value().empty()
                    || !padNets.emplace( wxString::FromUTF8( pad.number() ),
                                         wxString::FromUTF8( pad.net().name() ) ).second )
                {
                    ApiResponseStatus error;
                    error.set_status( ApiStatusCode::AS_BAD_REQUEST );
                    error.set_error_message(
                            "parsed footprint pad-to-net mapping is malformed" );
                    return tl::unexpected( error );
                }

                continue;
            }

            if( packed.UnpackTo( &field ) )
            {
                if( field.name().empty() || field.name().size() > 128
                    || !field.has_text() || field.text().text().text().size() > 4096
                    || !footprintFields.emplace(
                               wxString::FromUTF8( field.name() ),
                               wxString::FromUTF8( field.text().text().text() ) ).second )
                {
                    ApiResponseStatus error;
                    error.set_status( ApiStatusCode::AS_BAD_REQUEST );
                    error.set_error_message( "parsed footprint field mapping is malformed" );
                    return tl::unexpected( error );
                }

                continue;
            }

            ApiResponseStatus error;
            error.set_status( ApiStatusCode::AS_BAD_REQUEST );
            error.set_error_message( "parsed footprint instance item has an unsupported type" );
            return tl::unexpected( error );
        }

        std::set<wxString> foundPads;

        for( PAD* pad : footprint->Pads() )
        {
            if( padNets.contains( pad->GetNumber() ) )
                foundPads.emplace( pad->GetNumber() );
        }

        if( foundPads.size() != padNets.size() )
        {
            ApiResponseStatus error;
            error.set_status( ApiStatusCode::AS_BAD_REQUEST );
            error.set_error_message( "parsed footprint does not contain every connected pad" );
            return tl::unexpected( error );
        }

        const LIB_ID libraryId = kiapi::common::LibIdFromProto( requested.definition().id() );

        if( !libraryId.IsValid() )
        {
            ApiResponseStatus error;
            error.set_status( ApiStatusCode::AS_BAD_REQUEST );
            error.set_error_message( "parsed footprint library identifier is invalid" );
            return tl::unexpected( error );
        }

        footprint->SetFPID( libraryId );
        requestedId = requested.id().value();
        const_cast<KIID&>( footprint->m_Uuid ) = requestedId.empty()
                                                       ? KIID()
                                                       : KIID( requestedId );
        footprint->SetReference(
                wxString::FromUTF8( requested.reference_field().text().text().text() ) );
        footprint->SetValue(
                wxString::FromUTF8( requested.value_field().text().text().text() ) );
        footprint->GetField( FIELD_T::DATASHEET )->SetText(
                wxString::FromUTF8( requested.datasheet_field().text().text().text() ) );
        footprint->GetField( FIELD_T::DESCRIPTION )->SetText(
                wxString::FromUTF8( requested.description_field().text().text().text() ) );
        footprint->SetBoardOnly( false );
        footprint->SetDNP( requested.attributes().do_not_populate() );
        footprint->SetPath( kiapi::common::UnpackSheetPath( requested.symbol_path() ) );
        footprint->SetSheetname( wxString::FromUTF8( requested.symbol_sheet_name() ) );
        footprint->SetSheetfile( wxString::FromUTF8( requested.symbol_sheet_filename() ) );
        footprint->SetLayerAndFlip(
                FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( requested.layer() ) );
        footprint->SetPosition( VECTOR2I( requested.position().x_nm(),
                                          requested.position().y_nm() ) );
        footprint->SetOrientationDegrees( requested.orientation().value_degrees() );
        footprint->SetLocked( requested.locked() == common::types::LockedState::LS_LOCKED );

        if( aCtx.Request.field_mask().paths_size() > 0 )
        {
            google::protobuf::Any existingItem;
            footprint->Serialize( existingItem );
            google::protobuf::Any mergedItem;
            std::string mergeError;
            board::types::FootprintInstance merged;

            if( !mergeItemUpdate( aCtx.Request.item(), existingItem,
                                  aCtx.Request.field_mask(), mergedItem, mergeError )
                || !mergedItem.UnpackTo( &merged )
                || !validFootprintPresentationField( merged.reference_field() )
                || !validFootprintPresentationField( merged.value_field() ) )
            {
                ApiResponseStatus error;
                error.set_status( ApiStatusCode::AS_BAD_REQUEST );
                error.set_error_message(
                        mergeError.empty()
                                ? "parsed footprint presentation is malformed"
                                : mergeError );
                return tl::unexpected( error );
            }

            google::protobuf::Any reference;
            google::protobuf::Any value;
            reference.PackFrom( merged.reference_field() );
            value.PackFrom( merged.value_field() );

            if( !footprint->Reference().Deserialize( reference )
                || !footprint->Value().Deserialize( value ) )
            {
                ApiResponseStatus error;
                error.set_status( ApiStatusCode::AS_BAD_REQUEST );
                error.set_error_message(
                        "parsed footprint presentation could not be applied" );
                return tl::unexpected( error );
            }
        }

        for( const auto& [name, value] : footprintFields )
        {
            PCB_FIELD* field = footprint->HasField( name )
                                       ? footprint->GetField( name )
                                       : new PCB_FIELD( footprint, FIELD_T::USER, name );

            if( !footprint->HasField( name ) )
                footprint->Add( field );

            field->SetText( value );
            field->SetVisible( false );
            field->SetLayer( requested.layer() == board::types::BoardLayer::BL_F_Cu
                                     ? F_Fab
                                     : B_Fab );
            field->SetPosition( footprint->GetPosition() );
            field->Rotate( footprint->GetPosition(), footprint->GetOrientation() );
            field->StyleFromSettings( frame()->GetDesignSettings(), true );
        }

        for( PAD* pad : footprint->Pads() )
        {
            auto net = padNets.find( pad->GetNumber() );

            if( net == padNets.end() )
                continue;

            board::types::Net netMessage;
            netMessage.set_name( net->second.ToUTF8() );
            pad->UnpackNet( netMessage );
        }
    }

    // A library footprint UUID identifies the definition, not an instance on this board.  Use an
    // explicitly requested instance UUID when supplied (the create path rejects collisions), or a
    // fresh UUID otherwise.  handleCreateUpdateItemsInternal() also refreshes every child UUID.
    const_cast<KIID&>( footprint->m_Uuid ) = requestedId.empty()
                                                   ? KIID()
                                                   : KIID( requestedId );

    if( aCtx.Request.has_replace_item_id() )
    {
        const std::string replacedId = aCtx.Request.replace_item_id().value();

        if( !requestedMetadata || replacedId.empty()
            || !KIID::SniffTest( wxString::FromUTF8( replacedId ) )
            || requestedId.empty() )
        {
            ApiResponseStatus error;
            error.set_status( ApiStatusCode::AS_BAD_REQUEST );
            error.set_error_message(
                    "atomic footprint replacement requires valid existing and replacement UUIDs" );
            return tl::unexpected( error );
        }

        std::optional<BOARD_ITEM*> existingItem = getItemById( KIID( replacedId ) );

        if( !existingItem )
        {
            ApiResponseStatus error;
            error.set_status( ApiStatusCode::AS_BAD_REQUEST );
            error.set_error_message(
                    fmt::format( "the footprint selected for replacement, {}, does not exist",
                                 replacedId ) );
            return tl::unexpected( error );
        }

        if( ( *existingItem )->Type() != PCB_FOOTPRINT_T )
        {
            ApiResponseStatus error;
            error.set_status( ApiStatusCode::AS_BAD_REQUEST );
            error.set_error_message(
                    "the item selected for atomic footprint replacement is not a footprint" );
            return tl::unexpected( error );
        }

        if( requestedId != replacedId && getItemById( KIID( requestedId ) ) )
        {
            ApiResponseStatus error;
            error.set_status( ApiStatusCode::AS_BAD_REQUEST );
            error.set_error_message(
                    fmt::format( "replacement footprint UUID {} is already in use", requestedId ) );
            return tl::unexpected( error );
        }

        footprint->FixUpPadsForBoard( board );
        FOOTPRINT* replacement = static_cast<FOOTPRINT*>( parsed.release() );
        BOARD_COMMIT* commit =
                static_cast<BOARD_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );
        frame()->ExchangeFootprint( static_cast<FOOTPRINT*>( *existingItem ), replacement,
                                    *commit, true, true, true, true, true, true, true, true,
                                    requestedId == replacedId );

        // ExchangeFootprint intentionally preserves ordinary interactive-instance state.  KDS is
        // an authoritative replacement, so restore the validated requested instance metadata and
        // pad-net mapping after the geometry/child reconciliation and before readback.
        const board::types::FootprintInstance& requested = *requestedMetadata;
        replacement->SetReference(
                wxString::FromUTF8( requested.reference_field().text().text().text() ) );
        replacement->SetValue(
                wxString::FromUTF8( requested.value_field().text().text().text() ) );
        replacement->GetField( FIELD_T::DATASHEET )->SetText(
                wxString::FromUTF8( requested.datasheet_field().text().text().text() ) );
        replacement->GetField( FIELD_T::DESCRIPTION )->SetText(
                wxString::FromUTF8( requested.description_field().text().text().text() ) );
        replacement->SetBoardOnly( false );
        replacement->SetDNP( requested.attributes().do_not_populate() );
        replacement->SetPath( kiapi::common::UnpackSheetPath( requested.symbol_path() ) );
        replacement->SetSheetname( wxString::FromUTF8( requested.symbol_sheet_name() ) );
        replacement->SetSheetfile( wxString::FromUTF8( requested.symbol_sheet_filename() ) );
        replacement->SetLayerAndFlip(
                FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( requested.layer() ) );
        replacement->SetPosition( VECTOR2I( requested.position().x_nm(),
                                             requested.position().y_nm() ) );
        replacement->SetOrientationDegrees( requested.orientation().value_degrees() );
        replacement->SetLocked(
                requested.locked() == common::types::LockedState::LS_LOCKED );

        for( PAD* pad : replacement->Pads() )
        {
            auto net = padNets.find( pad->GetNumber() );

            if( net == padNets.end() )
                continue;

            board::types::Net netMessage;
            netMessage.set_name( net->second.ToUTF8() );
            pad->UnpackNet( netMessage );
        }

        CreateItemsResponse response;
        response.mutable_header()->mutable_document()->CopyFrom( aCtx.Request.document() );
        response.set_status( ItemRequestStatus::IRS_OK );
        ItemCreationResult* result = response.add_created_items();
        result->mutable_status()->set_code( ItemStatusCode::ISC_OK );
        replacement->Serialize( *result->mutable_item() );

        if( !m_activeClients.count( aCtx.ClientName ) )
            pushCurrentCommit( aCtx.ClientName, _( "Replaced footprint via API" ) );

        return response;
    }

    google::protobuf::RepeatedPtrField<google::protobuf::Any> items;
    footprint->Serialize( *items.Add() );

    types::ItemHeader header;
    header.mutable_document()->CopyFrom( aCtx.Request.document() );
    CreateItemsResponse response;
    response.mutable_header()->CopyFrom( header );

    HANDLER_RESULT<ItemRequestStatus> result = handleCreateUpdateItemsInternal(
            true, aCtx.ClientName, header, items,
            [&]( const ItemStatus& aStatus, const google::protobuf::Any& aItem )
            {
                ItemCreationResult itemResult;
                itemResult.mutable_status()->CopyFrom( aStatus );
                itemResult.mutable_item()->CopyFrom( aItem );
                response.mutable_created_items()->Add( std::move( itemResult ) );
            } );

    if( !result )
        return tl::unexpected( result.error() );

    response.set_status( *result );
    return response;
}


HANDLER_RESULT<BoardLayers> API_HANDLER_PCB::handleGetVisibleLayers(
        const HANDLER_CONTEXT<GetVisibleLayers>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BoardLayers response;

    for( PCB_LAYER_ID layer : frame()->GetBoard()->GetVisibleLayers() )
        response.add_layers( ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( layer ) );

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetVisibleLayers(
        const HANDLER_CONTEXT<SetVisibleLayers>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    LSET visible;
    LSET enabled = frame()->GetBoard()->GetEnabledLayers();

    for( int layerIdx : aCtx.Request.layers() )
    {
        PCB_LAYER_ID layer =
                FromProtoEnum<PCB_LAYER_ID>( static_cast<board::types::BoardLayer>( layerIdx ) );

        if( enabled.Contains( layer ) )
            visible.set( layer );
    }

    frame()->GetBoard()->SetVisibleLayers( visible );
    frame()->GetAppearancePanel()->OnBoardChanged();
    frame()->GetCanvas()->SyncLayersVisibility( frame()->GetBoard() );
    frame()->Refresh();
    return Empty();
}


HANDLER_RESULT<BoardLayerResponse> API_HANDLER_PCB::handleGetActiveLayer(
        const HANDLER_CONTEXT<GetActiveLayer>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BoardLayerResponse response;
    response.set_layer(
            ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( frame()->GetActiveLayer() ) );

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetActiveLayer(
        const HANDLER_CONTEXT<SetActiveLayer>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    PCB_LAYER_ID layer = FromProtoEnum<PCB_LAYER_ID>( aCtx.Request.layer() );

    if( !frame()->GetBoard()->GetEnabledLayers().Contains( layer ) )
    {
        ApiResponseStatus err;
        err.set_status( ApiStatusCode::AS_BAD_REQUEST );
        err.set_error_message( fmt::format( "Layer {} is not a valid layer for the given board",
                                            magic_enum::enum_name( layer ) ) );
        return tl::unexpected( err );
    }

    frame()->SetActiveLayer( layer );
    return Empty();
}


HANDLER_RESULT<BoardEditorAppearanceSettings> API_HANDLER_PCB::handleGetBoardEditorAppearanceSettings(
        const HANDLER_CONTEXT<GetBoardEditorAppearanceSettings>& aCtx )
{
    BoardEditorAppearanceSettings reply;

    // TODO: might be nice to put all these things in one place and have it derive SERIALIZABLE

    const PCB_DISPLAY_OPTIONS& displayOptions = frame()->GetDisplayOptions();

    reply.set_inactive_layer_display( ToProtoEnum<HIGH_CONTRAST_MODE, InactiveLayerDisplayMode>(
            displayOptions.m_ContrastModeDisplay ) );
    reply.set_net_color_display(
            ToProtoEnum<NET_COLOR_MODE, NetColorDisplayMode>( displayOptions.m_NetColorMode ) );

    reply.set_board_flip( frame()->GetCanvas()->GetView()->IsMirroredX()
                                  ? BoardFlipMode::BFM_FLIPPED_X
                                  : BoardFlipMode::BFM_NORMAL );

    PCBNEW_SETTINGS* editorSettings = frame()->GetPcbNewSettings();

    reply.set_ratsnest_display( ToProtoEnum<RATSNEST_MODE, RatsnestDisplayMode>(
            editorSettings->m_Display.m_RatsnestMode ) );

    return reply;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetBoardEditorAppearanceSettings(
        const HANDLER_CONTEXT<SetBoardEditorAppearanceSettings>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    PCB_DISPLAY_OPTIONS options = frame()->GetDisplayOptions();
    KIGFX::PCB_VIEW* view = frame()->GetCanvas()->GetView();
    PCBNEW_SETTINGS* editorSettings = frame()->GetPcbNewSettings();
    const BoardEditorAppearanceSettings& newSettings = aCtx.Request.settings();

    options.m_ContrastModeDisplay =
            FromProtoEnum<HIGH_CONTRAST_MODE>( newSettings.inactive_layer_display() );
    options.m_NetColorMode =
            FromProtoEnum<NET_COLOR_MODE>( newSettings.net_color_display() );

    bool flip = newSettings.board_flip() == BoardFlipMode::BFM_FLIPPED_X;

    if( flip != view->IsMirroredX() )
    {
        view->SetMirror( !view->IsMirroredX(), view->IsMirroredY() );
        view->RecacheAllItems();
    }

    editorSettings->m_Display.m_RatsnestMode =
            FromProtoEnum<RATSNEST_MODE>( newSettings.ratsnest_display() );

    frame()->SetDisplayOptions( options );
    frame()->GetCanvas()->GetView()->UpdateAllLayersColor();
    frame()->GetCanvas()->Refresh();

    return Empty();
}


HANDLER_RESULT<InjectDrcErrorResponse> API_HANDLER_PCB::handleInjectDrcError(
        const HANDLER_CONTEXT<InjectDrcError>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SEVERITY severity = FromProtoEnum<SEVERITY>( aCtx.Request.severity() );
    int      layer = severity == RPT_SEVERITY_WARNING ? LAYER_DRC_WARNING : LAYER_DRC_ERROR;
    int      code = severity == RPT_SEVERITY_WARNING ? DRCE_GENERIC_WARNING : DRCE_GENERIC_ERROR;

    std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( code );

    drcItem->SetErrorMessage( wxString::FromUTF8( aCtx.Request.message() ) );

    RC_ITEM::KIIDS ids;

    for( const auto& id : aCtx.Request.items() )
        ids.emplace_back( KIID( id.value() ) );

    if( !ids.empty() )
        drcItem->SetItems( ids );

    const auto& pos = aCtx.Request.position();
    VECTOR2I    position( static_cast<int>( pos.x_nm() ), static_cast<int>( pos.y_nm() ) );

    PCB_MARKER* marker = new PCB_MARKER( drcItem, position, layer );

    COMMIT* commit = getCurrentCommit( aCtx.ClientName );
    commit->Add( marker );
    commit->Push( wxS( "API injected DRC marker" ) );

    InjectDrcErrorResponse response;
    response.mutable_marker()->set_value( marker->GetUUID().AsStdString() );

    return response;
}
