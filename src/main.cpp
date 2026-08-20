#include <linux/input-event-codes.h>

#include <cmath>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/values/ConfigValues.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/macros.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <lua.hpp>

#include "config.hpp"
#include "globals.hpp"
#include "manager.hpp"
#include "types.hpp"
#include "view.hpp"

using namespace Config::Values;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// Each dispatcher body is exposed twice: as a classic `hyprscape:<name>` dispatcher and as a
// Lua function under `hl.plugin.hyprscape.<name>`, which is what a Lua config calls.
#define DISPATCHER(name)                                                                                                                                                           \
    static SDispatchResult dispatch_##name(std::string arg);                                                                                                                       \
    static int hs_lua_##name(lua_State* L) {                                                                                                                                          \
        const auto RESULT = dispatch_##name(luaL_optstring(L, 1, ""));                                                                                                             \
        if (!RESULT.success)                                                                                                                                                       \
            return luaL_error(L, "%s", RESULT.error.c_str());                                                                                                                      \
        return 0;                                                                                                                                                                  \
    }                                                                                                                                                                              \
    static SDispatchResult dispatch_##name(std::string arg)

DISPATCHER(toggle) {
    if (!hs_manager)
        return {.success = false, .error = "hyprscape is not initialized"};
    hs_manager->toggle(arg.empty() ? "all" : arg);
    return {};
}

DISPATCHER(open) {
    if (!hs_manager)
        return {.success = false, .error = "hyprscape is not initialized"};
    hs_manager->show(arg.empty() ? "all" : arg);
    return {};
}

DISPATCHER(close) {
    if (!hs_manager)
        return {.success = false, .error = "hyprscape is not initialized"};
    hs_manager->hide();
    return {};
}

// "up" / "down" walk the workspace stack, "left" / "right" walk the tape.
DISPATCHER(move) {
    if (!hs_manager)
        return {.success = false, .error = "hyprscape is not initialized"};

    const auto view = hs_manager->viewFromCursor();
    if (!view || !view->m_active)
        return {.success = false, .error = "overview is not open"};

    if (arg == "up")
        view->selectRow(-1);
    else if (arg == "down")
        view->selectRow(1);
    else if (arg == "left")
        view->selectColumn(-1);
    else if (arg == "right")
        view->selectColumn(1);
    else
        return {.success = false, .error = "expected up, down, left or right"};

    return {};
}

DISPATCHER(select) {
    if (!hs_manager)
        return {.success = false, .error = "hyprscape is not initialized"};

    const auto view = hs_manager->viewFromCursor();
    if (!view || !view->m_active)
        return {.success = false, .error = "overview is not open"};

    const auto hovered = view->m_hovered.lock();
    view->hide(hovered ? hovered : view->selectedAnchor());
    return {};
}

static int hs_lua_is_active(lua_State* L) {
    lua_pushboolean(L, hs_manager && hs_manager->anyActive());
    return 1;
}

//
// Render takeover
//

static void hook_render_workspace(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now, const CBox& geometry) {
    const auto original = (render_workspace_t)(render_workspace_hook->m_original);

    if (!hs_manager) {
        original(thisptr, monitor, workspace, now, geometry);
        return;
    }

    const auto view = hs_manager->viewForMonitor(monitor);
    if (!view || !view->rendering()) {
        original(thisptr, monitor, workspace, now, geometry);
        return;
    }

    view->render();
}

//
// Corrective hooks. Hyprland applies the active renderModif to the quads it draws, but not to
// the regions it derives scissors and blur samples from. Under our scaled render that mismatch
// cuts window contents off at the card edge and puts blur in the wrong place. These three hooks
// bring those paths back in line, and only while we are the ones driving the modif.
//

typedef void (*render_texture_t)(void* thisptr, SP<Render::ITexture> tex, const CBox& box, Render::GL::CHyprOpenGLImpl::STextureRenderData data);
typedef void (*render_border_t)(void* thisptr, const CBox& box, const Config::CGradientValueData& grad, Render::GL::CHyprOpenGLImpl::SBorderRenderData data);
typedef void (*render_border2_t)(void* thisptr, const CBox& box, const Config::CGradientValueData& grad1, const Config::CGradientValueData& grad2, float lerp,
                                 Render::GL::CHyprOpenGLImpl::SBorderRenderData data);
