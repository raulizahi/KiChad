# KiChad production status and roadmap

Status date: **2026-07-22**

KiChad is a KiCad 10.0.4 downstream in which Codex authors one reusable KiChad Design Script
(`.kicad_kds`) sidecar and KiChad compiles it into ordinary KiCad project, schematic, board, and
manufacturing files. KDS is the only authored design representation. Compiler IR, protobuf
messages, ownership state, journals, previews, and native KiCad files are implementation artifacts,
not competing design formats.

This page is the human-readable project status. The authoritative facet-level inventory is returned
at runtime by `design.describe` as `capabilityCoverage`. When this page and that inventory disagree,
the runtime inventory wins.

## Current baseline

| Contract | Current value |
| --- | --- |
| KiCad base | 10.0.4 stable |
| KDS source version | 1 |
| Native tool-schema version | 13 |
| Fabrication profile | `kichad-production-10.0.4-v17` |
| Authored representation | One project-relative `.kicad_kds` sidecar |
| Local release build | `build/install/bin/kicad` |
| Linux distribution | Ubuntu 24.04 x86_64 AppImage with pinned Codex app-server |

The capability ledger currently contains 103 facets: 51 qualified, 32 partial, and 20
unrepresented. Those counts are inventory counts, not a completion percentage: a multilayer router
is materially more work than a small metadata facet.

`qualified` means the applicable KDS form has parsing and type checking, deterministic lowering,
native application and readback, rollback, current-format save, bounded negative coverage, and a
disposable real-KiCad integration proof. `partial` means useful typed control exists but a stated
production behavior is missing. `unrepresented` means no reusable KDS form and production backend
exist.

## What exists today

### Application and Codex runtime

- KiChad owns one `codex app-server` child and communicates with it over redirected JSON-RPC stdio.
  No wrapper daemon, MCP server, or separate design-tool server is required.
- ChatGPT sign-in and device-code sign-in, live model discovery, reasoning-effort selection,
  streaming answers, supported reasoning summaries, visible tool requests/results, stop, and
  structured failure/recovery messages are integrated into the project manager.
- Model and reasoning selections persist as application preferences. Conversations are bound to the
  active project, survive application restarts, and can be cleared with the new-conversation action.
- `/goal` uses Codex's persistent goal lifecycle. New and resumed threads receive live,
  high-context web search for current component research.
- Each turn first requests a coherent KiCad local-history snapshot. **Revert turn** restores the
  project to that pre-turn snapshot. Mutating tools remain locked if a snapshot cannot be created.
- Required editor dependencies are requested through the host and opened through KiCad's normal
  application flow. Tool execution stays off the wxWidgets UI thread and reports progress rather
  than exposing process IDs.
- `inspect.render` attaches bounded schematic, PCB production, PCB assembly/layout, and 3D views to
  the model directly so it can visually review generated work without reopening a cache path
  through another tool. A schematic render without `page` attaches
  a hierarchy overview whose root-level sheet boxes contain scaled child-sheet previews, plus a
  bounded set of referenced subsheets separately in one native export, with page, sheet, instance,
  source-file, bounds, total-count, and truncation metadata. An explicit page can inspect any
  remaining sheet. Preview identity includes every hierarchy file, preventing a changed child page
  from reusing a stale image.
- `design.preview` and `design.apply` privately stage the complete generated hierarchy and require
  a native KiCad netlist with the exact compiled KDS component-pin membership and net names.
  Export eligibility comes from the exact resolved symbol metadata: `on_board no` symbols and
  their endpoints are excluded only from the PCB-oriented netlist comparison and remain subject to
  native ERC. Multi-unit placements compare as one distinct component reference. Apply validates
  again after installation and rolls back on any mismatch.
- The Ubuntu 24.04 AppImage includes the complete checksum-pinned Codex standalone package and
  runtime helpers. KiChad prefers that sibling executable, so end users do not install Codex or
  Node separately; credentials and durable user state remain outside the immutable image.

The embedded design agent is intentionally narrower than a general shell agent. Arbitrary shell,
GUI automation, inherited MCP servers, plugins, apps, browser control, image generation, and
multi-agent execution are disabled. This keeps project mutation inside the guarded KiChad contracts;
live web search remains available for engineering evidence.

### AI tool surface

KiChad advertises six native tools. Each tool owns its schema and implementation in one
`codex_tool_<name>.cpp` file.

