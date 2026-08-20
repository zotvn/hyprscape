#include "view.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/managers/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

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

CHyprColor colorOf(const std::string& key) {
    return CHyprColor {(uint64_t)HSConfig::value<Config::INTEGER>(key)};
}

CHyprColor fade(CHyprColor c, float f) {
    return c.modifyA(c.a * std::clamp(f, 0.F, 1.F));
}

// Windows within one scroll-tape column share an x exactly; a couple of pixels of slack keeps
// rounding from splitting a column in two.
constexpr double COLUMN_EPS = 2.0;

} // namespace

const HSCard* HSFrame::card(WORKSPACEID id) const {
    for (const auto& c : cards)
        if (c.id == id)
            return &c;
    return nullptr;
}

HSView::HSView(MONITORID monitorId) : m_monitorId(monitorId) {
    // Reuse the user's `workspaces` animation curve, so the overview feels like the rest of
    // their desktop without a separate config node to discover.
    auto& tree = Config::animationTree();
    const auto cfg = tree->getAnimationPropertyConfig("workspaces");

    g_pAnimationManager->createAnimation(0.F, m_progress, cfg, AVARDAMAGE_NONE);
    g_pAnimationManager->createAnimation(0.F, m_row, cfg, AVARDAMAGE_NONE);
    g_pAnimationManager->createAnimation(0.F, m_pan, cfg, AVARDAMAGE_NONE);
    g_pAnimationManager->createAnimation(1.F, m_fitZoom, cfg, AVARDAMAGE_NONE);
    g_pAnimationManager->createAnimation(0.F, m_anchorX, cfg, AVARDAMAGE_NONE);
}