typedef bool (*blur_optimizations_t)(void* thisptr, PHLLS layer, PHLWINDOW window);
typedef CRegion (*visible_region_t)(void* thisptr, bool& cancel);

static bool hs_scaled_render() {
    if (!hs_manager)
        return false;

    auto& modif = g_pHyprRenderer->m_renderData.renderModif;
    if (!modif.enabled || modif.modifs.empty())
        return false;

    return hs_manager->anyRendering();
}

static void hook_render_texture(void* thisptr, SP<Render::ITexture> tex, const CBox& box, Render::GL::CHyprOpenGLImpl::STextureRenderData data) {
    const auto original = (render_texture_t)(render_texture_hook->m_original);

    auto& renderData = g_pHyprRenderer->m_renderData;
    auto& modif = renderData.renderModif;

    if (!hs_scaled_render() || renderData.pMonitor == nullptr) {
        original(thisptr, tex, box, data);
        return;
    }

    // The scissor is derived from untransformed coordinates, so under the card modif it only
    // ever clips content away at the card edges. Widen it to the whole monitor.
    CRegion fullDamage = CBox {0, 0, renderData.pMonitor->m_transformedSize.x, renderData.pMonitor->m_transformedSize.y};
    data.damage = &fullDamage;
    data.clipRegion = {};

    const CBox savedClip = renderData.clipBox;
    renderData.clipBox = CBox {};

    if (data.blur) {
        // Blur derives both its quad and its sample UVs from `box`, but only the quad gets the
        // modif. Pre-bake the transform into the box and switch the modif off so both agree.
        CBox tbox = box;
        modif.applyToBox(tbox);
        const auto savedModif = modif;
        modif.enabled = false;
        original(thisptr, tex, tbox, data);
        modif = savedModif;
    } else
        original(thisptr, tex, box, data);

    renderData.clipBox = savedClip;
}

template <typename Fn>
static void render_border_scaled(CBox& box, Render::GL::CHyprOpenGLImpl::SBorderRenderData& data, Fn&& callOriginal) {
    auto& modif = g_pHyprRenderer->m_renderData.renderModif;

    if (!hs_scaled_render()) {
        callOriginal();
        return;
    }

    // renderBorder scissors with (transformed ring) minus (untransformed box); under a scaled
    // render the untransformed interior swallows the whole ring and no border draws. Pre-bake
    // the transform so every coordinate it touches lives in the same space.
    modif.applyToBox(box);
    data.borderSize = std::round(data.borderSize * modif.combinedScale());

    const auto savedModif = modif;
    modif.enabled = false;
    callOriginal();
    modif = savedModif;
}

static void hook_render_border(void* thisptr, const CBox& box, const Config::CGradientValueData& grad, Render::GL::CHyprOpenGLImpl::SBorderRenderData data) {
    CBox tbox = box;
    render_border_scaled(tbox, data, [&] { ((render_border_t)(render_border_hook->m_original))(thisptr, tbox, grad, data); });
}

static void hook_render_border2(void* thisptr, const CBox& box, const Config::CGradientValueData& grad1, const Config::CGradientValueData& grad2, float lerp,
                                Render::GL::CHyprOpenGLImpl::SBorderRenderData data) {
    CBox tbox = box;
    render_border_scaled(tbox, data, [&] { ((render_border2_t)(render_border2_hook->m_original))(thisptr, tbox, grad1, grad2, lerp, data); });
}

// A client can declare, through hyprland_surface_v1, which part of its surface is actually
// visible, so the compositor can skip the rest -- alacritty is one of the few that bothers.
// Hyprland derives that from the window's real on-screen position, so for a column scrolled off
// the viewport the region collapses and ElementRenderer cancels the draw outright. In the
// overview those windows ARE on screen, just somewhere else, so the whole notion has to be
// switched off while we render: an empty region with cancel left alone means "no restriction".
static CRegion hook_visible_region(void* thisptr, bool& cancel) {
    if (hs_manager && hs_manager->anyRendering())
        return CRegion {};

    return ((visible_region_t)(visible_region_hook->m_original))(thisptr, cancel);
}

// The optimized blur path samples a precomputed framebuffer holding the pre-overview desktop,
// which is at the wrong place once cards are scaled. Force the fresh path while we draw.
static bool hook_blur_optimizations(void* thisptr, PHLLS layer, PHLWINDOW window) {
    if (hs_scaled_render())
        return false;
    return ((blur_optimizations_t)(blur_optimizations_hook->m_original))(thisptr, layer, window);
}

