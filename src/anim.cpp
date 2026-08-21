#include "anim.hpp"

#include <algorithm>
#include <string>

#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprutils/math/Vector2D.hpp>

#include "config.hpp"
#include "globals.hpp"

using Hyprutils::Animation::SAnimationPropertyConfig;
using Hyprutils::Animation::SSpringCurve;

namespace {

constexpr const char* SMOOTH_BEZIER = "hyprscape_smooth";
constexpr const char* SPRING_NAME = "hyprscape_spring";
constexpr std::string_view SPRING_PREFIX = "spring:";

SP<SAnimationPropertyConfig> g_config;

// The legacy config parser drops every registered bezier before re-parsing, so ours have to be
// put back whenever they go missing. Registering the same name twice just overwrites it, and
// this runs every frame, so the work is skipped unless something actually changed.
void registerCurves() {
    if (!g_pAnimationManager->bezierExists(SMOOTH_BEZIER)) {
        // easeOutCubic: decisive at the start, flat at the end, and -- unlike Hyprland's
        // `default` -- it never travels past its goal, which is what "not springy" means.
        g_pAnimationManager->addBezierWithName(SMOOTH_BEZIER, Vector2D {0.33, 1.0}, Vector2D {0.68, 1.0});
    }

    SSpringCurve spring;
    spring.stiffness = std::max(HSConfig::value<Config::FLOAT>("spring_stiffness"), 1.F);
    spring.damping = std::max(HSConfig::value<Config::FLOAT>("spring_damping"), 0.1F);
    spring.mass = std::max(HSConfig::value<Config::FLOAT>("spring_mass"), 0.01F);

    static SSpringCurve lastSpring {.stiffness = -1.F};
    const bool changed = spring.stiffness != lastSpring.stiffness || spring.damping != lastSpring.damping || spring.mass != lastSpring.mass;

    if (changed || !g_pAnimationManager->springExists(SPRING_NAME)) {
        g_pAnimationManager->addSpringWithName(SPRING_NAME, spring);
        lastSpring = spring;
    }
}

// Hyprutils picks a spring over a bezier by the "spring:" prefix on the curve name.
std::string resolveCurve(const std::string& name) {
    if (name.empty() || name == "smooth")
        return SMOOTH_BEZIER;
    if (name == "spring")
        return std::string {SPRING_PREFIX} + SPRING_NAME;

    // Anything else names a curve from the user's own config -- hl.curve("wind", ...) or a
    // spring they defined -- so they can hand the overview any feel they already like.
    if (name.starts_with(SPRING_PREFIX)) {
        if (g_pAnimationManager->springExists(name.substr(SPRING_PREFIX.size())))
            return name;
    } else {
        if (g_pAnimationManager->springExists(name))
            return std::string {SPRING_PREFIX} + name;
        if (g_pAnimationManager->bezierExists(name))
            return name;
    }

    // This runs every frame, so only complain when the name itself changes.
    static std::string lastComplaint;
    if (lastComplaint != name) {
        lastComplaint = name;
        hs_log("no bezier or spring named \"{}\", using smooth", name);
    }

    return SMOOTH_BEZIER;
}

} // namespace

SP<SAnimationPropertyConfig> hs_animation_config() {
    hs_refresh_animation_config();
    return g_config;
}

void hs_refresh_animation_config() {
    if (!g_config) {
        g_config = makeShared<SAnimationPropertyConfig>();
        // An animated variable reads through pValues, and ours are its own.
        g_config->pValues = g_config;
    }

    registerCurves();

    const std::string curve = HSConfig::value<Config::STRING>("animation_curve");

    if (curve == "inherit") {
        // Whatever the user set for `workspaces`, values and all -- the pre-0.2 behaviour.
        if (const auto ws = Config::animationTree()->getAnimationPropertyConfig("workspaces"); ws && ws->pValues) {
            const auto values = *ws->pValues;
            g_config->overridden = values.overridden;
            g_config->internalBezier = values.internalBezier;
            g_config->internalSpeed = values.internalSpeed;
            g_config->internalEnabled = values.internalEnabled;
            // Never inherit the style: `slidefadevert` and friends mean nothing to a float.
            g_config->internalStyle = "";
            return;
        }
    }

    g_config->overridden = true;
    g_config->internalEnabled = HSConfig::value<Config::INTEGER>("animation_enabled") ? 1 : 0;
    g_config->internalStyle = "";
    g_config->internalSpeed = std::max(HSConfig::value<Config::FLOAT>("animation_speed"), 0.1F);
    g_config->internalBezier = resolveCurve(curve);
}
