#pragma once

// The single source of truth for hyprscape's version. flake.nix, packaging/arch/PKGBUILD and
// CHANGELOG.md have to agree with it; CI checks that they do.
#define HYPRSCAPE_VERSION "0.2.0"

// The Hyprland release series this build hooks into. The Makefile refuses to compile against
// anything else, because the render takeover resolves functions by mangled C++ symbol.
#define HYPRSCAPE_HYPRLAND_SERIES "0.55"
