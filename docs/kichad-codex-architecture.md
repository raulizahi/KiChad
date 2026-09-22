# KiChad Codex architecture

KiChad targets the exact stable KiCad version named in `.kichad-base-version`; today that is
KiCad 10.0.4.  There is one supported code path.  Development heads, release candidates, KiCad 9,
legacy SWIG automation, GUI automation, and multi-version compatibility layers are out of scope.

## Process boundary

The KiChad project-manager process owns the docked chat panel and starts `codex app-server` as its
Codex inference and conversation process.  It speaks newline-delimited JSON-RPC over redirected
stdio.  ChatGPT authentication, the account's live model catalog, reasoning options, persistent
threads, streaming events, and server-initiated dynamic-tool calls all use the app-server protocol.

Dynamic tool requests are handled by the KiChad host.  KiChad does not run an MCP server,
middleware daemon, tool-server process, or web UI.  The app-server process is never given an
arbitrary KiChad tool that executes shell commands or drives the GUI.  Unrecognized tool calls are
rejected with a structured JSON-RPC error.  The owned process starts with MCP, shell, unified exec,
apps, browser/computer use, image generation, plugins, and multi-agent features disabled, so it
cannot inherit broader tools or MCP connectors from the user's global Codex configuration. Codex
web search is forced to `live` with high context for both new and resumed conversations. The agent
instructions require it to verify the manufacturer, exact MPN, primary datasheet, lifecycle, and
current distributor availability before accepting each component. The agent caches those exact
facts in the component's KDS `source` form and invokes the native sourcing gate. Built-in web search
is separate from GUI browser automation, remains enabled while `browser_use` is disabled, and
cannot mutate the project by itself.

Set `KICHAD_CODEX_EXECUTABLE` when `codex` is not on `PATH`.  The executable is an installation
prerequisite, not a linked build dependency.  The owned child uses an isolated Codex home at
`~/.config/kichad/codex` by default, preventing global Codex configuration, MCP connectors, hooks,
plugins, or sessions from leaking into the design agent.  Codex stores the panel's native ChatGPT
authentication there; KiChad never stores access tokens in a project or its settings.  Set
`KICHAD_CODEX_HOME` to override that state location.

Before each submitted Codex turn, the project manager asks KiCad's registered schematic, board,
and project savers for a coherent incremental-history snapshot and records its commit identifier.
The panel's **New conversation** action starts a fresh thread bound to the current tool set and
policy and, by default, seeds it with the project's saved transcript (recovered from
`codex_dialog.txt` when the saved binding is gone) so stated requirements survive; clearing the
history is an explicit choice in the same dialog.
The panel's **Revert turn** action closes open editors through the normal KiCad flow and restores
that exact pre-turn state.  If a snapshot cannot be established, mutating native tools stay locked;
read-only conversation and inspection can continue.

