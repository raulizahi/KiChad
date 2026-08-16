# KiChad

KiChad is a Codex-oriented downstream of KiCad.  It preserves the complete upstream history and
keeps the application architecture compatible with KiCad while providing a reproducible local
development environment for AI-assisted work.

Development is pinned to the latest stable KiCad release: **10.0.4**.  Preview releases and the
upstream development branch are intentionally excluded.  The pinned version is recorded in
`.kichad-base-version`, and the build script verifies both the Git ancestry and resulting binary.

See [KICHAD.md](KICHAD.md) for the Linux quick start, repository layout, runtime libraries, and
upstream-sync workflow.  KiChad is an independent project and is not an official KiCad build.
The consolidated [production status and roadmap](docs/production-status.md) records what is
implemented, what has been qualified, the remaining production blockers, and the release exit
criteria. The detailed language reference remains in
[docs/kichad-design-script.md](docs/kichad-design-script.md).

On Ubuntu 24.04 or KDE neon, the repeatable local build is:

```sh
./tools/bootstrap-kichad-ubuntu.sh  # Run once to install native dependencies.
./tools/check-codex-app-server.sh   # Verify the Codex app-server prerequisite.
./tools/smoke-codex-app-server-protocol.sh  # Verify the embedded JSON-RPC contract.
./tools/build-kichad.sh             # Configure, compile, install, and smoke-check.
```

For the distributable Ubuntu 24.04 AppImage, use the same path locally that GitHub Actions uses:

```sh
./tools/bootstrap-kichad-appimage-ubuntu.sh  # Run once.
./tools/build-kichad-appimage.sh
```

The verified image and its portable SHA-256 file are written to `build/appimage/artifacts/`.
The single image includes KiChad, the complete pinned Codex standalone package, KDS/native
tools, schemas, symbols, footprints, templates, ngspice, database drivers, and GUI/3D runtime. It
also includes the complete official KiCad 3D-model library and does not require a host Codex or
Node installation. Local builds and GitHub Actions produce the same full artifact; there is no
reduced variant with a different capability boundary.

The AppImage contains executable code, never user credentials. ChatGPT authentication, durable
Codex state, model/effort preferences, and project conversation indices remain in the user's
KiChad configuration directory. KiChad resolves its bundled Codex beside the application and owns
the `codex app-server` child; `KICHAD_CODEX_EXECUTABLE` remains an explicit developer override.

Build products stay in `build/`, and the runnable installation is written to `build/install/`.
Rerun `tools/build-kichad.sh` after source changes; it uses the tracked CMake preset, defaults to
one build job per CPU, and accepts `KICHAD_BUILD_JOBS` when you need to cap parallelism.
The same full build pins, builds, and installs the official symbol, footprint, 3D-model, and
template repositories at `.kichad-base-version`; it never combines the stable application with
the libraries' moving development branch. Run `./tools/check-kichad-libraries.sh` to make KiCad
parse representative installed standard assets independently of the GUI.
After a successful build, run `./tools/run-kichad.sh` (or `build/install/bin/kicad`) and use
`build/install/bin/kicad-cli version` for a headless check; both launchers set the local runtime
and standard-library paths without changing the system installation. The install keeps the native
executables at `build/install/bin/_kicad` and `build/install/bin/_kicad-cli`; the public names are
small launchers and never resolve back to themselves.

