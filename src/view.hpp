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
    PHLANIMVAR<float> m_fitZoom;  // the zoom the overview settles at, fixed for the session

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

    // The window each workspace is centred on: whatever it has focused. Derived live rather
    // than cached, so closing a window or any outside focus change re-centres by itself.
    PHLWINDOW anchorWindow(PHLWORKSPACE workspace) const;

    // Focus a window for real, and switch to its workspace if needed. FOCUS_REASON_KEYBIND is
    // load-bearing: the scrolling layout treats a CLICK focus as "only scroll if the pointer is
    // already over the window", which can never hold for a zoomed-out copy of it.
    void applySelection(PHLWINDOW window);

    // Signed distance from the screen centre to the anchor column's centre, in workspace px.
    // This is the only thing that moves when you scroll columns; the centre itself never does.
    double anchorOffset(PHLWORKSPACE workspace) const;

    // Zoom at which a workspace fits entirely on screen with its anchor column centred.
    double requiredZoom(PHLWORKSPACE workspace, const CBox& monitorBox) const;
    void updateFit(bool warp);

    int rowIndexOf(WORKSPACEID id, const std::vector<HSCard>& cards) const;
    void syncSelectionToMonitor();
};

typedef SP<HSView> PHSVIEW;
