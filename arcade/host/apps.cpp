#include "apps.h"
#include "minecraft.h"
#include <ws2tcpip.h>
#include <objbase.h>
#include <cstdio>
#include <thread>
#include <algorithm>

namespace {

bool exists(const std::wstring& path) { return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring folderOf(const std::wstring& file) { return file.substr(0, file.find_last_of(L"\\/")); }

// Window size each app renders at; matches its rectangle on the tablet screen.
constexpr int RETRO_PANEL = 200;
constexpr int RETRO_W = SCREEN_W - 2 * RETRO_PANEL, RETRO_H = SCREEN_H;

struct Found { DWORD pid; HWND hwnd; };
BOOL CALLBACK findWindow(HWND hwnd, LPARAM param) {
    auto f = reinterpret_cast<Found*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != f->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    RECT r;
    GetWindowRect(hwnd, &r);
    if (r.right - r.left < 100 || r.bottom - r.top < 100) return TRUE;
    f->hwnd = hwnd;
    return FALSE;
}

// The light-gun overlay: invisible buttons for the gun's own buttons (see GunButton), plus a
// transparent pixel for an image (RetroArch's d3d11 driver needs one).
void writeGunOverlay(const std::wstring& dir) {
    static const unsigned char PNG[] = {
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
        0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,0x89,0x00,0x00,0x00,0x0d,0x49,0x44,0x41,0x54,0x78,0xda,0x63,0x60,0x60,0x60,0x60,
        0x00,0x00,0x00,0x05,0x00,0x01,0x7a,0xa8,0x57,0x50,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
    FILE* f = nullptr;
    if (!_wfopen_s(&f, (dir + L"\\lightgun_overlay.png").c_str(), L"wb") && f) {
        fwrite(PNG, 1, sizeof(PNG), f);
        fclose(f);
    }
    if (!_wfopen_s(&f, (dir + L"\\lightgun_overlay.cfg").c_str(), L"w") && f) {
        static const char* const NAMES[GunButtonCount] = {"gun_aux_a", "gun_aux_b", "gun_aux_c", "gun_start", "gun_select"};
        fprintf(f, "overlays = 1\noverlay0_overlay = \"lightgun_overlay.png\"\noverlay0_full_screen = true\n"
                   "overlay0_normalized = true\noverlay0_descs = %d\n", int(GunButtonCount));
        for (int b = 0; b < GunButtonCount; b++)
            fprintf(f, "overlay0_desc%d = \"%s,%.4f,%.4f,rect,%.4f,%.4f\"\n", b, NAMES[b], gunButtonX(b), GUN_BUTTON_Y,
                    GUN_BUTTON_RANGE, GUN_BUTTON_RANGE);
        fclose(f);
    }
}

void writeRetroConfig(const Config& c, const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") || !f) return;
    fprintf(f,
        "network_cmd_enable = \"true\"\nnetwork_cmd_port = \"%d\"\n"
        "network_remote_enable = \"true\"\nnetwork_remote_base_port = \"%d\"\nnetwork_remote_enable_user_p1 = \"true\"\n"
        "pause_nonactive = \"false\"\nvideo_fullscreen = \"false\"\nvideo_windowed_fullscreen = \"false\"\n"
        "video_window_save_positions = \"false\"\nui_menubar_enable = \"false\"\nui_companion_start_on_boot = \"false\"\n"
        "video_window_show_decorations = \"false\"\nmenu_mouse_enable = \"false\"\nmenu_pointer_enable = \"false\"\n"
        "input_autodetect_enable = \"true\"\nmenu_show_advanced_settings = \"false\"\n"
        "quit_press_twice = \"false\"\nconfirm_quit = \"false\"\n"
        "menu_swap_ok_cancel_buttons = \"false\"\n"  // A (right) confirms, B (bottom) goes back, as labelled
        "rgui_browser_directory = \"%ls\"\n"
        // apps\retroarch\logs\retroarch.log: why a game did or didn't load
        "log_verbosity = \"true\"\nlog_to_file = \"true\"\nlog_to_file_timestamp = \"false\"\nlog_dir = \"%ls\\logs\"\n"
        "frontend_log_level = \"1\"\nlibretro_log_level = \"1\"\n",
        c.retroCmdPort, c.retroPadPort, c.roms.c_str(), folderOf(c.retroarch).c_str());
    // Light gun (lightgun.cpp): an invisible overlay whose only job is RetroArch's overlay light
    // gun, which aims at a touch and pulls the trigger while it lasts, for every core and port
    // (the "focus" mode's mouse click counts as that touch too). Written out every time, since
    // RetroArch saves these settings into its own retroarch.cfg on exit.
    bool gun = c.lightGun == L"touch" || c.lightGun == L"focus";
    std::wstring overlay = c.dir + L"\\lightgun_overlay.cfg";
    if (gun) writeGunOverlay(c.dir);
    fprintf(f,
        "input_overlay_enable = \"%s\"\ninput_overlay = \"%ls\"\ninput_overlay_opacity = \"0.000000\"\n"
        "input_overlay_pointer_enable = \"%s\"\ninput_overlay_lightgun_trigger_on_touch = \"true\"\n"
        "input_overlay_lightgun_trigger_delay = \"1\"\ninput_overlay_lightgun_port = \"-1\"\n"
        "input_overlay_lightgun_allow_offscreen = \"true\"\ninput_overlay_lightgun_two_touch_input = \"0\"\n"
        "input_overlay_hide_when_gamepad_connected = \"false\"\ninput_overlay_hide_in_menu = \"false\"\n"
        "input_overlay_show_inputs = \"0\"\ninput_overlay_enable_autopreferred = \"false\"\n",
        gun ? "true" : "false", gun ? overlay.c_str() : L"", gun ? "true" : "false");
    fclose(f);
}

}  // namespace

bool isRetro(AppId id) { return id == AppId::RetroArch || id == AppId::Doom || id == AppId::DuckHunt || id == AppId::OwnGame; }

// On a thread of its own: the process's sound to the headset (Windows refuses until it has
// started), then its volume to app_volume once it plays something.
void routeAudioLater(DWORD pid, const AudioTarget& audio, float volume, const char* what) {
    std::thread([pid, audio, volume, what] {
        (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool routed = audio.id.empty();
        for (int attempt = 0; attempt < 20 && !routed; attempt++) {
            Sleep(attempt ? 250 : 50);
            routed = routeProcessAudio(pid, audio);
        }
        if (!audio.id.empty())
            hostLog(routed ? "audio: %s pid %lu -> %ls" : "audio: %s pid %lu: could not route (0x%08lx); it plays on the Windows default", what, pid,
                    routed ? audio.name.c_str() : reinterpret_cast<const wchar_t*>(uintptr_t(lastAudioError)));
        bool set = false;
        for (int attempt = 0; attempt < 120 && !set; attempt++) {  // up to a minute for its first sound
            Sleep(500);
            set = setProcessVolume(pid, audio, volume);
        }
        hostLog(set ? "audio: %s pid %lu volume %.0f%%" : "audio: %s pid %lu: no audio session seen (volume left as is)", what, pid, volume * 100);
        CoUninitialize();
    }).detach();
}

std::vector<Launcher> minecraftLaunchers(const Config& c) {
    std::vector<Launcher> out;
    std::wstring official = minecraft::findLauncher(c.minecraft);
    if (!official.empty()) out.push_back({L"Minecraft Launcher", official, L""});
    wchar_t buf[MAX_PATH];
    auto expand = [&](const wchar_t* s) { return ExpandEnvironmentStringsW(s, buf, MAX_PATH) ? std::wstring(buf) : std::wstring(); };
    // CurseForge: the standalone app, else the Overwolf one (started by its extension id).
    std::wstring curse = minecraft::fileExists(c.curseforge) ? c.curseforge : expand(L"%LOCALAPPDATA%\\Programs\\CurseForge Windows\\CurseForge.exe");
    if (minecraft::fileExists(curse)) out.push_back({L"CurseForge", curse, L""});
    else if (std::wstring ow = expand(L"%ProgramFiles(x86)%\\Overwolf\\OverwolfLauncher.exe");
             minecraft::fileExists(ow) && minecraft::fileExists(expand(L"%LOCALAPPDATA%\\Overwolf\\Extensions\\cchhcaiapeikjbdbpfplgmpobbcdkdaphclbmkbj")))
        out.push_back({L"CurseForge", ow, L"-launchapp cchhcaiapeikjbdbpfplgmpobbcdkdaphclbmkbj"});
    std::wstring modrinth = minecraft::fileExists(c.modrinth) ? c.modrinth : expand(L"%LOCALAPPDATA%\\Modrinth App\\Modrinth App.exe");
    if (!minecraft::fileExists(modrinth)) modrinth = expand(L"%ProgramFiles%\\Modrinth App\\Modrinth App.exe");
    if (minecraft::fileExists(modrinth)) out.push_back({L"Modrinth App", modrinth, L""});
    std::wstring prism = minecraft::fileExists(c.prism) ? c.prism : expand(L"%LOCALAPPDATA%\\Programs\\PrismLauncher\\prismlauncher.exe");
    if (minecraft::fileExists(prism)) out.push_back({L"Prism Launcher", prism, L""});
    return out;
}
bool isVideo(AppId id) { return id == AppId::Movies || id == AppId::Plex || id == AppId::YouTube; }
int writePlaylists(const Config& c);  // playlists.cpp

// Keep secrets (Plex tokens in stream URLs) out of logs.
static std::wstring redact(std::wstring s) {
    for (size_t p = 0; (p = s.find(L"X-Plex-Token=", p)) != std::wstring::npos;) {
        p += 13;
        size_t e = s.find_first_of(L"&\" ", p);
        s.replace(p, (e == std::wstring::npos ? s.size() : e) - p, L"***");
    }
    return s;
}

std::vector<AppInfo> listApps(const Config& c) {
    // Top row: games. Your own games first (up to three, playlists.cpp), then the stock ones
    // that still fit: Balatro, Duck Hunt, DOOM. Bottom row: RetroArch itself, Movies, YouTube.
    std::vector<AppInfo> games, apps;
    std::vector<OwnGame> own = exists(c.retroarch) ? ownGames(c, 3) : std::vector<OwnGame>{};
    static const uint32_t colours[3] = {rgb(222, 64, 64), rgb(64, 132, 230), rgb(200, 120, 40)};
    for (size_t i = 0; i < own.size() && i < 3; i++) {
        AppInfo a{AppId::OwnGame, own[i].title, own[i].system + L" via RetroArch",
                  exists(own[i].core) ? L"" : L"Its RetroArch core is missing - rerun the installer", colours[i]};
        a.core = own[i].core;
        a.rom = own[i].rom;
        games.push_back(a);
    }
    std::wstring balatroMissing = !exists(c.love) ? L"LOVE runtime missing - rerun the installer" :
                                  !exists(c.balatroSteam + L"\\main.lua") ? L"Install Balatro on Steam, then rerun prepare" : L"";
    std::wstring duckMissing = !exists(c.retroarch) ? L"RetroArch missing" :
                               !exists(c.duckHuntCore) ? L"Nestopia core missing" :
                               !exists(c.duckHuntRom) ? L"Put Duck Hunt (World).nes in roms\\nes" : L"";
    std::wstring doomMissing = !exists(c.retroarch) ? L"RetroArch missing" :
                               !exists(c.doomCore) ? L"prboom core missing" : !exists(c.doomWad) ? L"doom1.wad missing" : L"";
    const AppInfo stock[3] = {{AppId::BalatroSteam, L"BALATRO", L"Steam original", balatroMissing, colours[0]},
                              {AppId::DuckHunt, L"DUCK HUNT", L"NES Zapper light gun", duckMissing, colours[1]},
                              {AppId::Doom, L"DOOM", L"Shareware via RetroArch", doomMissing, colours[2]}};
    for (int i = 2; i >= 0 && games.size() < 3; i--) games.insert(games.begin() + int(own.size()), stock[i]);  // DOOM stays longest
    apps.push_back({AppId::RetroArch, L"RETROARCH", L"All cores - full menu",
                    exists(c.retroarch) ? L"" : L"RetroArch missing - rerun the installer", rgb(150, 90, 220)});
    if (c.moviesTile) {
        std::wstring mpvMissing = exists(c.mpv) ? L"" : L"mpv missing - rerun setup_apps.py";
        apps.push_back({AppId::Movies, L"MOVIES", L"Your video folders", mpvMissing, rgb(40, 170, 150)});
    } else {
        std::vector<Launcher> launchers = minecraftLaunchers(c);
        std::wstring sub = launchers.empty() ? L"Java Edition" : launchers.size() == 1 ? launchers[0].name : L"Java: " + std::to_wstring(launchers.size()) + L" launchers";
        apps.push_back({AppId::Minecraft, L"MINECRAFT", sub + L" - FREEZE to play",
                        launchers.empty() ? L"No Minecraft launcher found (Java Edition)" : L"", rgb(92, 160, 64)});
    }
    // Shows the YouTube page open in your browser; with yt-dlp (setup_apps.py) also search and saved links.
    apps.push_back({AppId::YouTube, L"YOUTUBE", exists(c.ytdlp) ? L"Browser YouTube + search" : L"Your browser's YouTube",
                    L"", rgb(255, 48, 64)});
    games.insert(games.end(), apps.begin(), apps.end());
    return games;
}

Session::Session(const Config& config) : config_(config) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    udp_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

Session::~Session() {
    kill();
    if (udp_ != INVALID_SOCKET) closesocket(udp_);
}

Rect Session::contentRect() const {
    if (isRetro(id_)) return {RETRO_PANEL, 0, RETRO_PANEL + RETRO_W, RETRO_H};  // movies and Balatro: full screen
    return {0, 0, SCREEN_W, SCREEN_H};
}

void Session::own(DWORD pid) {
    if (!pid || std::find(owned_.begin(), owned_.end(), pid) != owned_.end()) return;
    owned_.push_back(pid);
    minecraft::writeOwned(owned_);
}

bool Session::launch(AppId id, const std::wstring& target, double startSeconds,
                     const std::vector<std::wstring>& playlist, int playlistStart, const std::wstring& extra) {
    kill();
    id_ = id;
    minecraftAlive_ = false;
    minecraftGame_ = nullptr;
    minecraftName_.clear();
    std::wstring exe, args, cwd;
    AudioTarget audio = findAudioDevice(config_.audioDevice);
    auto setEnv = [](const wchar_t* k, const std::wstring& v) { SetEnvironmentVariableW(k, v.empty() ? nullptr : v.c_str()); };
    setEnv(L"ECHO_ARCADE_PORT", L"");
    if (id == AppId::BalatroSteam) {
        exe = config_.love;
        cwd = config_.balatroSteam;
        // --fused makes LOVE use %APPDATA%\Balatro, i.e. the Steam game's own saves.
        args = L"--fused \"" + cwd + L"\"";
        setEnv(L"ECHO_ARCADE_PORT", std::to_wstring(config_.balatroPort));
        setEnv(L"ECHO_ARCADE_STEAM", L"1");
        setEnv(L"ECHO_ARCADE_WIDTH", std::to_wstring(SCREEN_W));
        setEnv(L"ECHO_ARCADE_HEIGHT", std::to_wstring(SCREEN_H));
    } else if (id == AppId::Minecraft) {
        minecraft::prepareOptions();
        minecraftName_ = playlist.empty() ? L"" : playlist[0];  // the launcher's name rides in the playlist slot
        if (target.rfind(L"shell:", 0) == 0) {  // a Store app: explorer starts it and goes away
            wchar_t sys[MAX_PATH];
            GetSystemDirectoryW(sys, MAX_PATH);
            exe = std::wstring(sys) + L"\\explorer.exe";
            args = target;
        } else {
            exe = target;
            cwd = folderOf(exe);
            args = extra;
        }
        minecraftAlive_ = true;  // until its windows are gone
    } else if (isVideo(id)) {
        exe = config_.mpv;
        cwd = folderOf(exe);
        wchar_t start[32];
        swprintf_s(start, L"%.0f", startSeconds);
        std::wstring files = L"\"" + target + L"\"";
        if (id == AppId::YouTube) {
            // Each video is a playlist entry, so PREV / NEXT walk a mix. mpv asks yt-dlp for the
            // streams; 720p is plenty for the tablet screen and keeps it smooth.
            files.clear();
            for (auto& url : playlist) files += (files.empty() ? L"\"" : L" \"") + url + L"\"";
            files += L" --playlist-start=" + std::to_wstring(std::max(0, playlistStart)) +
                     L" \"--script-opts=ytdl_hook-ytdl_path=" + config_.ytdlp + L"\"" +
                     L" \"--ytdl-format=bestvideo[height<=?720]+bestaudio/best\"";
        }
        args = files + L" --input-ipc-server=" + MpvControl::pipeName() +
               L" --force-window=immediate --no-border --geometry=" + std::to_wstring(SCREEN_W) + L"x" + std::to_wstring(SCREEN_H) +
               L"+0+0 --keep-open=no --idle=no --hwdec=auto-safe --osd-level=1 --title=EchoArcadeMovie --volume=80 --start=" + start;
        if (!audio.id.empty()) args += L" --audio-device=wasapi/" + audio.id;
    } else {
        exe = config_.retroarch;
        cwd = folderOf(exe);
        std::wstring cfg = config_.dir + L"\\retroarch_echo.cfg";
        writeRetroConfig(config_, cfg);
        args = L"--appendconfig \"" + cfg + L"\"";
        if (id == AppId::Doom) args += L" -L \"" + config_.doomCore + L"\" \"" + config_.doomWad + L"\"";
        // Its Nestopia remap (config\remaps\Nestopia\Duck Hunt (World).rmp) plugs the Zapper into port 2.
        else if (id == AppId::DuckHunt) args += L" -L \"" + config_.duckHuntCore + L"\" \"" + config_.duckHuntRom + L"\"";
        else if (id == AppId::OwnGame) args += L" -L \"" + extra + L"\" \"" + target + L"\"";
        else hostLog("playlists: %d game(s) found in %ls", writePlaylists(config_), config_.roms.c_str());
    }
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, cwd.c_str(), &si, &pi)) {
        hostLog("launch failed (%lu): %ls", GetLastError(), cmd.c_str());
        return false;
    }
    // Route its sound to the headset. Windows refuses (E_INVALIDARG) until the process
    // has started, and moves any sound it already plays when the route is set.
    ResumeThread(pi.hThread);
    if (!(id == AppId::Minecraft && target.rfind(L"shell:", 0) == 0)) own(pi.dwProcessId);  // not explorer
    if (audio.id.empty()) hostLog("audio: no device matching \"%ls\"; using the Windows default", config_.audioDevice.c_str());
    routeAudioLater(pi.dwProcessId, audio, config_.appVolume, "app");
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    started_ = lastWindow_ = GetTickCount64();
    quitAt_ = 0;
    std::fill(std::begin(padState_), std::end(padState_), false);
    hostLog("launched pid=%lu: %ls", pid_, redact(cmd).c_str());
    if (isVideo(id)) mpv_.connect();
    return true;
}

bool Session::alive() const {
    if (!process_) return false;
    if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) return true;
    return id_ == AppId::Minecraft && minecraftAlive_;  // the launcher may close while the game runs on
}