The project manager includes a native docked Codex panel with ChatGPT sign-in, a dynamic account
model catalog, reasoning selection, streaming conversations, and visible app-server status.
Launching KiChad directly starts and owns one `codex app-server` child process and communicates
with it over redirected stdio; no wrapper daemon, MCP server, or separate tool server is involved.
Closing KiChad terminates that owned child. Distribution packages include and prefer their pinned
Codex executable; developer builds use `PATH` unless `KICHAD_CODEX_EXECUTABLE` selects an absolute
executable. The panel's ChatGPT sign-in is
stored by Codex in an isolated KiChad Codex home, not in the project; set `KICHAD_CODEX_HOME` only
when you intentionally want a different state location.  The design-tool boundary and safety model
are documented in [docs/kichad-codex-architecture.md](docs/kichad-codex-architecture.md).  Each
submitted turn first snapshots the project through KiCad's local-history system, and the panel can
restore that complete pre-turn state.  The initial native `project` and `inspect` calls expose
project context and bounded, read-only KiCad 10 design inspection without shell or GUI automation.
`inspect.render` uses the matching native KiCad backend to attach cropped schematic pages, a
production PCB (`pcb2d`), assembly/layout PCB (`pcblayout`, including Fab fields and courtyards),
or 3D board PNG directly to the Codex tool result, so the model can review actual generated documents
while iterating; these images are derived previews under `.kichad/previews/`, not another design
representation. Schematic preview revisions hash the complete referenced hierarchy, so changing a
child sheet cannot reuse a stale image. Omitting `page` renders a hierarchy overview with each
root-level sheet box populated by a scaled preview of that child, plus up to 24 referenced
subsheets as separate full-size model-visible images in one native export; the response reports
truncation, and an explicit page renders any one page. Superseded images for the same view are removed after a successful render. KDS
`place` declarations can independently control each footprint Reference and
Value field's visibility, absolute position, presentation layer, size, stroke, angle,
justification, and font styling. The Ubuntu bootstrap installs Poppler for the bounded PDF-to-PNG
stage. The
`design` call returns bounded paged semantic context, reads exact source, compiles, previews,
atomically saves, and transactionally
applies reusable `.kicad_kds` project sidecars. Preview and apply both stage the generated schematic
hierarchy privately and require KiCad's native netlist to match every compiled KDS net name and pin
before reporting successful lowering; the comparison uses each resolved symbol's native
`on_board` eligibility so schematic-only power and ERC symbols remain covered by ERC without being
mistaken for missing PCB-netlist components. Apply repeats that check against the installed files. The
`pcb` call exposes the exact protobuf field
schema and connects directly to the open PCB Editor through KiCad 10's protobuf IPC API for bounded
live reads and snapshot-gated, native undoable transactions. A successful `design.apply` saves the
live board but explicitly reports verification as `not_run`; Codex must render the affected views
and then repair KDS until ERC, DRC, and physical-layout acceptance are clean. The read-only `verify` call runs the
matching sibling KiCad 10.0.4 ERC or DRC engine (including DRC schematic parity), rejects reports
from any other KiCad version, and returns complete counts plus bounded pageable violations. Its
`electrical` evaluates typed rail budgets, component derating, thermal and logic contracts, plus
bounded operating-point, transient, DC-sweep, and AC-sweep simulations through `ngspice`. Its
`layout` operation evaluates the single KDS file's maximum board dimensions, semantic component
relationships, routing geometry, per-net via/length limits, and bundle skew with measured failure
details; the same result gates fabrication readiness. Its
`sourcing` operation compiles the project's one KDS sidecar and fails physical components whose
cached evidence is incomplete, stale, unavailable, or not active.

An optional KDS `synthesize` policy deterministically fills missing placement and single-layer
orthogonal routing for rectangular boards from the outline, native footprint pads/courtyards, clearances,
existing copper, and keepouts. It emits the same exact placement/route IR as authored geometry and
fails with corrective steering when no legal path exists. Declared KiCad stock global symbol and
footprint libraries are resolved generically from the installed stock set; portable custom parts
remain project libraries rather than hidden component-specific exceptions.

Native tool failures use a versioned, model-readable error contract rather than a generic failure
flag. Codex receives the failed stage, stable error code, safe request context, expected/observed
details when available, whether project state may have changed, retryability, and ordered recovery
steps. The Codex transcript displays the same error message and recovery summary so a user can see
why a turn is still running or what blocked it.

