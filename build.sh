#!/usr/bin/env bash
# Dev-loop build for hyprscape.
#
# NixOS has no global pkg-config entry for Hyprland, and the plugin ABI must match the
# *running* compositor exactly. So instead of guessing a nixpkgs revision we build inside
# the build environment of the very Hyprland derivation that produced the running binary,
# and point pkg-config at that build's -dev output.
#
# Usage:  ./build.sh [-o out.so]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${ROOT}/libhyprscape.so"
while getopts "o:" opt; do case "$opt" in o) OUT="$OPTARG";; esac; done

# --- locate the running Hyprland's store paths -------------------------------------
ABI="$(hyprctl version -j 2>/dev/null | sed -n 's/.*"commit": *"\([0-9a-f]*\)".*/\1/p')"
[ -n "$ABI" ] || { echo "hyprscape: cannot talk to hyprctl; is Hyprland running?" >&2; exit 1; }

HL_OUT="$(readlink -f "$(command -v Hyprland || command -v hyprland)")"
HL_OUT="${HL_OUT%/bin/*}"
# The wrapper lives in /run/wrappers; fall back to scanning the store for the matching build.
if [ ! -d "$HL_OUT/include" ] && [ ! -x "$HL_OUT/bin/Hyprland" ]; then
  HL_OUT="$(ls -d /nix/store/*-hyprland-* 2>/dev/null | grep -v -- '-dev$\|-man$\|-env\|\.drv\|\.lock' | head -1)"
fi

VER="$(hyprctl version 2>/dev/null | head -1 | sed -n 's/^Hyprland \([0-9.]*\).*/\1/p')"
# Several -dev-ish outputs exist (…-dev, …-env-dev); only one actually ships hyprland.pc.
HL_DEV=""
for d in /nix/store/*-hyprland-"${VER}"*-dev; do
  if [ -f "$d/share/pkgconfig/hyprland.pc" ] || [ -f "$d/lib/pkgconfig/hyprland.pc" ]; then HL_DEV="$d"; break; fi
done
HL_DRV="$(ls -d /nix/store/*-hyprland-"${VER}"*.drv 2>/dev/null | grep -v 'env\|man' | head -1)"

[ -n "$HL_DEV" ] || { echo "hyprscape: no hyprland -dev output for $VER in /nix/store" >&2; exit 1; }
[ -n "$HL_DRV" ] || { echo "hyprscape: no hyprland .drv for $VER in /nix/store" >&2; exit 1; }

echo "hyprscape: hyprland $VER ($ABI)"
echo "hyprscape:   dev = $HL_DEV"
echo "hyprscape:   drv = $HL_DRV"

SRC="$(find "$ROOT/src" -name '*.cpp' | sort | tr '\n' ' ')"

nix-shell "$HL_DRV" --run "
  set -euo pipefail
  export PKG_CONFIG_PATH='$HL_DEV/share/pkgconfig':'$HL_DEV/lib/pkgconfig':\$PKG_CONFIG_PATH
  CF=\$(pkg-config --cflags hyprland pixman-1 libdrm pangocairo libinput libudev wayland-server xkbcommon)
  echo 'hyprscape: compiling...'
  g++ -std=c++23 -shared -fPIC --no-gnu-unique \
      -Wall -Wno-narrowing -Wno-unused-parameter -Wno-unused-variable \
      -O2 \
      \$CF -I'$ROOT/src' \
      -o '$OUT' $SRC
"
echo "hyprscape: built $OUT"
