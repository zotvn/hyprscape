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

    // Where the selected row's anchor column sits relative to the centre. Its goal tracks the
    // live geometry every frame; the easing between goals is what makes a column scroll read as
    // the tape sliding through a stationary centre rather than snapping to it.
    PHLANIMVAR<float> m_anchorX;
    WORKSPACEID m_selected = WORKSPACE_INVALID;

    // The centre rectangle, in unscaled workspace pixels: the vertical centre of the anchor
    // window and its size. Both are animated so a change of anchor morphs the rectangle in
    // place; its horizontal position is never stored, because it is always the middle of the
    // output. See HSView::centerBox.
    PHLANIMVAR<float> m_centerY;
    PHLANIMVAR<Vector2D> m_centerSize;
    bool m_centerValid = false;

    PHLWINDOWREF m_hovered;

    // Hover only means anything once the pointer has actually moved. Walking the tape with the
    // keyboard slides windows underneath a parked cursor, and highlighting whichever one drifts
    // under it looks like a random window lighting up.
    bool m_hoverArmed = false;

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
    void disarmHover();

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

    // Where the anchor window comes to rest. Drawn instead of the anchor's live box so the
    // rectangle never travels: it is the fixed reference the windows move to, not with.
    CBox centerBox(const HSFrame& frame) const;

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

    // Follow the compositor and keep the selected row centred. Runs every frame, because the
    // workspace can change from outside the overview and rows can appear or disappear under it.
    void syncRow();

    // Retarget the centre rectangle at whatever the selected row is anchored on.
    void syncCenter(bool warp);
};

typedef SP<HSView> PHSVIEW;
