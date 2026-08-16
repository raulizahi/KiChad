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

#include "design_script_capabilities.h"

#include <initializer_list>
#include <map>
#include <string>


namespace
{

using JSON = nlohmann::json;


JSON strings( std::initializer_list<const char*> aValues )
{
    JSON result = JSON::array();

    for( const char* value : aValues )
        result.emplace_back( value );

    return result;
}


JSON capability( const char* aId, const char* aDomain, const char* aState,
                 std::initializer_list<const char*> aForms, const char* aControls,
                 std::initializer_list<const char*> aGaps = {} )
{
    return { { "id", aId },
             { "domain", aDomain },
             { "state", aState },
             { "kdsForms", strings( aForms ) },
             { "controls", aControls },
             { "gaps", strings( aGaps ) } };
}

} // namespace


namespace KICHAD
{

nlohmann::json DesignScriptCapabilities()
{
    JSON facets = JSON::array(
            {
                    capability( "language.source", "language", "qualified",
                                { "kichad_design", "version", "units" },
                                "One bounded UTF-8 s-expression source with explicit version and physical units." ),
                    capability( "language.identity", "language", "qualified", {},
                                "Deterministic IR, SHA-256 revision binding, and stable RFC 9562 UUIDv8 identities." ),
                    capability( "language.lifecycle", "language", "qualified", {},
                                "Bounded exact-source search/read, guarded exact-text patching, "
                                "compile, preview, atomic save, and transactional apply." ),
                    capability( "language.ai_context", "language", "partial", {},
                                "Self-description, exact KDS source, diagnostics, plans, capability inventory, and paged/queryable semantic project, library, schematic, PCB, and manufacturing context with large authored payloads omitted by digest.",
                                { "Native-file-versus-KDS semantic diff summaries and graph dependency slices are incomplete." } ),
                    capability( "language.sidecar_exchange", "language", "qualified", {},
                                "A reusable project-relative .kicad_kds sidecar is the single authored representation and is digest-verified into every portable design release beside its current native materialization." ),
                    capability( "language.modules", "language", "unrepresented", {},
                                "Reusable definitions and composition within or across KDS programs.",
                                { "No include, parameter, function, template, or module forms exist." } ),

                    capability( "project.identity", "project", "qualified", { "project" },
                                "Project name and canonical root schematic/board binding." ),
                    capability( "project.title_block", "project", "qualified", { "project" },
                                "One normalized title, company, revision, date, and nine indexed comments lower to the root schematic and live board with native readback and rollback." ),
                    capability( "project.hierarchy", "project", "qualified", { "sheet" },
                                "Nested sheets, files, instances, explicit or automatically inferred pins, matching hierarchical labels, readable parent-page net routing, page ordering, and rollback." ),
                    capability( "project.shared_screens", "project", "unrepresented", { "sheet" },
                                "Multiple hierarchy instances of one schematic screen.",
                                { "The current planner rejects shared-screen aliases." } ),
                    capability( "project.library_dependencies", "project", "qualified", { "library" },
                                "Complete project symbol/footprint tables plus global dependency declarations and model roots." ),
                    capability( "project.text_variables", "project", "qualified",
                                { "text_variables" },
                                "Complete bounded project text-variable replacement through "
                                "KiCad's typed API, with exact readback, editor refresh, "
                                "journaling, and rollback." ),
                    capability( "project.field_templates", "project", "qualified",
                                { "field_templates" },
                                "Complete ordered project schematic field-name templates with "
                                "default visibility and URL intent, typed native readback, "
                                "editor-cache synchronization, journaling, and rollback." ),
                    capability( "project.variants", "project", "unrepresented", {},
                                "Assembly/design variants, fitted state, and variant field overrides.",
                                { "Only component DNP state exists; named variants are absent." } ),
                    capability( "project.page_layout", "project", "unrepresented", {},
                                "Paper size, worksheet/page-layout file, margins, and drawing-sheet fields.",
                                { "New schematics currently use a compiler-selected A4 page." } ),
                    capability( "project.settings", "project", "partial", { "rules", "net_classes" },
                                "Board constraints, custom rules, net classes, and their schematic styles.",
                                { "The rest of .kicad_pro, schematic setup, board presets, and editor preferences are not represented." } ),
                    capability( "project.history", "project", "partial", {},
                                "Pre-turn local-history snapshots, restore, atomic journals, and stale-write guards.",
                                { "KDS does not declare version-control remotes, branches, commits, or release policy." } ),
                    capability( "project.design_blocks", "project", "unrepresented", {},
                                "Schematic/PCB design-block libraries, placement, authoring, and update.",
                                { "No KDS design-block form or backend exists." } ),

                    capability( "schematic.components", "schematic", "qualified", { "component" },
                                "Project-local symbols, fields, DNP, footprints, multi-unit placement, power/virtual symbols, rotation, and mirroring." ),
                    capability( "schematic.connectivity", "schematic", "qualified", { "net", "no_connect" },
                                "Resolved unit/pin endpoints, automatic same-sheet wiring, cross-sheet hierarchy synthesis, intentional global-label connectivity, repeated pins, and explicit no-connects." ),
                    capability( "schematic.wires_buses", "schematic", "qualified",
                                { "wire", "bus", "bus_entry", "bus_alias" },
                                "Stable-ID wire/bus geometry, entries, strokes, and name-owned aliases." ),
                    capability( "schematic.junctions", "schematic", "qualified", { "junction" },
                                "Stable junction position, diameter, color, reconciliation, and rollback." ),
                    capability( "schematic.labels", "schematic", "qualified", { "label", "sheet" },
                                "Local, global, and hierarchy-derived labels with electrical shape and typography." ),
                    capability( "schematic.directive_labels", "schematic", "qualified",
                                { "directive", "rule_area" },
                                "Net- and rule-area-targeted directives, validated polygon boundaries, inclusion policy, flag geometry, stroke/fill, and explicit field typography." ),
                    capability( "schematic.text_graphics", "schematic", "qualified",
                                { "text", "text_box", "polyline", "rectangle", "circle",
                                  "arc", "bezier", "image", "table" },
                                "Stable free text, text boxes, all native drawing geometries, digest-verified self-contained images, and semantic table grids with complete cell formatting and rectangular merges." ),
                    capability( "schematic.groups", "schematic", "qualified", { "group" },
                                "Stable named groups, native locking, typed direct membership, "
                                "nested containment, screen ownership, and repeated-pin occurrence "
                                "selection." ),
                    capability( "schematic.fields", "schematic", "qualified", { "component" },
                                "Bounded required and custom field values, Datasheet/Description "
                                "overrides, per-unit absolute placement, visibility, name display, "
                                "autoplace state, privacy, typography, color, and hyperlinks." ),
                    capability( "schematic.annotation", "schematic", "partial", { "component" },
                                "Explicit stable references, including power references.",
                                { "Automatic annotation, reset, scope, ordering, and back-annotation policy are absent." } ),
                    capability( "schematic.pin_swap", "schematic", "unrepresented", {},
                                "Pin/unit swapping and alternate pin assignments.",
                                { "No swap or alternate-selection form exists." } ),
                    capability( "schematic.erc", "schematic", "qualified", { "check erc" },
                                "Matching KiCad 10.0.4 native ERC with bounded complete results and fabrication gating." ),
                    capability( "schematic.erc_policy", "schematic", "partial",
                                { "rules erc_severities", "check erc" },
                                "Named native ERC severity overrides, run intent, transactional "
                                "project persistence, and explicit final-release waiver approval.",
                                { "The pin-conflict map and ERC exclusions are not authored in KDS." } ),

                    capability( "symbols.project_consumption", "symbols", "qualified", { "library", "component" },
                                "Confined project-local root/derived symbol resolution, flattening, pins, graphics, fields, and inclusion flags." ),
                    capability( "symbols.global_consumption", "symbols", "partial", { "library", "component" },
                                "Installed stock global nicknames are inventoried with bounded exact source and resolved through the same semantic symbol pipeline as project libraries.",
                                { "User-defined global tables and package-manager libraries are not snapshotted yet." } ),
                    capability( "symbols.authoring", "symbols", "partial", { "symbol" },
                                "AI-native project symbol creation with metadata, completely laid-out mandatory/custom fields, inclusion flags, numbered/common units, named and De Morgan body styles, unit display names, unit locking, footprint filters, global/local power semantics, checked same-library derived aliases, all native vector/text graphics, and fully typed pins with alternate and jumper functions lowered to current KiCad 10 native libraries.",
                                { "Embedded-font ownership and publish workflows are not represented yet." } ),
                    capability( "symbols.library_management", "symbols", "partial", { "library", "symbol" },
                                "Explicit whole-library KDS ownership, deterministic creation, project table generation, exact-byte journaling, atomic installation, native-loader validation, and rollback.",
                                { "Rename, selective merge into unmanaged libraries, migrate, rescue, and publish workflows are absent." } ),

                    capability( "simulation.electrical_contracts", "simulation", "qualified",
                                { "electrical rail", "electrical rating", "electrical thermal", "electrical logic", "check electrical" },
                                "Typed rail current budgets, component voltage/current/power derating, junction-temperature estimates, logic thresholds, absolute ratings, pageable failures, and fabrication gating." ),
                    capability( "simulation.models", "simulation", "partial", { "electrical simulation model", "electrical simulation device" },
                                "Injection-safe resistor, capacitor, inductor, diode, BJT, and MOSFET circuit devices plus typed finite SPICE model parameters and compiler-checked KDS component/unit/pin-to-net binding.",
                                { "Vendor subcircuits, encrypted models, transmission lines, and IBIS model import are not represented." } ),
                    capability( "simulation.directives", "simulation", "partial", { "electrical simulation source", "electrical simulation assert" },
                                "Typed DC, AC, and pulse voltage/current sources plus voltage, differential-voltage, and source-current probes.",
                                { "Initial conditions, piecewise-linear sources, behavioral sources, and net aliases are absent." } ),
                    capability( "simulation.analyses", "simulation", "partial", { "electrical simulation analysis", "check electrical" },
                                "Bounded operating-point, transient, AC sweep, and DC sweep execution through ngspice with private inputs, hard deadlines, and finite-result validation.",
                                { "Noise, pole-zero, S-parameter, sensitivity, Monte Carlo, and FFT analyses are absent." } ),
                    capability( "simulation.results", "simulation", "partial", { "electrical simulation assert", "check electrical" },
                                "Reproducible raw-vector parsing, all-sample or final-sample numeric acceptance assertions, measured extrema/final values, pageable failures, and fabrication gating.",
                                { "Persistent waveform artifacts, plot styling, cursors, and CSV/Touchstone export are absent." } ),

                    capability( "pcb.stackup", "pcb", "qualified", { "stackup" },
                                "Ordered physical layers, thickness, materials, dielectric properties, finish, impedance, connector, and plating." ),
                    capability( "pcb.global_rules", "pcb", "qualified", { "rules" },
                                "Complete KiCad 10 global Board Setup constraint protobuf with readback and rollback." ),
                    capability( "pcb.net_classes", "pcb", "qualified", { "net_classes" },
                                "Complete explicit class table, priorities, assignments, board/schematic fields, and native readback." ),
                    capability( "pcb.custom_rules", "pcb", "qualified", { "custom_rules" },
                                "All 35 KiCad conditional constraint types, native parser/DRC validation, readback, and rollback." ),
                    capability( "pcb.outline", "pcb", "qualified", { "board outline" },
                                "Stable line, rectangle, arc, circle, polygon, and Bezier Edge.Cuts geometry with rounded rectangles, implicit polygon closure, cutouts, and multiple outline islands through the typed BoardGraphicShape API." ),
                    capability( "pcb.placement", "pcb", "partial", { "board place", "board layout", "board synthesize placement" },
                                "Schematic-linked exact placement plus deterministic connectivity-ranked placement synthesis with bounded grid/orientation search, edge policy, native front/back courtyard geometry with pad-bound fallback, clearance collision checks, exact generated IR, and corrective failure steering.",
                                { "Curved/cutout/multi-island outline synthesis, room partitioning, thermal placement, and high-density optimization remain absent." } ),
                    capability( "pcb.footprint_presentation", "pcb", "qualified",
                                { "board place reference", "board place value" },
                                "Per-instance reference/value visibility, absolute position, silkscreen/fabrication layer, size, stroke, angle, justification, font, emphasis, mirroring, and upright policy with typed update and native readback." ),
                    capability( "pcb.layout_contract", "pcb", "qualified", { "board layout", "check layout" },
                                "Canonical maximum board dimensions, component-anchor proximity/alignment/edge/group constraints, global and per-net geometry/via/length policy, bundle skew, exact measurements, pageable violations, and fabrication gating." ),
                    capability( "pcb.tracks_arcs", "pcb", "qualified", { "board route" },
                                "Straight and circular-arc copper routes with net, width, layer, lock, and stable identity." ),
                    capability( "pcb.vias", "pcb", "qualified", { "board via" },
                                "Stable net-connected through/blind/buried/micro vias with either one circular diameter or a complete front/inner/back or arbitrary per-layer copper padstack; circle/rect/oval/trapezoid/roundrect/chamfer and semantic custom-primitive geometry; offsets; complete unconnected-layer ring and per-layer forced-flash policy; top/bottom backdrilling; tenting; covering; plugging; filling; capping; post-machining; full per-item teardrop geometry and policy; and lock state through the KiChad 10 typed IPC messages." ),
                    capability( "pcb.zones", "pcb", "qualified", { "board zone" },
                                "Multi-layer polygon/hole copper zones, thermal/island/fill/hatch/border policy, refill, and readback." ),
                    capability( "pcb.keepouts", "pcb", "qualified", { "board keepout" },
                                "Stable rule areas with exact copper/via/track/pad/footprint prohibitions." ),
                    capability( "pcb.text", "pcb", "qualified", { "board text" },
                                "Multiline board text, layer, position, typography, justification, hyperlink, knockout, and lock." ),
                    capability( "pcb.dimensions", "pcb", "qualified", { "board dimension" },
                                "Aligned, orthogonal, radial, leader, and center dimensions with complete applicable measurement/text policy." ),
                    capability( "pcb.graphics", "pcb", "qualified",
                                { "board line", "board rectangle", "board arc", "board circle", "board polygon", "board bezier" },
                                "Stable segments, rounded rectangles, three-point arcs, circles, implicitly closed polygons, and Bezier curves on arbitrary physical, technical, inner-copper, and user layers with stroke, fill, lock, copper-net ownership, and optional paired solder-mask expansion through the typed BoardGraphicShape API." ),
                    capability( "pcb.text_boxes_tables", "pcb", "qualified",
                                { "board text_box", "board table" },
                                "Stable rectangular and polygonal text boxes plus complete native board tables with ordered row/column dimensions, merged-cell topology, per-cell text state, independent outer/header/row/column stroke policy, layer, lock, and deterministic owned identities through typed BoardTextBox and BoardTable APIs." ),
                    capability( "pcb.images_barcodes", "pcb", "qualified",
                                { "board image", "board barcode" },
                                "Digest-verified self-contained reference images and native Code 39, Code 128, Data Matrix, QR, and Micro QR barcodes with deterministic identity, complete native geometry/style state, transactional reconciliation, and typed readback." ),
                    capability( "pcb.groups", "pcb", "unrepresented", {},
                                "PCB groups, nested membership, and group locking.",
                                { "No group or containment representation exists." } ),
                    capability( "pcb.generators", "pcb", "unrepresented", {},
                                "Native generators and generator-owned objects.",
                                { "No generator form, plugin contract, or deterministic regeneration backend exists." } ),
                    capability( "pcb.length_tuning", "pcb", "partial", { "custom_rules", "net_classes" },
                                "Length/skew/impedance constraints and net-class tuning-profile references.",
                                { "Tuning-pattern geometry, tuning-profile definitions, interactive tuning, and measured-result assertions are absent." } ),
                    capability( "pcb.routing", "pcb", "partial", { "board route", "board via", "board layout", "board synthesize routing" },
                                "Explicit copper plus bounded deterministic Manhattan maze routing for unrouted nets, using actual footprint pad centers, courtyard obstacles, board bounds, track keepouts, existing and synthesized copper obstacles, stable generated identities, and native DRC release gating.",
                                { "Multilayer via synthesis, push-and-shove, differential pairs, length tuning, escape fanout, and arbitrary-angle smoothing remain absent." } ),
                    capability( "pcb.connectivity", "pcb", "partial", { "net" },
                                "Schematic-derived nets, pad assignment for compiler-created footprints, DRC parity, and zone refill.",
                                { "KDS lacks direct connectivity queries, ratsnest controls, and measured path topology assertions." } ),
                    capability( "pcb.layers_origins_defaults", "pcb", "partial", { "stackup" },
                                "Enabled physical production layers are derived from stackup.",
                                { "Custom layer names, user layers, graphics defaults, grid/drill origins, and fabrication/auxiliary origin policy are absent." } ),
                    capability( "pcb.drc", "pcb", "qualified", { "check drc" },
                                "Matching KiCad 10.0.4 native DRC, schematic parity, pageable complete results, and release gating." ),
                    capability( "pcb.drc_policy", "pcb", "partial",
                                { "rules drc_severities", "check drc", "custom_rules" },
                                "Complete native DRC default/override severities, custom-rule "
                                "severities, transactional readback, and explicit final-release "
                                "waiver approval.",
                                { "Native exclusions and marker lifecycle are not authored in KDS." } ),

                    capability( "footprints.project_consumption", "footprints", "qualified", { "library", "component", "board place" },
                                "Confined project-local .pretty lookup, native parsing, deterministic instance creation, pad nets, and deletion/recreation." ),
                    capability( "footprints.global_consumption", "footprints", "partial", { "library", "component", "board place" },
                                "Installed stock global footprints are inventoried as bounded exact native sources and can create missing linked instances without hard-coded part names.",
                                { "User-defined global tables and package-manager libraries are not snapshotted yet." } ),
                    capability( "footprints.authoring", "footprints", "partial", { "footprint", "pad", "model" },
                                "KDS-owned current-format footprint definitions with mandatory and stable-ID custom properties, component-class membership, attributes, footprint-wide rules, custom/private layer policy, jumper/net-tie groups, standard/custom/chamfered pads, per-layer padstacks, all six native graphic primitives, rich text, rectangular/polygon text boxes, footprint copper zones and keepouts, nested stable-ID groups, explicit assembly variants, and project-local 3D assignments.",
                                { "Embedded assets and complete package-data-to-KLC geometry synthesis remain absent." } ),
                    capability( "footprints.padstacks", "footprints", "partial", { "footprint pad" },
                                "SMD, connect, plated and non-plated through-hole pads with standard, chamfered, and semantic custom shapes; deterministic pad-wide and per-copper-layer custom primitives; primary drilling plus top/bottom backdrilling; complete per-pad teardrop geometry and policy; offsets; mask/paste/clearance/thermal overrides; front/inner/back and sparse named-inner padstacks; tenting; post-machining; layer-removal policy; properties; and pin metadata through both current-format libraries and the typed Pad IPC message.",
                                { "Plugging, filling, capping, and covering are KiCad via rather than footprint-pad file semantics and remain gaps under pcb.vias." } ),
                    capability( "footprints.models", "footprints", "partial", { "library model", "footprint model" },
                                "Project-confined STEP/STP/WRL assignment with offset, rotation, scale, visibility, and opacity.",
                                { "Embedded model data, model acquisition, and model-file content validation remain absent." } ),
                    capability( "footprints.library_management", "footprints", "partial", { "library" },
                                "Project footprint-table generation plus opt-in whole-library creation/replacement with native validation, journaling, and rollback.",
                                { "Rename, removal after ownership is dropped, migration, and publishing workflows remain absent." } ),

                    capability( "manufacturing.sourcing", "manufacturing", "qualified", { "source", "check sourcing" },
                                "Current manufacturer/MPN/datasheet/lifecycle/supplier/SKU/stock evidence with deterministic freshness gating." ),
                    capability( "manufacturing.gerber", "manufacturing", "qualified", { "output gerbers" },
                                "Validated Gerber layers and job file from the fixed production profile." ),
                    capability( "manufacturing.drill", "manufacturing", "qualified", { "output drill" },
                                "Validated Excellon drill, maps, and report from the fixed production profile." ),
                    capability( "manufacturing.electrical_test", "manufacturing", "qualified", { "output ipcd356" },
                                "Validated IPC-D-356 electrical-test netlist." ),
                    capability( "manufacturing.ipc2581", "manufacturing", "qualified", { "output ipc2581" },
                                "Parsed and structurally validated IPC-2581C millimetre manufacturing XML." ),
                    capability( "manufacturing.odbpp", "manufacturing", "qualified", { "output odbpp" },
                                "Bounded streamed ODB 8.1 millimetre archive validation without extraction." ),
                    capability( "manufacturing.assembly", "manufacturing", "qualified", { "output pick_place", "output bom" },
                                "Reference-consistent placement CSV and KDS-sourced BOM." ),
                    capability( "manufacturing.assembly_process", "manufacturing", "partial",
                                { "production assembly" },
                                "IPC-A-610 acceptance class, soldering process/alloy, stencil sides and thickness, cleaning, conformal coating, and bounded board/component special instructions compile into the release plan.",
                                { "KiChad does not yet generate reflow thermal profiles or ingest serialized AOI/X-ray inspection results." } ),
                    capability( "manufacturing.firmware_handoff", "manufacturing", "partial",
                                { "production firmware", "production program" },
                                "Exact project-confined or self-contained base64 firmware bytes are identified by format, byte count, SHA-256, version, and target; typed programming protocol, device, voltage, speed, erase/reset/verify policy, connector, signal map, and options compile into the atomically installed release package.",
                                { "KiChad does not yet execute a hardware programmer or ingest signed per-unit programming results." } ),
                    capability( "manufacturing.device_code", "manufacturing", "partial",
                                { "production firmware device_code" },
                                "Self-contained UTF-8 C, C++, Rust, assembly, Arduino, linker-script, and configuration files carry exact byte counts and SHA-256 identities plus entry point, target, versioned trusted-adapter toolchain, and locked dependency identities into the release package.",
                                { "Trusted toolchain adapters are declared and packaged but KiChad does not yet reproduce the firmware artifact from source inside its release sandbox." } ),
                    capability( "manufacturing.bringup", "manufacturing", "partial",
                                { "production power", "production test" },
                                "Current-limited connector power profiles and ordered unpowered, power-on, programmed, and functional tests use typed targets, instruments, ranges/results, timing, testpoints, and procedures in KDS and the release package.",
                                { "KiChad does not yet execute a production test station or ingest signed per-unit measurement results." } ),
                    capability( "manufacturing.mechanical", "manufacturing", "qualified",
                                { "output step", "output stepz", "output brep", "output glb",
                                  "output stl", "output u3d", "output xao", "output 3d_pdf",
                                  "output pdf" },
                                "Validated optional STEP/STEPZ, BREP, binary glTF, triangular ASCII "
                                "STL, U3D, XAO, interactive 3D PDF, and fabrication PDF artifacts." ),
                    capability( "manufacturing.documentation", "manufacturing", "qualified",
                                { "output board_ps", "output schematic_pdf", "output schematic_svg",
                                  "output schematic_dxf", "output schematic_ps",
                                  "output board_render" },
                                "Validated native PCB layer PostScript plus schematic release "
                                "drawings in PDF, SVG, DXF, and PostScript, plus a lossless native "
                                "3D board render." ),
                    capability( "manufacturing.schematic_bom", "manufacturing", "qualified",
                                { "output schematic_bom", "output legacy_bom_xml" },
                                "Exact ungrouped native schematic BOM CSV and legacy Eeschema XML "
                                "interchange, both matched to compiled KDS components." ),
                    capability( "manufacturing.presets", "manufacturing", "partial", { "output", "check fabrication" },
                                "One immutable KiChad 10.0.4 production profile and explicit output intent.",
                                { "User-defined plot/drill/position/BOM presets, panelization, stencil policy, and fab-house profiles are absent." } ),
                    capability( "manufacturing.additional_exports", "manufacturing", "partial",
                                { "output assembly_svg", "output assembly_dxf",
                                  "output gencad", "output vrml", "output board_stats",
                                  "output netlist" },
                                "Validated front/back SVG and DXF assembly drawings, GenCAD, "
                                "VRML, typed JSON board statistics, and an exact KDS-matched "
                                "KiCad connectivity netlist from deterministic native jobs.",
                                { "IDF, PLY, custom BOM schemas, and the "
                                  "remaining board graphics and legacy exports are not represented yet." } ),

                    capability( "interchange.native_current", "interchange", "qualified", {},
                                "Exact KiCad 10.0.4 current schematic/board fixtures, native save, readback, and legacy-format rejection at release." ),
                    capability( "interchange.importers", "interchange", "unrepresented", {},
                                "Altium, Cadence, Eagle, EasyEDA, CADSTAR, EAGLE, LTspice, and other supported import workflows.",
                                { "No KDS import declaration, conversion plan, or round-trip qualification exists." } ),
                    capability( "interchange.exporters", "interchange", "partial", { "output" },
                                "Production Gerber/drill/IPC/ODB/BOM/placement/STEP/STEPZ/BREP/GLB/"
                                "STL/U3D/XAO/3D-PDF/PDF plus PCB PostScript and schematic "
                                "PDF/SVG/DXF/PostScript/PNG, native schematic BOM CSV, legacy BOM XML, "
                                "assembly SVG/DXF, GenCAD, VRML, JSON board statistics, and an exact "
                                "KiCad connectivity netlist.",
                                { "Remaining graphics/mechanical containers, legacy, and configurable "
                                  "report exporters remain absent." } ),
                    capability( "interchange.migration", "interchange", "unrepresented", {},
                                "Legacy KiCad rescue, remap, migration, and third-party conversion provenance.",
                                { "KDS rejects unsupported native versions instead of migrating them." } ),

                    capability( "editor.live_pcb_items", "editor", "partial", {},
                                "Official protobuf schema discovery plus bounded create/get/update/delete for nine live PCB item types.",
                                { "This generic escape hatch is not KDS, omits other item types and commands, and is not reusable design intent." } ),
                    capability( "editor.live_schematic_items", "editor", "unrepresented", {},
                                "Official live schematic item discovery, selection, mutation, save, and undoable transactions.",
                                { "There is no native Codex schematic tool; KDS apply uses file reconciliation." } ),
                    capability( "editor.selection_navigation", "editor", "unrepresented", {},
                                "Selection, hit testing, zoom/pan, cross-probe, highlight, find, and interactive move.",
                                { "No bounded AI tool contract exists for editor navigation state." } ),
                    capability( "editor.documents", "editor", "partial", {},
                                "Project context, open PCB targeting, explicit save in integration flows, and pre-turn restore.",
                                { "Open/close/save-copy/revert/refresh across every editor is not exposed as typed AI tools." } ),
                    capability( "editor.appearance", "editor", "unrepresented", {},
                                "Visible/active layers, colors, ratsnest, flip mode, opacity, grids, canvases, and render settings.",
                                { "No AI tool or KDS representation owns editor appearance." } ),
                    capability( "editor.actions", "editor", "unrepresented", {},
                                "Stable typed equivalents for interactive edit, route, place, inspect, and utility actions.",
                                { "Raw TOOL_ACTION execution is intentionally not exposed as a production API." } ),
                    capability( "editor.three_d_viewer", "editor", "unrepresented", {},
                                "3D viewer camera, materials, visibility, raytracing, inspection, and export.",
                                { "No typed AI control surface or KDS facet exists." } ),

                    capability( "auxiliary.gerber_viewer", "auxiliary", "unrepresented", {},
                                "Gerber/Excellon loading, layer inspection, measurement, comparison, and export.",
                                { "No typed AI tool contract exists for GerbView." } ),
                    capability( "auxiliary.bitmap_converter", "auxiliary", "unrepresented", {},
                                "Bitmap import, thresholding, scaling, layer choice, and symbol/footprint output.",
                                { "No KDS asset conversion form or typed tool exists." } ),
                    capability( "auxiliary.calculator", "auxiliary", "unrepresented", {},
                                "Regulator, track, spacing, attenuator, RF, color-code, and transmission-line calculators.",
                                { "No typed calculation tool contract or KDS assertion form exists." } ),
                    capability( "auxiliary.page_layout_editor", "auxiliary", "unrepresented", {},
                                "Worksheet geometry, fields, images, repeats, and page-layout files.",
                                { "No KDS worksheet authoring form or backend exists." } ),
                    capability( "auxiliary.plugins_packages", "auxiliary", "unrepresented", {},
                                "Plugin/package discovery, permissioning, installation, configuration, and deterministic invocation.",
                                { "The embedded Codex process intentionally disables inherited plugins and MCP connectors." } )
            } );

    std::map<std::string, size_t> counts = {
        { "qualified", 0 }, { "partial", 0 }, { "unrepresented", 0 }
    };

    for( const JSON& facet : facets )
        ++counts.at( facet.at( "state" ).get<std::string>() );

    return {
        { "target", { { "kicad", "10.0.4" }, { "kds", 1 } } },
        { "scope",
          "Authorable KiCad project/design state, deterministic design operations, verification, "
          "manufacturing, editor control surfaces, interchange, and bundled auxiliary applications." },
        { "authoritative", true },
        { "representation",
          ".kicad_kds remains the single authored design representation; this catalog is compiler "
          "introspection, not a second design format." },
        { "states",
          { { "qualified",
              "Has one KDS representation and the parser, typecheck, deterministic lowering, native "
              "apply/readback, rollback, current-format save, unit, and real KiCad integration "
              "coverage applicable to the facet." },
            { "partial", "Some typed control exists, but at least one required production behavior is missing." },
            { "unrepresented", "No reusable KDS representation and production backend exist." } } },
        { "qualification",
          strings( { "single KDS representation", "parser and typecheck", "deterministic lowering",
                     "native apply", "native readback", "failure rollback",
                     "current KiCad format save", "negative and bounded unit tests",
                     "disposable real-KiCad integration test" } ) },
        { "complete", counts.at( "partial" ) == 0 && counts.at( "unrepresented" ) == 0 },
        { "counts", counts },
        { "facets", std::move( facets ) }
    };
}

} // namespace KICHAD
