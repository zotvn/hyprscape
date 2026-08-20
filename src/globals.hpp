#pragma once

#include <format>
#include <memory>
#include <stdexcept>

#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include "manager.hpp"

inline constexpr auto LOG = Hyprutils::CLI::LOG_DEBUG;
inline constexpr auto ERR = Hyprutils::CLI::LOG_ERR;

inline HANDLE PHANDLE = nullptr;

// Render takeover. renderWorkspace is the one we replace; the rest are corrective hooks that
// keep Hyprland's scissor / blur / solitary paths honest while we draw through a renderModif.
inline CFunctionHook* render_workspace_hook = nullptr;
inline CFunctionHook* render_texture_hook = nullptr;
inline CFunctionHook* render_border_hook = nullptr;
inline CFunctionHook* render_border2_hook = nullptr;
inline CFunctionHook* blur_optimizations_hook = nullptr;
inline CFunctionHook* visible_region_hook = nullptr;
inline CFunctionHook* is_solitary_blocked_hook = nullptr;

typedef uint32_t (*orig_is_solitary_blocked_t)(void*, bool);

// renderWindow and renderLayer are protected, so we resolve their addresses and call them
// through raw pointers rather than hooking them.
inline void* render_window = nullptr;
inline void* render_layer = nullptr;

inline std::unique_ptr<HSManager> hs_manager;

template <typename... Args>
inline void fail_exit(const std::format_string<Args...>& fmt, Args... args) {
    std::string err = "[hyprscape] " + std::vformat(fmt.get(), std::make_format_args(args...));
    HyprlandAPI::addNotification(PHANDLE, err, CHyprColor {1.0, 0.2, 0.2, 1.0}, 6000);
    throw std::runtime_error(err);
}

template <typename... Args>
inline void hs_log(const std::format_string<Args...>& fmt, Args... args) {
    Log::logger->log(LOG, "[hyprscape] {}", std::vformat(fmt.get(), std::make_format_args(args...)));
}