| Tool | Implemented operations | Purpose |
| --- | --- | --- |
| `project` | `context` | Discover the active project, document paths, open-editor state, and mutation snapshot gate. |
| `inspect` | `summary`, `find`, `render` | Read bounded native schematics, boards, symbols, and footprints, or attach a native visual view. |
| `design` | `describe`, `context`, `search`, `read`, `compile`, `preview`, `save`, `patch`, `apply` | Author, understand, validate, incrementally edit, and transactionally materialize KDS. |
| `pcb` | `status`, `describe`, `get`, `mutate` | Inspect the exact KiCad 10 protobuf API and perform bounded native live-board transactions. |
| `verify` | `erc`, `electrical`, `drc`, `layout`, `sourcing` | Run native and KDS engineering gates with complete, pageable diagnostics. |
| `fabricate` | `plan`, `export` | Determine release readiness and atomically create the validated production package. |

KDS editing does not require resending an entire file. Codex can search source, page exact regions,
and apply SHA-256-guarded exact-text patches. Initial creation and intentional whole-file replacement
remain available. Invalid, stale, ambiguous, oversized, or NUL-containing edits cannot replace the
sidecar.

### KDS language and compiler

The implemented source/compiler foundation includes:

- bounded UTF-8 s-expression source, explicit units, stable logical IDs, deterministic UUIDv8
  native identities, SHA-256 revision binding, compile plans, diagnostics, and introspection;
- project metadata, title blocks, text variables, field templates, nested schematic hierarchy,
  project/global library dependencies, and current KiCad 10 library tables;
- project components, multi-unit and power/virtual symbols, fields, pin-resolved nets,
  no-connects, automatic same-page wires and cross-page hierarchy interfaces,
  local/global/hierarchical labels, reviewable orthogonal wires, buses, entries,
  aliases, junctions, directives, rule areas, graphics, text, images, tables, and groups;
- KDS-owned project symbols with units, De Morgan styles, graphics, fields, filters, and typed pins;
- KDS-owned project footprints with standard, custom, and numberless stencil-aperture pads,
  padstacks, graphics, text, groups, zones, keepouts, variants, properties, and project-local or
  installed-stock KiCad 10 3D-model assignments;
- pre-mutation existence/confinement checks for authored project and installed-stock 3D models,
  native-loader validation of generated aperture pads, and source-digest-aware transactional
  footprint creation plus native atomic footprint exchange with exact response readback and
  rollback;
- installed KiCad stock symbol and footprint resolution without component-specific hardcoding;
- stackup, complete global board constraints, net classes, all native conditional-rule constraint
  types, arbitrary explicit outline geometry, exact placement, routes/arcs, vias, zones, keepouts,
  board graphics, text, text boxes, tables, images, barcodes, and dimensions;
- a physical layout contract for board dimensions, component relationships, edge/alignment/group
  constraints, track geometry, via counts, path lengths, and bundle skew;
- deterministic synthesis of missing placement and single-layer orthogonal routes on rectangular
  boards, using actual pad centers, courtyards, edge/clearance rules, keepouts, and existing copper;
- typed electrical rail budgets, voltage/current/power derating, thermal estimates, logic-level
  contracts, and component-unit-pin-to-net validation;
- injection-safe ngspice generation and bounded operating-point, transient, DC-sweep, and AC-sweep
  execution for typed passive, diode, BJT, and MOSFET devices, sources, probes, and numeric
  assertions;
- exact component sourcing records containing manufacturer, MPN, primary datasheet, lifecycle,
  distributor SKU, availability, evidence date, and design quantity; and
- firmware/source, programming, power-up, assembly, and functional bring-up handoff declarations.

Unsupported executable intent is rejected before mutation. KDS never silently drops a form that it
cannot lower.

### Verification and production output

A successful `design.apply` means the requested state was materialized; it does not mean the design
is electrically correct or ready to manufacture. The production path separately requires the
applicable gates:

1. compile the exact KDS revision;
2. apply it transactionally and save current KiCad 10 native documents;
3. render and review schematic, production PCB, assembly layout, and 3D views;
4. pass ERC, electrical contracts/simulations, DRC with schematic parity, physical layout, and
   sourcing;
5. prove the native schematic/board component and connectivity inventories agree with KDS; and
6. export through the fixed profile from a private snapshot after visible user confirmation.

The production exporter can create and validate:

