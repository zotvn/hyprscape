#pragma once

#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprutils/animation/AnimationConfig.hpp>

// hyprscape owns its animation node instead of borrowing the user's `workspaces` one. Sliding a
// tape through a fixed centre is a different gesture from a workspace slide, and Hyprland's
// `default` curve ends at 1.05 -- it overshoots, which reads as a spring nobody asked for.
//
// The returned config is shared by every animated variable in the plugin and is refreshed in
// place on reload, so no animation has to be recreated when the user changes a key.
SP<Hyprutils::Animation::SAnimationPropertyConfig> hs_animation_config();

// Re-read plugin:hyprscape:animation_* and re-register our curves. Called every frame the
// overview draws, because `hyprctl eval` and `hl.config` change a value without ever emitting a
// config-reload -- every other key in the plugin is read per frame, and these should be too.
// The expensive parts are skipped unless a value actually changed.
void hs_refresh_animation_config();