// The solitary/direct-scanout branch calls renderWindow directly and never reaches
// renderWorkspace, so without this the overview simply would not draw over a fullscreen client.
static uint32_t hook_is_solitary_blocked(void* thisptr, bool full) {
    const auto original = (orig_is_solitary_blocked_t)(is_solitary_blocked_hook->m_original);

    if (!hs_manager || !hs_manager->anyRendering())
        return original(thisptr, full);

    return CMonitor::SC_UNKNOWN;
}

//
// Event bus
//

static void on_mouse_button(IPointer::SButtonEvent e, Event::SCallbackInfo& info) {
    if (hs_manager)
        info.cancelled = hs_manager->onMouseButton(e);
}

static void on_mouse_move(Vector2D coords, Event::SCallbackInfo& info) {
    if (hs_manager)
        info.cancelled = hs_manager->onMouseMove();
}

static void on_mouse_axis(IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
    if (hs_manager)
        info.cancelled = hs_manager->onMouseAxis(e);
}

static void on_key(IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) {
    if (hs_manager)
        info.cancelled = hs_manager->onKey(e);
}

static void on_swipe_begin(IPointer::SSwipeBeginEvent e, Event::SCallbackInfo& info) {
    if (hs_manager)
        hs_manager->swipeBegin(e);
}

static void on_swipe_update(IPointer::SSwipeUpdateEvent e, Event::SCallbackInfo& info) {
    if (hs_manager)
        info.cancelled = hs_manager->swipeUpdate(e);
}

static void on_swipe_end(IPointer::SSwipeEndEvent e, Event::SCallbackInfo& info) {
    if (hs_manager)
        info.cancelled = hs_manager->swipeEnd();
}

static void on_config_reloaded() {
    if (hs_manager)
        hs_manager->onConfigReloaded();
}

static void on_monitor_added(PHLMONITOR monitor) {
    if (hs_manager)
        hs_manager->rebuildViews();
}

static void on_monitor_removed(PHLMONITOR monitor) {
    if (hs_manager && monitor)
        hs_manager->removeViewForMonitor(monitor->m_id);
}

static void register_callbacks() {
    static auto P1 = Event::bus()->m_events.input.mouse.button.listen(on_mouse_button);
    static auto P2 = Event::bus()->m_events.input.mouse.move.listen(on_mouse_move);
    static auto P3 = Event::bus()->m_events.input.mouse.axis.listen(on_mouse_axis);
    static auto P4 = Event::bus()->m_events.input.keyboard.key.listen(on_key);

    static auto P5 = Event::bus()->m_events.gesture.swipe.begin.listen(on_swipe_begin);
    static auto P6 = Event::bus()->m_events.gesture.swipe.update.listen(on_swipe_update);
    static auto P7 = Event::bus()->m_events.gesture.swipe.end.listen(on_swipe_end);

    static auto P8 = Event::bus()->m_events.config.reloaded.listen(on_config_reloaded);
    static auto P9 = Event::bus()->m_events.monitor.added.listen(on_monitor_added);
    static auto P10 = Event::bus()->m_events.monitor.removed.listen(on_monitor_removed);
}

//
// Init
//

static void* resolve(const char* mangled, const char* what) {
    const auto fns = HyprlandAPI::findFunctionsByName(PHANDLE, mangled);
    if (fns.empty())
        fail_exit("could not resolve {} -- is this Hyprland {}?", what, HYPRLAND_API_VERSION);
    return fns[0].address;
}