- Gerbers and Gerber job data;
- Excellon drill files, maps, and report;
- IPC-D-356 electrical-test netlist;
- pick-and-place data and a KDS-sourced BOM;
- IPC-2581C and ODB++;
- STEP, STEPZ, BREP, GLB, STL, U3D, XAO, VRML, interactive 3D PDF, and fabrication PDF;
- front/back SVG and DXF assembly drawings, board PostScript, and native board renders;
- schematic PDF, SVG, DXF, PostScript, BOM CSV, legacy BOM XML, and native netlist;
- GenCAD and typed JSON board statistics; and
- a digest-checked portable `design/` tree containing the KDS source, current board and complete
  schematic hierarchy, project settings, tables, local libraries, rules, worksheet, and confined
  models.

Artifacts are structurally parsed and matched to KDS rather than accepted only because a command
exited successfully. Export stages into a private directory, writes a hash manifest, and atomically
replaces only `fabrication/`; a failed export retains the previous package.

### Meaning of readiness

`productionReady` means KiChad has enough validated evidence to release PCB manufacturing and
assembly artifacts under its fixed profile. It does not certify regulatory compliance, safety,
EMC, environmental qualification, or fitness for a particular use.

`runningReady` additionally requires hash-bound firmware, reproducible source/dependency identity,
programming intent, current-limited power-up, and ordered measured bring-up/functional acceptance.
The declarations and packaging exist; automated firmware rebuilding, hardware programming, and
test-station execution remain incomplete.

## What is still required

The completed P0 work establishes the source language, guarded compiler/apply path, visual feedback,
electrical gates, baseline physical synthesis, stock libraries, and fabrication boundary. KiChad is
not yet a production release for arbitrary board classes. The following work closes that gap.

### P1: production-release blockers

#### 1. Prove unattended end-to-end board creation

The release definition requires fresh natural-language runs to complete three independent reference
boards:

1. a two-layer 555-based design;
2. an MCU board with USB-C, regulation, and a custom footprint; and
3. a four-layer fine-pitch board with planes and a real physical stackup.

Each run must start from a disposable clean project, author KDS, create reviewable wired schematics,
resolve exact orderable parts, finish placement/routing, pass all applicable gates without hidden
waivers, and install the validated fabrication package. Reapplying the same KDS must converge
without duplicates or native-file drift.

#### 2. Advance physical synthesis beyond simple boards

Explicit KDS geometry can represent considerably more than the automatic synthesizer currently
creates. Arbitrary-board autonomy still needs:

- legal placement inside curved, cutout, and multi-island outlines;
- functional rooms, connector/mechanical anchoring, thermal placement, and high-density
  optimization;
- multilayer routing with legal via selection and plane-aware topology;
- fine-pitch/BGA escape fanout;
- differential-pair routing, impedance-aware topology, length matching, meanders, and bundle
  tuning; and
- push-and-shove or an equivalent deterministic conflict-resolution strategy plus manufacturable
  corner smoothing.

The exit criterion is completion of the three reference boards from constraints, without inserting
hand-authored coordinates merely to make the fixtures pass.

#### 3. Expand electrical-model fidelity

Current typed ngspice coverage is useful for bounded circuit assertions but not adequate for every
mixed-signal, power, high-speed, or RF design. Remaining work includes:

- checksum-bound vendor subcircuits and trustworthy model acquisition;
- transmission-line and IBIS models;
- PWL, behavioral, initial-condition, and richer controlled-source forms;
- noise, pole-zero, sensitivity, FFT, S-parameter, and statistical/Monte Carlo analyses;
- persistent waveform and CSV/Touchstone evidence in the release; and
- typed power-integrity, signal-integrity, isolation/creepage, and worst-case tolerance assertions
  where simulation alone is insufficient.

The correct requirement is adequate evidence for each reference design, not a simulation checkbox
on every component.

#### 4. Make component ingestion completely portable

Installed stock KiCad libraries and project-local libraries work. Production breadth still needs:

- snapshotting user-defined global and package-manager libraries into the portable release;
- model acquisition plus actual STEP/WRL content validation;
- deterministic package-data-to-KLC footprint geometry generation; and
- ownership-safe rename, migration, selective merge, rescue, and publishing workflows for managed
  symbol/footprint libraries.

#### 5. Complete release hardening

