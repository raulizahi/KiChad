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

#ifndef API_PROJECT_SETTINGS_H
#define API_PROJECT_SETTINGS_H

#include <string>
#include <utility>
#include <vector>

#include <kicommon.h>

class PROJECT_FILE;

/**
 * Project-settings accessors for the API handlers, expressed without naming
 * nlohmann::json.
 *
 * The handlers live in the static `common` library, while the settings JSON
 * document is owned by the `kicommon` shared library.  kicommon exports its
 * `JSON_SETTINGS::Get<nlohmann::json>` / `Set<nlohmann::json>` instantiations,
 * and MSVC exports the inline `basic_json` members along with them.  A handler
 * translation unit that also touches nlohmann::json emits its own copies of
 * those same members into common.lib, so every kiface linking both hits
 * LNK2005 (multiply defined symbol) and fails with LNK1169.  ELF and Mach-O
 * coalesce the duplicates silently, which is why this only bites on Windows.
 *
 * Keeping every JSON touch on this side of the DLL boundary leaves the handlers
 * with plain types and a single definition of the json members.
 */

struct SCHEMATIC_FIELD_TEMPLATE_ENTRY
{
    std::string name;
    bool        visible = false;
    bool        url = false;
};

using SCHEMATIC_RULE_SEVERITY_ENTRY = std::pair<std::string, std::string>;

/**
 * Serialize the value stored at @p aPath so a failed save can be rolled back.
 *
 * @param aDefault is returned verbatim when the project holds no value at that path.
 * @return the stored value as JSON text.
 */
KICOMMON_API std::string SnapshotProjectJson( const PROJECT_FILE& aProjectFile,
                                              const std::string& aPath,
                                              const std::string& aDefault );

/**
 * Restore JSON text captured by SnapshotProjectJson().
 *
 * @return true if @p aSnapshot parsed and was stored.
 */
KICOMMON_API bool RestoreProjectJson( PROJECT_FILE& aProjectFile, const std::string& aPath,
                                      const std::string& aSnapshot );

/// Replace the stored schematic field templates with @p aEntries.
KICOMMON_API void
StoreSchematicFieldTemplates( PROJECT_FILE& aProjectFile,
                              const std::vector<SCHEMATIC_FIELD_TEMPLATE_ENTRY>& aEntries );

/**
 * Read the stored schematic field templates.
 *
 * A project with no stored templates yields an empty list and succeeds.  Only the
 * structure is checked here; entry-count and naming policy stay with the caller.
 *
 * @return false and sets @p aError if the stored value is not a well-formed template list.
 */
KICOMMON_API bool
LoadSchematicFieldTemplates( const PROJECT_FILE& aProjectFile,
                             std::vector<SCHEMATIC_FIELD_TEMPLATE_ENTRY>& aEntries,
                             std::string& aError );

/// Replace the stored ERC rule severities with @p aEntries.
KICOMMON_API void
StoreSchematicRuleSeverities( PROJECT_FILE& aProjectFile,
                              const std::vector<SCHEMATIC_RULE_SEVERITY_ENTRY>& aEntries );

/**
 * Read the stored ERC rule severities as name/severity text pairs.
 *
 * A project with no stored severities yields an empty list and succeeds.  Severity
 * values are returned as stored; mapping them to the API enum stays with the caller.
 *
 * @return false and sets @p aError if the stored value is not an object of strings.
 */
KICOMMON_API bool
LoadSchematicRuleSeverities( const PROJECT_FILE& aProjectFile,
                             std::vector<SCHEMATIC_RULE_SEVERITY_ENTRY>& aEntries,
                             std::string& aError );

#endif  // API_PROJECT_SETTINGS_H