static void init_hooks() {
    bool success = true;

    const auto renderWorkspace = HyprlandAPI::findFunctionsByName(PHANDLE, "renderWorkspace");
    if (renderWorkspace.empty())
        fail_exit("could not resolve renderWorkspace");
    render_workspace_hook = HyprlandAPI::createFunctionHook(PHANDLE, renderWorkspace[0].address, (void*)hook_render_workspace);
    success = render_workspace_hook->hook() && success;

    render_texture_hook = HyprlandAPI::createFunctionHook(
        PHANDLE,
        resolve("_ZN6Render2GL15CHyprOpenGLImpl13renderTextureEN9Hyprutils6Memory14CSharedPointerINS_8ITextureEEERKNS2_4Math4CBoxENS1_18STextureRenderDataE", "renderTexture"),
        (void*)hook_render_texture);
    success = render_texture_hook->hook() && success;

    render_border_hook = HyprlandAPI::createFunctionHook(
        PHANDLE, resolve("_ZN6Render2GL15CHyprOpenGLImpl12renderBorderERKN9Hyprutils4Math4CBoxERKN6Config18CGradientValueDataENS1_17SBorderRenderDataE", "renderBorder"),
        (void*)hook_render_border);
    success = render_border_hook->hook() && success;

    render_border2_hook = HyprlandAPI::createFunctionHook(
        PHANDLE, resolve("_ZN6Render2GL15CHyprOpenGLImpl12renderBorderERKN9Hyprutils4Math4CBoxERKN6Config18CGradientValueDataESA_fNS1_17SBorderRenderDataE", "renderBorder (lerp)"),
        (void*)hook_render_border2);
    success = render_border2_hook->hook() && success;

    blur_optimizations_hook = HyprlandAPI::createFunctionHook(
        PHANDLE,
        resolve("_ZN6Render13IHyprRenderer29shouldUseNewBlurOptimizationsEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View13CLayerSurfaceEEENS3_INS5_7CWindowEEE",
                "shouldUseNewBlurOptimizations"),
        (void*)hook_blur_optimizations);
    success = blur_optimizations_hook->hook() && success;

    visible_region_hook = HyprlandAPI::createFunctionHook(PHANDLE, resolve("_ZN19CSurfacePassElement13visibleRegionERb", "CSurfacePassElement::visibleRegion"),
                                                         (void*)hook_visible_region);
    success = visible_region_hook->hook() && success;

    const auto solitary = HyprlandAPI::findFunctionsByName(PHANDLE, "isSolitaryBlocked");
    if (solitary.empty())
        fail_exit("could not resolve isSolitaryBlocked");
    is_solitary_blocked_hook = HyprlandAPI::createFunctionHook(PHANDLE, solitary[0].address, (void*)hook_is_solitary_blocked);
    success = is_solitary_blocked_hook->hook() && success;

    // Called, not hooked: both are protected members we cannot reach through the header.
    render_window = resolve("_ZN6Render13IHyprRenderer12renderWindowEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEENS3_I8CMonitorEERKNSt6chrono10time_pointINSA_3_V2"
                            "12steady_clockENSA_8durationIlSt5ratioILl1ELl1000000000EEEEEEbNS_15eRenderPassModeEbb",
                            "renderWindow");
    render_layer = resolve("_ZN6Render13IHyprRenderer11renderLayerEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View13CLayerSurfaceEEENS3_I8CMonitorEERKNSt6chrono10time_pointINSA_"
                           "3_V212steady_clockENSA_8durationIlSt5ratioILl1ELl1000000000EEEEEEbb",
                           "renderLayer");

    if (!success)
        fail_exit("failed installing hooks");
}

