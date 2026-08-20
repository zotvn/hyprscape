#pragma once

#include <optional>
#include <unordered_map>
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
    int index = 0;
    bool synthetic = false; // the trailing "drop here for a new workspace" card

    // The row's band: full monitor width, one card tall. This is what the row *is* now that
    // each tape slides underneath a stationary centre -- a viewport-shaped rectangle would have
    // to travel with the tape, which is exactly what it must not do. Used for hit-testing and
    // for the optional row background.
    CBox box;

    // The mapping this row draws under: a global-logical point p lands at
    // (p - viewOrigin) * zoom + contentOrigin. viewOrigin is the tape coordinate parked at the
    // centre of the screen (see HSView::anchorOffset); contentOrigin is where it lands.
    Vector2D viewOrigin;
    Vector2D contentOrigin;
};

// One column of the scroll tape: the windows that share an x, and where that column's centre
// sits in global logical coords.
struct HSColumn {
    double centerX = 0.0;
    std::vector<PHLWINDOW> windows;
};

// Everything geometric about one moment in time, computed once and passed around, so rendering
// and hit-testing can never disagree about where a window is.
struct HSFrame {
    float progress = 0.F;
    float zoom = 1.F;
    CBox monitorBox;
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
    PHLANIMVAR<float> m_pan;      // manual horizontal pan, in workspace pixels
    PHLANIMVAR<float> m_fitZoom;  // auto-fit target zoom, animated between workspaces

    // How far the selected row's anchor column sits from the screen centre. Animated, so
    // scrolling columns slides the tape through a stationary centre rather than jumping.
    PHLANIMVAR<float> m_anchorX;

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

    // Navigation.
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

    // The window the selected workspace is currently centred on -- what closing should focus.
    PHLWINDOW selectedAnchor() const;

    void render();

  private:
    void renderCard(const HSCard& card, const HSFrame& frame, const Time::steady_tp& time);
    void renderLayers(const Time::steady_tp& time, bool top);
    void postRender();

    std::vector<PHLWORKSPACE> visibleWorkspaces(WORKSPACEID& maxId) const;
    static std::vector<PHLWINDOW> workspaceWindows(PHLWORKSPACE workspace);

    // The scroll tape as columns, left to right.
    static std::vector<HSColumn> columnsOf(PHLWORKSPACE workspace);

    // The window each workspace is centred on. Captured when the overview opens -- the central
    // column is where you *were*, not wherever focus drifts to afterwards.
    std::unordered_map<WORKSPACEID, PHLWINDOWREF> m_anchors;

    void captureAnchors();
    PHLWINDOW anchorWindow(PHLWORKSPACE workspace) const;

    // Signed distance from the screen centre to the anchor column's centre, in workspace px.
    // This is the only thing that moves when you scroll columns; the centre itself never does.
    double anchorOffset(PHLWORKSPACE workspace) const;

    // Auto-fit zoom for the selected workspace, keeping its anchor column centred.
    void updateFit(bool warp);

    int rowIndexOf(WORKSPACEID id, const std::vector<HSCard>& cards) const;
    void syncSelectionToMonitor();
};

typedef SP<HSView> PHSVIEW;
