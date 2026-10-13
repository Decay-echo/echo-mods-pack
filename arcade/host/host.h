// ArcadeHost: renders the tablet screen (launcher, captured apps, touch controls)
// into shared memory for EchoArcade.dll, and routes tablet touches to the apps.
#pragma once
#include "../native/shared/arcade_ipc.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

constexpr int SCREEN_W = int(arcade::WIDTH), SCREEN_H = int(arcade::HEIGHT);

struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
};

void hostLog(const char* format, ...);

// Headset audio (audio.cpp). patterns: '|'-separated name fragments, best first; "default" = off.
struct AudioTarget { std::wstring id, name; };
AudioTarget findAudioDevice(const std::wstring& patterns);
bool routeProcessAudio(DWORD pid, const AudioTarget& target);  // that process's default output
bool clearProcessAudio(DWORD pid);                              // that choice forgotten: Windows' default device again
float masterVolume();                  // the default output device's volume 0..1 (-1 = could not read)
bool setMasterVolume(float level);
bool masterMuted();
bool toggleMasterMute();
bool setProcessVolume(DWORD pid, const AudioTarget& target, float level);  // its own volume there, 0..1 (once it plays)
extern thread_local long lastAudioError;                        // HRESULT of the last failed route
std::wstring localAppData();  // %LOCALAPPDATA%, even when the variable is missing

// ---- configuration (EchoArcade\arcade.ini, written by the installer) ----
struct Config {
    std::wstring dir;                 // folder containing ArcadeHost.exe
    std::wstring root;                // the Echo Arcade folder it was installed from (tools, .venv)
    std::wstring love;                // love.exe (11.5)
    std::wstring balatroSteam;        // prepared Steam game folder (extracted + bridge)
    std::wstring retroarch;           // retroarch.exe
    std::wstring doomCore, doomWad;   // prboom core + WAD for the DOOM tile
    std::wstring duckHuntCore, duckHuntRom;  // Nestopia core + ROM for the DUCK HUNT tile
    std::wstring roms;                // RetroArch's Load Content start folder
    std::wstring mpv;                 // mpv.exe
    std::wstring ytdlp;               // yt-dlp.exe (YouTube; mpv plays what it finds)
    std::wstring deno;                // deno.exe: the JavaScript runtime yt-dlp needs for YouTube
    std::wstring youtubeLinks;        // your saved YouTube links, one "Name | link" per line
    std::vector<std::wstring> movies; // video folders (arcade.ini movies=, ; separated)
    std::wstring windowMode;          // "desktop" (default) or "offscreen"
    std::wstring audioDevice;         // output device name fragments ('|' separated), "default" = Windows default
    float appVolume = 0.65f;          // each app's own volume on that device (Windows' per-app volume), 0..1
    std::wstring lightGun;            // RetroArch light gun from poster shots: "touch" (default), "focus" or "off"
    std::wstring freezeButtons;       // FREEZE: Touch A/B/X/Y on the RetroPad by "position" (default, like an Xbox pad) or "labels"
    std::wstring youtubeAudio;        // the browser's sound while the tablet shows its YouTube: "headset" (default) or "default" (Windows' device)
    std::wstring minecraft, curseforge, modrinth, prism;  // Minecraft launchers ([paths]; empty = found by itself)
    bool moviesTile = false;          // MOVIES on the launcher instead of MINECRAFT
    bool sharing = true;              // the SPLIT SCREEN / SHARE MY TABLET / WATCH A FRIEND tiles (echo_tweaks.ini [share] tiles)
    bool minecraftSleep = false;      // Minecraft's game suspended outright while hidden (echo_tweaks.ini [cpu] minecraft_sleep); off: idle priority
    bool minecraftMinimize = true;    // Minecraft's window minimized while hidden (echo_tweaks.ini [minecraft] minimize_hidden): 1.21.2+ then draws 10 fps
    bool echoInEcho = false;          // the ECHO VR tile (a second Echo VR in desktop mode); off unless asked for
    float minecraftLook = 3.f;        // FREEZE in Minecraft: mouse pixels per tick at full stick
    int balatroPort = 55410, retroCmdPort = 55355, retroPadPort = 55400;
    int sharePort = 6730;             // SHARE MY TABLET listens here (TCP)
};
Config loadConfig();

// ---- 2D drawing into a BGRA screen (GDI for text): the 1024x574 tablet screen, or the same
// picture at 2x for the high-quality poster screen (every coordinate and size is scaled) ----
class Canvas {
public:
    explicit Canvas(int width = SCREEN_W, int height = SCREEN_H, float scale = 1.f);
    ~Canvas();
    uint32_t* pixels() { return px_; }
    int width() const { return w_; }
    int height() const { return h_; }
    void clear(uint32_t color);
    void fill(Rect r, uint32_t color);
    void blend(Rect r, uint32_t color, int alpha);  // alpha 0..255
    void frame(Rect r, uint32_t color, int thickness);
    void text(Rect r, const std::wstring& s, int size, uint32_t color, bool bold = true, int align = 1);  // 0 left 1 center 2 right
    void blit(const uint8_t* src, int srcW, int srcH, int srcPitch, Rect dst);  // aspect-fit, bilinear
    void opaque();  // force alpha to 255 (GDI clears it; the tablet sprite honours alpha)
private:
    Rect sc(Rect r) const;  // tablet coordinates -> this canvas
    Rect clip(Rect r) const;
    int w_, h_;
    float scale_;
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr, old_ = nullptr;
    uint32_t* px_ = nullptr;
};

constexpr uint32_t rgb(int r, int g, int b) { return 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b); }

// ---- window capture (Windows.Graphics.Capture) ----
class WindowCapture {
public:
    WindowCapture();
    ~WindowCapture();
    bool start(HWND window);
    void stop();
    bool active() const;
    // Copies the newest frame if it changed since `serial`. Returns false if none.
    bool latest(std::vector<uint8_t>& out, int& w, int& h, uint64_t& serial);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- touch model ----
// A tablet touch cell (x, y: cell centre in screen pixels), or a finger on the docked lobby
// poster (cell = POSTER_TOUCH + finger, exact pixel; `move` = it slid while down).
struct Touch { int cell; bool down; int x, y; bool move = false; int side = -1; };  // side: a side poster's number (its touches), else -1
constexpr int POSTER_TOUCH = 100000, SHOT_TOUCH = POSTER_TOUCH + 16;
Touch touchFromCell(int cell, bool down, int cols, int rows);