The native `fabricate` call plans and exports the fixed `kichad-production-10.0.4-v17` release
profile. It accepts only the current KiCad 10.0.4 board and schematic formats, binds the request to
the exact compiled KDS SHA-256, and requires electrical designs to declare typed qualification plus
KDS declarations for ERC, electrical, DRC, layout, sourcing, fabrication,
Gerber, drill, IPC-D-356 electrical-test, placement, and BOM intent plus an explicit physical
stackup. Every schematic net must explicitly select `wired` or `labels` presentation, and a design
with electrical connectivity cannot pass the release plan as an implicit or entirely label-only
drawing. Wired nets compile into real orthogonal KiCad paths anchored to resolved native pins,
with deterministic lane separation, junctions, collinear normalization, and exact global net-name
anchors for schematic/PCB parity. Export reruns the gates and the matching sibling `kicad-cli` from a private bounded project
snapshot, so KiCad cannot rewrite live local settings while checking or plotting. The snapshot
includes project-local symbol and footprint libraries referenced by its native tables. A visible
final-action confirmation is mandatory; ignored checks or exclusions also require explicit waiver
approval. Every installed package includes a digest-checked `design/` tree containing the exact KDS
sidecar, current board and complete schematic hierarchy, project tables/libraries/rules, project
file, worksheet, and confined local 3D models. The release can therefore be reopened or imported
without reaching back into the original working directory. The native board's
schematic-footprint reference/library-ID inventory must exactly match KDS, and the completed BOM and
placement reference sets must exactly match the compiled physical and non-DNP component sets.
KiChad validates every native artifact, writes a hash manifest, and atomically replaces only
`fabrication/`; any failure preserves the prior package. Native KiCad plot timestamps are retained,
so the manifest records exact bytes for each run rather than claiming byte-identical Gerbers across
separate runs. Optional STEP, multipage fabrication PDF, inspectable IPC-2581C XML, and ODB++ can be
declared in the same KDS sidecar. The same single output representation now covers separate
front/back SVG and DXF assembly drawings, GenCAD, VRML, BREP, GLB, STL, STEPZ, U3D, interactive 3D
PDF, XAO, and typed JSON board statistics; each is generated by KiCad 10 with an explicit
deterministic profile and accepted only by its dedicated bounded structural validator. A native
KiCad s-expression netlist is also matched exactly against the compiled KDS component and ref/pin
connectivity sets. Native root-schematic release drawings can be declared as PDF, SVG, DXF, and
PostScript outputs, while `board_ps` emits one validated A4 PostScript drawing per enabled physical
board layer. `schematic_bom` emits KiCad's fixed five-column native CSV and `legacy_bom_xml` emits
the Eeschema `export version="E"` interchange document; both are parsed and matched exactly to the
compiled KDS component reference, value, footprint, and population intent. Each page set is
identity-checked and structurally validated. `board_render` creates a lossless transparent native
3D top-view PNG whose complete chunk CRCs, zlib scanlines, exact RGBA dimensions, and nonblank image
content are validated. IPC-2581 is fixed to
millimetres and precision 6, then parsed and
structurally checked against the planned board and KDS references. ODB++ is fixed to an ODB 8.1,
millimetre, precision-4 ZIP whose complete bounded archive and manufacturing structure are checked
without extraction.

KiChad forces the Codex app-server's built-in web search to live, high-context mode for new and
resumed project conversations. The embedded agent instructions require current manufacturer,
datasheet, lifecycle, and distributor evidence before a component can be accepted into a design;
that exact evidence is written into the component's KDS `source` form and checked by the native
sourcing gate. There is no parallel sourcing database or generated context document. GUI browser
automation and inherited MCP connectors remain disabled.

KiChad Design Script is the versioned source language Codex uses to describe a complete design.
A `project.kicad_kds` sidecar lives beside the normal project, schematic, and board files; KiChad
shows it in the project tree and can load, compile, preview, save, or apply it without losing source
text. KDS is the single external design representation: Codex reads and writes the same compact,
self-describing source that is exported with the project, while compiler IR and protobuf messages
remain private implementation details. Physical board statements use explicit units and stable
logical IDs; preview reports their deterministic target identities without changing the board. The
sidecar declares libraries, components, nets, board intent, rules, sourcing, checks, fabrication
outputs, assembly process/acceptance, self-contained device code, exact firmware, programming, current-limited power-up, and measured bring-up intent, while
ordinary KiCad and release-package files remain compiler artifacts. `fabricate.plan` distinguishes a
manufacturable `productionReady` board from a complete `runningReady` handoff. The format, grammar, safety rules,
and production support criteria are documented in
[docs/kichad-design-script.md](docs/kichad-design-script.md).
Double-clicking a `.kicad_kds` file in the project tree opens KiChad's integrated KDS source tab
with line numbers, s-expression highlighting, brace matching, undo/redo, automatic background
validation, atomic save with external-change protection, compile status, and SHA-256 revision
display. Diagnostics appear directly above the source, are deduplicated, mark the affected line,
and provide a **Go to issue** action when a source location is available. Use **Compile** or
`Ctrl+Enter` to validate immediately and **Save** or `Ctrl+S` to write the sidecar; invalid
work-in-progress source can still be saved without pretending it is compilable. The editor passes
Scintilla's exact logical UTF-8 byte span to the compiler, excluding its terminal C byte. If a file
really contains embedded NUL bytes, KiChad offers a snapshot-protected repair that replaces them
with spaces rather than silently corrupting or truncating the source.
The native `design.describe` operation also returns the authoritative AI-readable coverage catalog:
every design, verification, manufacturing, interchange, editor, and auxiliary-application facet is
marked `qualified`, `partial`, or `unrepresented` with explicit remaining gaps. The catalog is
compiler introspection, not a second design representation.

