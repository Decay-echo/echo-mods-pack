#include "host.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <shlobj.h>

std::wstring localAppData() {
    wchar_t base[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return base;
    std::wstring result;
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known))) result = known;
    CoTaskMemFree(known);
    return result;
}

void hostLog(const char* format, ...) {
    static std::mutex m;
    static FILE* file = nullptr;
    std::lock_guard<std::mutex> lock(m);
    if (!file) {
        std::wstring dir = localAppData() + L"\\EchoArcade";
        CreateDirectoryW(dir.c_str(), nullptr);
        file = _wfsopen((dir + L"\\host.log").c_str(), L"a", _SH_DENYNO);
        if (!file) return;
    }
    fprintf(file, "%llu ", GetTickCount64());
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fflush(file);
}

Config loadConfig() {
    Config c;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    c.dir = path;
    c.dir.resize(c.dir.find_last_of(L"\\/"));
    std::wstring ini = c.dir + L"\\arcade.ini";
    auto get = [&](const wchar_t* key, const wchar_t* fallback = L"") {
        wchar_t buf[2048];
        GetPrivateProfileStringW(L"paths", key, fallback, buf, 2048, ini.c_str());
        return std::wstring(buf);
    };
    c.root = get(L"root");
    if (c.root.empty()) {  // older arcade.ini: apps\roms sits in the arcade folder
        std::wstring roms = get(L"roms");
        c.root = roms.substr(0, roms.find_last_of(L"\\/"));
        c.root = c.root.substr(0, c.root.find_last_of(L"\\/"));
    }
    c.love = get(L"love");
    c.balatroSteam = get(L"balatro_steam");
    c.retroarch = get(L"retroarch");
    c.doomCore = get(L"doom_core");
    c.doomWad = get(L"doom_wad");
    c.roms = get(L"roms");
    // Older arcade.ini files have no duck_hunt_ keys: default next to RetroArch and the ROMs.
    std::wstring retroDir = c.retroarch.substr(0, c.retroarch.find_last_of(L"\\/"));
    c.duckHuntCore = get(L"duck_hunt_core", (retroDir + L"\\cores\\nestopia_libretro.dll").c_str());
    c.duckHuntRom = get(L"duck_hunt_rom", (c.roms + L"\\nes\\Duck Hunt (World).nes").c_str());
    c.mpv = get(L"mpv");
    // Older arcade.ini files have no YouTube keys: yt-dlp beside mpv, links beside the ROM folder.
    c.ytdlp = get(L"yt_dlp", (c.mpv.substr(0, c.mpv.find_last_of(L"\\/")) + L"\\yt-dlp.exe").c_str());
    c.youtubeLinks = get(L"youtube_links", (c.roms.substr(0, c.roms.find_last_of(L"\\/")) + L"\\youtube.txt").c_str());
    std::wstring movies = get(L"movies"), part;
    for (size_t i = 0; i <= movies.size(); i++) {
        if (i == movies.size() || movies[i] == L';') {
            wchar_t expanded[MAX_PATH];
            if (!part.empty() && ExpandEnvironmentStringsW(part.c_str(), expanded, MAX_PATH)) c.movies.push_back(expanded);
            part.clear();
        } else part += movies[i];
    }
    wchar_t mode[64];
    GetPrivateProfileStringW(L"host", L"window_mode", L"desktop", mode, 64, ini.c_str());
    c.windowMode = mode;
    wchar_t audio[256];
    GetPrivateProfileStringW(L"host", L"audio_device", L"Rift Audio|Meta Quest|Oculus Virtual Audio", audio, 256, ini.c_str());
    c.appVolume = float(std::clamp(int(GetPrivateProfileIntW(L"host", L"app_volume", 65, ini.c_str())), 0, 100)) / 100;
    c.audioDevice = audio;
    wchar_t gun[32];
    GetPrivateProfileStringW(L"host", L"light_gun", L"touch", gun, 32, ini.c_str());
    c.lightGun = gun;
    wchar_t buttons[32];
    GetPrivateProfileStringW(L"host", L"freeze_buttons", L"position", buttons, 32, ini.c_str());
    c.freezeButtons = buttons;
    wchar_t yt[32];
    GetPrivateProfileStringW(L"host", L"youtube_audio", L"headset", yt, 32, ini.c_str());
    c.youtubeAudio = yt;
    c.minecraft = get(L"minecraft");
    c.curseforge = get(L"curseforge");
    c.modrinth = get(L"modrinth");
    c.prism = get(L"prism");
    c.moviesTile = GetPrivateProfileIntW(L"host", L"movies_tile", 0, ini.c_str()) != 0;
    c.echoInEcho = GetPrivateProfileIntW(L"host", L"echo_in_echo", 0, ini.c_str()) != 0;
    wchar_t look[32];
    GetPrivateProfileStringW(L"minecraft", L"look_speed", L"3", look, 32, ini.c_str());
    c.minecraftLook = float(std::clamp(_wtof(look), 0.5, 20.0));
    c.balatroPort = GetPrivateProfileIntW(L"host", L"balatro_port", c.balatroPort, ini.c_str());
    c.retroCmdPort = GetPrivateProfileIntW(L"host", L"retroarch_command_port", c.retroCmdPort, ini.c_str());
    c.retroPadPort = GetPrivateProfileIntW(L"host", L"retroarch_pad_port", c.retroPadPort, ini.c_str());
    return c;
}

