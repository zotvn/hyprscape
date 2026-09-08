#include "render.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>

#include "globals.hpp"
#include "types.hpp"

Vector2D hs_workspace_render_offset(PHLWORKSPACE workspace) {
    if (!workspace)
        return {};
    return workspace->m_renderOffset->value();
}

Vector2D hs_window_render_pos(PHLWINDOW window) {
    if (!window)
        return {};

    // Mirrors Renderer.cpp:559 (REALPOS) and Renderer.cpp:625-626 (floating offset).
    Vector2D pos = window->positionAnimation()->value() + window->m_floatingOffset;

    if (!window->m_pinned)
        pos += hs_workspace_render_offset(window->m_workspace);

    return pos;
}

void hs_push_modif(PHLMONITOR monitor, const Vector2D& srcOrigin, const Vector2D& dstOrigin, float scale) {
    if (!monitor || scale <= 0.F)
        return;

    // Hyprland puts a global point p at (p - monPos) * monScale in buffer space. With a
    // translate-then-scale modif that becomes ((p - monPos) * monScale + T) * scale. Requiring
    // that to equal ((p - srcOrigin) * scale + dstOrigin - monPos) * monScale and solving for T:
    const Vector2D t = (monitor->m_position - srcOrigin) * monitor->m_scale + (dstOrigin - monitor->m_position) * monitor->m_scale / scale;

    SRenderModifData data {};
    data.modifs.push_back({SRenderModifData::eRenderModifType::RMOD_TYPE_TRANSLATE, t});
    data.modifs.push_back({SRenderModifData::eRenderModifType::RMOD_TYPE_SCALE, scale});
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData {data}));
}

void hs_pop_modif() {
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData {SRenderModifData {}}));
}

void hs_render_window(PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& time, int passMode) {
    if (!window || !monitor || render_window == nullptr)
        return;

    g_pHyprRenderer->damageWindow(window);

    // standalone=false keeps decorations, rounding and blur. renderWindow performs no
    // workspace-visibility check of its own, so windows from hidden workspaces draw fine.
    ((render_window_t)render_window)(g_pHyprRenderer.get(), window, monitor, time, true, (eRenderPassMode)passMode, false, false);
}

void hs_render_layer(PHLLS layer, PHLMONITOR monitor, const Time::steady_tp& time, bool popups) {
    if (!layer || !monitor || render_layer == nullptr)
        return;

    ((render_layer_t)render_layer)(g_pHyprRenderer.get(), layer, monitor, time, popups, false);
}

void hs_render_window_at_box(PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& time, CBox box, bool decorate) {
    if (!window || !monitor || render_window == nullptr)
        return;

    const Vector2D size = window->sizeAnimation()->value();
    if (size.x < 1.0 || size.y < 1.0 || box.w < 1.0 || box.h < 1.0)
        return;

    const float scale = box.w / size.x;

    hs_push_modif(monitor, hs_window_render_pos(window), box.pos(), scale);
    g_pHyprRenderer->damageWindow(window);
    ((render_window_t)render_window)(g_pHyprRenderer.get(), window, monitor, time, decorate, RENDER_PASS_MAIN, false, false);
    hs_pop_modif();
}

HSWorkspaceAlphaGuard::HSWorkspaceAlphaGuard(PHLWORKSPACE workspace) : m_workspace(workspace) {
    if (!m_workspace)
        return;

    m_saved = m_workspace->m_alpha->value();
    if (m_saved >= 1.F)
        return;

    m_workspace->m_alpha->value() = 1.F;
    m_touched = true;
}

HSWorkspaceAlphaGuard::~HSWorkspaceAlphaGuard() {
    if (m_touched)
        m_workspace->m_alpha->value() = m_saved;
}