The eight advertised native tools are `project`, `inspect`, `design`, `pcb`, `verify`, `fabricate`,
`diagram`, and `document`.
`project` reports the active design context and snapshot gate.  `inspect` parses KiCad 10 schematic,
board, symbol, and footprint s-expressions in-process and returns structural summaries or bounded
matching expressions.
It accepts only existing project-relative paths, resolves symlinks before enforcing the project
root, checks the file extension against the document root, and caps input/output sizes.  Its only
writes are derived artifacts: `render` previews under `.kichad/previews/`, and `pdf` documents.
`inspect.pdf` plots a complete schematic hierarchy (drawing sheet and colours retained, property
popups excluded) or a multipage board layer set through the sibling `kicad-cli` into a
project-confined `.pdf` destination, defaulting to `documentation/<stem>.pdf` and
`documentation/<stem>-board.pdf`.  The destination must end in `.pdf`, may only replace a file
that already carries a PDF signature, and is validated for a PDF header and trailer before its
size and SHA-256 are reported.  It has no fabrication gates; the fabrication package's
`schematic_pdf` and `pdf` outputs remain the gated documents of record.
`diagram` renders block and architecture diagrams natively: the agent supplies Mermaid flowchart
source (directions, every standard node shape, labelled solid/dotted/thick links, chains, `&`
fan-out, nested subgraphs, `classDef`/`class`/`:::`/`style` colours) and KiChad parses it
in-process, runs a deterministic layered layout (cycle-tolerant longest-path ranking, barycenter
ordering, one band per top-level subgraph so boxes never overlap foreign nodes), and plots a
single custom-sized PDF page through KiCad's own `PDF_PLOTTER`.  No browser, Node runtime, or
Mermaid renderer is involved.  The PDF lands at a project-confined `.pdf` destination
(`documentation/<name>.pdf` by default) with the Mermaid text saved beside it as `<stem>.mmd`,
and the response attaches a `pdftoppm` PNG preview when that rasterizer is available so the
agent can review the drawing it produced.
Native tool calls run one at a time, and the panel now says which call is still running and for
how long when it refuses a concurrent one.  Pressing **Stop** requests cancellation through the
tool registry: a running external place-and-route child is terminated, its partial output
directory removed, and the call returns `cancelled` with the project untouched, so an interrupted
turn cannot leave the executor permanently busy.  The router's copper layer count comes from the
user's External Layout preference; `layout` accepts no per-call override, and a KDS stackup
declaring a different copper count fails with `layer_count_mismatch`.

External place and route is per board.  A project may hold several designs, one KDS per board
paired with `<name>.kicad_pcb`; `layout.path` names which one an operation acts on, `run` routes it
into its own `<sibling>-<stem>` directory, and `adopt`, `revert`, and `reconcile` act on that board
alone, each keeping its own pre-layout backup.  External routers take one board per run and need
no new flag: a multi-board project is staged into a private directory holding only the target
board, its KDS, project file, and schematics, with shared libraries copied along, so the
invocation keeps the original `--input-dir/--output-dir/--layers` contract.  A single-design
project is handed over directly.

