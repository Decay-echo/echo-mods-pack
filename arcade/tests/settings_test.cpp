// The SETTINGS tab's HUD-lines switch (host/mod_settings.h) on manifest.json text.
#include "../host/mod_settings.h"
#include <cstdio>

static int failures = 0, checks = 0;
#define CHECK(x) do { checks++; if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

int main() {
    // As NvrAssetPatches' manifest.json is written (Python json.dumps, indent 4): others around ours.
    const std::string manifest = R"({
    "version": "1.0",
    "patches": [
        {
            "label": "netgun_base",
            "type_hash": "0x6EF7231BBB0F6342",
            "file": "0xCB9977F7FC2B4526.bin",
            "enabled": true
        },
        {
            "label": "hud_lines_c98ec7f37bb9c291",
            "type_hash": "0xBEAC1969CB7B8861",
            "file": "hud_lines/c98ec7f37bb9c291.desc",
            "buffer_index": 0,
            "enabled": true
        },
        {
            "label": "hud_lines_layout_speed",
            "file": "hud_lines/d66fcf1b64026ebd.canvas",
            "enabled":true
        },
        {
            "label": "poster_a_tex",
            "enabled": false
        }
    ]
}
)";
    CHECK(mods::hudLinesState(manifest) == 1);
    std::string off = mods::setHudLines(manifest, false);
    CHECK(mods::hudLinesState(off) == 0);
    CHECK(off.find("\"label\": \"netgun_base\",\n            \"type_hash\": \"0x6EF7231BBB0F6342\",\n            \"file\": \"0xCB9977F7FC2B4526.bin\",\n            \"enabled\": true") != std::string::npos);
    CHECK(off.find("\"enabled\":false") != std::string::npos);                     // spacing kept
    CHECK(off.size() == manifest.size() + 2);                                      // two "true" -> "false"
    std::string on = mods::setHudLines(off, true);
    CHECK(on == manifest);                                                         // back byte for byte
    CHECK(mods::setHudLines(manifest, true) == manifest);                          // already on: unchanged
    // poster_a_tex (not ours) stays off either way
    CHECK(on.find("\"label\": \"poster_a_tex\",\n            \"enabled\": false") != std::string::npos);

    // No HUD-lines patches at all: -1 and nothing changes.
    const std::string none = R"({"version": "1.0", "patches": [{"label": "netgun_base", "enabled": true}]})";
    CHECK(mods::hudLinesState(none) == -1);
    CHECK(mods::setHudLines(none, false) == none);
    CHECK(mods::hudLinesState(R"({"patches": []})") == -1);

    // A patch without "enabled" counts as on, and gets the key when switched off.
    const std::string bare = "{\"patches\": [\n        {\n            \"label\": \"hud_lines_x\",\n            \"file\": \"a\"\n        }\n    ]}";
    CHECK(mods::hudLinesState(bare) == 1);
    std::string bareOff = mods::setHudLines(bare, false);
    CHECK(mods::hudLinesState(bareOff) == 0);
    CHECK(bareOff.find("\"file\": \"a\",\n            \"enabled\": false\n        }") != std::string::npos);
    CHECK(mods::hudLinesState(mods::setHudLines(bareOff, true)) == 1);

    // Some on, some off: counts as on; switching on turns all on.
    std::string mixed = manifest;
    mixed.replace(mixed.find("\"enabled\":true"), 14, "\"enabled\":false");
    CHECK(mods::hudLinesState(mixed) == 1);
    CHECK(mods::setHudLines(mixed, true) == manifest);

    std::printf("%d/%d settings checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
