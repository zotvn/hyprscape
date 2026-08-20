#!/usr/bin/env bash
# Launch a throwaway nested Hyprland with hyprscape loaded, so the plugin can be exercised
# without putting the real session at risk. Requires an existing Wayland session.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$ROOT/libhyprscape.so" ] || "$ROOT/build.sh"

sed "s|@PLUGIN@|$ROOT/libhyprscape.so|" "$ROOT/test/nested.lua" > /tmp/hyprscape-nested.lua

echo "starting nested Hyprland (close it with SUPER+M inside the window)"
Hyprland -c /tmp/hyprscape-nested.lua