The sourcing gate requires DigiKey, Mouser, or Newark stock evidence unless the user approves
another distributor.  That approval is recorded on the component's KDS source as
`distributor_exception` (the user's own words) and `distributor_exception_approved_on` (the date),
either without the other being a compile error.  With both present the gate stops failing and
instead reports the exception under `sourcing.distributorExceptions`, which keeps the release clean
while the fabrication manifest repeats supplier, date, and approval, so an approved deviation is
never silently forgotten and never has to be approved twice.

`document` is the agent's only route to PDF content, because the owned Codex process has no shell,
no file tool, and no chat attachments.  Reading is native: `pdf_text_document.cpp` indexes objects
by scanning for `N G obj` (so damaged cross-reference tables still open), decodes Flate, LZW,
ASCIIHex, ASCII85 and RunLength streams with predictors, expands object streams, walks the page
tree, and interprets content streams with the text matrix, font widths, ToUnicode CMaps and
simple-font encodings before assembling glyph runs into column-preserving lines.  `fetch` downloads an `https` PDF (64 MiB cap, redirects
followed, signature checked) into the project's `datasheets/` directory through KiCad's libcurl
wrapper, `import` copies a PDF the user named by absolute path (honoured only below the user's home
directory or inside the project) into the same place, `list` enumerates project PDFs, `read`
returns `pdftotext` layout text for a bounded page range with page markers, `search` reports
case-insensitive matches with page numbers across every page, and `render` attaches one page as a
PNG for pinout tables and figures; that one operation is optional and needs poppler's `pdftoppm`,
reported as `dependency_unavailable` when absent.  Everything else runs in-process.

Previews never need an external rasterizer either: `inspect.render` and the `diagram` preview plot
SVG (through `kicad-cli` or KiCad's SVG plotter) and rasterize it in-process with the vendored
nanosvg, so `kicad-cli` is the only executable KiChad depends on.
`design` is the compiler front end for reusable `.kicad_kds` KiChad Design Script sidecars.  It
describes the versioned language, reads the exact bounded source as model context, compiles either
inline source or a project-confined sidecar into a private deterministic validated IR and pass plan,
previews typed physical board statements and schematic hierarchy files through their KDS logical
and stable UUIDv8 identities,
atomically saves only valid programs, and applies the currently supported physical subset behind the
pre-turn snapshot gate. Read and preview are bounded and read-only. Compiler IR and protobuf
payloads are deliberately not exposed as a second design representation. Existing sidecars require
a matching SHA-256 revision before replacement. KDS has
no host-language or shell escape; it declares project metadata, libraries, schematic hierarchy,
components, connectivity, board intent, design rules, sourcing, verification, and outputs.  See
`kichad-design-script.md` for the file contract and support policy.
`pcb` discovers the matching open board and instance token through KiCad 10's supported protobuf IPC
API.  Its bounded `describe` operation exposes exact protobuf JSON fields and nested enum values, so
the model does not infer message shapes.  It can read typed live items and create, field-mask update,
or delete footprints, traces, vias, arcs, copper zones, rule areas, graphics, text, text boxes, tables, and all five
native dimension styles. Each mutation requires the pre-turn snapshot and is enclosed by KiCad
`BeginCommit`/`EndCommit`; any validation, item-status,
transport, or commit failure drops the pending commit.  IPC requests have bounded timeouts and
execute off the wxWidgets UI thread; socket paths and instance tokens are not returned to the model.
KiChad persists the required API-enabled setting in its isolated configuration before it launches
editor children.  If a previous transaction response was lost, the next mutation first drops that
client's orphaned commit and retries `BeginCommit` once.

`verify` runs the sibling executable from the exact same KiChad installation as a bounded,
read-only KiCad backend. `erc` accepts only a project-confined `.kicad_sch`; `drc` accepts only a
project-confined `.kicad_pcb` and always enables schematic parity. Both request KiCad's JSON report
with errors, warnings, and exclusions in millimetres. KiChad rejects malformed reports, a source or
schema mismatch, and any report whose exact major/minor/patch version differs from the host. The
model receives complete severity and category counts, explicit ignored-check/waiver state, and a
bounded pageable list with the volatile report date removed. The current backend verifies the
on-disk artifact named by `path`; it does not silently save a dirty editor document.

The same native `verify` function has a `sourcing` operation for a project-confined `.kicad_kds`.
It compiles the exact sidecar, requires one complete source record for every footprint-bearing
component, and rejects unavailable, non-active, future-dated, or stale evidence. Footprintless
virtual and power symbols do not require distributor records. Evidence contains the manufacturer,
exact MPN, HTTPS datasheet and distributor URLs, lifecycle, supplier, SKU, reported stock,
verification date, and design quantity. Freshness defaults to seven days and can be tightened or
relaxed from one through ninety days in the explicit call. The result uses the same AI-oriented
shape as ERC/DRC: complete severity counts and a bounded pageable issue list. This deterministic
gate validates the evidence cached by Codex's live search; it does not claim to re-fetch or
independently attest third-party web content.

`fabricate` has a read-only `plan` operation and a permission-gated `export` operation. Both bind a
project-confined current-format board and root schematic to the exact SHA-256 of a compiled KDS
sidecar. The fixed `kichad-production-10.0.4-v17` profile requires explicit stackup and schematic
net-presentation intent, at least one reviewable wired path when nets exist, ERC, DRC,
the canonical KDS physical-layout contract and layout qualification, sourcing, and fabrication
checks, plus Gerber, drill, IPC-D-356 electrical-test, placement, and BOM
outputs; STEP, STEPZ, BREP, GLB, STL, U3D, XAO, interactive 3D PDF, fabrication PDF, IPC-2581C XML,
ODB++, front/back assembly SVG/DXF, GenCAD, VRML, and typed board statistics are optional
declarations, as are native schematic BOM CSV, legacy BOM XML, and a lossless native 3D board-render
PNG. PCB PostScript emits an exact A4 drawing set for every enabled physical layer.
Root-schematic PDF, SVG, DXF, and PostScript release drawings use the same guarded schematic snapshot
as ERC. A native Eeschema netlist can also be declared and is matched against the
exact compiled KDS component and ref/pin connectivity sets. Both BOM formats are parsed with
structural limits and matched exactly to compiled KDS component
reference, value, footprint, and DNP intent. Each auxiliary artifact has an exact planned path and a
dedicated bounded structural validator. IPC-2581 export uses one fixed
millimetre/precision-6 representation and is parsed to verify its schema identity, native producer,
board step, stackup, layers, and KDS component references. ODB++ uses one fixed ODB 8.1
millimetre/precision-4 ZIP representation; validation streams every
entry without extraction, rejects unsafe or ambiguous archive metadata, bounds expanded data, and
checks the matrix, board step, layer features, netlist, EDA/component data, native producer, and KDS
references. Final export requires the pre-turn snapshot and a separate visible host confirmation
that the model cannot forge in tool arguments. Planning structurally compares the
native board's enabled production layers and complete ordered stackup—including thicknesses,
materials, dielectric values/locks, finish, impedance, connector, and plating policy—to compiled
KDS intent. KiChad copies the bounded verification and plotting inputs into a private directory,
reruns native ERC/DRC and KDS sourcing there, and
rejects errors, warnings, stale input, or unapproved ignored-check/exclusion state. It then invokes
only the exact sibling `kicad-cli`, creates the KDS-sourced BOM, validates expected artifact counts,
paths, sizes, signatures, Gerber-job structure, and SHA-256 values, and writes a structured manifest.
The same private snapshot is copied into a bounded, digest-verified `design/` release tree: exactly
one KDS source plus current native board/schematic hierarchy, project settings, library tables,
project symbols/footprints, rules, worksheet, and confined local models. Local UI preference files
and history are excluded. Manifest source entries name both their original project-relative path and
portable package path.
The validated staging directory atomically replaces only `fabrication/`; installation failure rolls
the prior directory back. Private snapshots, logs, and staging are removed on every tested success
and failure path; a backup-cleanup failure is surfaced as `backupRetained` after a verified install
instead of deleting the new package. KiCad-generated plot timestamps remain in native outputs and
are reflected in the per-run hashes rather than normalized away.

`tools/smoke-kichad-live-ipc.sh` provides an explicit, opt-in integration proof against an already
open disposable board copy.  It creates, commits, field-mask updates, commits, deletes, and commits a
temporary trace while checking per-item statuses and preservation of unmasked geometry.  Normal
builds and test runs never invoke this mutating smoke test.

`tools/smoke-kichad-kds-apply.sh --allow-mutation` is the self-contained compiler integration proof.
It starts only its own build-tree PCB Editor, uses isolated settings and a temporary copy of the
committed fixture, applies the KDS sidecar, and repeats the apply to prove stable object identity and
duplicate-free convergence. It applies the single complete KDS net-class table through the typed
native project API, reads every class and assignment back, rejects an invalid atomic replacement,
and verifies the prior table did not change. Project text variables and ordered schematic field-name
templates use dedicated typed IPC messages and one focused client module; apply inventories,
journals, replaces, reads back, and reverse-rolls back each complete set. Setting-specific editor
notifications synchronize only the affected caches, and a serializer failure restores the prior
in-memory state while preserving the existing project file. It also lowers the single KDS
custom-rule
representation to one internal native document, installs it through a bounded board endpoint,
reloads KiCad's DRC engine, verifies exact readback, and proves invalid input cannot mutate the
active rules. It generates and repeats exact KiCad-parsed project symbol and footprint tables from
the canonical KDS library declarations. It verifies a deterministic managed copper zone is filled
by KiCad's official zone engine after each transaction and a distinct managed keepout remains an unfilled,
locked rule area with exact prohibited-item settings. It also creates deterministic native board
text and all five native dimension styles, then reapplies each distinct oneof field mask. It verifies
reference-resolved placement through a narrow native footprint transform: the parent and pad UUIDs,
schematic symbol path, and child geometry survive a front-to-back flip. It also creates an absent
schematic-linked footprint from a confined project-local library, assigns its native pad net, and
records its deterministic root UUID as KDS-owned state. The proof covers duplicate-free reapply,
exact removal when its placement disappears, and recreation with the same UUID. The fixture's
board, schematic, symbol library, and footprint library use the exact current KiCad 10.0.4 native
format versions; the test refuses to launch with an older serialization. Its cleanup targets only
the process and directory it created. The fixture also reconciles an existing root schematic and a new
child screen, preserves the root screen UUID and unmanaged title-block data, proves byte-idempotent
reapply, places a real footprintless power symbol through the same canonical component path while
preserving native artifact-inclusion flags, injects native validation failure and verifies exact
rollback/recovery, and loads the final hierarchy through `kicad-cli` to export exact signal and
power connectivity.

`tools/generate-codex-protocol-schema.sh` regenerates the installed app-server's experimental JSON
Schema and TypeScript contract under the ignored `build/` tree.  This is the review/update path for
protocol changes; the runtime client accepts unknown notifications and relies only on the fields it
negotiates during `initialize`.

## Compiler and native tool surface

Codex primarily authors a KDS program.  The compiler parses and type-checks that program, resolves
logical design identities, produces a reviewable plan, establishes a snapshot, and invokes the
native schematic/library/PCB/sourcing/check/output backends.  The lower-level native calls remain
available for bounded inspection, diagnostics, and compiler implementation; they are not a second,
untracked source of design truth.  The sidecar is reusable source and the ordinary KiCad project
files are its compiled artifacts.

KDS is the only external representation of design intent. Internal JSON IR, transaction actions,
managed-state records, and protobuf messages are compiler implementation details. The hidden
`*.kicad_kds_state` file records only deterministic ownership identities needed for idempotent
reconciliation; a short-lived `*.kicad_kds_journal` safely carries ownership across an interrupted
apply and is merged automatically by the next apply, so a retained journal is never a blocker.
Managed deletions are matched to KiCad's per-item results by UUID (KiCad answers in UUID order,
not request order), an item already absent from the live board counts as deleted, and a genuine
refusal names the item and the reason; a wholesale design replacement therefore converges the
existing project onto the new design regardless of stale editor state.  Pad nets converge on
every apply, not only at creation: after a placed footprint's metadata update, its live pads are
read and exactly those whose net differs from the planned KDS connectivity are updated in place
inside the same transaction (a numbered pad absent from the plan is cleared, unnumbered mechanical
pads are left alone), so a net added to the KDS after a part was first placed reaches its pads.  A product with several
boards is several KDS files in one project, each named after its KDS project so
`<name>.kicad_kds`, `<name>.kicad_sch` (the KDS root sheet) and `<name>.kicad_pcb` pair up for DRC
parity and fabrication; `design.apply` with `createBoard: true` writes a new empty KiCad 10 board
at a project-confined path before opening it in the editor, so a second board never has to be
created by hand.  KiCad keeps one active project per session, so opening the second board loads
its own `<name>.kicad_pro` (created on first open) and unloads the first; the panel keeps its
binding and turn snapshot across that swap because both boards share the project directory, and
each board keeps its own design rules and netclasses. The generated `.kicad_dru` file is likewise an internal compiler artifact; conditional-rule
intent is authored only in the exported `.kicad_kds` sidecar. Its exact prior presence and bytes are
journaled so a failed apply restores both the file and live DRC engine. Existing schematic-linked
footprints are resolved uniquely by reference and transformed in place. When a referenced footprint
is absent, a confined project-local `.kicad_mod` is parsed in-process and inserted atomically with
its deterministic schematic path and pad nets; the next apply resolves the same instance rather
than duplicating it. Footprints are not added to deterministic PCB-item ownership state. Managed
copper zones are committed unfilled,
then KiCad's official refill command is polled until every desired zone is authoritatively present
and filled. A rejected or timed-out refill retains the recovery journal, and the next apply safely
reconciles and retries. Keepout rule areas use their own ownership namespace and exact protobuf
field mask; they never participate in copper-fill completion polling. Both state files are project-confined and
included in whole-turn local history snapshots.

Hierarchy declarations compile into stable native sheet, sheet-pin, and hierarchical-label UUIDs.
The planner first inventories existing screen UUIDs and uses those exact identities in nested KiCad
instance paths; only missing screens receive deterministic compiler UUIDs. A lossless reconciler
replaces or inserts direct managed expressions and removes only identities proven in the previous
state. It updates the root title while retaining other title-block fields. Exact prior schematic
presence and bytes are stored in the apply journal, files install atomically, and a bounded sibling
`kicad-cli` process loads the complete hierarchy before any PCB item commit. Timeout or native parse
failure restores schematics, library tables, and board settings in reverse order.

Component declarations use one explicit multi-unit form. Before planning, KiChad inventories only
the referenced project-local symbol libraries, confines canonical paths to the project, rejects
symlinks, and passes their exact bytes to the lossless symbol resolver. The resolver extracts the
named native symbol and unit pin geometry; the planner then emits stable placed-symbol/pin UUIDs and
transforms each KDS net or no-connect endpoint onto a real pin coordinate. Project-global KDS nets
lower to native global labels, so connectivity spans hierarchy screens without a second net
representation. Cached symbols are reconciled by library ID inside each screen's `lib_symbols`
container while unrelated cache entries retain their bytes. Bounded same-library inheritance is
flattened with KiCad's field override semantics because schematic caches cannot contain `extends`;
missing parents, cycles, unsupported inherited content, unresolved pins, and unmanaged collisions
abort before installation. The same atomic file journal and native
hierarchy netlist validation cover sheets, cached symbols, placed units, connectivity, and rollback.

Native schematic wires, junctions, buses, and bus entries flow through the same compiler/planner
boundary. KDS authors endpoints rather than KiCad's signed bus-entry size, carries explicit
sheet ownership and stable human-readable IDs, and lowers styling into bounded native stroke,
junction-diameter, and RGBA fields. The reconciler proves kind/UUID agreement before touching an
existing direct item, so an unmanaged collision or stale kind aborts instead of claiming user data.
Local and global labels travel as the same typed drawing IR but reference an already resolved KDS
net; the planner derives native label text and scope-specific native kind from that reference.
Hierarchical labels continue to derive from sheet pins, keeping connectivity in one representation.
Bus aliases similarly reference declared KDS nets rather than copying member semantics. Because the
native format has no alias UUID, the sidecar owns a stable logical identity while reconciliation is
keyed by the exact sheet-local alias name; unmanaged name collisions abort before any file edit.
KDS directives carry a semantic net or rule-area reference plus explicit native flag and field
appearance. They lower only to KiCad 10's current `netclass_flag` token and reconcile by a stable
UUID like other direct schematic items. Rule areas carry validated simple polygons, complete
stroke/fill and inclusion policy, and a nested native polyline UUID; the reconciler extracts that
nested identity but owns and journals the entire native rule-area expression. A rule-area directive
must be on the same sheet with its anchor exactly on the declared boundary.
Schematic free text follows the same content-first semantic shape as board text while adding a
sheet reference. Text boxes add independently bounded container size, four margins, preserved
hidden/visible border state, every native line/fill mode, and the same complete text-effects block.
Both lower to stable-UUID KiCad expressions with simulation inclusion, color, mirroring, and
hyperlink state intact; no presentation field is silently dropped.
Managed AI-native symbols also lower named or De Morgan body-style inventories, consistent logical
unit display names, non-interchangeable unit locking, and ordered footprint-filter patterns into the
current KiCad 10 symbol-library semantics before native-loader validation.
Native schematic polylines, rounded rectangles, circles, three-point arcs, and cubic Bézier curves
use separate canonical forms with shared complete stroke/fill IR. Geometry is normalized and
validated before planning, then lowered to same-named current KiCad 10 objects under stable UUID
ownership.
Schematic images remain portable by storing one bounded payload in KDS alongside an AI-readable
description, declared media type, and SHA-256. Compilation strictly decodes and authenticates the
payload before the planner chunks it into KiCad's native embedded-image expression; opaque bytes
are never trusted as geometry or executable content.
Schematic tables use one semantic grid in KDS: an anchor and orientation, ordered column widths and
row heights, complete border/separator policy, and one fully styled cell for every 1-based grid
address. Rectangular merge ranges are declared once. Native cell positions, extents, span markers,
and stable UUIDs are derived, so the AI-facing source never duplicates or contradicts geometry.

Project-local library declarations compile into complete native `sym-lib-table` and
`fp-lib-table` artifacts. KiCad's library-table parser validates the generated type, version, and
rows before project-confined atomic replacement. The apply journal retains the exact prior presence
and bytes of both files and restores them in reverse order with board settings after a pre-commit
failure. Installed/global library declarations remain nickname dependencies and are never copied
into the project tables; model declarations remain KDS dependencies because KiCad has no model
table.

The complete advertised surface currently contains six schema-validated host functions. Each tool
owns its schema, validation, and handler in exactly one `codex_tool_<name>.cpp` file: `project`,
`inspect`, `design`, `pcb`, `verify`, and `fabricate`. The small registry only assembles those six
specifications, routes calls, and wraps structured results; it contains no tool implementation.
Shared confinement, native-format, IPC, and atomic-file primitives live behind a narrow internal
interface rather than being duplicated across tools.

Functionality is expanded inside these stable tools instead of creating narrowly named one-off
tools. KDS remains the one persistent AI-native representation: library, schematic, board,
sourcing, verification, and production capabilities are language facets compiled and applied by
`design`, while `pcb` provides exact live KiCad protobuf access and the other tools provide bounded
context, verification, and release operations.

The host advertises only implemented tools.  A tool is not exposed with a stub implementation.

## Mutation safety

Live board changes enter KiCad through the supported KiCad 10 IPC API and existing transaction
machinery so they participate in undo/redo, dirty-state handling, and editor refresh.  Each agent
turn also receives an atomic whole-project snapshot.  A failed or interrupted turn rolls back to
that snapshot, while a completed turn is one user-visible undo unit.

KiCad 10's public IPC surface does not cover the complete schematic and library editors.  Those
formats therefore use a lossless s-expression document layer that preserves trivia, unknown
expressions, ordering, quoting, and UUIDs.  Every mutation is written atomically and validated by
the corresponding KiCad 10 parser/CLI before it becomes the active project state.  Validation
failure restores the previous files and becomes structured tool output.

Destructive operations, snapshot rollback, and fabrication export require visible confirmation.
Tool calls, arguments, affected objects, previews, progress, validation results, and errors remain
visible in the conversation.

## Definition of done

The integration is not production-ready until unattended natural-language runs create all three
reference boards described in the project specification: a two-layer 555 design, an MCU board with
USB-C/regulation/custom footprint, and a four-layer fine-pitch board with planes and a real stackup.
Each fixture must finish routed, sourced, clean under ERC/DRC, and produce validated fabrication
outputs.  Injected auth, sourcing, parser, IPC, validation, cancellation, and export failures must
leave the project recoverable.

Current implementation status, prioritized remaining work, and milestone exit criteria are tracked
in [production-status.md](production-status.md).