void Session::poll() {
    if (!process_) return;
    if (!alive()) {
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        hostLog("app exited with code %lu", code);
        kill();
        return;
    }
    if (quitAt_ && GetTickCount64() - quitAt_ > 4000) { hostLog("app did not quit in time; terminating"); kill(); return; }
    if (window_ && !IsWindow(window_)) { window_ = nullptr; capture_.stop(); lastWindow_ = GetTickCount64(); }
    Rect r = contentRect();
    int x = config_.windowMode == L"offscreen" ? -8000 : 0;
    if (id_ == AppId::Minecraft) {
        // The launcher first, then the game's own window once it is up (another process). Alive
        // while either is: the launcher may close itself once the game runs.
        uint64_t now = GetTickCount64();
        if (now - lastLook_ > 500) {
            lastLook_ = now;
            minecraft::Windows w = minecraft::find(pid_, minecraft::launcherTitle(minecraftName_));
            bool processUp = WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
            minecraftAlive_ = processUp || w.game || w.launcher || now - started_ < 60000;  // Overwolf can take a while to come up
            HWND want = w.game ? w.game : w.launcher;
            if (want && want != window_) {
                if (window_) capture_.stop();
                window_ = want;
                lastPlace_ = now;
                DWORD windowPid = 0;
                GetWindowThreadProcessId(want, &windowPid);
                own(windowPid);
                if (w.game && w.game != minecraftGame_) {
                    minecraftGame_ = w.game;
                    DWORD gamePid = 0;
                    GetWindowThreadProcessId(w.game, &gamePid);
                    hostLog("minecraft: game window %p (pid %lu, class %ls, \"%ls\")", static_cast<void*>(w.game), gamePid, w.gameClass, w.gameTitle);
                    if (gamePid) routeAudioLater(gamePid, findAudioDevice(config_.audioDevice), config_.appVolume, "minecraft");
                }
                HWND hwnd = window_;
                std::thread([hwnd, x, w = r.w(), h = r.h()] {
                    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
                    SetWindowLongW(hwnd, GWL_STYLE, (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MAXIMIZEBOX | WS_MINIMIZEBOX)) | WS_POPUP);
                    SetWindowPos(hwnd, HWND_BOTTOM, x, 0, w, h, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                }).detach();
                capture_.start(window_);
                hostLog("minecraft: showing the %s window %p", w.game ? "game" : "launcher", static_cast<void*>(want));
            }
        }
        if (!minecraftAlive_) { hostLog("minecraft: no window of %ls or the game for a minute; giving up", minecraftName_.c_str()); kill(); }
        return;
    }
    if (window_) {
        // Apps resize themselves (RetroArch does on content load); keep nudging it back.
        uint64_t now = GetTickCount64();
        RECT wr;
        if (!isRetro(id_) && now - lastPlace_ > 1000 && GetWindowRect(window_, &wr) && (wr.right - wr.left != r.w() || wr.bottom - wr.top != r.h())) {
            SetWindowPos(window_, HWND_BOTTOM, x, 0, r.w(), r.h(), SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            lastPlace_ = now;
        }
        return;
    }
    Found f{pid_, nullptr};
    EnumWindows(findWindow, reinterpret_cast<LPARAM>(&f));
    if (!f.hwnd) return;
    window_ = f.hwnd;
    lastPlace_ = GetTickCount64();
    // Borderless, sized to its tablet rectangle, parked where it won't get in the way.
    // Changing another process's window sends it messages synchronously, and a busy
    // app (RetroArch loading a core) would stall us, so do it on a throwaway thread.
    HWND hwnd = window_;
    if (isRetro(id_)) {
        // RetroArch rebuilds its window when a core starts, and restyling it mid-
        // rebuild hangs it. It is already borderless (retroarch_echo.cfg); just park
        // it, asynchronously, and let the capture scale whatever size it picks.
        SetWindowPos(hwnd, HWND_BOTTOM, x, 0, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_ASYNCWINDOWPOS);
        hostLog("window %p found; parking RetroArch window", window_);
        capture_.start(window_);
        return;
    }
    std::thread([hwnd, x, w = r.w(), h = r.h()] {
        LONG style = GetWindowLongW(hwnd, GWL_STYLE);
        SetWindowLongW(hwnd, GWL_STYLE, (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MAXIMIZEBOX | WS_MINIMIZEBOX)) | WS_POPUP);
        LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        SetWindowLongW(hwnd, GWL_EXSTYLE, (ex & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW);
        SetWindowPos(hwnd, HWND_BOTTOM, x, 0, w, h, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }).detach();
    hostLog("window %p found; placing %dx%d (%ls mode)", window_, r.w(), r.h(), config_.windowMode.c_str());
    capture_.start(window_);
}

void Session::quit() {
    if (!process_ || quitAt_) return;
    quitAt_ = GetTickCount64();
    if (isRetro(id_)) command("QUIT");
    else if (id_ == AppId::Minecraft) {
        minecraft::Windows w = minecraft::find(pid_);
        if (w.game) PostMessageW(w.game, WM_CLOSE, 0, 0);
        if (w.launcher) PostMessageW(w.launcher, WM_CLOSE, 0, 0);
    } else if (isVideo(id_)) mpv_.command(R"(["quit"])");
    else {
        const char msg[] = "quit";
        send(config_.balatroPort, msg, int(sizeof(msg) - 1));
    }
}

void Session::kill() {
    mpv_.disconnect();
    capture_.stop();
    window_ = nullptr;
    for (DWORD pid : owned_) minecraft::killTree(pid);  // the app, a launcher, the game: all of it
    owned_.clear();
    minecraft::writeOwned(owned_);
    minecraftGame_ = nullptr;
    minecraftAlive_ = false;
    if (process_) {
        if (alive()) TerminateProcess(process_, 0);
        CloseHandle(process_);
    }
    process_ = nullptr;
    pid_ = 0;
}

void Session::send(int port, const void* data, int size) {
    if (udp_ == INVALID_SOCKET) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(u_short(port));
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(udp_, static_cast<const char*>(data), size, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
}

void Session::pointer(const char* kind, float u, float v) {
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "%s %.4f %.4f", kind, u, v);
    send(config_.balatroPort, msg, n);
}

void Session::command(const char* text) {
    send(config_.retroCmdPort, text, int(strlen(text)));
    hostLog("retroarch command: %s", text);
}

// RetroArch's answer to GET_STATUS ("GET_STATUS PLAYING snes,Game,crc32=..."): its first word.
std::string Session::retroStatus() {
    if (udp_ == INVALID_SOCKET) return "";
    char buf[512];
    u_long waiting = 0;
    while (ioctlsocket(udp_, FIONREAD, &waiting) == 0 && waiting > 0)  // drop anything stale
        if (recv(udp_, buf, sizeof(buf), 0) <= 0) break;
    send(config_.retroCmdPort, "GET_STATUS", 10);
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(udp_, &ready);
    timeval wait{0, 250000};
    if (select(0, &ready, nullptr, nullptr, &wait) <= 0) return "";
    int n = recv(udp_, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return "";
    buf[n] = 0;
    std::string reply(buf);
    if (reply.rfind("GET_STATUS ", 0) != 0) return "";
    reply = reply.substr(11);
    return reply.substr(0, reply.find_first_of(" \r\n"));
}

bool Session::pauseRetro() {
    std::string status = retroStatus();
    hostLog("retroarch status: %s", status.empty() ? "(no answer)" : status.c_str());
    if (status != "PLAYING") return false;
    command("PAUSE_TOGGLE");
    return true;
}

void Session::resumeRetro() {
    std::string status = retroStatus();
    hostLog("retroarch status: %s", status.empty() ? "(no answer)" : status.c_str());
    if (status == "PAUSED") command("PAUSE_TOGGLE");
}

// RetroArch network RetroPad packet (input_remote.c: struct remote_message).
// Stock RetroArch 1.22 reads ONE packet per frame, so only ever send changes: periodic
// refreshes would queue up behind real presses and make input lag by seconds.
// tools/patch_retroarch.py makes it read every packet waiting each frame, so several
// buttons pressed together (FREEZE's controllers) land on the same frame.
struct RemoteMessage { int32_t port, device, index, id; uint16_t state; };

void Session::pad(Pad button, bool down) {
    if (padState_[button] == down) return;
    padState_[button] = down;
    RemoteMessage msg{0, 1 /* RETRO_DEVICE_JOYPAD */, 0, int32_t(button), uint16_t(down)};
    send(config_.retroPadPort, &msg, int(sizeof(msg)));
}

void Session::analog(int stick, float x, float y) {
    stick = std::clamp(stick, 0, 1);
    int16_t v[2] = {int16_t(std::clamp(x, -1.f, 1.f) * 32767), int16_t(std::clamp(y, -1.f, 1.f) * 32767)};
    for (int axis = 0; axis < 2; axis++) {
        if (v[axis] == stick_[stick][axis]) continue;
        stick_[stick][axis] = v[axis];
        // RETRO_DEVICE_ANALOG, index 0 = left stick / 1 = right stick, id 0/1 = x/y.
        RemoteMessage msg{0, 5, stick, axis, uint16_t(v[axis])};
        send(config_.retroPadPort, &msg, int(sizeof(msg)));
    }
}

