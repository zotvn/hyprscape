#include "view.hpp"

#include <algorithm>
#include <cmath>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/layout/space/Space.hpp>
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
    g_pAnimationManager->createAnimation(0.F, m_fitPan, cfg, AVARDAMAGE_NONE);
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

CBox HSView::contentBounds(PHLWORKSPACE workspace, const CBox& monitorBox) const {
    // Start from the viewport so a workspace whose windows all fit behaves exactly like niri.
    double left = monitorBox.x, right = monitorBox.x + monitorBox.w;

    if (workspace) {
        const Vector2D offset = hs_workspace_render_offset(workspace);
        for (const auto& w : workspaceWindows(workspace)) {
            const Vector2D pos = hs_window_render_pos(w) - offset;
            left = std::min(left, pos.x);
            right = std::max(right, pos.x + w->m_realSize->value().x);
        }
    }

    return CBox {left, monitorBox.y, right - left, monitorBox.h};
}

void HSView::updateFit(bool warp) {
    const auto monitor = this->monitor();
    if (!monitor)
        return;

    const CBox mbox = monitor->logicalBox();
    float target = std::clamp(HSConfig::value<Config::FLOAT>("zoom"), 0.05F, 0.95F);
    float pan = 0.F;

    // Fit the workspace you are actually looking at. Fitting the union of every workspace would
    // let one very long tape shrink all the others into illegibility.
    if (HSConfig::value<Config::INTEGER>("auto_fit")) {
        const CBox bounds = contentBounds(g_pCompositor->getWorkspaceByID(m_selected), mbox);
        if (bounds.w > mbox.w) {
            const float minZoom = std::clamp(HSConfig::value<Config::FLOAT>("min_zoom"), 0.02F, 0.95F);
            target = std::clamp((float)(mbox.w / bounds.w), minZoom, target);

            // Recentre on the content, or a tape that grew to the left sits half off-screen even
            // after zooming out.
            pan = (float)((bounds.x + bounds.w / 2.0) - (mbox.x + mbox.w / 2.0));
        }
    }

    if (HSConfig::value<Config::INTEGER>("fit_rows")) {
        WORKSPACEID maxId = 0;
        const float gapFactor = std::max(HSConfig::value<Config::FLOAT>("workspace_gap"), 0.F);
        const float rows = (float)std::max<size_t>(visibleWorkspaces(maxId).size(), 1);
        if (rows > 1.F)
            target = std::min(target, 1.F / (rows + (rows - 1.F) * gapFactor));
    }

    if (warp) {
        m_fitZoom->setValueAndWarp(target);
        m_fitPan->setValueAndWarp(pan);
    } else {
        *m_fitZoom = target;
        *m_fitPan = pan;
    }
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
        f.cards.push_back({.id = ws->m_id, .workspace = ws, .box = {}, .index = (int)f.cards.size(), .synthetic = false});

    // niri always keeps one empty workspace at the bottom, which is what makes "drag a window
    // down to make a new workspace" discoverable. Mirror that with a synthetic card.
    if (HSConfig::value<Config::INTEGER>("trailing_workspace") || (m_selected > maxId && m_selected != WORKSPACE_INVALID)) {
        const WORKSPACEID newId = std::max(maxId + 1, (WORKSPACEID)1);
        if (!g_pCompositor->getWorkspaceByID(newId))
            f.cards.push_back({.id = newId, .workspace = nullptr, .box = {}, .index = (int)f.cards.size(), .synthetic = true});
    }

    if (f.cards.empty())
        return f;

    const float gapFactor = std::max(HSConfig::value<Config::FLOAT>("workspace_gap"), 0.F);

    // niri: zoom = 1 - progress * (1 - configured), so progress 0 is a pixel-exact desktop and
    // opening the overview is literally a zoom-out. The target comes from updateFit().
    f.zoom = std::max(1.F - f.progress * (1.F - m_fitZoom->value()), 0.01F);

    // The auto-pan fades in with the animation too, or the tape jumps sideways on open.
    f.viewOrigin = f.monitorBox.pos() + Vector2D {(double)(m_fitPan->value() * f.progress + m_pan->value()), 0.0};

    const float cardW = f.monitorBox.w * f.zoom;
    const float cardH = f.monitorBox.h * f.zoom;
    const float pitch = cardH + f.monitorBox.h * gapFactor * f.zoom;

    const float baseX = f.monitorBox.x + (f.monitorBox.w - cardW) / 2.F;
    const float baseY = f.monitorBox.y + (f.monitorBox.h - cardH) / 2.F;
    const float row = m_row->value();

    for (auto& c : f.cards)
        c.box = CBox {baseX, baseY + ((float)c.index - row) * pitch, cardW, cardH};

    return f;
}

