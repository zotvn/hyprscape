#pragma once

#include <optional>
#include <vector>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>

#include "types.hpp"

// One workspace's "card": the monitor viewport, scaled by the current zoom and parked at its
// row. Windows that are scrolled off the workspace viewport land OUTSIDE this box horizontally
// -- that spill is the whole point, it is the scroll tape made visible.
struct HSCard {
    WORKSPACEID id = WORKSPACE_INVALID;
    PHLWORKSPACE workspace = nullptr;
    CBox box; // global logical coords
    int index = 0;
    bool synthetic = false; // the trailing "drop here for a new workspace" card
};

// Everything geometric about one moment in time, computed once and passed around, so rendering
// and hit-testing can never disagree about where a window is.
struct HSFrame {
    float progress = 0.F;
    float zoom = 1.F;
    CBox monitorBox;
    Vector2D viewOrigin; // global logical point that maps to each card's top-left
    std::vector<HSCard> cards;

    const HSCard* card(WORKSPACEID id) const;
};

class HSView {
  public:
    explicit HSView(MONITORID monitorId);

    MONITORID m_monitorId;

    // m_active covers "open or closing"; the render hook stays engaged until the close
    // animation has fully played out.
    bool m_active = false;
    bool m_closing = false;

    PHLANIMVAR<float> m_progress; // 0 = desktop, 1 = fully zoomed out
    PHLANIMVAR<float> m_row;      // animated row index of the selected workspace
    PHLANIMVAR<float> m_pan;      // manual horizontal pan along the tape, in workspace pixels

    // Auto-fit, animated so that moving between workspaces with very different tape lengths
    // eases rather than snaps.
    PHLANIMVAR<float> m_fitZoom;
    PHLANIMVAR<float> m_fitPan;

    WORKSPACEID m_selected = WORKSPACE_INVALID;
    PHLWINDOWREF m_hovered;

    // Window being dragged between workspaces. It is lifted out of its card and drawn under the
    // cursor instead, so the drop target is unambiguous.
    PHLWINDOWREF m_dragged;
    Vector2D m_dragGrab; // cursor-relative offset of the dragged window's top-left, in overview px

    PHLMONITOR monitor() const;
    bool rendering() const;

    void show();
    void hide(PHLWINDOW focusWindow = nullptr);
    void toggle();
    void onConfigReloaded();

    // Navigation. selectRow moves between workspaces, selectColumn defers to the layout's own
    // focus so it works with scrolling, dwindle and master alike.
    void selectRow(int delta);
    void selectWorkspace(WORKSPACEID id);
    void selectColumn(int delta);
    void panBy(double dx);

    // Drag and drop between workspace rows.
    bool beginDrag(const Vector2D& cursor);
    void endDrag(const Vector2D& cursor);
    void cancelDrag();

    // Geometry.
    HSFrame frame() const;
    CBox windowBox(PHLWINDOW window, const HSCard& card, const HSFrame& frame) const;

    std::optional<HSCard> cardAt(const Vector2D& global) const;
    PHLWINDOW windowAt(const Vector2D& global) const;

    void render();

  private:
    void renderCard(const HSCard& card, const HSFrame& frame, const Time::steady_tp& time);
    void renderTopLayers(const Time::steady_tp& time);
    void postRender();

    std::vector<PHLWORKSPACE> visibleWorkspaces(WORKSPACEID& maxId) const;
    static std::vector<PHLWINDOW> workspaceWindows(PHLWORKSPACE workspace);

    // Union of the monitor viewport and every window on the workspace, in global logical coords.
    // Drives auto-fit: a tape longer than the screen makes the overview zoom out further instead
    // of running off the edge.
    CBox contentBounds(PHLWORKSPACE workspace, const CBox& monitorBox) const;

    // Recompute the auto-fit zoom and pan for the selected workspace. `warp` skips the animation,
    // which is what opening the overview wants.
    void updateFit(bool warp);

    int rowIndexOf(WORKSPACEID id, const std::vector<HSCard>& cards) const;
    void syncSelectionToMonitor();
};

typedef SP<HSView> PHSVIEW;
