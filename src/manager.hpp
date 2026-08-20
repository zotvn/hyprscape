#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>

#include "view.hpp"

class HSManager {
  public:
    std::vector<PHSVIEW> m_views;

    void rebuildViews();
    void removeViewForMonitor(MONITORID monitorId);

    PHSVIEW viewForMonitor(PHLMONITOR monitor) const;
    PHSVIEW viewFromCursor() const;

    bool anyRendering() const;
    bool anyActive() const;

    // arg is "all" (every monitor) or "cursor" (only the one under the pointer).
    void toggle(const std::string& arg);
    void show(const std::string& arg);
    void hide();

    void onConfigReloaded();

    // Input. Each returns true when the event must not reach the rest of the compositor.
    bool onMouseMove();
    bool onMouseButton(const IPointer::SButtonEvent& event);
    bool onMouseAxis(const IPointer::SAxisEvent& event);
    bool onKey(const IKeyboard::SKeyEvent& event);

    void swipeBegin(const IPointer::SSwipeBeginEvent& event);
    bool swipeUpdate(const IPointer::SSwipeUpdateEvent& event);
    bool swipeEnd();

  private:
    void updateHover();

    // Held modifier keycodes. The overview only claims a key when nothing is held, so binds
    // like SUPER+SHIFT+L keep reaching the user's own config.
    std::unordered_set<uint32_t> m_modsHeld;
    bool shiftHeld() const;
    bool anyModHeld() const;

    bool m_buttonDown = false;
    bool m_dragging = false;
    Vector2D m_pressPos;
    PHLWINDOWREF m_pressWindow;

    bool m_panning = false;
    Vector2D m_panLastPos;

    uint32_t m_lastRowScrollMs = 0;

    enum eSwipeState : uint8_t {
        SWIPE_NONE = 0,
        SWIPE_OPEN,
    };

    eSwipeState m_swipeState = SWIPE_NONE;
    float m_swipeAmount = 0.F;
};