Codex can locate exact source with `design.search`, page only the required lines with bounded
`design.read`, and update an existing sidecar with `design.patch` instead of retransmitting the
whole KDS. Each ordered edit supplies exact `oldText` and `newText`, and the request includes the
`sourceSha256` returned by `design.search` or `design.read`. KiChad rejects stale revisions, missing or
ambiguous edit contexts, oversized output, embedded NUL bytes, and any candidate that does not
compile. Only a valid candidate is atomically installed, so a failed edit leaves the original
sidecar untouched. `design.save` remains available for initial creation or an intentional
whole-source replacement; KDS remains the single authored representation in both workflows.

For an opt-in transaction proof, first open a disposable project copy in the installed PCB Editor,
then run `tools/smoke-kichad-live-ipc.sh --allow-mutation PROJECT_DIRECTORY BOARD_FILE`.  The smoke
test creates, field-mask updates, and deletes one temporary trace through the official KiCad 10 IPC
transaction API; it is never run implicitly by the build.

`tools/smoke-kichad-stepper-reference.sh --allow-mutation` is the end-to-end running-board proof.
It opens only a disposable current-format project, compiles and applies the committed
`reference_stepper_controller.kicad_kds`, generates its managed symbols/footprints and routed board,
requires clean native ERC/DRC, then installs the complete fabrication, portable design, exact Nano
firmware/source, programming, dual-supply, and bring-up package. Set
`KICHAD_KEEP_REFERENCE_OUTPUT=1` to retain that disposable package for inspection.

