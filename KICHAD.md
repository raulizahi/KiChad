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

## macOS development build (experimental)

The qualified platform remains Ubuntu 24.04; macOS is a development convenience only.  The tracked
`CMakePresets.json` stays Linux-only by policy, so the macOS configuration ships as a sample user
preset:

```sh
cp CMakeUserPresets.macos.sample.json CMakeUserPresets.json
cmake --preset kichad-macos
cmake --build --preset kichad-macos
```

Notes on the sample:

- Paths assume arm64 Homebrew under `/opt/homebrew`; on Intel Macs substitute `/usr/local`.
- Boost is pinned to the `boost@1.85` keg: Boost 1.90 removed the Boost.Process v1 API used by
  `kicad/codex`.  If the plain `boost` formula (1.90+) is installed, `brew unlink boost` so its
  headers do not shadow 1.85.
- The preset pins the `python@3.13` framework, `protobuf@33`, `opencascade`, and `libngspice`
  kegs; install those plus KiCad's usual build dependencies (wxWidgets, ninja, ccache, glew, glm,
  cairo, …) from Homebrew.
- Building this source line on macOS requires the toolchain compatibility shims from the
  `feature/macos-build-compat` branch until they are merged.
- Developer builds expect a `codex` executable on `PATH` (distribution packaging is not covered
  here).  Running the GUI and the QA suite from the build tree additionally needs bundle
  scaffolding (SharedSupport data, a `Python.framework` link) that no script creates yet.

## Windows development build (experimental)

The qualified platform remains Ubuntu 24.04; Windows is a development convenience only.  As with
macOS, the tracked `CMakePresets.json` stays Linux-only by policy, so the Windows configuration
ships as a sample user preset:

```sh
cp CMakeUserPresets.windows.sample.json CMakeUserPresets.json
cmake --preset kichad-windows
cmake --build --preset kichad-windows
```

Notes on the sample:

- Dependencies come from vcpkg in manifest mode against the tracked `vcpkg.json`.  Edit the
  preset's `VCPKG_ROOT` to your vcpkg checkout, or drop that `environment` block and export
  `VCPKG_ROOT` yourself.  The first configure builds the whole dependency set and takes hours.
  `VCPKG_INSTALL_OPTIONS` passes `--clean-buildtrees-after-build` and `--clean-packages-after-build`
  so each port's intermediates and staging tree are dropped once it installs, holding peak disk near
  `vcpkg_installed/` plus one in-flight port; drop them if you would rather keep the trees for faster
  single-port rebuilds.
- `VCPKG_OVERLAY_TRIPLETS` points at `tools/kichad_vcpkg_triplets`, whose `x64-windows` triplet
  inherits the stock one and adds `VCPKG_BUILD_TYPE release`.  The preset builds KiChad itself as
  `RelWithDebInfo`, which links the release CRT and never touches the debug dependency set, so
  skipping it roughly halves both dependency build time and installed size.  A triplet is the only
  way to set this in manifest mode: the vcpkg toolchain forwards `VCPKG_OVERLAY_TRIPLETS` but
  ignores a `-DVCPKG_BUILD_TYPE=` on the CMake command line.  Delete the `set()` in that triplet if
  you need to debug into a dependency.  This is separate from the upstream
  `tools/custom_vcpkg_triplets` that `.gitlab/Windows-CI.yml` uses, which is left untouched.
- Build from a Visual Studio x64 developer shell so MSVC, the Windows SDK, and `ninja` are on
  `PATH`.  `/bigobj` is already applied for MSVC by the top-level `CMakeLists.txt`, which the
  larger `kicad/codex` translation units need.
- SWIG 4.0 or newer must be installed separately; `winget install SWIG.SWIG` puts it on `PATH`.
  `CMakeLists.txt` calls `find_package( SWIG 4.0 REQUIRED )` unconditionally, so configure fails
  with `Could NOT find SWIG` even though the preset sets `KICAD_SCRIPTING_WXPYTHON=OFF`.  vcpkg
  cannot supply it: there is no `swig` port, and `vcpkg.json` does not list one.  If your SWIG is
  not on `PATH`, point the preset at it instead by adding
  `"SWIG_EXECUTABLE": "<swigwin>/swig.exe"` and `"SWIG_DIR": "<swigwin>/Lib"` to `cacheVariables`.
- `KICAD_IPC_API` must stay `ON`: `kicad/CMakeLists.txt` fails configuration without it because
  the native Codex PCB tools are built on it.
- `vcpkg.json` pins protobuf to 3.21.12, the same generation Ubuntu 24.04 ships, so the
  `kichad_protobuf_compat.h` shims that macOS needs for protobuf 33 should not be required here.
- Developer builds expect a `codex.exe` on `PATH`.  `tools/fetch-codex-standalone.sh` pins the
  `x86_64-unknown-linux-musl` package and checks for Linux-only payload (`bwrap`, bundled `zsh`),
  so it does not serve Windows; there is no Windows packaging path yet.
- Live PCB tools are not expected to work yet.  `KICHAD_IPC_CLIENT` discovers an open editor by
  scanning the temp directory for `api*.sock` files, but nng's `ipc://` transport uses named pipes
  on Windows and creates no such files.  This is unverified and needs a named-pipe discovery path;
  until then the s-expression schematic and library tools are the usable surface.
- Everything under `tools/` is bash; there are no PowerShell equivalents, so the smoke and library
  check scripts need Git Bash or MSYS2, and some will not work regardless.

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
