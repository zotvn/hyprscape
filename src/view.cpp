#include "view.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include "anim.hpp"
#include "config.hpp"
#include "globals.hpp"
#include "pass/pass_element.hpp"
#include "render.hpp"

using Hyprutils::Utils::CScopeGuard;

namespace {

// Pass elements take monitor-local, already-scaled (buffer) coordinates.
CBox toBuffer(PHLMONITOR monitor, CBox globalBox) {
    globalBox.translate(-monitor->m_position);
    globalBox.scale(monitor->m_scale);
    return globalBox;
}

// renderBorder derives its scissor from CRegion{box}, and a CRegion is pixman -- integers, with
// both the origin AND the size truncated on the way in. A box at x=100.6 w=300.7 therefore
// scissors to [100, 400) while the quad really spans [100.6, 401.3), so the far edge loses a
// pixel that the near edge keeps: a 2px ring drew 2px top-left and 1px bottom-right. Snapping
// the box to whole buffer pixels first -- keeping x+w integral, not just rounding w -- makes
// every side come out the same width.
CBox snapToPixels(CBox box) {
    const double x = std::round(box.x);
    const double y = std::round(box.y);
    box.w = std::max(std::round(box.x + box.w) - x, 1.0);
    box.h = std::max(std::round(box.y + box.h) - y, 1.0);
    box.x = x;
    box.y = y;
    return box;
}

CHyprColor colorOf(const std::string& key) {
    return CHyprColor {(uint64_t)HSConfig::value<Config::INTEGER>(key)};
}

CHyprColor fade(CHyprColor c, float f) {
    return c.modifyA(c.a * std::clamp(f, 0.F, 1.F));
}

// Windows within one scroll-tape column share an x exactly; a couple of pixels of slack keeps
// rounding from splitting a column in two.
constexpr double COLUMN_EPS = 2.0;

// Half-extent of the damage region we hand the render pass, in buffer pixels. Comfortably
// larger than any scroll tape, and small enough that pixman's int32 arithmetic stays sane.
constexpr int HS_DAMAGE_SLACK = 1 << 20;

} // namespace

// Corner radius for a ring, in buffer pixels. -1 means "whatever the window itself rounds to",
// scaled by the zoom so a ring around a shrunken window is rounded like a shrunken window --
// renderBorder takes its radius in final screen pixels, not in the space the modif works in.
static int roundingFor(PHLWINDOW window, const std::string& key, PHLMONITOR monitor, float zoom) {
    const double scale = monitor ? monitor->m_scale : 1.0;

    const float configured = HSConfig::value<Config::FLOAT>(key);
    if (configured >= 0.F)
        return (int)std::round(configured * scale);

    return window ? (int)std::round(window->rounding() * scale * zoom) : 0;
}

const HSCard* HSFrame::card(WORKSPACEID id) const {
    for (const auto& c : cards)
        if (c.id == id)
            return &c;
    return nullptr;
}