For the self-contained KDS transaction proof, run
`tools/smoke-kichad-kds-apply.sh --allow-mutation`. It launches a disposable build-tree PCB Editor
with an isolated configuration and project copy, applies the committed KDS fixture, and proves that
a repeated apply converges the same thirteen managed identities—twelve authored PCB primitives and
one compiler-created footprint—without duplicates. It also applies and
reapplies a two-file hierarchical schematic, preserves the existing root screen UUID and unmanaged
title-block company field, creates stable sheet/pin/interface UUIDs, proves the second apply is
byte-idempotent, injects a native-validation failure to prove exact rollback, and exports the
resulting hierarchy through the real `kicad-cli` schematic loader. The same proof resolves exact
project-local resistor symbols, places units on root and child sheets with rotation/mirroring,
places a real derived virtual `GND` power symbol with the canonical `(footprint none)` spelling,
flattens its inheritance into the native cache, preserves each symbol's native
BOM/board/position/simulation flags, attaches project-global signal and power
nets to resolved pin coordinates, and checks their exact nodes in the exported netlist. The native schematic proof also
reconciles explicit label presentation and generated reviewable wired-net paths, a name-owned bus alias, plus stable-ID wires, junctions,
buses, diagonal bus entries, net-targeted directive flags, and polygonal schematic rule areas with
border-attached directives, plus stable multiline free text and text boxes with complete geometry,
border/fill, typography, and hyperlink state, and all native schematic polyline, rounded-rectangle,
circle, arc, and cubic-Bézier geometries, plus digest-verified self-contained images with semantic
descriptions, AI-native schematic table grids with explicit dimensions, complete per-cell
typography/fill, rectangular merges, stable identities, and native rotation, plus named locked
schematic groups with typed membership and safe nested containment, and complete per-unit
component-field placement, typography, visibility, privacy, autoplace policy, color, and
hyperlinks. It also applies and
reads back the one authored physical stackup—including finish, impedance policy, bevelled edge
connector, edge plating, masks, paste, silkscreen, copper, and locked dielectric properties—through
KiCad's native stackup API. The same live proof applies and reads back the complete global Board
Setup constraint set through a typed native rules endpoint, including physical via consistency and
the semantic legacy copper-edge mode. It also applies and reads back the one canonical KDS net-class
table—including explicit priority order, inherited fields, via and microvia geometry, schematic
styles, colors, tuning profiles, and pattern assignments—then proves an invalid native replacement
is rejected without mutation. The same transaction completely replaces project text variables and
ordered schematic field-name templates through typed native APIs, verifies exact live and on-disk
readback, keeps editor caches synchronized with setting-specific notifications, repeats without
drift, and restores both sets after an injected downstream failure. It compiles the canonical KDS
custom-rule set into one internal native rule document, loads all rule semantics through KiCad's
real DRC engine, reads the exact document back, and proves malformed replacement input cannot change
the active rules. It also generates the complete native project symbol and footprint tables from the
same KDS declarations, validates them
with KiCad's parser, repeats them byte-for-byte, and covers exact rollback. It also compiles a
deterministic KDS-owned symbol library from AI-native metadata, properties, common/numbered units,
named and De Morgan body styles, unit display names and locking, footprint filters, fully laid-out
mandatory/custom fields, all native vector/text graphics, and typed pins; the generated current KiCad 10 format is atomically
installed, native-loader validated, journaled, and restored exactly after an injected rejection. It also compiles a
deterministic KDS-owned footprint library from semantic metadata, standard/custom/chamfered
SMD/connect/PTH/NPTH pads, front/inner/back and named-inner padstacks with per-layer custom copper,
top/bottom backdrilling, complete per-pad teardrop geometry and policy, tenting, and post-machining,
fixed and curved artwork, polygons and fills, rich text and text boxes, local
pad and footprint-wide mask/paste/clearance/thermal policy, custom copper stacks, private layers,
stable-ID displayable metadata properties, native component-class membership, jumper/net-tie groups, footprint-local copper zones and
keepouts, nested stable-ID item groups, explicit assembly variants, and project-local or installed
KiCad 10 stock 3D model transforms, including numberless side-specific stencil-aperture pads;
the current KiCad 10 `.kicad_mod` artifacts are whole-library swapped, native-loader
validated, journaled, and restored exactly after an injected rejection. Project and installed-stock
model assets are resolved and confined before mutation, while live footprint instances retain an
exact source digest so both library-ID changes and same-name geometry revisions are replaced once,
read back through typed IPC, and rolled back as one editor transaction on any mismatch. Board vias also carry
AI-native arbitrary per-layer copper geometry including custom primitives, explicit unconnected-ring
and forced-flash policy, complete per-via teardrop geometry and policy, top/bottom backdrilling, tenting, covering,
plugging, filling, capping, and post-machining intent through KiCad's official typed padstack IPC
message. It creates and fills a
deterministic copper zone through KiCad's official zone engine, creates a distinct locked keepout
rule area with exact prohibited-item policy, creates native multiline board text with deterministic
typography, and creates all five native dimension styles with exact geometry and measurement policy.
The manufactured board contour supports stable line, rounded-rectangle, arc, circle, polygon, and
Bezier geometry, including cutouts and multiple islands, through the typed board-shape API.
Those same primitives also create arbitrary-layer board artwork with native net ownership and
paired solder-mask expansion, including sequential inner-copper and custom user layers.
Board text boxes use that same AI-readable, stable-ID authoring model and become editable native
`BoardTextBox` objects with rectangle or polygon geometry, four-sided margins, border style,
typography, justification, hyperlink, knockout, layer, and lock state. Board tables remain a
distinct implementation and become atomic native `BoardTable` objects with ordered dimensions,
merged grids, independently styled borders/separators, and deterministic owned cells.
It also resolves and places an existing schematic-linked footprint on the back side while proving
the footprint UUID, symbol path, pad UUID, and flipped pad layers are preserved. A second absent
footprint is parsed from the declared project-local `.pretty` library, linked to its deterministic
hierarchical symbol path, assigned its pad net, and created with a deterministic KDS-owned instance
UUID in the same native transaction. Repeat apply proves it is not duplicated; removing and
restoring its placement proves exact deletion and recreation with the same identity. The committed
board, schematic, symbol, and footprint fixtures are serialized in the exact formats emitted by
KiCad 10.0.4, and the smoke test rejects stale fixture versions before opening the editor. The
harness also runs the native `verify` tool against the current-format schematic and board, proving
the real 10.0.4 ERC/DRC JSON contracts and schematic-parity category. Unit coverage separately
proves the deterministic KDS sourcing gate, including physical-versus-virtual coverage, freshness,
lifecycle, stock, malformed evidence, and failure paths. It never connects to or stops an existing
KiChad process.

