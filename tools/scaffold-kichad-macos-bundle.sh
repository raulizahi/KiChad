#!/usr/bin/env bash
#
# Prepare a macOS development build tree so the KiChad GUI, kicad-cli, and the QA suite run
# straight from build/release-macos.  CMake builds the binaries and the KiCad.app bundle but does
# not create the runtime scaffolding a real installer would: shared data, the Python framework
# links, the standalone editor apps inside the bundle, the Codex binary, and the sibling
# kicad-cli wrapper the tests expect.  This script creates all of it and is safe to re-run after
# every rebuild (copies that went stale are refreshed).
#
# Prerequisites: an official KiCad 10 install (for the stock symbol/footprint/3D data), Homebrew
# python@3.13, and the @openai/codex npm package.  Override any location through the variables
# below.

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${KICHAD_MACOS_BUILD_DIR:-${repo_root}/build/release-macos}"
official_app="${KICHAD_OFFICIAL_KICAD_APP:-/Applications/KiCad/KiCad.app}"
brew_prefix="${HOMEBREW_PREFIX:-$(brew --prefix 2>/dev/null || echo /opt/homebrew)}"
python_version="${KICHAD_PYTHON_VERSION:-3.13}"
python_framework="${KICHAD_PYTHON_FRAMEWORK:-${brew_prefix}/opt/python@${python_version}/Frameworks/Python.framework/Versions/${python_version}}"
codex_binary="${KICHAD_CODEX_BINARY:-}"

bundle="${build_dir}/kicad/KiCad.app/Contents"

if [[ ! -x "${bundle}/MacOS/kicad" || ! -x "${bundle}/MacOS/kicad-cli" ]]; then
    echo "No built KiCad.app in ${build_dir}." >&2
    echo "Run: cmake --preset kichad-macos && cmake --build --preset kichad-macos" >&2
    exit 1
fi

link() {
    # link TARGET LINK: replace LINK with a symlink to TARGET.
    local target="$1" path="$2"
    rm -rf "$path"
    ln -s "$target" "$path"
}

echo "Scaffolding ${bundle}"

# 1. Shared data: stock libraries from the official app, schemas from this repository.
if [[ ! -d "${official_app}/Contents/SharedSupport" ]]; then
    echo "Official KiCad app not found at ${official_app}; set KICHAD_OFFICIAL_KICAD_APP." >&2
    exit 1
fi

mkdir -p "${bundle}/SharedSupport"

for entry in 3dmodels footprints help internat plugins resources scripting symbols template; do
    if [[ -e "${official_app}/Contents/SharedSupport/${entry}" ]]; then
        link "${official_app}/Contents/SharedSupport/${entry}" "${bundle}/SharedSupport/${entry}"
    fi
done

link "${repo_root}/resources/schemas" "${bundle}/SharedSupport/schemas"

# 2. Python framework: Homebrew's framework has no Current link, without which the GUI aborts
#    with "Failed to import encodings".
if [[ ! -d "$python_framework" ]]; then
    echo "Python framework not found at ${python_framework}; set KICHAD_PYTHON_FRAMEWORK." >&2
    exit 1
fi

mkdir -p "${bundle}/Frameworks/Python.framework/Versions"
link "$python_framework" "${bundle}/Frameworks/Python.framework/Versions/${python_version}"
link "${python_version}" "${bundle}/Frameworks/Python.framework/Versions/Current"

# 3. Standalone editors inside the bundle as real copies.  Symlinks do not work: the bundle root
#    is resolved through them and kiface lookup then fails.  Copies go stale after a relink, so
#    they are refreshed on every run.
mkdir -p "${bundle}/Applications"

for spec in eeschema/eeschema.app pcbnew/pcbnew.app gerbview/gerbview.app \
            pagelayout_editor/pl_editor.app pcb_calculator/pcb_calculator.app \
            bitmap2component/bitmap2component.app; do
    source="${build_dir}/${spec}"
    name="$(basename "$spec")"

    if [[ -d "$source" ]]; then
        rm -rf "${bundle}/Applications/${name}"
        cp -R "$source" "${bundle}/Applications/${name}"
    else
        echo "warning: ${source} is not built; the project manager cannot open ${name%.app} documents" >&2
    fi
done

# 4. Codex: the bundle needs the self-contained native binary.  The npm wrapper on PATH needs
#    node, which GUI processes do not have on PATH.
if [[ -z "$codex_binary" ]]; then
    npm_root="$(npm root -g 2>/dev/null || true)"
    arch_dir="aarch64-apple-darwin"
    pkg="codex-darwin-arm64"

    if [[ "$(uname -m)" != "arm64" ]]; then
        arch_dir="x86_64-apple-darwin"
        pkg="codex-darwin-x64"
    fi

    for candidate in \
        "${npm_root}/@openai/codex/node_modules/@openai/${pkg}/vendor/${arch_dir}/bin/codex" \
        "${npm_root}/@openai/${pkg}/vendor/${arch_dir}/bin/codex"; do
        if [[ -x "$candidate" ]]; then
            codex_binary="$candidate"
            break
        fi
    done
fi

if [[ -n "$codex_binary" && -x "$codex_binary" ]]; then
    link "$codex_binary" "${bundle}/MacOS/codex"
else
    echo "warning: no native codex binary found; install @openai/codex globally or set KICHAD_CODEX_BINARY" >&2
fi

# 5. Sibling kicad-cli wrapper: tests and tools expect the Linux layout <build>/kicad/kicad-cli,
#    and a plain symlink breaks kiface resolution, so exec the bundle binary instead.
cat > "${build_dir}/kicad/kicad-cli" <<WRAPPER
#!/bin/sh
exec "${bundle}/MacOS/kicad-cli" "\$@"
WRAPPER
chmod +x "${build_dir}/kicad/kicad-cli"

# 6. QA suite shared data.
mkdir -p "${build_dir}/qa/tests/common/Contents"
link "${official_app}/Contents/SharedSupport" "${build_dir}/qa/tests/common/Contents/SharedSupport"

echo "Done. Launch with: ./tools/run-kichad-macos.sh"
