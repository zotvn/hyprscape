#pragma once

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/math/Box.hpp>

using namespace Render;

typedef void (*render_workspace_t)(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now, const CBox& geometry);

typedef void (*render_window_t)(void* thisptr, PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& time, bool decorate, eRenderPassMode mode, bool ignorePosition,
                                bool standalone);

typedef void (*render_layer_t)(void* thisptr, PHLLS layer, PHLMONITOR monitor, const Time::steady_tp& time, bool popups, bool lockscreen);

typedef long VIEWID;
