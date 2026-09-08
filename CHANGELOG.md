# Changelog

## Unreleased

### Changed

- **Ported to Hyprland 0.56.x**, which is now the only supported series (`SUPPORTED_HYPRLAND` in
  the Makefile). 0.56 moved `CMonitor` into `namespace Monitor`, so the mangled names for
  `renderWindow` and `renderLayer` changed and the 0.55-built plugin failed to `dlopen` on
  `undefined symbol: CMonitor::changeWorkspace`. A plugin that cannot load registers none of its
  config values, so every `plugin:hyprscape:*` key then reads as an unknown config key --
  `hyprctl plugin list` reporting "no plugins loaded" is the tell, since the load failure itself
  is quiet (`debug:disable_logs` is on by default, the notification expires in five seconds, and
  `updateConfigPlugins` will not retry until the plugin list changes).
- Followed 0.56 in breaking up the `CCompositor` god-object: monitor and workspace lookups go
  through `State::monitorState()` / `State::workspaceState()` queries, windows through
  `Desktop::windowState()`, workspace moves through `Desktop::globalWindowController()`, and
  `scheduleFrameForMonitor` is now `monitor->scheduleFrame()`. `g_pAnimationManager` became
  `Animation::mgr()`, and the cursor shape override controller moved into `Pointer::Cursor`.
- `m_realPosition` / `m_realSize` are protected in 0.56, so geometry reads go through the public
  `positionAnimation()` / `sizeAnimation()` accessors.
- A closing window is snapshotted into a `Desktop::CWindowFadeout` and dropped from the live
  window list, so there is no `m_fadingOut` left to hold it in the overview: an unmapped window
  is simply gone. Fading-out layer surfaces likewise no longer appear in `m_layerSurfaceLayers`.

## 0.2.0

First release intended for other people to install.

### Distribution

- **hyprpm support.** `hyprpm add https://github.com/cybergaz/hyprscape` builds against headers
  matching whatever Hyprland you are running, on any distribution.
- A `PKGBUILD` for Arch, a Nix flake with an overlay, and a plain `make install` path.
- The Makefile now refuses to build against a Hyprland series hyprscape does not support, with a
  message saying so, rather than producing a plugin that loads and then cannot find its hooks.
- BSD-3-Clause `LICENSE`, CI, and install instructions per distribution.

### Fixed

- **Rings drew a pixel thin on the right and bottom.** `renderBorder` derives its scissor from a
  `CRegion`, which is pixman — integers, with both the origin *and* the size truncated on the way
  in — so a fractional box loses a pixel off its far edges that its near edges keep. A 2px ring
  came out 2px top-left and 1px bottom-right; hover looked random because the error depends on
  each window's own fractional box. Border boxes are snapped to whole device pixels first.
- **The centre rectangle travelled with the window it was ringing.** It was drawn around the
  anchor window's live box, and the anchor changes the instant focus does, so it jumped to the
  new window wherever that window was and rode in alongside it. It is now drawn from where the
  anchor comes to rest — horizontally the middle of the output by construction, vertically the
  selected row at rest — and only its size animates.
- **The hover ring lit up windows nobody was pointing at.** Walking the tape with the keyboard
  slides windows underneath a parked cursor. Hover is now armed only by a real pointer event.
- Windows kept their full-size corner radius while shrunk, because the radius is a shader uniform
  in screen pixels and the render transform moves the quad but not the radius.

### Added

- `animation_curve`, `animation_speed`, `animation_enabled`, `spring_stiffness`,
  `spring_damping`, `spring_mass`. hyprscape owns its animation node instead of borrowing the
  `workspaces` one, whose default curve ends at 1.05 — that overshoot was the "springiness".
  `"smooth"` never travels past its goal, `"spring"` is a real spring, `"inherit"` is the old
  behaviour, and any other value names a curve from your own config.
- `window_rounding`, `active_border_rounding`, `hover_border_rounding`.

## 0.1.0

Initial working version: a niri-style zoom-out overview for Hyprland's scrolling layout, with
per-workspace scroll tapes sliding through a fixed centre, drag-and-drop between workspaces,
touchpad gestures, and layer-shell surfaces left unscaled.
