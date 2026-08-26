#!/usr/bin/env bash
# Launch a throwaway nested Hyprland with hyprscape loaded, so the plugin can be exercised
# without putting the real session at risk. Requires an existing Wayland session.
#
#   ./test/nested.sh              build if needed, then start
#   ./test/nested.sh --rebuild    always rebuild first
#
# Screenshot the nested output with:  WAYLAND_DISPLAY=wayland-2 grim out.png
# (check `ls "$XDG_RUNTIME_DIR"/wayland-*` for which display it actually took)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGIN="$ROOT/libhyprscape.so"

if [ "${1:-}" = "--rebuild" ]; then rm -f "$PLUGIN"; fi

if [ ! -f "$PLUGIN" ]; then
  # build.sh compiles inside the running Hyprland's own Nix build environment; everywhere else
  # the plain Makefile is the right thing.
  if [ -d /nix/store ] && command -v nix-shell >/dev/null; then
    "$ROOT/build.sh"
  else
    make -C "$ROOT" all
  fi
fi

CONF="$(mktemp -t hyprscape-nested-XXXXXX.lua)"
trap 'rm -f "$CONF"' EXIT
sed "s|@PLUGIN@|$PLUGIN|" "$ROOT/test/nested.lua" > "$CONF"

echo "starting nested Hyprland (close it with SUPER+M inside the window)"
Hyprland -c "$CONF"
