# hyprscape

A [niri](https://github.com/YaLTeR/niri)-style **Overview** for Hyprland's built-in scrolling
layout: one keybind zooms the desktop out so you can see the whole scroll tape at once, with
every window still exactly where it really is.

Built and tested against **Hyprland 0.55.4**.

```
        ┌──────────────────── workspace 1 ────────────────────┐
   ┌────┼────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌──────┼───┐
   │ ff │    │  │  term   │  │  term   │  │  editor │  │ chat │   │     ← the real scroll tape,
   └────┼────┘  └─────────┘  └─────────┘  └─────────┘  └──────┼───┘        spilling past the
        └──────────── the workspace's viewport ──────────────┘            viewport on both sides

        ┌──────────────────── workspace 3 ────────────────────┐
        │   ┌─────────┐  ┌───────────────────┐                │
        │   │ browser │  │      terminal     │                │
        │   └─────────┘  └───────────────────┘                │
        └─────────────────────────────────────────────────────┘

        ┌──────────────────── new workspace ──────────────────┐
        └─────────────────────────────────────────────────────┘
```

## Why not hyprtasking?

hyprtasking lays workspaces out on a fixed `rows × cols` grid. That model has no idea what a
scrolling layout is:

- it draws every grid cell, so 2 real workspaces become 6 tiles of mostly nothing;
- each tile is only one screenful wide, so the columns you have scrolled off-screen spill out of
  their tile and land on top of the neighbouring workspace.

hyprscape drops the grid entirely:

- **one row per workspace that actually exists**, sorted by id — never a phantom tile;
- **workspaces stack vertically**, one horizontal tape each, exactly like niri;
- **windows keep their true tape position.** Columns scrolled off the viewport render *outside*
  the workspace card, to its left and right. That overflow is the feature — it is what makes the
  tape visible;
- **the centre of the screen is a fixed reference.** Each row is anchored on the window it was
  focused on when you opened the overview, so that window sits dead centre with its earlier
  columns to the left. Scrolling columns slides the tape *through* that centre; the centre itself
  never moves. Rows are anchored independently, so scrolling one workspace never shifts another;
- **it is a real zoom.** At progress 0 the transform is the identity, so opening the overview is a
  continuous zoom-out from your desktop rather than a cut to a different screen.

## Build

The plugin ABI is tied to one exact Hyprland build, so it must be compiled against the Hyprland
you actually run.

### Quick, on the machine that runs Hyprland

```sh
./build.sh          # → ./libhyprscape.so
```

`build.sh` finds the running compositor's version via `hyprctl`, locates that build's `-dev`
output and derivation in the Nix store, and compiles inside its build environment. No pinning, no
`PKG_CONFIG_PATH` fiddling, and it cannot silently build against the wrong Hyprland.

### Nix

```sh
nix build .#hyprscape
```

### Anywhere `pkg-config hyprland` already works

```sh
make
```

## Install

> **Unload hyprtasking first.** Both plugins hook `renderWorkspace`; running them together gives
> you whichever one wins the race.

### Home Manager (Lua config)

```nix
{
  wayland.windowManager.hyprland.plugins = [
    (inputs.hyprscape.lib.mkHyprscape {
      inherit pkgs;
      hyprland = config.wayland.windowManager.hyprland.package;
    })
  ];
}
```

Then in your Lua config:

```lua
hl.bind("SUPER + U", function() hl.plugin.hyprscape.toggle("all") end)

-- Optional: only act when the overview is open.
hl.bind("SUPER + SHIFT + U", function()
    if hl.plugin.hyprscape.is_active() then
        hl.plugin.hyprscape.close("")
    end
end)
```

### Trying it without committing to it

```sh
hyprctl plugin load /absolute/path/to/libhyprscape.so
hyprctl plugin unload /absolute/path/to/libhyprscape.so
```

### Plain `hyprland.conf`

```
plugin = /absolute/path/to/libhyprscape.so
bind = SUPER, U, hyprscape:toggle, all
```

## Using it

| Input | What it does |
| --- | --- |
| `hyprscape:toggle` | open / close the overview |
| mouse wheel | walk the workspace stack, up and down |
| shift + wheel, or horizontal wheel | slide the selected row's tape through the centre, one column at a time |
| left click on a window | focus it, switch to its workspace, close the overview |
| left click beside a card | switch to that workspace and close |
| left drag a window | move it to whatever workspace row you drop it on — including the empty one at the bottom, which creates a new workspace |
| right drag | pan the tape sideways |
| `Escape` | close |
| `Enter` | close onto the hovered window, or the centred one |
| arrows / `hjkl` | navigate rows and columns |
| four-finger swipe up | open, continuously, tracking the swipe |

Your own keybinds keep working while the overview is open; only the keys above are intercepted.

## Dispatchers

Each is available both as `hyprscape:<name>` and as `hl.plugin.hyprscape.<name>(arg)`.

| Dispatcher | Argument | Effect |
| --- | --- | --- |
| `toggle` | `all` \| `cursor` | toggle on every monitor, or only the one under the cursor |
| `open` | `all` \| `cursor` | open |
| `close` | — | close, committing the selected workspace |
| `move` | `up`/`down`/`left`/`right` | navigate |
| `select` | — | close onto the hovered window |
| `is_active` | — | *(Lua only)* returns a boolean |

## Configuration

All keys live under `plugin:hyprscape:`.

### Geometry

| Key | Default | Meaning |
| --- | --- | --- |
| `zoom` | `0.5` | how far to zoom out; a workspace card is this fraction of the monitor. niri's default. |
| `workspace_gap` | `0.1` | vertical gap between rows, as a fraction of monitor height (scaled with the zoom, as in niri) |
| `auto_fit` | `1` | if the **selected** workspace's tape is wider than the screen even at `zoom`, keep zooming out until it fits, and recentre on the content. Animated, so moving between workspaces with very different tape lengths eases rather than snaps. |
| `min_zoom` | `0.12` | the floor `auto_fit` will not go below |
| `fit_rows` | `0` | also zoom out until *every* workspace row fits on screen at once |

`auto_fit` is the one that matters for long tapes: with it on, "show me everything" actually shows
you everything instead of running off the bezel. Turn it off for niri's fixed-zoom behaviour.

### Appearance

| Key | Default | Meaning |
| --- | --- | --- |
| `backdrop_color` | `rgba(16161ecc)` | dims the wallpaper as the overview opens, and is the opaque base when background layers are off. Its alpha is faded in with the animation, so at rest it changes nothing. |
| `card_color` | `rgba(00000000)` | optional band behind each workspace row, full output width |
| `render_background_layers` | `1` | draw background/bottom layer surfaces (your wallpaper) — unscaled and unmoved, as a backdrop |
| `render_top_layers` | `1` | draw top/overlay layer surfaces (your bar) — unscaled and unmoved, over everything |
| `active_border_size` | `2` | ring around the centred window of the selected row — what closing will focus; `0` disables |
| `active_border_color` | `rgba(3399ffff)` | its colour |
| `hover_border_size` | `3` | ring around the hovered window; `0` disables |
| `hover_border_color` | `rgba(88bbffff)` | its colour |

### Which workspaces get a row

| Key | Default | Meaning |
| --- | --- | --- |
| `show_empty` | `0` | also give a row to empty workspaces you are not on. **Leave this off** — it is what made hyprtasking's grid look like nonsense. |
| `trailing_workspace` | `1` | keep one empty row at the bottom, niri style, as a drop target for making a new workspace |

### Behaviour

| Key | Default | Meaning |
| --- | --- | --- |
| `exit_on_click` | `1` | a click picks a target and leaves; `0` keeps the overview open |
| `close_on_reload` | `1` | close when the config reloads |
| `warp_cursor` | `0` | warp the pointer onto the window you select |
| `scroll_speed` | `1.0` | wheel sensitivity when walking rows |
| `select_button` | `BTN_LEFT` (272) | button that picks a window |
| `pan_button` | `BTN_RIGHT` (273) | button that drags the tape sideways; `0` disables |

### Touchpad

| Key | Default | Meaning |
| --- | --- | --- |
| `gestures:enabled` | `1` | |
| `gestures:open_fingers` | `4` | niri uses four |
| `gestures:open_distance` | `300` | swipe distance for a full open/close |
| `gestures:open_positive` | `0` | `1` if swiping *down* should open |

### Animation

hyprscape drives the whole thing from one animated scalar and borrows your existing `workspaces`
animation curve, so it already matches the rest of your desktop. To make it snappier:

```lua
hl.animation({ leaf = "workspaces", enabled = true, speed = 5, bezier = "default" })
```

## How it works

`renderWorkspace` is hooked; while the overview is up, hyprscape draws the monitor itself instead.

For each workspace row it pushes a single render modifier that maps that workspace's coordinate
space onto the row, then walks the workspace's windows in Hyprland's own z-order
(tiled → popups → floating) calling `renderWindow` directly. Because the modifier covers the whole
row rather than a clipped tile, a window scrolled off the viewport simply lands beside its row,
which is exactly the overflow we want.

The horizontal placement of each row is one number: the distance from the centre of the screen to
the centre of that workspace's *anchor column*. Anchors are captured when the overview opens and
only change when you scroll columns, so the centre is a stationary reference and the tapes are
what move. Fading that offset in with the open animation is what keeps progress 0 a pixel-exact
identity transform.

Layer surfaces are drawn outside the modifier entirely: the wallpaper stays put behind the rows
and the bar stays put in front of them, at their real sizes.

Three details that are easy to get wrong and are handled explicitly:

- **Slid-out workspaces.** Hyprland parks a non-visible workspace's `m_renderOffset` at roughly a
  screen width, and `renderWindow` adds it to every window position. hyprscape folds that offset
  into each card's source origin so it cancels exactly.
- **Faded-out workspaces.** With a `slidefade*` workspace animation — the default in many configs —
  a hidden workspace's `m_alpha` sits at `0`, and every surface is multiplied by it. A scoped guard
  neutralises it for the duration of the draw, writing through the animated variable's value rather
  than its goal so an in-flight animation is left alone.
- **Scissors under a transform.** Hyprland applies the render modifier to the quads it draws but
  not to the regions it derives scissors and blur samples from, so borders vanish and blur lands in
  the wrong place. `renderTexture`, both `renderBorder` overloads and `shouldUseNewBlurOptimizations`
  are hooked to keep those paths in the same coordinate space.

Navigating the overview never touches the compositor's active workspace; the choice is committed
once, on close. Column navigation goes through `CSpace::layoutMsg`, so it works with scrolling,
dwindle and master alike.

## Limitations

- Written for Hyprland **0.55.4**. It resolves several functions by mangled symbol, so a different
  Hyprland will refuse to load it (loudly, at init) rather than misbehave.
- Special workspaces (scratchpads) are not shown.
- A workspace containing a genuinely fullscreen window is shown from its window positions, which
  the scrolling layout stops updating while fullscreen is active.
- Multi-monitor is implemented but has only been exercised on a single output.
- Windows are not vertically cropped to their card. In a scrolling layout columns fit the work
  area, so this is not visible in practice.
