#!/usr/bin/env bash
#
# Launch the KiChad development bundle built by the kichad-macos preset.

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${KICHAD_MACOS_BUILD_DIR:-${repo_root}/build/release-macos}"
app="${build_dir}/kicad/KiCad.app"

if [[ ! -x "${app}/Contents/MacOS/kicad" ]]; then
    echo "KiChad is not built in ${build_dir}." >&2
    echo "Run: cmake --preset kichad-macos && cmake --build --preset kichad-macos" >&2
    exit 1
fi

if [[ ! -e "${app}/Contents/SharedSupport/symbols" || ! -e "${app}/Contents/MacOS/codex" ]]; then
    echo "The bundle is not scaffolded yet; running tools/scaffold-kichad-macos-bundle.sh first." >&2
    "${repo_root}/tools/scaffold-kichad-macos-bundle.sh"
fi

exec open -n -W "$app" --args "$@"
