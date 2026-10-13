#pragma once
#include "host.h"
#include "media.h"
#include <winsock2.h>
#include <condition_variable>
#include <deque>
#include <thread>

enum class AppId { BalatroSteam, DuckHunt, RetroArch, Doom, Movies, Plex, YouTube, OwnGame, Minecraft, EchoInner };

struct AppInfo {
    AppId id;
    std::wstring title, subtitle, missing;  // missing: why it cannot start (empty = ready)
    uint32_t accent;
    std::wstring core, rom;                 // OwnGame: its RetroArch core and the game file
};

// A way to start Minecraft Java Edition: the official launcher, CurseForge, Modrinth, Prism.
struct Launcher {
    std::wstring name, exe, args;  // exe may be "shell:AppsFolder\..." (explorer starts a Store app)
};
std::vector<Launcher> minecraftLaunchers(const Config& config);
std::vector<AppInfo> listApps(const Config& config);

// Your own games for the launcher's tiles (playlists.cpp): the ones named in roms\tiles.txt,
// else the newest game files in the ROM folders, up to `most`.
struct OwnGame {
    std::wstring title, system, core, rom;
};
std::vector<OwnGame> ownGames(const Config& config, size_t most);
bool isRetro(AppId id);
bool isVideo(AppId id);
bool isDesktop(AppId id);  // a desktop game on the tablet: taps are real clicks, FREEZE is keyboard and mouse (Minecraft, Echo in Echo)

// RetroPad button ids (libretro RETRO_DEVICE_ID_JOYPAD_*).
enum Pad { PadB = 0, PadY, PadSelect, PadStart, PadUp, PadDown, PadLeft, PadRight, PadA, PadX, PadL, PadR, PadL2, PadR2, PadL3, PadR3, PadCount };

class Session {
public:
    Session(const Config& config);
    ~Session();
    // target: video for Movies/Plex. YouTube: playlist = the videos' links, played from playlistStart.
    // target: video for Movies/Plex, the game file for OwnGame (extra = its core), the launcher
    // for Minecraft (extra = its arguments). YouTube: playlist = the videos' links, from playlistStart.
    bool launch(AppId id, const std::wstring& target = L"", double startSeconds = 0,
                const std::vector<std::wstring>& playlist = {}, int playlistStart = 0, const std::wstring& extra = L"");
    void poll();                 // finds and places the window, starts capture
    bool alive() const;
    bool hasWindow() const { return window_ != nullptr; }
    HWND window() const { return window_; }
    AppId id() const { return id_; }
    Rect contentRect() const;    // where the app goes on the tablet screen
    WindowCapture& capture() { return capture_; }
    void quit();                 // polite request, then forced after a grace period
    void kill();
    // CPU for Echo: a desktop game (Minecraft, Echo in Echo) runs at low priority, at normal
    // only while it is played (FREEZE); Minecraft's game is suspended outright (no CPU at
    // all) while the tablet does not show it and nobody plays it.
    void background(bool shown, bool playing);
    uint64_t age() const { return GetTickCount64() - started_; }
    // How long it has had no window: since launch, or since its window went away (RetroArch
    // replaces its window when a core starts).
    uint64_t windowless() const { return window_ ? 0 : GetTickCount64() - lastWindow_; }

    // Input routing (UDP to 127.0.0.1 only).
    void pointer(const char* kind, float u, float v);  // Balatro bridge: "down", "move", "up"
    void pad(Pad button, bool down);
    void command(const char* text);                    // RetroArch network command
    void analog(float x, float y) { analog(0, x, y); }  // left stick, -1..1
    void analog(int stick, float x, float y);          // stick 0 left, 1 right; -1..1, y down
    // RetroArch's GET_STATUS: "PLAYING", "PAUSED", "CONTENTLESS", or "" when it doesn't answer.
    std::string retroStatus();
    bool pauseRetro();                                 // pauses a playing game; true if it did
    void resumeRetro();                                // un-pauses a paused game
    MpvControl& mpv() { return mpv_; }

private:
    void send(int port, const void* data, int size);
    const Config& config_;
    AppId id_ = AppId::BalatroSteam;
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    HWND window_ = nullptr;
    uint64_t started_ = 0, quitAt_ = 0, lastPlace_ = 0, lastWindow_ = 0, lastLook_ = 0;
    bool minecraftAlive_ = false;   // Minecraft: its windows are still up (the launcher process may be gone)
    HWND minecraftGame_ = nullptr;  // the game's own window, once it is up
    std::wstring minecraftName_;    // the launcher chosen (its windows are found by that name)
    // Every process this session put on the tablet (the one it started, a launcher's, the
    // game's): all ended on kill(), and by the watchdog if the host dies first (Echo closing).
    std::vector<DWORD> owned_;
    bool gameSuspended_ = false;     // Minecraft's game process is suspended right now
    DWORD suspendedPid_ = 0;
    int lastPriority_ = -1;          // 1 normal (played), 0 below normal
    void own(DWORD pid);
    bool padState_[PadCount] = {};
    int16_t stick_[2][2] = {};
    MpvControl mpv_;
    SOCKET udp_ = INVALID_SOCKET;
    WindowCapture capture_;
};

// The light gun's own buttons (GunCon A/B, Justifier special, Super Scope pause...): invisible
// overlay buttons in a row along the bottom-left edge of RetroArch's window, pressed by a tap.
enum GunButton { GunAuxA, GunAuxB, GunAuxC, GunStart, GunSelect, GunButtonCount };
inline float gunButtonX(int b) { return .0125f + .025f * b; }  // centre, 0..1 across the window
constexpr float GUN_BUTTON_Y = .985f, GUN_BUTTON_RANGE = .012f;

// Light gun for RetroArch (lightgun.cpp): a shot on the docked poster becomes an aimed trigger
// pull at the same spot of RetroArch's picture. arcade.ini light_gun= touch (default), focus, off.
class LightGun {
public:
    explicit LightGun(const Config& config);
    ~LightGun();
    bool enabled() const { return enabled_; }
    void shoot(HWND window, float u, float v, DWORD holdMs = 100);  // u, v: 0..1 across the window; runs on a worker thread
    // Buttons are held longer than the trigger: some games (Time Crisis) miss a 0.1 s press.
    void press(HWND window, GunButton button) { shoot(window, gunButtonX(button), GUN_BUTTON_Y, 300); }
    void appClosed();                           // park the window and give focus back now

private:
    struct Shot { HWND window; float u, v; DWORD hold; };
    void run();
    void fire(const Shot& shot);
    bool touch(POINT at, DWORD hold);
    bool click(HWND window, POINT at, DWORD hold);
    void raise(HWND window);
    void park();
    void giveFocusBack();

    const bool focusMode_, enabled_, offscreen_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Shot> queue_;
    bool stop_ = false, closed_ = false;
    std::thread worker_;
    // worker thread only
    HWND raised_ = nullptr, previous_ = nullptr;
    POINT cursor_{};
    bool focused_ = false, touchReady_ = false;
    uint64_t lastShot_ = 0;
    int shots_ = 0, covered_ = 0;
};
