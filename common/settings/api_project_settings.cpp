/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
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

#include <settings/api_project_settings.h>

#include <nlohmann/json.hpp>

#include <project/project_file.h>

namespace
{

const std::string FIELD_TEMPLATES_PATH = "schematic.drawing.field_names";
const std::string RULE_SEVERITIES_PATH = "erc.rule_severities";

}  // namespace


std::string SnapshotProjectJson( const PROJECT_FILE& aProjectFile, const std::string& aPath,
                                 const std::string& aDefault )
{
    const std::optional<nlohmann::json> stored = aProjectFile.GetJson( aPath );

    if( !stored )
        return aDefault;

    return stored->dump();
}


bool RestoreProjectJson( PROJECT_FILE& aProjectFile, const std::string& aPath,
                         const std::string& aSnapshot )
{
    nlohmann::json parsed = nlohmann::json::parse( aSnapshot, nullptr, false );

    if( parsed.is_discarded() )
        return false;

    aProjectFile.SetJson( aPath, std::move( parsed ) );
    return true;
}


void StoreSchematicFieldTemplates( PROJECT_FILE& aProjectFile,
                                   const std::vector<SCHEMATIC_FIELD_TEMPLATE_ENTRY>& aEntries )
{
    nlohmann::json encoded = nlohmann::json::array();

    for( const SCHEMATIC_FIELD_TEMPLATE_ENTRY& entry : aEntries )
    {
        encoded.push_back( { { "name", entry.name },
                             { "visible", entry.visible },
                             { "url", entry.url } } );
    }

    aProjectFile.SetJson( FIELD_TEMPLATES_PATH, std::move( encoded ) );
}


bool LoadSchematicFieldTemplates( const PROJECT_FILE& aProjectFile,
                                  std::vector<SCHEMATIC_FIELD_TEMPLATE_ENTRY>& aEntries,
                                  std::string& aError )
{
    aEntries.clear();

    const std::optional<nlohmann::json> stored = aProjectFile.GetJson( FIELD_TEMPLATES_PATH );

    if( !stored )
        return true;

    if( !stored->is_array() )
    {
        aError = "project contains invalid schematic field templates";
        return false;
    }

    for( const nlohmann::json& entry : *stored )
    {
        if( !entry.is_object() || entry.size() != 3 || !entry.contains( "name" )
            || !entry["name"].is_string() || !entry.contains( "visible" )
            || !entry["visible"].is_boolean() || !entry.contains( "url" )
            || !entry["url"].is_boolean() )
        {
            aEntries.clear();
            aError = "project contains invalid schematic field templates";
            return false;
        }

        aEntries.push_back( { entry["name"].get<std::string>(), entry["visible"].get<bool>(),
                              entry["url"].get<bool>() } );
    }

    return true;
}


void StoreSchematicRuleSeverities( PROJECT_FILE& aProjectFile,
                                   const std::vector<SCHEMATIC_RULE_SEVERITY_ENTRY>& aEntries )
{
    nlohmann::json encoded = nlohmann::json::object();

    for( const SCHEMATIC_RULE_SEVERITY_ENTRY& entry : aEntries )
        encoded[entry.first] = entry.second;

    aProjectFile.SetJson( RULE_SEVERITIES_PATH, std::move( encoded ) );
}


bool LoadSchematicRuleSeverities( const PROJECT_FILE& aProjectFile,
                                  std::vector<SCHEMATIC_RULE_SEVERITY_ENTRY>& aEntries,
                                  std::string& aError )
{
    aEntries.clear();

    const std::optional<nlohmann::json> stored = aProjectFile.GetJson( RULE_SEVERITIES_PATH );

    if( !stored )
        return true;

    if( !stored->is_object() )
    {
        aError = "project ERC severity settings are malformed";
        return false;
    }

    for( auto entry = stored->begin(); entry != stored->end(); ++entry )
    {
        if( !entry.value().is_string() )
        {
            aEntries.clear();
            aError = "project ERC severity settings contain a non-string value";
            return false;
        }

        aEntries.emplace_back( entry.key(), entry.value().get<std::string>() );
    }

    return true;
}
