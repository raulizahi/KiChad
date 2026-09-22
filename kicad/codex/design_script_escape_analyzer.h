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

#ifndef KICHAD_DESIGN_SCRIPT_ESCAPE_ANALYZER_H
#define KICHAD_DESIGN_SCRIPT_ESCAPE_ANALYZER_H

#include <nlohmann/json.hpp>

namespace KICHAD
{

/**
 * Estimate the fabrication feature sizes a design's packages actually require.
 *
 * A fine-pitch grid array decides the board's minimum track, clearance, and via before any
 * routing is attempted: a track escaping between two adjacent balls needs one track width plus
 * two clearances inside the channel left between their pads, and a dogbone via must fit the
 * diagonal pocket between four balls.  Declaring coarser minimums than that makes the board
 * unroutable no matter how long a router runs, which is only discovered late and reported as a
 * routing failure.
 *
 * This analyzer derives the requirement from the inventoried footprint pad geometry, compares it
 * against the KDS rules when they are declared, and reports the floors a fabricator must meet.
 * It also recommends a copper layer count from the same geometry: how deep an escape must reach,
 * whether the board needs reference and power planes, and whether it carries differential pairs.
 * When external layout is enabled the user's configured count governs and the recommendation is
 * advice; otherwise it is what the design should declare.
 * It is deterministic, reads no files, and never changes the design.
 */
class DESIGN_SCRIPT_ESCAPE_ANALYZER
{
public:
    using JSON = nlohmann::json;

    struct RESULT
    {
        /// False when a declared rule set cannot escape a package in the design.
        bool feasible = true;
        /// Copper layers the design needs, derived from escape depth, planes, and pairs.
        int  recommendedCopperLayers = 2;
        /// Why that many: one plain sentence per contributing factor.
        JSON layerRationale = JSON::array();
        /// One entry per fine-pitch package: geometry and the floors it requires.
        JSON requirements = JSON::array();
        /// Blocking findings, each naming the component and the floors it needs.
        JSON issues = JSON::array();
        JSON summary = JSON::object();
    };

    /**
     * @param aCompilerIr compiled KDS IR (components, footprints, and rules when declared).
     * @param aFootprintSources native footprint text keyed by library id, as inventoried for
     *                          physical synthesis.
     */
    static RESULT Analyze( const JSON& aCompilerIr, const JSON& aFootprintSources );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_ESCAPE_ANALYZER_H
