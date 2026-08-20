#pragma once

#include <string>
#include <unordered_map>

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/values/ConfigValues.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include "globals.hpp"

using namespace Config::Values;

namespace HSConfig {

// CConfigValue resolves the key once and then reads through a pointer, so caching one per key
// keeps per-frame reads free.
template <typename T>
inline T value(const std::string& key) {
    static std::unordered_map<std::string, CConfigValue<T>> cache;

    auto it = cache.find(key);
    if (it == cache.end())
        it = cache.emplace(key, CConfigValue<T>("plugin:hyprscape:" + key)).first;

    return *it->second;
}

} // namespace HSConfig
