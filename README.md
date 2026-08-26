# hyprscape

A [niri](https://github.com/YaLTeR/niri)-style **Overview** for Hyprland's built-in scrolling
layout: one keybind zooms the desktop out so you can see the whole scroll tape at once, with
every window still exactly where it really is.

[![build](https://github.com/cybergaz/hyprscape/actions/workflows/build.yml/badge.svg)](https://github.com/cybergaz/hyprscape/actions/workflows/build.yml)

**Hyprland 0.55.x** · install with [hyprpm](#hyprpm-any-distribution), a
[PKGBUILD](#arch-pkgbuild), the [Nix flake](#nixos--home-manager), or [make](#by-hand).

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

## Install

hyprscape is a Hyprland **plugin**, and a Hyprland plugin is locked to one exact compositor
build: the loader compares a hash and refuses anything else. So the only question any install
method has to answer is *"how do I compile against the Hyprland I am actually running, and
recompile when it changes?"* Pick whichever answer suits your distribution.

| You run | Use | Rebuilds itself on a Hyprland update |
| --- | --- | --- |
| Arch, Fedora, openSUSE, Gentoo, Debian, anything else | [**hyprpm**](#hyprpm-any-distribution) | `hyprpm update` does it, for every plugin at once |
| Arch, and you would rather have a package | [**PKGBUILD**](#arch-pkgbuild) | no — rebuild it yourself |
| NixOS / Home Manager | [**the flake**](#nixos--home-manager) | yes, automatically, on the next `switch` |
| Something else, by hand | [**make**](#by-hand) | no |

> **Unload hyprtasking first if you have it.** Both plugins hook `renderWorkspace`; running them
> together gives you whichever one wins the race.

### hyprpm (any distribution)

hyprpm ships with Hyprland. It fetches the headers matching *your* compositor and builds the
plugin against them, and after a Hyprland upgrade one `hyprpm update` rebuilds every plugin you
have installed — which is exactly the chore an ABI-locked plugin creates.

```sh
hyprpm update                                            # once, to fetch/build headers
hyprpm add https://github.com/cybergaz/hyprscape
hyprpm enable hyprscape
```

Then add to your config so plugins load at startup, and bind the overview:

```lua
-- hyprland.lua  (there is no hl.exec_once; run it from the start event)
hl.on("hyprland.start", function() hl.exec_cmd("hyprpm reload -n") end)
hl.bind("SUPER + U", function() hl.plugin.hyprscape.toggle("all") end)
```

```conf
# hyprland.conf
exec-once = hyprpm reload -n
bind = SUPER, U, hyprscape:toggle, all
```

**After every Hyprland upgrade, run `hyprpm update`.** Until you do, Hyprland will refuse to
load the plugin, because it was built against the previous version.

hyprpm compiles Hyprland's headers from source, so it needs a toolchain. It checks for `cpio`,
`cmake`, `pkg-config`, `g++`, `gcc` and `git` up front, and the cmake configure step wants
Hyprland's own build dependencies on top of that:

```sh
# Arch
sudo pacman -S --needed base-devel cmake cpio git meson ninja pkgconf

# Fedora
sudo dnf install @development-tools cmake cpio git meson ninja-build pkgconf

# openSUSE
sudo zypper install -t pattern devel_basis && sudo zypper install cmake cpio git meson ninja

# Debian / Ubuntu
sudo apt install build-essential cmake cpio git meson ninja-build pkg-config
```

hyprpm also works on NixOS — it wraps its build steps in `nix develop` — but the flake below is
the better fit there.

If `hyprpm add` reports that the plugin failed to build, run it again with `-v`. A message saying
hyprscape supports a different Hyprland series is the version guard doing its job — see
[Compatibility](#compatibility).

### Arch (PKGBUILD)

A `PKGBUILD` is in [`packaging/arch/`](packaging/arch), for the tagged release and for git:

```sh
git clone https://github.com/cybergaz/hyprscape
cd hyprscape/packaging/arch
makepkg -si
```

It installs `/usr/lib/libhyprscape.so`. Load it the normal way:

```lua
hl.plugin.load("/usr/lib/libhyprscape.so")
hl.bind("SUPER + U", function() hl.plugin.hyprscape.toggle("all") end)
```

```conf
plugin = /usr/lib/libhyprscape.so
bind = SUPER, U, hyprscape:toggle, all
```

**A package cannot know when Hyprland changes underneath it.** `pacman -Syu` will happily upgrade
Hyprland and leave you with a plugin that no longer loads, and Hyprland will simply refuse it at
startup. Rebuild the package after every Hyprland upgrade, or use hyprpm, which does that for you.

### NixOS / Home Manager

Add the input:

```nix
{
  inputs.hyprscape = {
    url = "github:cybergaz/hyprscape";
    inputs.nixpkgs.follows = "nixpkgs";
  };
}
```

Then, in your Home Manager Hyprland config — passing the *same* Hyprland package your session
runs, which is what keeps the ABI matched across every rebuild:

```nix
{ config, pkgs, inputs, ... }:
{
  wayland.windowManager.hyprland.plugins = [
    (inputs.hyprscape.lib.mkHyprscape {
      inherit pkgs;
      hyprland = config.wayland.windowManager.hyprland.package;
    })
  ];
}
```

Home Manager emits the `hl.plugin.load(...)` call for you, so all that is left is the bind:

```lua
hl.bind("SUPER + U", function() hl.plugin.hyprscape.toggle("all") end)
```

If you run the Hyprland from nixpkgs rather than from the Hyprland flake, the packaged output is
already exactly that and takes no arguments:

```nix
wayland.windowManager.hyprland.plugins = [ inputs.hyprscape.packages.${pkgs.system}.hyprscape ];
```

There is also an overlay, if you would rather have `pkgs.hyprscape`:

```nix
nixpkgs.overlays = [ inputs.hyprscape.overlays.default ];
```

> Because plugin keys do not exist until `hl.plugin.load()` has actually run, a Lua config that
> sets `plugin.hyprscape.*` values logs one "unknown config key" warning on the first parse pass
> and then works. Harmless, but `hyprland --verify-config` will flag it.

### By hand

Any distribution where `pkg-config hyprland` resolves — that is, with Hyprland's headers
installed:

```sh
git clone https://github.com/cybergaz/hyprscape
cd hyprscape
make check          # what will it build against?
make                # → ./libhyprscape.so
make install        # → ~/.local/lib/libhyprscape.so   (PREFIX= to change)
```

```lua
hl.plugin.load(os.getenv("HOME") .. "/.local/lib/libhyprscape.so")
```

Try it without committing to anything:

```sh
hyprctl plugin load  /absolute/path/to/libhyprscape.so
hyprctl plugin unload /absolute/path/to/libhyprscape.so
```

**On NixOS**, there is no system-wide `pkg-config` entry for Hyprland, so `make` cannot work. Use
the flake, or `./build.sh`, which finds the running compositor's derivation in the Nix store and
compiles inside its build environment — no pinning, no `PKG_CONFIG_PATH` fiddling, and it cannot
silently build against the wrong Hyprland.

## Compatibility

hyprscape hooks Hyprland's renderer by **mangled C++ symbol name**, which is how it can take over
`renderWorkspace` without patching the compositor. The cost is that it is tied to one release
series.

| hyprscape | Hyprland |
| --- | --- |
| 0.2.x | 0.55.x |

`make` refuses to build against anything else and says so, rather than producing a plugin that
loads and then cannot find what it needs. If you want to try regardless:

```sh
make HYPRSCAPE_SKIP_VERSION_CHECK=1
```

Hyprland's own loader is the second line of defence: it stores a hash of the build a plugin was
compiled against and refuses to load a mismatch, so the worst case is a plugin that does not
load, not a broken session.

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

The view follows the compositor, not just its own navigation: switch workspace with your own bind
while the overview is open and the rows scroll to centre it. The selected row is always the one in
the middle.

Navigating the overview really navigates: the compositor switches workspace and moves focus along
with you. So every bind you already have — close the window, move it to workspace 3, swap columns —
acts on whatever is in the centre, not on whatever was focused before you opened the overview.

Only *bare* keys from the table above are intercepted. Anything with a modifier held goes straight
to your own binds, so `SUPER+SHIFT+L` still swaps columns and `ALT+2` still throws a window at
workspace 2.

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
| `auto_fit` | `1` | `0` — always use `zoom`. `1` — pick **one** zoom when the overview opens, tight enough that the most demanding workspace fits with its anchor centred, and hold it for the whole session. `2` — re-fit for whichever workspace is selected, so the zoom changes as you move between rows. |
| `min_zoom` | `0.12` | the floor `auto_fit` will not go below |
| `fit_rows` | `0` | also zoom out until *every* workspace row fits on screen at once. Worth turning on if you keep many workspaces and would rather see all of them than see any of them clearly. |

`auto_fit` matters for long tapes: at mode `1` "show me everything" actually shows you everything
instead of running off the bezel, and the zoom stays put while you navigate. Set it to `0` for
niri's strictly fixed zoom.

### Appearance

| Key | Default | Meaning |
| --- | --- | --- |
| `backdrop_color` | `rgba(16161ecc)` | dims the wallpaper as the overview opens, and is the opaque base when background layers are off. Its alpha is faded in with the animation, so at rest it changes nothing. |
| `card_color` | `rgba(00000000)` | band behind every workspace row, full output width |
| `active_row_color` | `rgba(00000000)` | band behind the *selected* row. Off by default — the ring on the centred window already says where you are. |
| `render_background_layers` | `1` | draw background/bottom layer surfaces (your wallpaper) — unscaled and unmoved, as a backdrop |
| `render_top_layers` | `1` | draw top/overlay layer surfaces (your bar) — unscaled and unmoved, over everything |
| `active_border_size` | `2` | **the centre rectangle** — the ring around the centred window of the selected row, which is what closing will focus; `0` disables |
| `active_border_color` | `rgba(3399ffff)` | its colour |
| `active_border_rounding` | `-1` | its corner radius, in logical pixels. `-1` follows the ringed window's own rounding, scaled by the zoom |
| `hover_border_size` | `3` | ring around the hovered window; `0` disables. Only ever shown after the pointer has actually moved — walking the tape with the keyboard never lights one up |
| `hover_border_color` | `rgba(88bbffff)` | its colour |
| `hover_border_rounding` | `-1` | its corner radius; `-1` follows the window's own rounding |
| `window_rounding` | `-1` | corner radius of the windows themselves in the overview. `-1` scales each window's own rounding by the zoom, which is what a true zoom-out looks like; set a number to force one flat radius |

Every ring is snapped to whole device pixels before it is drawn, so all four of its sides come
out the same width regardless of where the zoom happens to land the box.

### Which workspaces get a row

| Key | Default | Meaning |
| --- | --- | --- |
| `show_empty` | `0` | also give a row to empty workspaces you are not on. **Leave this off** — it is what made hyprtasking's grid look like nonsense. |
| `trailing_workspace` | `1` | keep one empty row at the bottom, niri style, as a drop target for making a new workspace |
| `new_workspace_hint` | `0` | outline that trailing row so the drop target is visible. Off by default: rows span the whole output, so an outline reads as two lines across the screen rather than as a card. |

### Behaviour

| Key | Default | Meaning |
| --- | --- | --- |
| `exit_on_click` | `1` | a click picks a target and leaves; `0` keeps the overview open |
| `close_on_reload` | `1` | close when the config reloads |
| `warp_cursor` | `0` | warp the pointer onto the window you select |
| `scroll_speed` | `1.0` | wheel sensitivity when walking rows |
| `select_button` | `BTN_LEFT` (272) | button that picks a window |
| `pan_button` | `BTN_RIGHT` (273) | button that drags the tape sideways; `0` disables |

### Everything, in one block

Copy-paste this into your Lua config and edit — every key with its default:

```lua
hl.config({
    plugin = {
        hyprscape = {
            -- geometry
            zoom          = 0.5,          -- workspace card size, as a fraction of the monitor
            workspace_gap = 0.1,          -- vertical gap between rows, fraction of monitor height
            auto_fit      = 1,            -- 0 fixed | 1 one zoom per session | 2 per workspace
            min_zoom      = 0.12,         -- floor for auto_fit
            fit_rows      = 0,            -- also fit every workspace row on screen at once

            -- appearance
            backdrop_color           = "rgba(16161ecc)",  -- wallpaper dim; base when layers are off
            card_color               = "rgba(00000000)",  -- band behind every row
            active_row_color         = "rgba(00000000)",  -- band behind the selected row
            render_background_layers = 1,                 -- wallpaper, unscaled and unmoved
            render_top_layers        = 1,                 -- bar, unscaled and unmoved
            active_border_size       = 2,                 -- the centre rectangle
            active_border_color      = "rgba(3399ffff)",
            active_border_rounding   = -1,                -- -1 = follow the window's rounding
            hover_border_size        = 3,                 -- ring on the hovered window
            hover_border_color       = "rgba(88bbffff)",
            hover_border_rounding    = -1,
            window_rounding          = -1,                -- -1 = scale each window's own rounding

            -- which workspaces get a row
            show_empty         = 0,       -- also show empty workspaces you are not on
            trailing_workspace = 1,       -- keep one empty row at the bottom as a drop target
            new_workspace_hint = 0,       -- outline that trailing row

            -- behaviour
            exit_on_click   = 1,          -- a click picks a target and leaves
            close_on_reload = 1,
            warp_cursor     = 0,          -- warp the pointer onto what you select
            scroll_speed    = 1.0,        -- wheel sensitivity when walking rows
            select_button   = 272,        -- BTN_LEFT
            pan_button      = 273,        -- BTN_RIGHT, 0 disables
            debug           = 0,          -- log per-window overview geometry each frame

            -- motion
            animation_curve   = "smooth", -- "smooth" | "spring" | "inherit" | your own curve name
            animation_speed   = 4,        -- deciseconds; higher is slower. Springs ignore it
            animation_enabled = 1,        -- 0 snaps with no animation at all
            spring_stiffness  = 250,      -- only for animation_curve = "spring"
            spring_damping    = 25,       -- raise it to take the bounce out
            spring_mass       = 1,

            -- touchpad
            gestures = {
                enabled       = 1,
                open_fingers  = 4,
                open_distance = 300,
                open_positive = 0,        -- 1 if swiping down should open
            },
        },
    },
})
```

Remember the first-pass caveat: `hl.plugin.load()` only records the path, so these keys do not
exist until after your config has finished evaluating. Setting them produces one "unknown config
key" warning on the first pass, then Hyprland loads the plugin and re-parses. Harmless, but
`--verify-config` will flag it.

### Touchpad

| Key | Default | Meaning |
| --- | --- | --- |
| `gestures:enabled` | `1` | |
| `gestures:open_fingers` | `4` | niri uses four |
| `gestures:open_distance` | `300` | swipe distance for a full open/close |
| `gestures:open_positive` | `0` | `1` if swiping *down* should open |

### Motion

| Key | Default | Meaning |
| --- | --- | --- |
| `animation_curve` | `smooth` | `"smooth"`, `"spring"`, `"inherit"`, or the name of any bezier or spring you defined with `hl.curve` |
| `animation_speed` | `4` | duration in deciseconds — **higher is slower**, same units as Hyprland's own `speed`. Springs ignore it; they run until they settle. |
| `animation_enabled` | `1` | `0` snaps instantly, with no animation at all |
| `spring_stiffness` | `250` | for `animation_curve = "spring"` |
| `spring_damping` | `25` | raise it to take the bounce out; drop it to about `5` for a pronounced overshoot |
| `spring_mass` | `1` | |

Everything the overview animates — opening and closing, walking columns, walking rows, the zoom
settling — runs off this one node.

- **`smooth`** is an ease-out cubic. It never travels past its goal, so nothing bounces.
- **`spring`** is a real spring: it accelerates, overshoots if it is underdamped, and settles.
  It ignores `animation_speed`; how long it takes is a consequence of stiffness, damping and mass.
- **`inherit`** is the pre-0.2 behaviour: whatever you set for the `workspaces` leaf, curve and
  speed and all. Hyprland's `default` bezier ends at 1.05, so this one *does* overshoot slightly.
- Any other value names a curve of your own:

```lua
hl.curve("myease", { type = "bezier", points = { { 0.2, 0.9 }, { 0.1, 1 } } })
hl.curve("mybounce", { type = "spring", stiffness = 400, damping = 12 })

hl.config({ plugin = { hyprscape = {
    animation_curve = "myease",   -- or "mybounce": springs are matched by name too
    animation_speed = 3,
} } })
```

If the name matches nothing, hyprscape logs once and falls back to `smooth`.

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

- **Off-viewport columns.** The render pass hands each surface `frameDamage ∩ its own
  untransformed box`. For a column scrolled off the viewport that intersection is empty and the
  surface is dropped before the modifier ever gets a chance to move it on screen — which caps you
  at the two or three columns nearest the viewport. hyprscape widens the frame damage well past
  the monitor for the duration of its own pass.
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

Column navigation is pure overview state — an index into that workspace's own tape — so it works on
any row rather than only on whichever workspace the compositor considers active. Selecting anything
focuses it for real, with `FOCUS_REASON_KEYBIND`: the scrolling layout treats a `CLICK` focus as
"only scroll if the pointer is already over the window", which can never be true of a zoomed-out
copy, so a click focus would leave you focused on something you could not see.

## Limitations

- Written for Hyprland **0.55.x**. It resolves several functions by mangled symbol, so a different
  Hyprland is refused at build time by `make`, and by Hyprland's own loader if you force it.
- Special workspaces (scratchpads) are not shown.
- A workspace containing a genuinely fullscreen window is shown from its window positions, which
  the scrolling layout stops updating while fullscreen is active.
- Multi-monitor is implemented but has only been exercised on a single output.
- Windows are not vertically cropped to their card. In a scrolling layout columns fit the work
  area, so this is not visible in practice.

## Development

```sh
nix develop            # or: install Hyprland's headers
make check             # what would this build against?
make                   # → ./libhyprscape.so
./build.sh             # NixOS: compile inside the running Hyprland's own build env
./test/nested.sh       # a throwaway nested Hyprland with the plugin loaded
```

`test/nested.sh` starts a second Hyprland inside your session with a minimal scrolling-layout
config and a few terminals, so the plugin can be exercised without putting your real session at
risk. Screenshot it with `WAYLAND_DISPLAY=wayland-2 grim out.png`.

Hyprland's plugin API is undocumented; the compositor's own source is the reference. On a Nix
system it is already unpacked in the store — find it with `fd -t f OpenGL.cpp /nix/store -d 6`.

### Releasing

1. Bump `version` in `flake.nix`, `pkgver` in `packaging/arch/PKGBUILD`, and the table in
   [Compatibility](#compatibility); add a `CHANGELOG.md` entry.
2. `git tag -a v0.2.0 -m 'hyprscape 0.2.0' && git push --tags`.
3. When support for a new Hyprland series lands, add a `commit_pins` entry to `hyprpm.toml` for
   the last commit that worked on the old one, so `hyprpm update` keeps building for people who
   have not upgraded.

## Contributing

Issues and pull requests welcome. If you are reporting a rendering bug, `plugin:hyprscape:debug
= 1` logs the box hyprscape computes for every window each frame, which is usually enough to tell
a geometry bug from a drawing one.

## License

BSD-3-Clause. See [LICENSE](LICENSE).