Touch touchFromCell(int cell, bool down, int cols, int rows) {
    int c = cell % cols, r = cell / cols;
    return {cell, down, int((c + .5f) * SCREEN_W / cols), int((r + .5f) * SCREEN_H / rows)};
}

Canvas::Canvas(int width, int height, float scale) : w_(width), h_(height), scale_(scale) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w_;
    bi.bmiHeader.biHeight = -h_;  // top-down, matches the texture
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    dc_ = CreateCompatibleDC(nullptr);
    bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&px_), nullptr, 0);
    old_ = static_cast<HBITMAP>(SelectObject(dc_, bmp_));
    SetBkMode(dc_, TRANSPARENT);
}

Canvas::~Canvas() {
    SelectObject(dc_, old_);
    DeleteObject(bmp_);
    DeleteDC(dc_);
}

Rect Canvas::sc(Rect r) const {
    if (scale_ == 1.f) return r;
    return {int(r.x0 * scale_ + .5f), int(r.y0 * scale_ + .5f), int(r.x1 * scale_ + .5f), int(r.y1 * scale_ + .5f)};
}

Rect Canvas::clip(Rect r) const {
    return {std::max(r.x0, 0), std::max(r.y0, 0), std::min(r.x1, w_), std::min(r.y1, h_)};
}

void Canvas::clear(uint32_t color) { std::fill(px_, px_ + size_t(w_) * h_, color); }

void Canvas::fill(Rect r, uint32_t color) {
    r = clip(sc(r));
    for (int y = r.y0; y < r.y1; y++) std::fill(px_ + size_t(y) * w_ + r.x0, px_ + size_t(y) * w_ + r.x1, color);
}

void Canvas::blend(Rect r, uint32_t color, int a) {
    r = clip(sc(r));
    uint32_t cr = (color >> 16) & 255, cg = (color >> 8) & 255, cb = color & 255;
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            uint32_t& p = px_[size_t(y) * w_ + x];
            uint32_t pr = (p >> 16) & 255, pg = (p >> 8) & 255, pb = p & 255;
            p = rgb((pr * (255 - a) + cr * a) / 255, (pg * (255 - a) + cg * a) / 255, (pb * (255 - a) + cb * a) / 255);
        }
}

void Canvas::frame(Rect r, uint32_t color, int t) {
    fill({r.x0, r.y0, r.x1, r.y0 + t}, color);
    fill({r.x0, r.y1 - t, r.x1, r.y1}, color);
    fill({r.x0, r.y0, r.x0 + t, r.y1}, color);
    fill({r.x1 - t, r.y0, r.x1, r.y1}, color);
}