#define ADD_DISPATCHER(name)                                                                                                                                                       \
    do {                                                                                                                                                                           \
        HyprlandAPI::addDispatcherV2(PHANDLE, "hyprscape:" #name, dispatch_##name);                                                                                                \
        HyprlandAPI::addLuaFunction(PHANDLE, "hyprscape", #name, hs_lua_##name);                                                                                                      \
    } while (0)

static void add_dispatchers() {
    ADD_DISPATCHER(toggle);
    ADD_DISPATCHER(open);
    ADD_DISPATCHER(close);
    ADD_DISPATCHER(move);
    ADD_DISPATCHER(select);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprscape", "is_active", hs_lua_is_active);
}

// IValue keeps `name` and `description` as non-owning const char*, so both must be literals.
#define ADD_CONFIG(T, key, description, def)                                                                                                                                       \
    do {                                                                                                                                                                           \
        SP<Config::Values::IValue> value = makeShared<T>("plugin:hyprscape:" key, description, def);                                                                               \
        if (!Config::mgr()->registerPluginValue(PHANDLE, value))                                                                                                                   \
            Log::logger->log(ERR, "[hyprscape] could not register plugin:hyprscape:{}", key);                                                                                      \
    } while (0)

static void init_config() {
    // Geometry
    ADD_CONFIG(CFloatValue, "zoom", "how far to zoom out, as a fraction of the monitor (niri default 0.5)", 0.5F);
    ADD_CONFIG(CFloatValue, "workspace_gap", "vertical gap between workspaces, as a fraction of monitor height", 0.1F);
    ADD_CONFIG(CIntValue, "auto_fit", "0 = always use zoom; 1 = pick one zoom per session from the longest tape; 2 = re-fit per workspace", 1);
    ADD_CONFIG(CFloatValue, "min_zoom", "how far auto_fit is allowed to zoom out", 0.12F);
    ADD_CONFIG(CIntValue, "fit_rows", "also zoom out until every workspace row fits on screen", 0);

    // Appearance
    ADD_CONFIG(CColorValue, "backdrop_color", "wallpaper dim, and the opaque base when background layers are off", 0xCC16161E);
    ADD_CONFIG(CColorValue, "card_color", "band behind every workspace row, full output width; alpha 0 disables it", 0x00000000);
    ADD_CONFIG(CColorValue, "active_row_color", "band behind the selected workspace row; alpha 0 disables it", 0x00000000);
    ADD_CONFIG(CIntValue, "render_background_layers", "draw background/bottom layer surfaces (your wallpaper), unscaled and unmoved", 1);
    ADD_CONFIG(CIntValue, "render_top_layers", "draw top/overlay layer surfaces (your bar), unscaled and unmoved", 1);
    ADD_CONFIG(CFloatValue, "active_border_size", "ring around the centred window of the selected row, 0 to disable", 2.F);
    ADD_CONFIG(CColorValue, "active_border_color", "colour of that ring", 0xFF3399FF);
    ADD_CONFIG(CFloatValue, "hover_border_size", "border around the hovered window, 0 to disable", 3.F);
    ADD_CONFIG(CColorValue, "hover_border_color", "border colour for the hovered window", 0xFF88BBFF);

    // Which workspaces get a row
    ADD_CONFIG(CIntValue, "show_empty", "also show empty workspaces you are not on", 0);
    ADD_CONFIG(CIntValue, "trailing_workspace", "keep one empty workspace at the bottom, niri style", 1);
    ADD_CONFIG(CIntValue, "new_workspace_hint", "outline that trailing row so the drag target is visible", 0);

    // Behaviour
    ADD_CONFIG(CIntValue, "exit_on_click", "clicking a window or workspace closes the overview", 1);
    ADD_CONFIG(CIntValue, "close_on_reload", "close the overview when the config reloads", 1);
    ADD_CONFIG(CIntValue, "warp_cursor", "warp the cursor onto the window you select", 0);
    ADD_CONFIG(CFloatValue, "scroll_speed", "wheel sensitivity when walking workspaces", 1.F);
    ADD_CONFIG(CIntValue, "select_button", "mouse button that picks a window", BTN_LEFT);
    ADD_CONFIG(CIntValue, "pan_button", "mouse button that drags the tape sideways, 0 to disable", BTN_RIGHT);

    ADD_CONFIG(CIntValue, "debug", "log per-window overview geometry each frame", 0);

    // Touchpad
    ADD_CONFIG(CIntValue, "gestures:enabled", "enable touchpad gestures", 1);
    ADD_CONFIG(CIntValue, "gestures:open_fingers", "fingers for the open/close swipe", 4);
    ADD_CONFIG(CFloatValue, "gestures:open_distance", "swipe distance for a full open/close", 300.F);
    ADD_CONFIG(CIntValue, "gestures:open_positive", "1 if swiping down should open instead of up", 0);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // Hyprland's plugin loader already refuses a plugin built against a different ABI string,
    // so there is nothing useful to re-check here.
    init_config();
    init_hooks();
    add_dispatchers();
    register_callbacks();

    hs_manager = std::make_unique<HSManager>();
    hs_manager->rebuildViews();

    hs_log("initialized");

    return {"hyprscape", "A niri-style zoom-out overview for Hyprland's scrolling layout", "hyprscape", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (!hs_manager)
        return;

    // Commit whatever workspace the user was looking at, then snap the animation shut: the
    // render hook is about to disappear, so there is nothing left to animate into.
    hs_manager->hide();
    for (const auto& view : hs_manager->m_views) {
        if (!view)
            continue;
        view->m_progress->setValueAndWarp(0.F);
        view->m_active = false;
        view->m_closing = false;
    }

    hs_manager.reset();
}