For the real fabrication integration proof, build `qa_common` and `kicad-cli`, then run
`tools/smoke-kichad-fabrication.sh --allow-mutation`. The harness uses isolated configuration and a
disposable copy of the exact current-format fixture, runs native ERC/DRC and every production export,
checks the installed manifest and artifacts, proves live `.kicad_prl` state is untouched, and removes
the temporary project afterward.

For the complete KDS-to-fabrication component proof, also build `pcbnew` and run
`tools/smoke-kichad-fabrication-component.sh --allow-mutation`. It launches only a disposable PCB
Editor, applies and saves a sourced two-resistor KDS design through official IPC, requires clean
native ERC and DRC with zero ignored checks, and proves that the native board inventory, routed
placement CSV, KDS BOM, and production manifest contain the same exact component references.

Developers can run `tools/generate-codex-protocol-schema.sh` to inspect the exact protocol exposed
by their installed Codex app-server without committing generated schemas.

---

# KiCad upstream README

For specific documentation about [building KiCad](https://dev-docs.kicad.org/en/build/), policies
and guidelines, and source code documentation see the
[Developer Documentation](https://dev-docs.kicad.org) website.

You may also take a look into the [Wiki](https://gitlab.com/kicad/code/kicad/-/wikis/home),
the [contribution guide](https://dev-docs.kicad.org/en/contribute/).

For general information about KiCad and information about contributing to the documentation and
libraries, see our [Website](https://kicad.org/) and our [Forum](https://forum.kicad.info/).

## Build state

KiCad uses a host of CI resources.

GitLab CI pipeline status can be viewed for Linux and Windows builds of the latest commits.

## Release status
[![latest released version(s)](https://repology.org/badge/latest-versions/kicad.svg)](https://repology.org/project/kicad/versions)
[![Release status](https://repology.org/badge/tiny-repos/kicad.svg)](https://repology.org/metapackage/kicad/versions)

## Files
* [AUTHORS.txt](AUTHORS.txt) - The authors, contributors, document writers and translators list
* [CMakeLists.txt](CMakeLists.txt) - Main CMAKE build tool script
* [copyright.h](copyright.h) - A very short copy of the GNU General Public License to be included in new source files
* [Doxyfile](Doxyfile) - Doxygen config file for KiCad
* [INSTALL.txt](INSTALL.txt) - The release (binary) installation instructions
* [uncrustify.cfg](uncrustify.cfg) - Uncrustify config file for uncrustify sources formatting tool
* [_clang-format](_clang-format) - clang config file for clang-format sources formatting tool

## Subdirectories

* [3d-viewer](3d-viewer)         - Sourcecode of the 3D viewer
* [bitmap2component](bitmap2component)  - Sourcecode of the bitmap to PCB artwork converter
* [cmake](cmake)      - Modules for the CMAKE build tool
* [common](common)            - Sourcecode of the common library
* [cvpcb](cvpcb)             - Sourcecode of the CvPCB tool
* [demos](demos)             - Some demo examples
* [doxygen](doxygen)     - Configuration for generating pretty doxygen manual of the codebase
* [eeschema](eeschema)          - Sourcecode of the schematic editor
* [gerbview](gerbview)          - Sourcecode of the gerber viewer
* [include](include)           - Interfaces to the common library
* [kicad](kicad)             - Sourcecode of the project manager
* [libs](libs)           - Sourcecode of KiCad utilities (geometry and others)
* [pagelayout_editor](pagelayout_editor) - Sourcecode of the pagelayout editor
* [patches](patches)           - Collection of patches for external dependencies
* [pcbnew](pcbnew)           - Sourcecode of the printed circuit board editor
* [plugins](plugins)           - Sourcecode for the 3D viewer plugins
* [qa](qa)                - Unit testing framework for KiCad
* [resources](resources)         - Packaging resources such as bitmaps and operating system specific files
    - [bitmaps_png](resources/bitmaps_png)       - Menu and program icons
    - [project_template](resources/project_template)          - Project template
* [scripting](scripting)         - Python integration for KiCad
* [thirdparty](thirdparty)           - Sourcecode of external libraries used in KiCad but not written by the KiCad team
* [tools](tools)             - Helpers for developing, testing and building
* [translation](translation) - Translation data files (managed through [Weblate](https://hosted.weblate.org/projects/kicad/master-source/) for most languages)
* [utils](utils)             - Small utils for KiCad, e.g. IDF, STEP, and OGL tools and converters
