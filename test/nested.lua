-- Nested Hyprland used only to exercise hyprscape. Deliberately minimal.

hl.plugin.load("@PLUGIN@")

hl.config({
    general = {
        layout = "scrolling",
        gaps_in = 4,
        gaps_out = 12,
        border_size = 2,
        col = {
            active_border = "rgba(0066ffcc)",
            inactive_border = "rgba(444444aa)",
        },
    },
    scrolling = { column_width = 0.5 },
    decoration = { rounding = 8 },
    misc = {
        disable_hyprland_logo = true,
        disable_splash_rendering = true,
        force_default_wallpaper = 0,
    },
    animations = { enabled = true },
    debug = { disable_logs = false },
})

-- Same workspace animation style as the real session, so the alpha/offset compensation is
-- actually exercised rather than trivially satisfied.
hl.animation({ leaf = "workspaces", enabled = true, speed = 3, bezier = "default", style = "slidefadevert 15%" })

hl.bind("SUPER + Q", hl.dsp.exec_cmd("alacritty"))
hl.bind("SUPER + C", hl.dsp.window.close())
hl.bind("SUPER + M", hl.dsp.exit())

hl.bind("SUPER + U", function() hl.plugin.hyprscape.toggle("all") end)
hl.bind("SUPER + L", hl.dsp.layout("focus r"))
hl.bind("SUPER + H", hl.dsp.layout("focus l"))

for i = 1, 5 do
    hl.bind("SUPER + " .. i, hl.dsp.focus({ workspace = i }))
    hl.bind("SUPER + SHIFT + " .. i, hl.dsp.window.move({ workspace = i }))
end

hl.on("hyprland.start", function()
    -- Populate a few workspaces so the overview has a real scroll tape to show.
    hl.exec_cmd("hyprctl dispatch workspace 1")
    hl.exec_cmd("sh -c 'sleep 1; hyprctl dispatch exec [workspace 1] alacritty -t w1a'")
    hl.exec_cmd("sh -c 'sleep 2; hyprctl dispatch exec [workspace 1] alacritty -t w1b'")
    hl.exec_cmd("sh -c 'sleep 3; hyprctl dispatch exec [workspace 1] alacritty -t w1c'")
    hl.exec_cmd("sh -c 'sleep 4; hyprctl dispatch exec [workspace 3] alacritty -t w3a'")
    hl.exec_cmd("sh -c 'sleep 5; hyprctl dispatch exec [workspace 3] alacritty -t w3b'")
    hl.exec_cmd("sh -c 'sleep 6; hyprctl dispatch exec [workspace 5] alacritty -t w5a'")
end)
