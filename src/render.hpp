#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/helpers/math/Math.hpp>

// Push a renderModif that maps any global-logical point p to (p - srcOrigin) * scale + dstOrigin.
// Everything drawn until hs_pop_modif() lands inside that frame, so one push per workspace card
// is enough for all of its windows, decorations and layer surfaces.
void hs_push_modif(PHLMONITOR monitor, const Vector2D& srcOrigin, const Vector2D& dstOrigin, float scale);
void hs_pop_modif();

// Draw `window` so its main surface lands exactly on `box` (global logical coords). Used for the
// window being dragged, which follows the cursor rather than a card.
void hs_render_window_at_box(PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& time, CBox box, bool decorate);

// Draw one window with whatever modif is currently pushed.
void hs_render_window(PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& time, int passMode);

// Draw one layer surface with whatever modif is currently pushed.
void hs_render_layer(PHLLS layer, PHLMONITOR monitor, const Time::steady_tp& time, bool popups);

// Where Hyprland will actually draw `window` if left alone: real position plus the workspace's
// slide offset plus the floating drag offset. Overview geometry has to be derived from this, not
// from positionAnimation() alone, or every window on a slid-out workspace lands a screenful off.
Vector2D hs_window_render_pos(PHLWINDOW window);

// The slide offset Hyprland has parked on a workspace. Folded into the card's srcOrigin so the
// whole card cancels it in one go.
Vector2D hs_workspace_render_offset(PHLWORKSPACE workspace);

// Hidden workspaces are parked by Hyprland's workspace animation: with a `slidefade*` style their
// m_alpha sits at 0, and renderWindow multiplies every surface by it. Neutralise it around the
// draw. Writing through the non-const value() leaves the goal and the animation timer untouched,
// so an in-flight workspace animation is not disturbed.
class HSWorkspaceAlphaGuard {
  public:
    explicit HSWorkspaceAlphaGuard(PHLWORKSPACE workspace);
    ~HSWorkspaceAlphaGuard();

    HSWorkspaceAlphaGuard(const HSWorkspaceAlphaGuard&) = delete;
    HSWorkspaceAlphaGuard& operator=(const HSWorkspaceAlphaGuard&) = delete;

  private:
    PHLWORKSPACE m_workspace;
    float m_saved = 1.F;
    bool m_touched = false;
};
