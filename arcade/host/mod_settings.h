// The HUD-lines switch on the tablet's SETTINGS tab: the "hud_lines_..." patches in
// NvrAssetPatches' manifest.json (bin\win10\asset_patches), read when Echo starts.
// Header-only so tests/settings_test.cpp can check it on its own.
#pragma once
#include <algorithm>
#include <string>

namespace mods {

// Calls f(begin, end) for the {...} of every patch whose label starts with "hud_lines_".
template <class F> void eachHudLinesPatch(const std::string& json, F f) {
    for (size_t at = json.find("\"label\""); at != std::string::npos; at = json.find("\"label\"", at + 7)) {
        size_t colon = json.find(':', at + 7), quote = colon == std::string::npos ? colon : json.find('"', colon);
        if (quote == std::string::npos || json.compare(quote + 1, 10, "hud_lines_") != 0) continue;
        size_t begin = json.rfind('{', at), end = json.find('}', at);
        if (begin != std::string::npos && end != std::string::npos) f(begin, end);
    }
}

// Where the true/false of "enabled" sits between begin and end: {position, length}, or {npos, 0}.
inline std::pair<size_t, size_t> enabledValue(const std::string& json, size_t begin, size_t end) {
    size_t key = json.find("\"enabled\"", begin);
    if (key == std::string::npos || key > end) return {std::string::npos, 0};
    size_t v = json.find_first_not_of(" \t\r\n:", key + 9);
    if (v < end && json.compare(v, 4, "true") == 0) return {v, 4};
    if (v < end && json.compare(v, 5, "false") == 0) return {v, 5};
    return {std::string::npos, 0};
}

// -1: no HUD-lines patches; 0: all switched off; 1: at least one on (no "enabled" = on).
inline int hudLinesState(const std::string& json) {
    int state = -1;
    eachHudLinesPatch(json, [&](size_t begin, size_t end) {
        auto v = enabledValue(json, begin, end);
        bool on = v.first == std::string::npos || v.second == 4;
        state = std::max(state, on ? 1 : 0);
    });
    return state;
}

// The same text with every HUD-lines patch's "enabled" set to `on`; nothing else changes.
inline std::string setHudLines(std::string json, bool on) {
    std::string out;
    size_t copied = 0;
    eachHudLinesPatch(json, [&](size_t begin, size_t end) {
        auto v = enabledValue(json, begin, end);
        out.append(json, copied, (v.first == std::string::npos ? end : v.first) - copied);
        if (v.first == std::string::npos) {  // no "enabled" key: add one before the closing brace
            while (!out.empty() && (out.back() == ' ' || out.back() == '\n' || out.back() == '\r' || out.back() == '\t')) out.pop_back();
            out += on ? ",\n            \"enabled\": true\n        " : ",\n            \"enabled\": false\n        ";
            copied = end;
        } else {
            out += on ? "true" : "false";
            copied = v.first + v.second;
        }
    });
    out.append(json, copied, std::string::npos);
    return out;
}

}  // namespace mods
