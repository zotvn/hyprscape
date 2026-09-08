#include "manager.hpp"

#include <algorithm>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include "anim.hpp"
#include "config.hpp"
#include "globals.hpp"

void HSManager::rebuildViews() {
    // Drop views whose monitor is gone, add one for every monitor we do not have yet.
    std::erase_if(m_views, [](const PHSVIEW& v) { return !v || !v->monitor(); });

    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor || monitor->m_id == MONITOR_INVALID)
            continue;
        if (viewForMonitor(monitor))
            continue;
        m_views.push_back(makeShared<HSView>(monitor->m_id));
        hs_log("added view for monitor {}", monitor->m_id);
    }
}

void HSManager::removeViewForMonitor(MONITORID monitorId) {
    std::erase_if(m_views, [monitorId](const PHSVIEW& v) { return !v || v->m_monitorId == monitorId; });
}

PHSVIEW HSManager::viewForMonitor(PHLMONITOR monitor) const {
    if (!monitor)
        return nullptr;
    for (const auto& v : m_views)
        if (v && v->m_monitorId == monitor->m_id)
            return v;
    return nullptr;
}

PHSVIEW HSManager::viewFromCursor() const {
    return viewForMonitor(State::monitorState()->query().vec(g_pInputManager->getMouseCoordsInternal()).run());
}

bool HSManager::anyRendering() const {
    return std::ranges::any_of(m_views, [](const PHSVIEW& v) { return v && v->rendering(); });
}

bool HSManager::anyActive() const {
    return std::ranges::any_of(m_views, [](const PHSVIEW& v) { return v && v->m_active && !v->m_closing; });
}

void HSManager::toggle(const std::string& arg) {
    if (anyActive())
        hide();
    else
        show(arg);
}

void HSManager::show(const std::string& arg) {
    if (arg == "cursor") {
        if (const auto v = viewFromCursor())
            v->show();
        return;
    }

    for (const auto& v : m_views)
        if (v)
            v->show();
}

void HSManager::hide() {
    for (const auto& v : m_views)
        if (v && v->m_active)
            v->hide(nullptr);
}

void HSManager::onConfigReloaded() {
    // Curves and speeds are read straight out of the config, and the legacy parser drops every
    // registered bezier when it re-parses, so this has to happen before anything animates again.
    hs_refresh_animation_config();

    rebuildViews();
    for (const auto& v : m_views)
        if (v)
            v->onConfigReloaded();
}