void Canvas::text(Rect r, const std::wstring& s, int size, uint32_t color, bool bold, int align) {
    r = sc(r);
    size = std::max(1, int(size * scale_ + .5f));
    GdiFlush();
    HFONT font = CreateFontW(-size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ previous = SelectObject(dc_, font);
    SetTextColor(dc_, RGB((color >> 16) & 255, (color >> 8) & 255, color & 255));
    RECT rc{r.x0, r.y0, r.x1, r.y1};
    UINT flags = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX |
                 (align == 0 ? DT_LEFT : align == 2 ? DT_RIGHT : DT_CENTER);
    DrawTextW(dc_, s.c_str(), int(s.size()), &rc, flags);
    SelectObject(dc_, previous);
    DeleteObject(font);
    GdiFlush();
}

void Canvas::blit(const uint8_t* src, int sw, int sh, int pitch, Rect dst) {
    if (!src || sw <= 0 || sh <= 0) return;
    dst = sc(dst);
    // Aspect-fit into dst, letterboxing with black.
    float scale = std::min(float(dst.w()) / sw, float(dst.h()) / sh);
    int w = std::max(1, int(sw * scale)), h = std::max(1, int(sh * scale));
    Rect in{dst.x0 + (dst.w() - w) / 2, dst.y0 + (dst.h() - h) / 2, 0, 0};
    in.x1 = in.x0 + w; in.y1 = in.y0 + h;
    fill({dst.x0, dst.y0, dst.x1, in.y0}, rgb(0, 0, 0));
    fill({dst.x0, in.y1, dst.x1, dst.y1}, rgb(0, 0, 0));
    fill({dst.x0, in.y0, in.x0, in.y1}, rgb(0, 0, 0));
    fill({in.x1, in.y0, dst.x1, in.y1}, rgb(0, 0, 0));
    if (w == sw && h == sh && in.x0 >= 0 && in.x1 <= w_) {
        for (int y = 0; y < h; y++) {
            int ty = in.y0 + y;
            if (ty < 0 || ty >= h_) continue;
            memcpy(px_ + size_t(ty) * w_ + in.x0, src + size_t(y) * pitch, size_t(w) * 4);
        }
        return;
    }
    float fx = float(sw) / w, fy = float(sh) / h;
    for (int y = 0; y < h; y++) {
        int ty = in.y0 + y;
        if (ty < 0 || ty >= h_) continue;
        float syf = std::max(0.f, (y + .5f) * fy - .5f);
        int sy0 = std::min(int(syf), sh - 1), sy1 = std::min(sy0 + 1, sh - 1);
        uint32_t wy = uint32_t((syf - sy0) * 256);
        auto r0 = reinterpret_cast<const uint32_t*>(src + size_t(sy0) * pitch);
        auto r1 = reinterpret_cast<const uint32_t*>(src + size_t(sy1) * pitch);
        uint32_t* out = px_ + size_t(ty) * w_ + in.x0;
        for (int x = 0; x < w; x++) {
            if (in.x0 + x < 0 || in.x0 + x >= w_) continue;
            float sxf = std::max(0.f, (x + .5f) * fx - .5f);
            int sx0 = std::min(int(sxf), sw - 1), sx1 = std::min(sx0 + 1, sw - 1);
            uint32_t wx = uint32_t((sxf - sx0) * 256);
            uint32_t a = r0[sx0], b = r0[sx1], c = r1[sx0], d = r1[sx1];
            uint32_t result = 0xff000000u;
            for (int shift = 0; shift < 24; shift += 8) {
                uint32_t top = (((a >> shift) & 255) * (256 - wx) + ((b >> shift) & 255) * wx) >> 8;
                uint32_t bottom = (((c >> shift) & 255) * (256 - wx) + ((d >> shift) & 255) * wx) >> 8;
                result |= ((top * (256 - wy) + bottom * wy) >> 8) << shift;
            }
            out[x] = result;
        }
    }
}

void Canvas::opaque() {
    GdiFlush();
    for (size_t i = 0; i < size_t(w_) * h_; i++) px_[i] |= 0xff000000u;
}
