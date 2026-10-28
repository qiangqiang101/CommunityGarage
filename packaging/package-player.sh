#!/usr/bin/env bash
# Builds the player zip: what a player extracts into their Azimuth folder. Same layout as package-player.ps1 (used on Windows).
#   usage: packaging/package-player.sh <CommunityGarage.dll> <platform dll (build/platform/d3dcompiler_47.dll)> <output.zip>
# Set PACKAGE_SKIP_ZIP=1 to only stage the files (printed), without zipping.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
mod_dll=${1:?path to CommunityGarage.dll}
platform_dll=${2:?path to the platform dll (d3dcompiler_47.dll)}
out=${3:?output zip path}

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/fh2-mods" "$stage/communitygarage" "$stage/licenses"
cp "$mod_dll"                                  "$stage/fh2-mods/CommunityGarage.dll"
# Built as d3dcompiler_47.dll; Azimuth 1.0.1+ loads it as DWrite.dll, so ship it under that name.
cp "$platform_dll"                             "$stage/DWrite.dll"
cp "$root/packaging/templates/player-server.ini"  "$stage/communitygarage/server.ini"
cp "$root/packaging/templates/PLAYER-README.txt"  "$stage/README.txt"
cp "$root/docs/TERMS.md"                       "$stage/TERMS.md"
cp "$root/LICENSE"                             "$stage/LICENSE"
cp "$root/platform/LICENSE"                    "$stage/licenses/FH2-Mod-Platform-LICENSE.txt"

(cd "$stage" && find . -type f -printf '  %P\n' | sort)
if [ "${PACKAGE_SKIP_ZIP:-0}" != 1 ]; then
  mkdir -p "$(dirname "$out")"
  out_abs=$(cd "$(dirname "$out")" && pwd)/$(basename "$out")
  rm -f "$out_abs"
  (cd "$stage" && zip -q -r -9 "$out_abs" .)
  echo "wrote $out_abs"
fi