Before calling the platform production-ready, the full reference workflow must prove recovery from
injected authentication, app-server transport, sourcing, parser, stale-revision, native IPC,
dependency-launch, validation, cancellation, and export/install failures. Long-running turns need
repeatable cancellation and restart/restore behavior, and the application needs sustained-run and
large-design performance qualification.

The current repeatable build is qualified on Ubuntu 24.04/KDE neon. Reproducible distributable
packages, upgrade/migration policy, and equivalent supported-platform qualification are still
release work.

### P1: complete running-product handoff

These items are not required to order bare PCBs, but they are required before `runningReady` can
mean an automated production-cell result:

- execute declared trusted firmware toolchain adapters inside the bounded release sandbox and prove
  that they reproduce the hash-bound artifact;
- drive supported hardware programmers and ingest signed per-unit programming records;
- generate/qualify reflow profiles and ingest serialized AOI/X-ray results; and
- drive production test stations and ingest signed per-unit measurement and acceptance records.

### P2: language and workflow breadth

These are valuable but are not blockers for the core text-to-fabrication path:

- KDS modules, parameters, templates, includes, and reusable design-block composition;
- named assembly/design variants beyond per-component DNP;
- page-layout/worksheet authoring, complete project settings, auto-annotation, and pin/unit swap;
- configurable fabrication-house profiles, panelization, stencil policy, and custom output presets;
- third-party and legacy import/migration provenance;
- native live schematic editing, editor selection/navigation/appearance, and 3D-viewer camera
  control; and
- typed automation for GerbView, Bitmap Converter, PCB Calculator, Page Layout Editor, and governed
  plugin/package workflows.

Interactive editor-control breadth should be added only where it materially helps create or inspect
the KDS-derived product. It must not become a second button-click automation language.

## Milestones and exit criteria

| Milestone | Deliverable | Exit criterion |
| --- | --- | --- |
| P0 foundation | One KDS representation, guarded apply, real wires/views, engineering gates, baseline synthesis, stock libraries, and production export | Implemented and present in the current release build. |
| M1 simple-board autonomy | Two-layer 555 reference | Fresh prompt to validated fabrication package, clean gates, deterministic reapply, and injected-failure recovery. |
| M2 mainstream MCU autonomy | USB-C/regulator MCU board with a managed custom footprint | Exact sourced parts/models, adequate electrical evidence, reviewable schematic, routed board, and complete portable package. |
| M3 complex-board autonomy | Four-layer fine-pitch reference | Plane-aware stackup, legal fanout/routing, differential/length constraints, clean native gates, and validated outputs. |
| M4 release candidate | Supported installation and sustained autonomous operation | All three proofs repeat reliably; failure matrix, upgrade path, performance bounds, and distribution documentation are complete. |

## Current qualification evidence

The 2026-07-21 P0 qualification run produced:

- a clean 1,065-case `qa_common` suite;
- passing focused real-ngspice and deterministic physical-synthesis cases;
- a successful `RelWithDebInfo` build and install through `tools/build-kichad.sh`; and
- installed KiCad 10.0.4 application, schema, symbol, footprint, 3D-model, and template packages,
  with representative stock symbols and footprints parsed and rendered through the installed CLI.

The 2026-07-22 distribution qualification produced one 583 MiB full Ubuntu 24.04 AppImage
containing KiChad 10.0.4, Codex 0.144.4, stock symbols/footprints/templates/3D models, ngspice,
and runtime-loaded GUI, OpenGL, and database components. Its clean-profile bundled app-server
completed initialize, account/model discovery, thread start/resume, goal lifecycle, and
dynamic-tool registration. The exact same repeatable full build is the GitHub Actions job.

This evidence qualifies the implemented P0 slice. It is not a substitute for the three unattended
reference-board release proofs above.

The repository provides focused disposable integration harnesses for live PCB IPC, KDS apply and
rollback, fabrication, component fabrication, and the stepper-controller reference. These harnesses
must remain opt-in because they launch native editors and mutate only disposable project copies.

## Keeping this status accurate

- Change the facet-level state and gap text in `design_script_capabilities.cpp` with the production
  implementation. Do not maintain a second manually enumerated capability matrix here.
- Do not mark a facet `qualified` merely because its parser exists. Use the qualification contract
  returned by `design.describe`.
- Update this page's date, baseline, milestones, and human summary when a release boundary changes.
- Keep detailed KDS syntax in `kichad-design-script.md` and process/safety design in
  `kichad-codex-architecture.md`; this page should remain the concise answer to “what works and what
  remains?”