HSView::HSView(MONITORID monitorId) : m_monitorId(monitorId) {
    // One animation node for the whole plugin, owned by us and refreshed on reload, so the
    // overview's feel is a config key rather than a side effect of the user's workspace curve.
    const auto cfg = hs_animation_config();

    Animation::mgr()->createAnimation(0.F, m_progress, cfg, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(0.F, m_row, cfg, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(0.F, m_pan, cfg, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(1.F, m_fitZoom, cfg, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(0.F, m_anchorX, cfg, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(0.F, m_centerY, cfg, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(Vector2D {}, m_centerSize, cfg, AVARDAMAGE_NONE);
}

PHLMONITOR HSView::monitor() const {
    return State::monitorState()->query().id(m_monitorId).run();
}

bool HSView::rendering() const {
    return m_active || m_progress->value() > 0.001F || m_progress->isBeingAnimated();
}

int HSView::rowIndexOf(WORKSPACEID id, const std::vector<HSCard>& cards) const {
    for (const auto& c : cards)
        if (c.id == id)
            return c.index;
    return 0;
}

std::vector<PHLWORKSPACE> HSView::visibleWorkspaces(WORKSPACEID& maxId) const {
    const auto monitor = this->monitor();
    std::vector<PHLWORKSPACE> out;
    maxId = 0;

    if (!monitor)
        return out;

    const bool showEmpty = HSConfig::value<Config::INTEGER>("show_empty");

    for (const auto& ref : State::workspaceState()->workspaces()) {
        const auto ws = ref.lock();
        if (!ws || ws->inert() || ws->m_isSpecialWorkspace)
            continue;
        if (ws->m_monitor != monitor)
            continue;

        maxId = std::max(maxId, ws->m_id);

        // This is the whole fix for hyprtasking's phantom tiles: a workspace earns a row by
        // having something on it, being where you are, or being where you are headed.
        const bool occupied = ws->getWindowCount() > 0;
        const bool current = monitor->m_activeWorkspace == ws;
        const bool selected = ws->m_id == m_selected;

        if (!showEmpty && !occupied && !current && !selected)
            continue;

        out.push_back(ws);
    }

    std::ranges::sort(out, [](const auto& a, const auto& b) { return a->m_id < b->m_id; });
    return out;
}

std::vector<PHLWINDOW> HSView::workspaceWindows(PHLWORKSPACE workspace) {
    std::vector<PHLWINDOW> out;
    if (!workspace)
        return out;

    for (const auto& w : Desktop::windowState()->windows()) {
        if (!w || w->m_workspace != workspace)
            continue;
        // 0.56 snapshots a closing window into a Desktop::CWindowFadeout and drops it from the
        // live list, so there is no m_fadingOut left to hold it in the overview: unmapped is gone.
        if (w->isHidden() || !w->m_isMapped || w->m_pinned)
            continue;
        out.push_back(w);
    }

    return out;
}

std::vector<HSColumn> HSView::columnsOf(PHLWORKSPACE workspace) {
    std::vector<HSColumn> columns;
    if (!workspace)
        return columns;

    const Vector2D offset = hs_workspace_render_offset(workspace);

    auto windows = workspaceWindows(workspace);
    // Floating windows are not part of the tape; they sit wherever they were put.
    std::erase_if(windows, [](const PHLWINDOW& w) { return w->m_isFloating; });

    std::ranges::sort(windows, [&offset](const PHLWINDOW& a, const PHLWINDOW& b) {
        const double ax = hs_window_render_pos(a).x - offset.x;
        const double bx = hs_window_render_pos(b).x - offset.x;
        if (std::abs(ax - bx) > COLUMN_EPS)
            return ax < bx;
        return hs_window_render_pos(a).y < hs_window_render_pos(b).y;
    });

    for (const auto& w : windows) {
        const double center = hs_window_render_pos(w).x - offset.x + w->sizeAnimation()->value().x / 2.0;

        if (!columns.empty() && std::abs(columns.back().centerX - center) <= COLUMN_EPS) {
            columns.back().windows.push_back(w);
            continue;
        }

        columns.push_back({.centerX = center, .windows = {w}});
    }

    return columns;
}

PHLWINDOW HSView::anchorWindow(PHLWORKSPACE workspace) const {
    if (!workspace)
        return nullptr;

    // Whatever the compositor has focused here is the centre. Reading it live rather than
    // caching it is what makes the view survive a window being closed underneath it: the layout
    // picks a new focus, and the next frame re-centres on that instead of drifting off-screen.
    const auto focused = Desktop::focusState()->window();
    if (focused && focused->m_workspace == workspace && focused->m_isMapped && !focused->m_isFloating)
        return focused;

    if (const auto w = workspace->getLastFocusedWindow(); w && w->m_isMapped && !w->m_isFloating)
        return w;

    const auto columns = columnsOf(workspace);
    return columns.empty() ? nullptr : columns.front().windows.front();
}

double HSView::anchorOffset(PHLWORKSPACE workspace) const {
    const auto monitor = this->monitor();
    if (!monitor)
        return 0.0;

    const auto anchor = anchorWindow(workspace);
    if (!anchor)
        return 0.0;

    const CBox mbox = monitor->logicalBox();
    const double center = hs_window_render_pos(anchor).x - hs_workspace_render_offset(workspace).x + anchor->sizeAnimation()->value().x / 2.0;

    return center - (mbox.x + mbox.w / 2.0);
}

void HSView::applySelection(PHLWINDOW window) {
    const auto monitor = this->monitor();
    if (!monitor)
        return;

    const auto ws = State::workspaceState()->query().id(m_selected).run();

    // Navigating the overview really navigates: the compositor follows along, so every keybind
    // the user already has -- close, move-to-workspace, swapcol -- acts on what they are looking
    // at rather than on whatever was focused before the overview opened.
    if (ws && monitor->m_activeWorkspace != ws) {
        const auto outgoing = monitor->m_activeWorkspace;

        // noMouseMove + noFocus: we place focus ourselves below, and a synthetic pointer move
        // would land on a scaled-down copy of a window.
        monitor->changeWorkspace(ws, false, true, true);

        for (const auto& w : {outgoing, ws}) {
            if (!w)
                continue;
            w->m_renderOffset->setValueAndWarp(Vector2D {0, 0});
            w->m_alpha->setValueAndWarp(1.F);
        }
    }

    if (window && window->m_isMapped)
        Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_KEYBIND);
}

PHLWINDOW HSView::selectedAnchor() const {
    return anchorWindow(State::workspaceState()->query().id(m_selected).run());
}

// The zoom at which every window on `workspace` is on screen while its anchor column stays
// pinned to the centre. Because the anchor is centred, the side that has to fit is the longer
// one -- assuming the content is centred instead is what left the far end of a tape off-screen.
double HSView::requiredZoom(PHLWORKSPACE workspace, const CBox& monitorBox) const {
    if (!workspace)
        return 1.0;

    const Vector2D offset = hs_workspace_render_offset(workspace);
    double left = monitorBox.x, right = monitorBox.x + monitorBox.w;

    for (const auto& w : workspaceWindows(workspace)) {
        const double x = hs_window_render_pos(w).x - offset.x;
        left = std::min(left, x);
        right = std::max(right, x + w->sizeAnimation()->value().x);
    }

    const double anchorX = monitorBox.x + monitorBox.w / 2.0 + anchorOffset(workspace);
    const double halfSpan = std::max({anchorX - left, right - anchorX, 1.0});

    return (monitorBox.w / 2.0) / halfSpan;
}

void HSView::syncRow() {
    const auto monitor = this->monitor();
    if (!monitor || m_closing)
        return;

    // The user's own binds keep working while the overview is open, so a workspace switch can
    // arrive from outside it -- SUPER+3, a focus dispatcher, an IPC call. Adopt it, or the view
    // stays parked on the row it opened at while focus wanders off the bottom of the screen.
    if (const auto active = monitor->m_activeWorkspace; active && active->m_id != m_selected) {
        m_selected = active->m_id;
        m_pan->setValueAndWarp(0.F);
        // Rows carry their own anchor, so arriving at a new row should not also slide sideways:
        // the vertical travel is the animation, and a diagonal drift on top of it reads as a bug.
        m_anchorX->setValueAndWarp((float)anchorOffset(active));
    }

    // Re-target every frame rather than only on navigation: a workspace appearing or being
    // destroyed shifts every row index below it, which would otherwise leave the selected row
    // sitting at someone else's position.
    const float rowTarget = (float)rowIndexOf(m_selected, frame().cards);
    if (std::abs(m_row->goal() - rowTarget) > 0.001F)
        *m_row = rowTarget;

    // Likewise for the horizontal anchor: retargeting every frame means a window closing or the
    // layout scrolling under us is absorbed by the same easing, instead of snapping.
    const float anchorTarget = (float)anchorOffset(State::workspaceState()->query().id(m_selected).run());
    if (std::abs(m_anchorX->goal() - anchorTarget) > 0.5F)
        *m_anchorX = anchorTarget;

    syncCenter(false);
}

void HSView::syncCenter(bool warp) {
    const auto ws = State::workspaceState()->query().id(m_selected).run();
    const auto anchor = anchorWindow(ws);

    m_centerValid = anchor != nullptr;
    if (!anchor)
        return;

    const Vector2D size = anchor->sizeAnimation()->value();
    const float centerY = (float)(hs_window_render_pos(anchor).y - hs_workspace_render_offset(ws).y + size.y / 2.0);

    if (warp) {
        m_centerY->setValueAndWarp(centerY);
        m_centerSize->setValueAndWarp(size);
        return;
    }

    // Only the height and the vertical placement can actually change here -- horizontally the
    // rectangle is pinned to the middle of the output -- so this is what makes a change of
    // anchor morph the rectangle in place instead of popping.
    if (std::abs(m_centerY->goal() - centerY) > 0.5F)
        *m_centerY = centerY;
    if (m_centerSize->goal().distance(size) > 0.5)
        *m_centerSize = size;
}

void HSView::updateFit(bool warp) {
    const auto monitor = this->monitor();
    if (!monitor)
        return;

    const CBox mbox = monitor->logicalBox();
    const float configured = std::clamp(HSConfig::value<Config::FLOAT>("zoom"), 0.05F, 0.95F);
    const float minZoom = std::clamp(HSConfig::value<Config::FLOAT>("min_zoom"), 0.02F, 0.95F);
    const auto mode = HSConfig::value<Config::INTEGER>("auto_fit");

    float target = configured;

    if (mode == 1) {
        // One zoom for the whole session, tight enough for the most demanding workspace. Only
        // recomputed when the overview opens: a zoom that drifted while you navigated -- or
        // every time a window closed -- made the overview feel like it was breathing at you.
        if (!warp)
            return;

        WORKSPACEID maxId = 0;
        for (const auto& ws : visibleWorkspaces(maxId))
            target = std::min(target, (float)requiredZoom(ws, mbox));

        target = std::clamp(target, minZoom, configured);
    } else if (mode == 2) {
        // Re-fit for whichever workspace is selected. Varies as you move between rows.
        target = std::clamp((float)requiredZoom(State::workspaceState()->query().id(m_selected).run(), mbox), minZoom, configured);
    }

    if (HSConfig::value<Config::INTEGER>("fit_rows")) {
        WORKSPACEID maxId = 0;
        const float gapFactor = std::max(HSConfig::value<Config::FLOAT>("workspace_gap"), 0.F);
        const float rows = (float)std::max<size_t>(visibleWorkspaces(maxId).size(), 1);
        if (rows > 1.F)
            target = std::min(target, 1.F / (rows + (rows - 1.F) * gapFactor));
    }

    if (warp)
        m_fitZoom->setValueAndWarp(target);
    else
        *m_fitZoom = target;
}

HSFrame HSView::frame() const {
    HSFrame f;

    const auto monitor = this->monitor();
    if (!monitor)
        return f;

    f.progress = std::clamp(m_progress->value(), 0.F, 1.F);
    f.monitorBox = monitor->logicalBox();

    WORKSPACEID maxId = 0;
    // Rows, in workspace-id order.
    for (const auto& ws : visibleWorkspaces(maxId))
        f.cards.push_back({.id = ws->m_id, .workspace = ws, .index = (int)f.cards.size(), .synthetic = false});

    // niri always keeps one empty workspace at the bottom, which is what makes "drag a window
    // down to make a new workspace" discoverable. Mirror that with a synthetic card.
    if (HSConfig::value<Config::INTEGER>("trailing_workspace") || (m_selected > maxId && m_selected != WORKSPACE_INVALID)) {
        const WORKSPACEID newId = std::max(maxId + 1, (WORKSPACEID)1);
        if (!State::workspaceState()->query().id(newId).run())
            f.cards.push_back({.id = newId, .workspace = nullptr, .index = (int)f.cards.size(), .synthetic = true});
    }

    if (f.cards.empty())
        return f;

    const float gapFactor = std::max(HSConfig::value<Config::FLOAT>("workspace_gap"), 0.F);

    // niri: zoom = 1 - progress * (1 - configured), so progress 0 is a pixel-exact desktop and
    // opening the overview is literally a zoom-out.
    f.zoom = std::max(1.F - f.progress * (1.F - m_fitZoom->value()), 0.01F);

    const float cardW = f.monitorBox.w * f.zoom;
    const float cardH = f.monitorBox.h * f.zoom;
    const float pitch = cardH + f.monitorBox.h * gapFactor * f.zoom;

    const float baseX = f.monitorBox.x + (f.monitorBox.w - cardW) / 2.F;
    const float baseY = f.monitorBox.y + (f.monitorBox.h - cardH) / 2.F;
    const float row = m_row->value();

    for (auto& c : f.cards) {
        const float rowY = baseY + ((float)c.index - row) * pitch;

        c.box = CBox {f.monitorBox.x, rowY, f.monitorBox.w, cardH};
        c.contentOrigin = {baseX, rowY};

        // Every row is anchored on its own column, so the centre of the screen is a fixed
        // reference that the tapes slide through -- it is the tape that moves, never the centre.
        // The selected row's offset is animated; the others read straight off their anchor.
        // Fading the offset in with the progress keeps progress 0 an exact identity transform:
        // there viewOrigin is the monitor origin, zoom is 1 and contentOrigin is baseX == mbox.x.
        // The selected row eases toward its anchor; the others sit exactly on theirs, since
        // nothing moves them.
        double offset = c.id == m_selected ? (double)m_anchorX->value() + m_pan->value() : anchorOffset(c.workspace);

        c.viewOrigin = {f.monitorBox.x + offset * f.progress, f.monitorBox.y};
    }

    return f;
}

CBox HSView::windowBox(PHLWINDOW window, const HSCard& card, const HSFrame& f) const {
    if (!window)
        return {};

    const Vector2D srcOrigin = card.viewOrigin + hs_workspace_render_offset(card.workspace);
    return CBox {(hs_window_render_pos(window) - srcOrigin) * f.zoom + card.contentOrigin, window->sizeAnimation()->value() * f.zoom};
}

CBox HSView::centerBox(const HSFrame& f) const {
    if (!m_centerValid || f.cards.empty())
        return {};

    const auto ws = State::workspaceState()->query().id(m_selected).run();
    const Vector2D size = m_centerSize->value() * f.zoom;
    if (size.x <= 0.0 || size.y <= 0.0)
        return {};

    const double baseX = f.monitorBox.x + (f.monitorBox.w - f.monitorBox.w * f.zoom) / 2.0;
    const double baseY = f.monitorBox.y + (f.monitorBox.h - f.monitorBox.h * f.zoom) / 2.0;

    // The same mapping a card draws under, evaluated with the row and the anchor already at
    // rest. Feeding it the live anchorOffset rather than the animated m_anchorX is what nails
    // the rectangle down: the offset term is multiplied by (1 - progress), so at progress 1 it
    // vanishes and the centre is exactly the middle of the output no matter where the tape has
    // got to, while at progress 0 it still lands on the window's real on-screen box.
    const double cx = (f.monitorBox.w / 2.0 + anchorOffset(ws) * (1.0 - f.progress)) * f.zoom + baseX;
    const double cy = (m_centerY->value() - f.monitorBox.y) * f.zoom + baseY;

    return CBox {cx - size.x / 2.0, cy - size.y / 2.0, size.x, size.y};
}

std::optional<HSCard> HSView::cardAt(const Vector2D& global) const {
    const auto f = frame();

    // The band spans the whole output width, so clicking the empty space either side of a row
    // still selects that workspace -- same affordance niri gives.
    for (const auto& c : f.cards) {
        if (c.box.containsPoint(global))
            return c;
    }

    return std::nullopt;
}

PHLWINDOW HSView::windowAt(const Vector2D& global) const {
    const auto f = frame();

    // Front-to-back: later cards and later windows are drawn on top.
    for (auto it = f.cards.rbegin(); it != f.cards.rend(); ++it) {
        if (!it->workspace)
            continue;

        const auto windows = workspaceWindows(it->workspace);
        for (auto wit = windows.rbegin(); wit != windows.rend(); ++wit) {
            if (*wit == m_dragged.lock())
                continue;
            if (windowBox(*wit, *it, f).containsPoint(global))
                return *wit;
        }
    }

    return nullptr;
}

void HSView::syncSelectionToMonitor() {
    const auto monitor = this->monitor();
    if (!monitor || !monitor->m_activeWorkspace)
        return;
    m_selected = monitor->m_activeWorkspace->m_id;
}

void HSView::show() {
    const auto monitor = this->monitor();
    if (!monitor || !monitor->m_activeWorkspace)
        return;

    // Re-sync whenever we are not already showing, and also if the workspace we had selected
    // has since been destroyed under us.
    const bool stale = !m_active || m_closing || (m_selected != WORKSPACE_INVALID && !State::workspaceState()->query().id(m_selected).run());

    m_active = true;
    m_closing = false;

    if (stale) {
        syncSelectionToMonitor();
        m_pan->setValueAndWarp(0.F);
        m_anchorX->setValueAndWarp((float)anchorOffset(State::workspaceState()->query().id(m_selected).run()));
        updateFit(true);
        m_row->setValueAndWarp((float)rowIndexOf(m_selected, frame().cards));
        syncCenter(true);
    }

    // The pointer has not moved yet, so nothing is hovered -- whatever it was resting on when
    // the overview opened is not a choice the user made.
    disarmHover();

    *m_progress = 1.F;
    m_progress->setCallbackOnEnd(nullptr);

    Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);

    g_pHyprRenderer->damageMonitor(monitor);
    monitor->scheduleFrame();
}

void HSView::hide(PHLWINDOW focusWindow) {
    const auto monitor = this->monitor();
    if (!monitor || !m_active)
        return;

    PHLWORKSPACE target = State::workspaceState()->query().id(m_selected).run();
    if (!target && m_selected != WORKSPACE_INVALID)
        target = State::workspaceState()->create(m_selected, monitor->m_id);

    // Closing commits the column you scrolled to, not just the workspace: whatever ended up in
    // the centre is what you were pointing at. Navigation already moved the compositor along, so
    // this is usually a no-op -- it matters when you click a window directly.
    if (!focusWindow)
        focusWindow = anchorWindow(target);

    applySelection(focusWindow);

    if (HSConfig::value<Config::INTEGER>("warp_cursor") && focusWindow)
        focusWindow->warpCursor(false);

    m_closing = true;
    disarmHover();
    m_pan->setValueAndWarp(0.F);

    m_progress->setCallbackOnEnd([this](auto) {
        m_active = false;
        m_closing = false;
    });
    *m_progress = 0.F;

    // operator= is a no-op when the goal already matches, and a no-op never fires the end
    // callback -- which would leave the view "active" at progress 0 and the render hook engaged
    // forever. Settle it by hand in that case.
    if (!m_progress->isBeingAnimated() && m_progress->value() <= 0.001F) {
        m_active = false;
        m_closing = false;
    }

    Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);

    g_pHyprRenderer->damageMonitor(monitor);
    monitor->scheduleFrame();
}

void HSView::toggle() {
    if (m_active && !m_closing)
        hide(nullptr);
    else
        show();
}

void HSView::onConfigReloaded() {
    if (m_active && HSConfig::value<Config::INTEGER>("close_on_reload"))
        hide(nullptr);
}

void HSView::disarmHover() {
    m_hoverArmed = false;
    m_hovered.reset();
}

void HSView::selectWorkspace(WORKSPACEID id) {
    if (id == WORKSPACE_INVALID)
        return;

    disarmHover();

    m_selected = id;
    m_pan->setValueAndWarp(0.F);

    const auto ws = State::workspaceState()->query().id(id).run();
    applySelection(anchorWindow(ws));
    m_anchorX->setValueAndWarp((float)anchorOffset(ws));

    updateFit(false);
    *m_row = (float)rowIndexOf(id, frame().cards);

    if (const auto monitor = this->monitor()) {
        g_pHyprRenderer->damageMonitor(monitor);
        monitor->scheduleFrame();
    }
}

void HSView::selectRow(int delta) {
    const auto f = frame();
    if (f.cards.empty())
        return;

    const int idx = std::clamp(rowIndexOf(m_selected, f.cards) + delta, 0, (int)f.cards.size() - 1);
    selectWorkspace(f.cards[idx].id);
}

void HSView::selectColumn(int delta) {
    // Purely an overview-side move: it walks this workspace's own tape and re-anchors, so it
    // works on any row, not just whichever workspace the compositor happens to think is active.
    const auto ws = State::workspaceState()->query().id(m_selected).run();
    if (!ws)
        return;

    const auto columns = columnsOf(ws);
    if (columns.empty())
        return;

    disarmHover();

    const auto anchor = anchorWindow(ws);

    int idx = 0;
    for (size_t i = 0; i < columns.size(); ++i) {
        if (std::ranges::find(columns[i].windows, anchor) != columns[i].windows.end()) {
            idx = (int)i;
            break;
        }
    }

    idx = std::clamp(idx + delta, 0, (int)columns.size() - 1);

    // Within a stacked column, keep whichever window was last focused there.
    const auto& target = columns[idx].windows;
    const auto lastFocused = ws->getLastFocusedWindow();
    const auto pick = std::ranges::find(target, lastFocused) != target.end() ? lastFocused : target.front();

    m_pan->setValueAndWarp(0.F);
    applySelection(pick);
    updateFit(false);

    if (const auto monitor = this->monitor()) {
        g_pHyprRenderer->damageMonitor(monitor);
        monitor->scheduleFrame();
    }
}

void HSView::panBy(double dx) {
    *m_pan = (float)(m_pan->goal() + dx);

    if (const auto monitor = this->monitor()) {
        g_pHyprRenderer->damageMonitor(monitor);
        monitor->scheduleFrame();
    }
}

bool HSView::beginDrag(const Vector2D& cursor) {
    const auto window = windowAt(cursor);
    if (!window || !window->m_workspace || window->m_pinned)
        return false;

    const auto f = frame();
    const auto* card = f.card(window->m_workspace->m_id);
    if (!card)
        return false;

    m_dragged = window;
    m_dragGrab = windowBox(window, *card, f).pos() - cursor;
    return true;
}

void HSView::endDrag(const Vector2D& cursor) {
    const auto window = m_dragged.lock();
    m_dragged.reset();

    const auto monitor = this->monitor();
    if (!window || !monitor)
        return;

    const auto target = cardAt(cursor);
    if (!target)
        return;

    PHLWORKSPACE workspace = target->workspace;
    if (!workspace)
        workspace = State::workspaceState()->create(target->id, monitor->m_id);
    if (!workspace)
        return;

    if (window->m_workspace != workspace)
        Desktop::globalWindowController()->moveWindowToWorkspace(window, workspace);

    // Dropping is also a choice of workspace and column: follow the window.
    m_selected = workspace->m_id;
    applySelection(window);
    updateFit(false);
    *m_row = (float)rowIndexOf(m_selected, frame().cards);
}

void HSView::cancelDrag() {
    m_dragged.reset();
}

void HSView::render() {
    const auto monitor = this->monitor();
    if (!monitor)
        return;

    CScopeGuard guard([this] { postRender(); });

    // Curve, speed and spring constants are ordinary config keys, so read them like every other
    // one: per frame. A `hyprctl eval` never fires a config-reload, and waiting for one would
    // make these the only keys in the plugin that need a restart.
    hs_refresh_animation_config();

    // Windows move while the overview is open (drags, new clients), so re-derive the fit each
    // frame; the animated variable absorbs it smoothly.
    updateFit(false);
    syncRow();

    const auto time = Time::steadyNow();
    const auto f = frame();

    // Simplification decides what is occluded from untransformed boxes, which say nothing
    // useful once every element is being moved by a modifier.
    g_pHyprRenderer->m_renderData.noSimplify = true;

    // Opaque base, so nothing stale shows through when the layers below are disabled.
    CClearPassElement::SClearData clear;
    clear.color = colorOf("backdrop_color").stripA();
    g_pHyprRenderer->m_renderPass.add(makeUnique<CClearPassElement>(clear));

    // Wallpaper and other background/bottom layer surfaces are drawn UNSCALED, exactly where
    // they normally sit. Zooming them along with the cards made the desktop look like it was
    // shrinking out from under the windows, and drawing one copy per card was worse.
    if (HSConfig::value<Config::INTEGER>("render_background_layers"))
        renderLayers(time, false);

    // Dim the wallpaper as the overview opens so the cards read against it. At progress 0 this
    // is fully transparent, which is what keeps opening a pixel-exact zoom.
    const auto dim = colorOf("backdrop_color");
    if (dim.a > 0.001 && f.progress > 0.001F) {
        CRectPassElement::SRectData rect;
        rect.color = fade(dim, f.progress);
        rect.box = CBox {{0, 0}, monitor->m_transformedSize};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(rect));
    }

    // Cull by row only: windows deliberately overflow their card horizontally, so a card whose
    // own box is off to the side may still have visible content.
    const float cullMargin = f.monitorBox.h;

    for (const auto& c : f.cards) {
        if (c.box.y + c.box.h < f.monitorBox.y - cullMargin || c.box.y > f.monitorBox.y + f.monitorBox.h + cullMargin)
            continue;
        renderCard(c, f, time);
    }

    // The centre rectangle: the window the selected row is anchored on, drawn where it comes to
    // rest rather than where it currently is. That is the whole point of it -- it is the fixed
    // reference the tape slides through, so it must not travel in alongside the window it rings.
    const float activeBorder = HSConfig::value<Config::FLOAT>("active_border_size");
    if (activeBorder > 0.F && f.progress > 0.01F) {
        const auto box = centerBox(f);
        if (box.w > 0 && box.h > 0) {
            CBorderPassElement::SBorderData border;
            border.box = snapToPixels(toBuffer(monitor, box));
            border.grad1 = Config::CGradientValueData {fade(colorOf("active_border_color"), f.progress)};
            border.borderSize = std::round(activeBorder);
            border.round = roundingFor(selectedAnchor(), "active_border_rounding", monitor, f.zoom);
            g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
        }
    }

    // Hover ring around the window a click would pick. This one does follow the live box: it is
    // about what is under the pointer, which is a fact about right now.
    const float hoverBorder = HSConfig::value<Config::FLOAT>("hover_border_size");
    if (hoverBorder > 0.F && f.progress > 0.01F && m_hoverArmed) {
        if (const auto hovered = m_hovered.lock()) {
            if (const auto* c = hovered->m_workspace ? f.card(hovered->m_workspace->m_id) : nullptr) {
                CBorderPassElement::SBorderData border;
                border.box = snapToPixels(toBuffer(monitor, windowBox(hovered, *c, f)));
                border.grad1 = Config::CGradientValueData {fade(colorOf("hover_border_color"), f.progress)};
                border.borderSize = std::round(hoverBorder);
                border.round = roundingFor(hovered, "hover_border_rounding", monitor, f.zoom);
                g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
            }
        }
    }

    // The window being dragged rides the cursor, above every card.
    if (const auto dragged = m_dragged.lock()) {
        const Vector2D cursor = g_pInputManager->getMouseCoordsInternal();
        const CBox box {cursor + m_dragGrab, dragged->sizeAnimation()->value() * f.zoom};
        hs_render_window_at_box(dragged, monitor, time, box, true);
    }

    // Bars and other top/overlay surfaces stay put, unscaled, over everything.
    if (HSConfig::value<Config::INTEGER>("render_top_layers"))
        renderLayers(time, true);

    // Nothing else damages the monitor while the overview is up, and renderWorkspace is skipped
    // entirely on an undamaged monitor -- so the animation would stall without this.
    g_pHyprRenderer->damageMonitor(monitor);
    monitor->scheduleFrame();
}

void HSView::renderCard(const HSCard& card, const HSFrame& f, const Time::steady_tp& time) {
    const auto monitor = this->monitor();
    if (!monitor)
        return;

    for (const auto& key : {"card_color", "active_row_color"}) {
        // The second band marks the selected row. Without it the only cue for "which workspace
        // am I on" is the focus ring on one window, which is easy to lose among six rows.
        if (key == std::string_view {"active_row_color"} && card.id != m_selected)
            continue;

        const auto color = colorOf(key);
        if (color.a <= 0.001)
            continue;

        CRectPassElement::SRectData rect;
        rect.color = fade(color, f.progress);
        rect.box = toBuffer(monitor, card.box);
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(rect));
    }

    if (!card.workspace) {
        // The trailing row is the "drop a window here for a new workspace" target. Outlining it
        // is off by default: rows span the whole output, so its top and bottom edges drew as two
        // lines running the full width of the screen rather than as anything card-shaped.
        const float size = HSConfig::value<Config::FLOAT>("active_border_size");
        if (HSConfig::value<Config::INTEGER>("new_workspace_hint") && size > 0.F && f.progress > 0.01F) {
            // Sized to what a workspace viewport looks like at this zoom, and centred, so it
            // reads as an empty card instead of a band.
            const float w = f.monitorBox.w * f.zoom;
            CBorderPassElement::SBorderData hint;
            hint.box = snapToPixels(toBuffer(monitor, CBox {f.monitorBox.x + (f.monitorBox.w - w) / 2.F, card.box.y, w, card.box.h}));
            hint.grad1 = Config::CGradientValueData {fade(colorOf("active_border_color"), f.progress * 0.35F)};
            hint.borderSize = std::round(size);
            hint.round = roundingFor(nullptr, "active_border_rounding", monitor, f.zoom);
            g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(hint));
        }
        return;
    }

    HSWorkspaceAlphaGuard alphaGuard(card.workspace);

    const Vector2D srcOrigin = card.viewOrigin + hs_workspace_render_offset(card.workspace);

    hs_push_modif(monitor, srcOrigin, card.contentOrigin, f.zoom);
    CScopeGuard popModif([] { hs_pop_modif(); });

    // Mirrors renderWorkspaceWindows' ordering: tiled first (focused last), then their popups,
    // then floating on top. Pinned windows are handled separately -- they belong to no single
    // workspace, and renderWindow skips the workspace offset for them.
    const auto windows = workspaceWindows(card.workspace);
    const auto focusedWindow = Desktop::focusState()->window();
    const auto dragged = m_dragged.lock();
    PHLWINDOW lastWindow;

    if (HSConfig::value<Config::INTEGER>("debug")) {
        for (const auto& w : windows) {
            const auto b = windowBox(w, card, f);
            hs_log("ws{} \"{}\" real={:.0f} box=({:.0f},{:.0f} {:.0f}x{:.0f}) tex={}", card.id, w->m_title, hs_window_render_pos(w).x, b.x, b.y, b.w, b.h,
                   w->wlSurface() && w->wlSurface()->resource() && w->wlSurface()->resource()->m_current.texture ? "yes" : "NO");
        }
    }

    for (const auto& w : windows) {
        if (w->m_isFloating || w == dragged)
            continue;
        if (w == focusedWindow) {
            lastWindow = w;
            continue;
        }
        hs_render_window(w, monitor, time, RENDER_PASS_MAIN);
    }

    if (lastWindow)
        hs_render_window(lastWindow, monitor, time, RENDER_PASS_MAIN);

    for (const auto& w : windows) {
        if (w->m_isFloating || w == dragged)
            continue;
        hs_render_window(w, monitor, time, RENDER_PASS_POPUP);
    }

    for (const auto& w : windows) {
        if (!w->m_isFloating || w == dragged)
            continue;
        hs_render_window(w, monitor, time, RENDER_PASS_ALL);
    }
}