PHLMONITOR HSView::monitor() const {
    return g_pCompositor->getMonitorFromID(m_monitorId);
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

    for (const auto& ref : g_pCompositor->getWorkspaces()) {
        const auto ws = ref.lock();
        if (!ws || ws->inert() || ws->m_isSpecialWorkspace)
            continue;
        if (ws->m_monitor != monitor)
            continue;

        maxId = std::max(maxId, ws->m_id);

        // This is the whole fix for hyprtasking's phantom tiles: a workspace earns a row by
        // having something on it, being where you are, or being where you are headed.
        const bool occupied = ws->getWindows() > 0;
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

    for (const auto& w : g_pCompositor->m_windows) {
        if (!w || w->m_workspace != workspace)
            continue;
        if (w->isHidden() || (!w->m_isMapped && !w->m_fadingOut) || w->m_pinned)
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
        const double center = hs_window_render_pos(w).x - offset.x + w->m_realSize->value().x / 2.0;

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

    if (const auto it = m_anchors.find(workspace->m_id); it != m_anchors.end()) {
        const auto w = it->second.lock();
        if (w && w->m_workspace == workspace && w->m_isMapped && !w->m_isFloating)
            return w;
    }

    if (const auto w = workspace->getLastFocusedWindow(); w && !w->m_isFloating)
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
    const double center = hs_window_render_pos(anchor).x - hs_workspace_render_offset(workspace).x + anchor->m_realSize->value().x / 2.0;

    return center - (mbox.x + mbox.w / 2.0);
}

void HSView::captureAnchors() {
    m_anchors.clear();

    WORKSPACEID maxId = 0;
    for (const auto& ws : visibleWorkspaces(maxId)) {
        if (const auto w = anchorWindow(ws))
            m_anchors[ws->m_id] = w;
    }
}

PHLWINDOW HSView::selectedAnchor() const {
    return anchorWindow(g_pCompositor->getWorkspaceByID(m_selected));
}

void HSView::updateFit(bool warp) {
    const auto monitor = this->monitor();
    if (!monitor)
        return;

    const CBox mbox = monitor->logicalBox();
    float target = std::clamp(HSConfig::value<Config::FLOAT>("zoom"), 0.05F, 0.95F);

    // Fit the workspace you are actually looking at, keeping its anchor column dead centre.
    // Fitting the union of every workspace would let one very long tape shrink all the others.
    if (HSConfig::value<Config::INTEGER>("auto_fit")) {
        const auto ws = g_pCompositor->getWorkspaceByID(m_selected);

        double left = mbox.x, right = mbox.x + mbox.w;
        if (ws) {
            const Vector2D offset = hs_workspace_render_offset(ws);
            for (const auto& w : workspaceWindows(ws)) {
                const double x = hs_window_render_pos(w).x - offset.x;
                left = std::min(left, x);
                right = std::max(right, x + w->m_realSize->value().x);
            }
        }

        // The anchor stays centred, so what has to fit is the larger of the two sides.
        const double anchorTapeX = mbox.x + mbox.w / 2.0 + anchorOffset(ws);
        const double halfSpan = std::max({anchorTapeX - left, right - anchorTapeX, 1.0});

        const float minZoom = std::clamp(HSConfig::value<Config::FLOAT>("min_zoom"), 0.02F, 0.95F);
        target = std::clamp((float)((mbox.w / 2.0) / halfSpan), minZoom, target);
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
        if (!g_pCompositor->getWorkspaceByID(newId))
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
        double offset = c.id == m_selected ? (double)m_anchorX->value() + m_pan->value() : anchorOffset(c.workspace);

        c.viewOrigin = {f.monitorBox.x + offset * f.progress, f.monitorBox.y};
    }

    return f;
}

CBox HSView::windowBox(PHLWINDOW window, const HSCard& card, const HSFrame& f) const {
    if (!window)
        return {};

    const Vector2D srcOrigin = card.viewOrigin + hs_workspace_render_offset(card.workspace);
    return CBox {(hs_window_render_pos(window) - srcOrigin) * f.zoom + card.contentOrigin, window->m_realSize->value() * f.zoom};
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
    const bool stale = !m_active || m_closing || (m_selected != WORKSPACE_INVALID && !g_pCompositor->getWorkspaceByID(m_selected));

    m_active = true;
    m_closing = false;

    if (stale) {
        syncSelectionToMonitor();
        captureAnchors();
        m_pan->setValueAndWarp(0.F);
        m_anchorX->setValueAndWarp((float)anchorOffset(g_pCompositor->getWorkspaceByID(m_selected)));
        updateFit(true);
        m_row->setValueAndWarp((float)rowIndexOf(m_selected, frame().cards));
    }

    *m_progress = 1.F;
    m_progress->setCallbackOnEnd(nullptr);

    Cursor::overrideController->setOverride("left_ptr", Cursor::CURSOR_OVERRIDE_UNKNOWN);

    g_pHyprRenderer->damageMonitor(monitor);
    g_pCompositor->scheduleFrameForMonitor(monitor);
}

void HSView::hide(PHLWINDOW focusWindow) {
    const auto monitor = this->monitor();
    if (!monitor || !m_active)
        return;

    PHLWORKSPACE target = g_pCompositor->getWorkspaceByID(m_selected);
    if (!target && m_selected != WORKSPACE_INVALID)
        target = g_pCompositor->createNewWorkspace(m_selected, monitor->m_id);

    // Closing commits the column you scrolled to, not just the workspace: whatever ended up in
    // the centre is what you were pointing at.
    if (!focusWindow)
        focusWindow = anchorWindow(target);

    if (target && monitor->m_activeWorkspace != target) {
        const auto outgoing = monitor->m_activeWorkspace;

        monitor->changeWorkspace(target, false);

        // changeWorkspace kicks off the usual slide/fade. Ours is a zoom, and two animations
        // fighting over the same pixels looks broken -- squash theirs to its end state.
        for (const auto& ws : {outgoing, target}) {
            if (!ws)
                continue;
            ws->m_renderOffset->setValueAndWarp(Vector2D {0, 0});
            ws->m_alpha->setValueAndWarp(1.F);
        }
    }

    if (focusWindow && focusWindow->m_isMapped)
        Desktop::focusState()->fullWindowFocus(focusWindow, Desktop::FOCUS_REASON_CLICK);

    if (HSConfig::value<Config::INTEGER>("warp_cursor") && focusWindow)
        focusWindow->warpCursor(false);

    m_closing = true;
    m_hovered.reset();
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

    Cursor::overrideController->unsetOverride(Cursor::CURSOR_OVERRIDE_UNKNOWN);

    g_pHyprRenderer->damageMonitor(monitor);
    g_pCompositor->scheduleFrameForMonitor(monitor);
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

void HSView::selectWorkspace(WORKSPACEID id) {
    if (id == WORKSPACE_INVALID)
        return;

    m_selected = id;
    m_pan->setValueAndWarp(0.F);

    // Rows carry their own anchor, so switching rows warps rather than sliding sideways: the
    // vertical move is the animation, and a diagonal drift on top of it reads as a glitch.
    m_anchorX->setValueAndWarp((float)anchorOffset(g_pCompositor->getWorkspaceByID(id)));

    updateFit(false);
    *m_row = (float)rowIndexOf(id, frame().cards);

    if (const auto monitor = this->monitor()) {
        g_pHyprRenderer->damageMonitor(monitor);
        g_pCompositor->scheduleFrameForMonitor(monitor);
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
    const auto ws = g_pCompositor->getWorkspaceByID(m_selected);
    if (!ws)
        return;

    const auto columns = columnsOf(ws);
    if (columns.empty())
        return;

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
    m_anchors[m_selected] = std::ranges::find(target, lastFocused) != target.end() ? lastFocused : target.front();

    m_pan->setValueAndWarp(0.F);
    *m_anchorX = (float)anchorOffset(ws);
    updateFit(false);

    if (const auto monitor = this->monitor()) {
        g_pHyprRenderer->damageMonitor(monitor);
        g_pCompositor->scheduleFrameForMonitor(monitor);
    }
}

void HSView::panBy(double dx) {
    *m_pan = (float)(m_pan->goal() + dx);

    if (const auto monitor = this->monitor()) {
        g_pHyprRenderer->damageMonitor(monitor);
        g_pCompositor->scheduleFrameForMonitor(monitor);
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
        workspace = g_pCompositor->createNewWorkspace(target->id, monitor->m_id);
    if (!workspace)
        return;

    if (window->m_workspace != workspace)
        g_pCompositor->moveWindowToWorkspaceSafe(window, workspace);

    // Dropping is also a choice of workspace and column: follow the window.
    m_anchors[workspace->m_id] = window;
    selectWorkspace(workspace->m_id);
}

void HSView::cancelDrag() {
    m_dragged.reset();
}

void HSView::render() {
    const auto monitor = this->monitor();
    if (!monitor)
        return;

    CScopeGuard guard([this] { postRender(); });

    // Windows move while the overview is open (drags, new clients), so re-derive the fit each
    // frame; the animated variable absorbs it smoothly.
    updateFit(false);

    const auto time = Time::steadyNow();
    const auto f = frame();

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

    // Ring the window sitting in the centre of the selected row -- the one closing will land
    // on. The centre itself is deliberately not drawn: it is a reference, not furniture.
    const float activeBorder = HSConfig::value<Config::FLOAT>("active_border_size");
    if (activeBorder > 0.F && f.progress > 0.01F) {
        const auto* selected = f.card(m_selected);
        const auto anchor = selected ? anchorWindow(selected->workspace) : nullptr;
        if (selected && anchor) {
            CBorderPassElement::SBorderData border;
            border.box = toBuffer(monitor, windowBox(anchor, *selected, f));
            border.grad1 = Config::CGradientValueData {fade(colorOf("active_border_color"), f.progress)};
            border.borderSize = std::round(activeBorder);
            g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
        }
    }

    // Hover ring around the window a click would pick.
    const float hoverBorder = HSConfig::value<Config::FLOAT>("hover_border_size");
    if (hoverBorder > 0.F && f.progress > 0.01F) {
        if (const auto hovered = m_hovered.lock()) {
            if (const auto* c = hovered->m_workspace ? f.card(hovered->m_workspace->m_id) : nullptr) {
                CBorderPassElement::SBorderData border;
                border.box = toBuffer(monitor, windowBox(hovered, *c, f));
                border.grad1 = Config::CGradientValueData {fade(colorOf("hover_border_color"), f.progress)};
                border.borderSize = std::round(hoverBorder);
                g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
            }
        }
    }

    // The window being dragged rides the cursor, above every card.
    if (const auto dragged = m_dragged.lock()) {
        const Vector2D cursor = g_pInputManager->getMouseCoordsInternal();
        const CBox box {cursor + m_dragGrab, dragged->m_realSize->value() * f.zoom};
        hs_render_window_at_box(dragged, monitor, time, box, true);
    }

    // Bars and other top/overlay surfaces stay put, unscaled, over everything.
    if (HSConfig::value<Config::INTEGER>("render_top_layers"))
        renderLayers(time, true);

    // Nothing else damages the monitor while the overview is up, and renderWorkspace is skipped
    // entirely on an undamaged monitor -- so the animation would stall without this.
    g_pHyprRenderer->damageMonitor(monitor);
    g_pCompositor->scheduleFrameForMonitor(monitor);
}

void HSView::renderCard(const HSCard& card, const HSFrame& f, const Time::steady_tp& time) {
    const auto monitor = this->monitor();
    if (!monitor)
        return;

    const auto cardColor = colorOf("card_color");
    if (cardColor.a > 0.001) {
        CRectPassElement::SRectData rect;
        rect.color = fade(cardColor, f.progress);
        rect.box = toBuffer(monitor, card.box);
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(rect));
    }

    if (!card.workspace) {
        // The trailing card has nothing to draw, so give it an outline -- otherwise the "drop a
        // window here to get a new workspace" affordance is invisible.
        const float size = HSConfig::value<Config::FLOAT>("active_border_size");
        if (size > 0.F && f.progress > 0.01F) {
            CBorderPassElement::SBorderData hint;
            hint.box = toBuffer(monitor, card.box);
            hint.grad1 = Config::CGradientValueData {fade(colorOf("active_border_color"), f.progress * 0.35F)};
            hint.borderSize = std::round(size);
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
            if (ls && !ls->m_fadingOut)
                hs_render_layer(ls, monitor, time, false);
        }
    }

    if (!top)
        return;

    for (const auto& lsl : monitor->m_layerSurfaceLayers) {
        for (const auto& ref : lsl) {
            const auto ls = ref.lock();
            if (ls && !ls->m_fadingOut)
                hs_render_layer(ls, monitor, time, true);
        }
    }

    // Pinned windows float above everything, on every workspace -- drawing them unscaled keeps
    // that promise and sidesteps renderWindow's pinned-window offset special case.
    for (const auto& w : g_pCompositor->m_windows) {
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
}