CBox HSView::windowBox(PHLWINDOW window, const HSCard& card, const HSFrame& f) const {
    if (!window)
        return {};

    const Vector2D srcOrigin = f.viewOrigin + hs_workspace_render_offset(card.workspace);
    return CBox {(hs_window_render_pos(window) - srcOrigin) * f.zoom + card.box.pos(), window->m_realSize->value() * f.zoom};
}

std::optional<HSCard> HSView::cardAt(const Vector2D& global) const {
    const auto f = frame();

    // niri extends each card's hit region to the full output width, so clicking the empty space
    // to the left or right of a card still selects that workspace.
    for (const auto& c : f.cards) {
        if (global.y >= c.box.y && global.y < c.box.y + c.box.h && global.x >= f.monitorBox.x && global.x < f.monitorBox.x + f.monitorBox.w)
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
        m_pan->setValueAndWarp(0.F);
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
    // Acts on the selected workspace's own layout rather than the focused window's, so browsing
    // the overview never has to disturb what the compositor thinks is active.
    const auto ws = g_pCompositor->getWorkspaceByID(m_selected);
    if (!ws || !ws->m_space)
        return;

    ws->m_space->layoutMsg(delta > 0 ? "focus r" : "focus l");

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

    // Dropping is also a choice of workspace: follow the window.
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
    // frame; the animated variables absorb it smoothly.
    updateFit(false);

    const auto time = Time::steadyNow();
    const auto f = frame();

    // Backdrop. At progress 0 the selected card covers the screen exactly, so this is only ever
    // visible once the zoom-out has started.
    CClearPassElement::SClearData clear;
    clear.color = colorOf("backdrop_color").stripA();
    g_pHyprRenderer->m_renderPass.add(makeUnique<CClearPassElement>(clear));

    // Cull by row only: windows deliberately overflow their card horizontally, so a card whose
    // own box is off to the side may still have visible content.
    const float cullMargin = f.monitorBox.h;

    for (const auto& c : f.cards) {
        if (c.box.y + c.box.h < f.monitorBox.y - cullMargin || c.box.y > f.monitorBox.y + f.monitorBox.h + cullMargin)
            continue;
        renderCard(c, f, time);
    }

    // Selection ring around the workspace you would land on.
    const float activeBorder = HSConfig::value<Config::FLOAT>("active_border_size");
    if (activeBorder > 0.F && f.progress > 0.01F) {
        if (const auto* selected = f.card(m_selected)) {
            CBorderPassElement::SBorderData border;
            border.box = toBuffer(monitor, selected->box);
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

    renderTopLayers(time);

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

    const Vector2D srcOrigin = f.viewOrigin + hs_workspace_render_offset(card.workspace);

    hs_push_modif(monitor, srcOrigin, card.box.pos(), f.zoom);
    CScopeGuard popModif([] { hs_pop_modif(); });

    // Background and bottom layers ride along with the card, so each workspace keeps its
    // wallpaper. Top and overlay layers (bars) are drawn unscaled afterwards.
    if (HSConfig::value<Config::INTEGER>("render_layers")) {
        for (const auto& layerIdx : {0, 1}) {
            for (const auto& ref : monitor->m_layerSurfaceLayers[layerIdx]) {
                const auto ls = ref.lock();
                if (ls && !ls->m_fadingOut)
                    hs_render_layer(ls, monitor, time, false);
            }
        }
    }

    // Mirrors renderWorkspaceWindows' ordering: tiled first (focused last), then their popups,
    // then floating on top. Pinned windows are handled separately -- they belong to no single
    // workspace, and renderWindow skips the workspace offset for them.
    const auto windows = workspaceWindows(card.workspace);
    const auto focusedWindow = Desktop::focusState()->window();
    PHLWINDOW lastWindow;

    const auto dragged = m_dragged.lock();

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

void HSView::renderTopLayers(const Time::steady_tp& time) {
    const auto monitor = this->monitor();
    if (!monitor || !HSConfig::value<Config::INTEGER>("render_top_layers"))
        return;

    for (const auto& layerIdx : {2, 3}) {
        for (const auto& ref : monitor->m_layerSurfaceLayers[layerIdx]) {
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
