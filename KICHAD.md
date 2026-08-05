# KiChad development baseline

KiChad is a downstream of the canonical
[KiCad source repository](https://gitlab.com/kicad/code/kicad).  The `upstream` remote tracks
GitLab, while `origin` tracks the KiChad GitHub repository.  The application executable names and
internal APIs intentionally remain compatible with KiCad at this stage; downstream builds carry a
`KiChad` version suffix and use `~/.config/kichad` so they do not overwrite normal KiCad settings.
The active downstream line is based on the stable `10.0.4` tag.  It does not track KiCad's moving
development branch or release candidates.

## Ubuntu 24.04 / KDE neon quick start

```sh
./tools/bootstrap-kichad-ubuntu.sh
./tools/check-codex-app-server.sh
./tools/build-kichad.sh
./tools/run-kichad.sh
```

## Ubuntu 24.04 AppImage

Build the distributable image locally before changing or diagnosing CI:

```sh
./tools/bootstrap-kichad-appimage-ubuntu.sh
./tools/build-kichad-appimage.sh
build/appimage/artifacts/KiChad-*.AppImage
```

This invokes the same build script as `.github/workflows/linux-appimage.yml`, pins the official
KiCad AppImage packager and every downloaded packaging tool by commit or SHA-256, downloads the
complete official Codex standalone package at its pinned version and checksum, and then verifies
both KiCad CLI and the Codex app-server protocol from the assembled package. It always installs the
complete official symbols, footprints, templates, and 3D-model libraries; there is one artifact
with one capability boundary.

No credentials are copied into the image. At runtime KiChad stores its isolated Codex state below
the user's KiChad configuration directory and launches the bundled app-server as its own child.

The build and install trees live below the ignored `build/` directory.  Nothing is installed into
`/usr/local`, and the system KiCad installation is not modified.  The install contains `kicad` and
`kicad-cli` launchers that configure the local runtime library path while retaining the upstream
executable names for compatibility. They execute the installed native `_kicad` and `_kicad-cli`
files, so the wrappers cannot recurse into themselves.

A full build fetches the official symbol, footprint, 3D-model, and template repositories at the
exact stable version in `.kichad-base-version`, then installs them beside KiChad under
`build/install/share/kicad`. This keeps the application and its standard libraries on one supported
version and makes the installed launchers self-contained. `./tools/fetch-kicad-libraries.sh` and
`./tools/install-kichad-libraries.sh` remain available when only the library runtime needs to be
refreshed. Neither command tracks the libraries' moving development branch.

## Useful checks

```sh
build/install/bin/kicad-cli version
./tools/check-kichad-libraries.sh
./tools/smoke-codex-app-server-protocol.sh
ctest --preset kichad-release
./tools/run-kichad.sh  # GUI smoke check; close the window after startup.
```

`tools/build-kichad.sh` always uses the tracked `kichad-release` preset, installs only after a
successful compile, and finishes with a headless version check.  It also verifies that `HEAD`
descends from the stable tag named by `.kichad-base-version` and that the installed binary reports
that version.  Set `KICHAD_BUILD_JOBS` to override its default of one job per detected CPU, or pass
target names to build a smaller target set.

For a narrow change, prefer the closest QA target or test label instead of running the entire suite.
CMake always exports `build/release/compile_commands.json` for language tooling.

## Codex integration seam

KiChad embeds a native Codex app-server client in the KiCad process. Distribution packages carry
the complete pinned Codex standalone runtime; developer builds may use a Codex on `PATH`. When
KiChad starts, the application directly launches and owns one `codex app-server` child over
redirected stdio; closing the application terminates that exact child. App-server dynamic-tool
calls are dispatched by the
host itself, so there is no wrapper daemon, MCP service, or separate tool server.  PCB mutations use
the supported KiCad 10 IPC API and KiCad transactions.  Schematic and library work uses a lossless
s-expression layer with KiCad validation because KiCad 10's public IPC surface does not cover those
editors.  Network work stays off the UI thread, and credentials are never stored in the repository
or compiled defaults.

The read-only `inspect.render` operation plots current schematic and 2D board views through the
matching `kicad-cli`, renders a native 3D board view when requested, crops blank plot margins, and
attaches the resulting PNG directly to the Codex tool response. Preview files live only under the
project's derived `.kichad/previews/` directory. A committed `design.apply` saves the live board and
returns `verification.status = not_run`; the embedded agent must inspect the rendered result and run
ERC/DRC before it can describe a design as correct.

## External layout integration

KiChad can delegate placement and routing to a third-party tool while keeping the KDS as the
single authored source of truth.  Enable it under Preferences → PCB Editor → External Layout:
a checkbox turns the mode on, a file picker names the executable, and a spin control sets the
desired copper layer count.  Settings live in `kicad.json` (`codex.external_layout.*`); the
`KICHAD_EXTERNAL_PNR` environment variable is a fallback for the tool path.  The executable is
invoked as `<tool> --input-dir <project> --output-dir <sibling> --layers N`; a project directory
named `Foo-no-layout` outputs to sibling `Foo`, anything else to `Foo-routed`.

With the mode enabled, new Codex conversations stage a handoff instead of placing and routing:
the outline is sized so every component fits without abutting, only connectors and mechanically
constrained parts are placed, and everything else sits outside the outline connected by the
schematic-derived ratsnest.  The agent then drives the `layout` dynamic tool:

- `run` executes the external tool and returns its machine-readable verdict when a
  `*verdict*.json` or `*-result.json` lands beside the output board; gates consume that data,
  not log text.
- `adopt` backs up the staged board to the project's `.kichad/pre-layout/` directory and copies
  the routed board into the project for review; `revert` restores the backup if the result is
  rejected.
- `reconcile` back-annotates an accepted routed board into the KDS — outline, placements, and
  every track and via become authored statements — validated by a compile with automatic
  restore on failure.  After reconcile, `design.apply` reproduces the routed board instead of
  erasing it, and the fabrication package of record is generated by KiChad's own `fabricate`
  pipeline; the external tool's fab outputs are standalone or cross-check artifacts only.

Layout-contract semantics external tools must match: `(edge …)` limits are measured from the
component's authored anchor (the `place` position, not a pad centroid) to the outline
bounding-box edge, `(board (maximum_width|maximum_height))` is the outline bounding-box extent,
and both comparisons are strict with no tolerance.  KDS additions for external flows: a
top-level `(fab "NAME")` declares the fabrication vendor profile for tools that read the KDS
directly, and components declared `(footprint none)` materialize as `(on_board no)` so external
parts never trip schematic parity.

Related behavior notes: generated schematic net labels anchor exactly on pin endpoints (no stub
wires), schematic parity DRC always compares against the on-disk schematic rather than an open
editor's in-memory document, and every Codex exchange is appended to `codex_dialog.txt` in the
project directory.

Generated schematics carry an enforced readability contract in addition to the agent-policy
guidance (1.27mm placement grid, 10.16mm of clear sheet between symbol bodies, right-side-up
field text): the schematic planner rejects any plan in which two symbol anchors on the same
sheet sit closer than 7.62mm, emitting a `crowded_schematic_placement` diagnostic that names
both components and their distance.  One-pin symbols (power flags, test points) are exempt
because they legitimately sit on a neighboring symbol's pin.  A rejected plan is never
rendered; the agent must re-space and resubmit.

## Syncing upstream

```sh
git fetch upstream --prune --tags
git switch -c upgrade/kicad-X.Y.Z X.Y.Z^{}
# Port the focused KiChad commits, build, test, and review before promoting it.
```

Only move `.kichad-base-version` to an official stable release after it has passed the KiChad test
matrix.  Keep downstream infrastructure in focused commits so conflicts remain easy to resolve.
Never modify or force-push an upstream maintenance branch or tag.