void HSView::renderLayers(const Time::steady_tp& time, bool top) {
    const auto monitor = this->monitor();
    if (!monitor)
        return;

    // No render modifier is pushed here on purpose: layer surfaces belong to the screen, not to
    // any one workspace, so they must not move or scale with the cards.
    const std::array<int, 2> levels = top ? std::array<int, 2> {2, 3} : std::array<int, 2> {0, 1};

    // popups=false draws the surface itself; the popup pass is separate and comes after
    // everything, exactly as renderWorkspace orders it.
    for (const auto& level : levels) {
        for (const auto& ref : monitor->m_layerSurfaceLayers[level]) {
            const auto ls = ref.lock();
            if (ls)
                hs_render_layer(ls, monitor, time, false);
        }
    }

    if (!top)
        return;

    for (const auto& lsl : monitor->m_layerSurfaceLayers) {
        for (const auto& ref : lsl) {
            const auto ls = ref.lock();
            if (ls)
                hs_render_layer(ls, monitor, time, true);
        }
    }

    // Pinned windows float above everything, on every workspace -- drawing them unscaled keeps
    // that promise and sidesteps renderWindow's pinned-window offset special case.
    for (const auto& w : Desktop::windowState()->windows()) {
        if (!w || !w->m_pinned || !w->m_isMapped || w->isHidden())
            continue;
        if (w->m_monitor != monitor)
            continue;
        hs_render_window(w, monitor, time, RENDER_PASS_ALL);
    }
}

void HSView::postRender() {
    // Keep the render pass from deciding an opaque element hides the ones behind it: under a
    // renderModif that reasoning no longer holds.
    g_pHyprRenderer->m_renderPass.add(makeUnique<HSPassElement>());

    // The pass hands each element `frameDamage ∩ its own UNTRANSFORMED box`. For a column
    // scrolled off the viewport that intersection is empty, so the surface was dropped before
    // the render modifier ever got a chance to move it on screen -- which is why only the two
    // or three columns nearest the viewport used to draw. Widening the damage past the monitor
    // keeps every intersection non-empty; the modifier then puts each window where it belongs.
    // The pass overwrites m_renderData.damage with its own result afterwards, so this does not
    // leak into the next frame.
    g_pHyprRenderer->m_renderData.damage = CRegion {CBox {-HS_DAMAGE_SLACK, -HS_DAMAGE_SLACK, HS_DAMAGE_SLACK * 2, HS_DAMAGE_SLACK * 2}};
}
