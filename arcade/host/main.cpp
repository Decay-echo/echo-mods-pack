// ArcadeHost.exe -- started by EchoArcade.dll when the PLAY tab is first opened.
// Owns the tablet UI (launcher, browsers, touch controls) and the running app;
// exits when Echo VR goes away.
#include "apps.h"
#include "thumbs.h"
#include "share.h"
#include "discord.h"
#include "../native/shared/discord_ipc.h"
#include "minecraft.h"
#include "mod_settings.h"
#include "touchpad.h"
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <ctime>
#include <algorithm>
#include <functional>
#include <future>
#include <map>

namespace {

// ---------------------------------------------------------------- layout
// Cells are 32 x ~32 px, so every control spans at least 2 x 2 cells.
struct PadButton { Pad pad; Rect rect; const wchar_t* label; };
const PadButton PAD[] = {
    {PadL2, {16, 16, 96, 72}, L"L2"}, {PadL, {104, 16, 184, 72}, L"L"},
    {PadUp, {67, 201, 133, 267}, L"\x25B2"}, {PadDown, {67, 333, 133, 399}, L"\x25BC"},
    {PadLeft, {1, 267, 67, 333}, L"\x25C0"}, {PadRight, {133, 267, 199, 333}, L"\x25B6"},
    {PadSelect, {16, 470, 100, 526}, L"SELECT"},
    {PadR, {840, 16, 920, 72}, L"R"}, {PadR2, {928, 16, 1008, 72}, L"R2"},
    {PadX, {891, 201, 957, 267}, L"X"}, {PadB, {891, 333, 957, 399}, L"B"},
    {PadY, {825, 267, 891, 333}, L"Y"}, {PadA, {957, 267, 1023, 333}, L"A"},
    {PadStart, {844, 470, 928, 526}, L"START"},
};
const Rect STICK_AREA{1, 201, 199, 399}, STICK_TOGGLE{16, 412, 184, 458};
const Rect RETRO_HOME{108, 470, 184, 526}, RETRO_MENU{936, 470, 1012, 526};
const Rect RETRO_FREEZE{16, 84, 184, 160};                  // FREEZE on the RetroArch screen
const Rect RETRO_FULL{840, 84, 1008, 160};                  // FULL SCREEN: the pad goes, the picture gets the whole tablet
const Rect BALATRO_HOME{0, 0, 64, 64};
const Rect RESUME_BUTTON{312, 170, 712, 240}, QUIT_BUTTON{312, 252, 712, 322};
const Rect BACK_BUTTON{12, 10, 150, 66};
const Rect DOCK_BUTTON{744, 12, 1008, 64};                  // launcher header
const Rect FREEZE_BUTTON{470, 12, 734, 64};                 // launcher header
const Rect MENU_DOCK_BUTTON{312, 334, 712, 396};            // in-app menu
const Rect MENU_FREEZE_BUTTON{312, 408, 712, 470};          // in-app menu
const Rect MENU_HOME_BUTTON{312, 482, 712, 544};            // in-app menu: the launcher, the app kept running
const Rect SIDE_BIG_BUTTON{640, 500, 1004, 560};            // the cross-play side poster: bring it to the big screen
constexpr int SIDE_LISTEN = 0, SIDE_CROSS = 1, SIDE_DISCORD = 2, SIDE_FRIENDS = 3;  // top-left, top-right, bottom-left, bottom-right
constexpr uint64_t SHOT_PRESS_MS = 90;                      // a shot holds its tap this long
const Rect PAGE_UP{916, 84, 1012, 298}, PAGE_DOWN{916, 306, 1012, 520};
constexpr int ROWS_PER_PAGE = 6, ROW_Y = 84, ROW_H = 72;
const Rect SEEK_BAR{20, 448, 1004, 476};
constexpr int PLAYER_BUTTONS = 10;
const wchar_t* const PLAYER_LABELS[PLAYER_BUTTONS] = {L"-30s", L"-10s", L"PLAY", L"+10s", L"+30s", L"VOL -", L"VOL +", L"SUBS", L"AUDIO", L"CLOSE"};

// YouTube search keyboard: four rows of ten keys, then DEL / SPACE / CLEAR / SEARCH.
const wchar_t* const KEY_ROWS[4] = {L"1234567890", L"QWERTYUIOP", L"ASDFGHJKL'", L"ZXCVBNM,.-"};
const Rect QUERY_BOX{12, 88, 1012, 158};
const Rect KEY_DEL{12, 456, 212, 524}, KEY_SPACE{216, 456, 612, 524}, KEY_CLEAR{616, 456, 812, 524}, KEY_SEARCH{816, 456, 1012, 524};
constexpr size_t QUERY_MAX = 80;

Rect grow(Rect r, int by) { return {r.x0 - by, r.y0 - by, r.x1 + by, r.y1 + by}; }
// YouTube on this PC: the web page (touch it like a tablet), with a toolbar on top while
// the small button at the top-right has it shown. Videos (mpv) keep their controls away the
// same way: the picture gets the whole tablet until you ask.
constexpr int TOOL_BUTTONS = 7;
const wchar_t* const TOOL_LABELS[TOOL_BUTTONS] = {L"\x25C0 BACK", L"HOME", L"SEARCH", L"PREV", L"PAUSE", L"NEXT", L"CLOSE"};
Rect toolButton(int i) { return {6 + i * 136, 4, 6 + i * 136 + 130, 54}; }
const Rect PAGE_AREA{0, 58, SCREEN_W, SCREEN_H};        // under the toolbar
const Rect CONTROLS_BUTTON{962, 6, 1016, 56};           // the small button: controls / toolbar on and off
constexpr int TAP_SLOP = 28;          // tablet pixels a finger may drift and still be a tap
constexpr uint64_t TAP_WAIT_MS = 110;  // a lift this short followed by a touch nearby is one slide
Rect keyRect(int row, int col) { return {12 + col * 100, 170 + row * 70, 12 + col * 100 + 96, 170 + row * 70 + 64}; }
Rect rowRect(int i) { return {12, ROW_Y + i * ROW_H, 904, ROW_Y + i * ROW_H + ROW_H - 6}; }
Rect playerButton(int i) { return {12 + i * 101, 484, 12 + i * 101 + 95, 566}; }

std::wstring clock(double s) {
    if (s < 0) s = 0;
    int t = int(s), h = t / 3600, m = t / 60 % 60, sec = t % 60;
    wchar_t b[32];
    if (h) swprintf_s(b, L"%d:%02d:%02d", h, m, sec); else swprintf_s(b, L"%d:%02d", m, sec);
    return b;
}

enum class Mode { Launcher, Browse, PlexLink, Running, Menu, Keyboard, Screen, Media, Watch, Split };
const Rect WATCH_BACK{12, 6, 236, 52}, WATCH_LEAVE{248, 6, 420, 52};  // WATCH A FRIEND: back to my own tablet (still connected) / disconnect
const Rect SPLIT_STOP{12, 6, 180, 52}, SPLIT_MINE{192, 6, 416, 52};   // SPLIT SCREEN: end it / my own tablet (it goes on)
// The DISCORD page: the bottom row of buttons, the lists, the sign-in button, the call overlay.
const Rect DC_MUTE{20, 486, 250, 552}, DC_DEAF{262, 486, 492, 552}, DC_LEAVE{504, 486, 734, 552}, DC_MSGS{746, 486, 1004, 552};
const Rect DC_CONNECT{312, 300, 712, 380}, DC_SIGNOUT{880, 14, 1010, 62};
const Rect DC_UP{916, 84, 1012, 296}, DC_DOWN{916, 304, 1012, 520};
const Rect CALL_ANSWER{232, 340, 482, 440}, CALL_IGNORE{542, 340, 792, 440};
constexpr int DC_ROWS = 6;
// The MEDIA page: now playing, then play / skip, then volume.
const Rect MEDIA_PREV{40, 230, 320, 330}, MEDIA_PLAY{372, 230, 652, 330}, MEDIA_NEXT{704, 230, 984, 330};
const Rect MEDIA_VOLDOWN{40, 360, 320, 460}, MEDIA_MUTE{372, 360, 652, 460}, MEDIA_VOLUP{704, 360, 984, 460};
const Rect MEDIA_LISTEN{704, 474, 984, 530};  // listening mode: the album art alone
#define canvas_ (*target_)  // the canvas being drawn: the tablet screen, or its 2x copy for the poster

struct Entry {
    std::wstring label, sub;
    std::function<void()> open;
    std::wstring thumb;  // a picture's URL (video lists), drawn at the row's left when it is there
};
struct Screen {
    std::wstring title;
    std::vector<Entry> entries;
    int page = 0;
};

class Host {
public:
    Host(arcade::Shared* shared, bool standalone)
        : shared_(shared), standalone_(standalone), config_(loadConfig()), session_(config_), lightGun_(config_) {
        readPos_ = shared_->touchWrite;
        openDiscordShared();
        ytSetDeno(exists(config_.deno) ? config_.deno : L"");
        if (exists(config_.ytdlp) && !exists(config_.deno)) hostLog("youtube: no Deno (%ls): YouTube searches may fail; rerun the installer", config_.deno.c_str());
        apps_ = listApps(config_);
        loadSettings();
        buildPages();
        shared_->hostPid = LONG(GetCurrentProcessId());
    }

    ~Host() {
        for (auto& f : workers_) f.wait();
    }

    void run() {
        uint64_t lastPublish = 0, seenGame = 0;
        while (true) {
            uint64_t now = GetTickCount64();
            shared_->hostHeartbeat = LONG64(now);
            if (arcade::alive(shared_->gameHeartbeat, now)) seenGame = now;
            if (!standalone_ && now - seenGame > (seenGame ? 15000u : 120000u)) {
                hostLog("Echo VR is gone; shutting down");
                break;
            }
            readTouches();
            runPending();
            session_.poll();
            {
                bool onTablet = (shared_->pageVisible != 0 || standalone_ || shared_->dockState == arcade::Docked) &&
                                (mode_ == Mode::Running || mode_ == Mode::Menu);
                session_.background(onTablet, freeze_);
            }
            if ((mode_ == Mode::Running || mode_ == Mode::Menu) && !session_.alive()) appClosed();
            pollFreeze(now);
            pollMinecraftFocus(now);
            if (mode_ == Mode::Running && session_.windowless() > (session_.id() == AppId::EchoInner ? 120000u : 30000u)) {  // Echo loads for a while
                message_ = L"It did not open a window in 30 s (see host.log).";
                session_.kill();
                appClosed();
            }
            if (mode_ == Mode::PlexLink) pollPlexLink(now);
            reportPlexProgress(now);
            pollScreen(now);
            pollMedia(now);
            pollAds(now);
            pollWatch();
            pollDiscord(now);
            pollBackground();
            share_.audioSource(shareAudioPid());
            pollDock();
            autoDock(now);
            if (shotUpAt_ && now >= shotUpAt_) { shotUpAt_ = 0; onTouch({SHOT_TOUCH, false, shotX_, shotY_}); }
            bool visible = shared_->pageVisible != 0 || standalone_ || shared_->dockState == arcade::Docked;
            if (now - lastPublish >= (visible ? 16u : 200u)) {
                render();
                publish();
                composeSplit();
                share_.offer(share_.split() ? split_.pixels() : screen_.pixels(), SCREEN_W, SCREEN_H);
                offerMyTablet();
                renderSides(now);
                lastPublish = now;
            }
            Sleep(4);
        }
        shared_->controllerWant = 0;  // Echo gets the controllers back
        if (screenWin_) closeScreen("the arcade is closing");  // the browser's sound and window state back as they were
        if (session_.alive()) {
            finishPlex();
            session_.quit();
            Sleep(1500);
        }
        session_.kill();
    }

private:
    // ------------------------------------------------ background work
    // Network and disk work runs on worker threads; results are applied on the UI thread.
    void async(std::function<std::function<void()>()> job, const std::wstring& what) {
        loading_ = what;
        spawn([this, job] {
            auto apply = job();
            std::lock_guard<std::mutex> lock(pendingLock_);
            pending_.push_back(apply);
        });
    }
    void spawn(std::function<void()> work) {
        workers_.erase(std::remove_if(workers_.begin(), workers_.end(), [](auto& f) {
            return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }), workers_.end());
        workers_.push_back(std::async(std::launch::async, std::move(work)));
    }
    void runPending() {
        std::vector<std::function<void()>> ready;
        {
            std::lock_guard<std::mutex> lock(pendingLock_);
            ready.swap(pending_);
        }
        for (auto& f : ready) { loading_.clear(); f(); }
    }

    // ------------------------------------------------ navigation
    void toLauncher() {
        mode_ = Mode::Launcher;
        screens_.clear();
        resetInput();
        apps_ = listApps(config_);
    }
    void resetInput() {
        held_.clear();
        gameCells_.clear();
        pointerDown_ = false;
        pressed_ = -1;
        shotUpAt_ = 0;
        std::fill(std::begin(padHeld_), std::end(padHeld_), false);
    }
    void push(Screen s) {
        screens_.push_back(std::move(s));
        mode_ = Mode::Browse;
        resetInput();
    }
    void back() {
        if (!screens_.empty()) screens_.pop_back();
        if (screens_.empty()) toLauncher();
        resetInput();
    }
    void appClosed() {
        if (freeze_) {
            freeze_ = false;
            shared_->controllerWant = 0;
            hostLog("freeze: off (the app closed)");
        }
        pausedByFreeze_ = false;
        finishPlex();
        lightGun_.appClosed();
        gunUsed_ = false;
        if (message_.empty()) message_ = current_.title + L" closed.";
        if (isVideo(session_.id()) && !screens_.empty()) { mode_ = Mode::Browse; resetInput(); }
        else toLauncher();
    }

    void launch(const AppInfo& app) {
        pressed_ = -1;
        if (!app.missing.empty()) { message_ = app.title + L": " + app.missing; return; }
        message_.clear();
        if (background_ && session_.alive() && sameApp(app, backgroundApp_)) {  // back to the running app
            background_ = false;
            current_ = backgroundApp_;
            mode_ = Mode::Running;
            resetInput();
            return;
        }
        current_ = app;
        if (app.id == AppId::Movies) { openMovies(); return; }
        if (app.id == AppId::Plex) { openPlex(); return; }
        if (app.id == AppId::YouTube) { openYouTube(); return; }
        if (app.id == AppId::Media) { openMedia(); return; }
        if (app.id == AppId::Share) { shareTapped(); return; }
        if (app.id == AppId::Watch) { openWatch(); return; }
        if (app.id == AppId::Split) { splitTapped(); return; }
        if (app.id == AppId::Minecraft) { openMinecraft(); return; }
        if (!session_.launch(app.id, app.rom, 0, {}, 0, app.core)) { message_ = L"Could not start " + app.title + L" (see host.log)."; return; }
        gunUsed_ = false;
        padHidden_ = false;
        mode_ = Mode::Running;
        resetInput();
    }

    void play(const std::wstring& target, const std::wstring& title, double start = 0) {
        message_.clear();
        if (!session_.launch(current_.id, target, start)) { message_ = L"Could not start mpv (see host.log)."; return; }
        playing_ = title;
        mode_ = Mode::Running;
        controlsUntil_ = 0;
        resetInput();
    }

    // ------------------------------------------------ Minecraft (Java Edition)
    // One launcher starts at once; several (CurseForge, Modrinth, Prism) are offered first.
    void openMinecraft() {
        std::vector<Launcher> launchers = minecraftLaunchers(config_);
        if (launchers.empty()) { message_ = L"No Minecraft launcher found: install Java Edition (or CurseForge / Modrinth / Prism)."; return; }
        if (launchers.size() == 1) { startMinecraft(launchers[0]); return; }
        Screen s{L"MINECRAFT"};
        for (auto& l : launchers) s.entries.push_back({l.name, l.exe.rfind(L"shell:", 0) == 0 ? L"Microsoft Store" : l.exe, [this, l] { startMinecraft(l); }});
        push(s);
    }
    void startMinecraft(const Launcher& l) {
        if (!session_.launch(AppId::Minecraft, l.exe, 0, {l.name}, 0, l.args)) { message_ = L"Could not start " + l.name + L" (see host.log)."; return; }
        hostLog("minecraft: starting %ls%s", l.name.c_str(), freeze_ ? " (frozen already: it gets the controllers when its window is up)" : "");
        if (freeze_) { if (!echoWindow_) echoWindow_ = GetForegroundWindow(); lastRefocus_ = 0; }
        gunUsed_ = false;
        padHidden_ = false;
        mode_ = Mode::Running;
        screens_.clear();
        resetInput();
        message_ = L"Pick a profile and PLAY (tap the tablet), then FREEZE: your controllers play Minecraft.";
    }
    // A tap on the picture clicks there in the launcher or the game's menus (posted: no focus).
    // Minecraft's picture: a tap is a real click there and a slide scrolls (the world and server
    // lists), as real mouse input. Its menus take no posted clicks, so the window gets the focus
    // for the click and Echo gets it back a moment later (not while FREEZE holds it on Minecraft).
    void minecraftTouch(const Touch& t) {
        if (BALATRO_HOME.contains(t.x, t.y)) { if (!t.down) openMenu(); return; }
        if (!session_.hasWindow()) { if (!t.down) pressed_ = -1; return; }
        float u, v;
        if (!toApp(t.x, t.y, u, v)) { if (!t.down) mcFinger_ = false; return; }
        HWND window = session_.window();
        RECT client{};
        GetClientRect(window, &client);
        POINT at{LONG(u * client.right), LONG(v * client.bottom)};
        ClientToScreen(window, &at);
        if (t.down) {
            if (!mcFinger_) {
                mcFinger_ = true;
                mcScrolled_ = false;
                mcX0_ = mcX_ = t.x;
                mcY0_ = mcY_ = t.y;
                mcAt_ = at;
                return;
            }
            int dy = t.y - mcY_;
            mcX_ = t.x;
            mcY_ = t.y;
            if (!mcScrolled_ && std::abs(t.x - mcX0_) + std::abs(t.y - mcY0_) < TAP_SLOP) {
                // The same finger covering more cells: the click goes to the middle of all of them.
                int sx = 0, sy = 0, n = 0;
                for (auto& [cell, h] : held_)
                    if (fit_.contains(h.x, h.y)) { sx += h.x; sy += h.y; n++; }
                if (n && toApp(sx / n, sy / n, u, v)) {
                    mcAt_ = {LONG(u * client.right), LONG(v * client.bottom)};
                    ClientToScreen(window, &mcAt_);
                }
                return;
            }
            mcScrolled_ = true;
            if (dy) mcMouse(window, at, dy * 120 / 80);  // the list follows the finger: 80 tablet px a notch
            return;
        }
        for (auto& [cell, h] : held_)
            if (fit_.contains(h.x, h.y)) return;  // another cell of the same finger is still down
        if (!mcFinger_) return;
        mcFinger_ = false;
        if (!mcScrolled_) mcMouse(window, mcAt_, 0);
    }
    // Real mouse input into Minecraft's window at a screen point: a click (wheel 0) or a wheel
    // turn. The focus goes to Minecraft for it when it isn't there, and back to Echo soon after.
    void mcMouse(HWND window, POINT at, int wheel) {
        bool hadFocus = GetForegroundWindow() == window;
        if (!hadFocus) {
            if (!freeze_ && !echoWindow_) echoWindow_ = GetForegroundWindow();
            if (!minecraft::focus(window)) { message_ = L"Minecraft's window would not come to the front."; return; }
            Sleep(60);
        }
        SetCursorPos(at.x, at.y);
        Sleep(20);
        INPUT in{};
        in.type = INPUT_MOUSE;
        if (wheel) {
            in.mi.dwFlags = MOUSEEVENTF_WHEEL;
            in.mi.mouseData = DWORD(wheel);
            SendInput(1, &in, sizeof in);
        } else {
            in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            SendInput(1, &in, sizeof in);
            Sleep(40);
            in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(1, &in, sizeof in);
        }
        if (!freeze_) mcFocusBackAt_ = GetTickCount64() + 300;  // after the last tap or slide
    }
    void pollMinecraftFocus(uint64_t now) {
        if (!mcFocusBackAt_ || now < mcFocusBackAt_) return;
        mcFocusBackAt_ = 0;
        if (freeze_) return;  // FREEZE keeps the focus on Minecraft
        HWND echo = echoWindow();
        if (echo && GetForegroundWindow() != echo && !minecraft::focus(echo)) hostLog("minecraft: could not give Echo VR the focus back after a tap");
    }
    HWND echoWindow() { return echoWindow_ && IsWindow(echoWindow_) ? echoWindow_ : minecraft::mainWindowOf(DWORD(shared_->gamePid)); }

    // ------------------------------------------------ movies
    void openMovies() {
        std::vector<std::wstring> folders;
        for (auto& f : config_.movies) if (GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES) folders.push_back(f);
        if (folders.empty()) { message_ = L"No movie folders found - see movies= in arcade.ini"; return; }
        if (folders.size() == 1) { openFolder(folders[0], L"MOVIES"); return; }
        Screen s{L"MOVIES"};
        for (auto& f : folders) s.entries.push_back({f.substr(f.find_last_of(L"\\/") + 1), f, [this, f] { openFolder(f, f.substr(f.find_last_of(L"\\/") + 1)); }});
        push(s);
    }
    void openFolder(const std::wstring& folder, const std::wstring& title) {
        async([this, folder, title] {
            auto files = listVideos(folder);
            return std::function<void()>([this, files, title] {
                Screen s{title};
                for (auto& f : files) {
                    if (f.folder) s.entries.push_back({L"\x25B8  " + f.name, L"folder", [this, f] { openFolder(f.path, f.name); }});
                    else s.entries.push_back({f.name, L"", [this, f] { play(f.path, f.name); }});
                }
                if (s.entries.empty()) s.entries.push_back({L"No videos here", L"Add files to this folder, then reopen", nullptr});
                push(s);
            });
        }, L"Reading " + title + L"...");
    }

    // ------------------------------------------------ plex
    void openPlex() {
        if (!plex_.linked()) {
            async([this] {
                std::wstring code;
                std::string pin;
                bool ok = plex_.startLink(code, pin);
                return std::function<void()>([this, ok, code, pin] {
                    if (!ok) { message_ = L"Could not reach plex.tv"; return; }
                    linkCode_ = code; linkPin_ = pin; linkStarted_ = GetTickCount64();
                    mode_ = Mode::PlexLink;
                    resetInput();
                });
            }, L"Contacting plex.tv...");
            return;
        }
        async([this] {
            std::wstring error;
            bool ok = plex_.serverReady() || plex_.findServer(error);
            auto libs = ok ? plex_.libraries() : std::vector<PlexItem>{};
            return std::function<void()>([this, ok, error, libs] {
                if (!ok) { message_ = L"Plex: " + error; return; }
                Screen s{L"PLEX - " + plex_.serverName()};
                for (auto& l : libs) s.entries.push_back({l.title, l.subtitle, [this, l] { openPlexFolder(l); }});
                s.entries.push_back({L"Unlink Plex", L"Sign this tablet out of your Plex account", [this] { plex_.unlink(); toLauncher(); message_ = L"Plex unlinked."; }});
                push(s);
            });
        }, L"Finding your Plex server...");
    }
    void openPlexFolder(const PlexItem& folder) {
        async([this, folder] {
            auto items = plex_.children(folder.key);
            return std::function<void()>([this, folder, items] {
                Screen s{folder.title};
                for (auto& it : items) {
                    if (it.playable()) s.entries.push_back({it.title, it.subtitle, [this, it] { playPlex(it); }});
                    else s.entries.push_back({L"\x25B8  " + it.title, it.subtitle, [this, it] { openPlexFolder(it); }});
                }
                if (s.entries.empty()) s.entries.push_back({L"Nothing here", L"", nullptr});
                push(s);
            });
        }, L"Loading " + folder.title + L"...");
    }
    void playPlex(const PlexItem& item) {
        plexItem_ = item;
        plexPlaying_ = true;
        lastTimeline_ = 0;
        play(plex_.streamUrl(item), item.title, item.resumeSeconds > 30 ? item.resumeSeconds : 0);
    }
    void pollPlexLink(uint64_t now) {
        if (!loading_.empty() || now - lastLinkPoll_ < 2000) return;
        lastLinkPoll_ = now;
        if (now - linkStarted_ > 10 * 60 * 1000) { message_ = L"The Plex code expired."; toLauncher(); return; }
        std::string pin = linkPin_;
        spawn([this, pin] {
            bool linked = plex_.pollLink(pin);
            std::lock_guard<std::mutex> lock(pendingLock_);
            if (linked) pending_.push_back([this] { if (mode_ == Mode::PlexLink) { mode_ = Mode::Launcher; openPlex(); } });
        });
    }
    // Keep Plex's resume point and watched state in sync.
    void reportPlexProgress(uint64_t now) {
        if (!plexPlaying_ || mode_ != Mode::Running || now - lastTimeline_ < 10000 || !session_.mpv().connected()) return;
        lastTimeline_ = now;
        auto item = plexItem_;
        double pos = session_.mpv().position();
        bool paused = session_.mpv().paused();
        spawn([this, item, pos, paused] { plex_.timeline(item, paused ? "paused" : "playing", pos); });
    }
    void finishPlex() {
        if (!plexPlaying_) return;
        plexPlaying_ = false;
        auto item = plexItem_;
        double pos = session_.mpv().position();
        spawn([this, item, pos] { plex_.timeline(item, "stopped", pos); });
    }

    // ------------------------------------------------ youtube
    // The YOUTUBE tile: search (on-screen keyboard), your saved links (youtube.txt; a playlist or
    // mix opens as a list), and a yt-dlp update for when YouTube changes and videos stop playing.
    void openYouTube() {
        if (GetFileAttributesW(config_.ytdlp.c_str()) == INVALID_FILE_ATTRIBUTES) { openScreen(); return; }
        Screen s{L"YOUTUBE"};
        if (ytSignedIn()) {
            s.entries.push_back({L"HOME", L"Your YouTube feed (signed in)", [this] { openYouTubeHome(); }});
        } else {
            s.entries.push_back({L"SIGN IN TO YOUTUBE", L"A code appears here; you enter it on your phone (google.com/device). Your feed after that", [this] { ytSignInStart(); }});
            s.entries.push_back({L"CONTINUE AS GUEST", exists(youTubeCookies()) ? L"HOME from youtube_cookies.txt" : L"HOME from the topics in youtube_home.txt",
                                 [this] { openYouTubeHome(); }});
        }
        s.entries.push_back({L"SEARCH YOUTUBE", L"Type with the on-screen keyboard", [this] { mode_ = Mode::Keyboard; resetInput(); }});
        for (auto& link : loadYouTubeLinks(config_.youtubeLinks)) {
            if (link.playlist) s.entries.push_back({L"\x25B8  " + link.name, L"Playlist / mix - tap to see the videos", [this, link] { openYouTubeList(link); }});
            else s.entries.push_back({link.name, L"Video", [this, link] { playYouTube({link.url}, 0, link.name); }});
        }
        s.entries.push_back({L"YOUTUBE ON THIS PC", L"The YouTube page open in your browser, with touch controls", [this] { openScreen(); }});
        s.entries.push_back({L"Update yt-dlp", L"Tap this if videos stop playing (YouTube changes often)", [this] { updateYtdlp(); }});
        if (ytSignedIn()) s.entries.push_back({L"Sign out of YouTube", L"Forgets the sign-in on this PC (HOME goes back to guest)", [this] { ytSignOut(); }});
        std::wstring file = config_.youtubeLinks;  // shown as its folder and name, e.g. apps\youtube.txt
        size_t cut = file.find_last_of(L"\\/");
        if (cut != std::wstring::npos && cut > 0) cut = file.find_last_of(L"\\/", cut - 1);
        if (cut != std::wstring::npos) file = file.substr(cut + 1);
        s.entries.push_back({L"Add your own links", L"On the PC, edit " + file + L"  -  one per line:  Name | link", nullptr});
        push(s);
    }
    std::wstring youTubeFolder() const { return parent(config_.youtubeLinks); }
    std::wstring youTubeCookies() const { return youTubeFolder() + L"\\youtube_cookies.txt"; }
    // HOME: the signed-in feed when a cookies file is there, else a page built from searches for the
    // topics in youtube_home.txt (one a line; a few topics by default).
    void openYouTubeHome() {
        std::wstring ytdlp = config_.ytdlp, cookies = youTubeCookies();
        std::vector<std::wstring> topics;
        std::string text;
        if (readFile(youTubeFolder() + L"\\youtube_home.txt", text)) {
            std::wstring all = widenUtf8(text), line;
            for (size_t i = 0; i <= all.size(); i++) {
                if (i < all.size() && all[i] != L'\n') { line += all[i]; continue; }
                while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
                if (!line.empty() && line[0] != L'#') topics.push_back(line);
                line.clear();
            }
        }
        if (topics.empty()) topics = {L"music", L"gaming", L"echo vr"};
        bool signedIn = exists(cookies);
        if (ytSignedIn()) {
            std::wstring dump = localAppData() + L"\\EchoArcade\\youtube_feed.json";
            async([this, dump] {
                Screen s{L"YOUTUBE HOME"};
                std::wstring error, access = ytAccessToken(error);
                std::vector<YtVideo> found;
                if (access.empty()) s.entries.push_back({L"Could not sign in", error, nullptr});
                else if (ytFeedSignedIn(access, found, error, dump))
                    for (auto& v : found) s.entries.push_back({v.title, v.info, [this, v] { playYouTube({v.url}, 0, v.title); }, v.thumb});
                else s.entries.push_back({L"Could not read your feed", error, nullptr});
                return std::function<void()>([this, s] { push(s); });
            }, L"Loading your YouTube feed");
            return;
        }
        async([this, ytdlp, cookies, topics, signedIn] {
            Screen s{L"YOUTUBE HOME"};
            std::wstring error;
            if (signedIn) {
                std::vector<YtVideo> found;
                if (ytFeed(ytdlp, cookies, found, error))
                    for (auto& v : found) s.entries.push_back({v.title, v.info, [this, v] { playYouTube({v.url}, 0, v.title); }, v.thumb});
                else s.entries.push_back({L"Could not read your feed", error, nullptr});
            } else {
                for (auto& topic : topics) {
                    std::vector<YtVideo> found;
                    if (!ytSearch(ytdlp, topic, found, error, 6)) { s.entries.push_back({topic, L"search failed: " + error, nullptr}); continue; }
                    s.entries.push_back({L"\x25B8  " + topic, L"", nullptr});
                    for (auto& v : found) s.entries.push_back({v.title, v.info, [this, v] { playYouTube({v.url}, 0, v.title); }, v.thumb});
                }
            }
            if (s.entries.empty()) s.entries.push_back({L"Nothing found", error, nullptr});
            return std::function<void()>([this, s] { push(s); });
        }, L"Loading YouTube home");
    }
    // ---- signing in to YouTube without a browser: Google's device code (the TV way). The
    // sign-in lives in apps\youtube_login.json (a refresh token; the access token is renewed).
    std::wstring ytLoginPath() const { return youTubeFolder() + L"\\youtube_login.json"; }
    bool ytSignedIn() const { return exists(ytLoginPath()); }
    void ytSignOut() {
        DeleteFileW(ytLoginPath().c_str());
        message_ = L"Signed out of YouTube on this PC.";
        toLauncher();
        openYouTube();
    }
    static std::wstring jsonEscape(const std::wstring& s) {
        std::wstring out;
        for (wchar_t c : s) { if (c == L'"' || c == L'\\') out += L'\\'; out += c; }
        return out;
    }
    void ytSaveLogin(const std::wstring& refresh, const std::wstring& access, int expiresIn) {
        std::wstring json = L"{\"refresh_token\":\"" + jsonEscape(refresh) + L"\",\"access_token\":\"" + jsonEscape(access) + L"\",\"expires\":" +
                            std::to_wstring(uint64_t(time(nullptr)) + uint64_t(std::max(0, expiresIn))) + L"}";
        std::string bytes = narrowUtf8(json);
        FILE* f = nullptr;
        if (_wfopen_s(&f, ytLoginPath().c_str(), L"wb") == 0 && f) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); }
    }
    // A usable access token (renewed with the refresh token when it ran out); "" with the reason.
    std::wstring ytAccessToken(std::wstring& error) {
        std::string text;
        if (!readFile(ytLoginPath(), text)) { error = L"not signed in"; return L""; }
        std::wstring refresh, access;
        uint64_t expires = 0;
        try {
            auto o = winrt::Windows::Data::Json::JsonObject::Parse(widenUtf8(text));
            refresh = o.GetNamedString(L"refresh_token", L"");
            access = o.GetNamedString(L"access_token", L"");
            expires = uint64_t(o.GetNamedNumber(L"expires", 0));
        } catch (...) { error = L"the sign-in file is unreadable (sign in again)"; return L""; }
        if (!access.empty() && expires > uint64_t(time(nullptr)) + 120) return access;
        int expiresIn = 0;
        if (!ytRefreshToken(refresh, access, expiresIn, error)) return L"";
        ytSaveLogin(refresh, access, expiresIn);
        return access;
    }
    void ytSignInStart() {
        async([this] {
            std::wstring device, code, url, error;
            int interval = 5, expires = 1800;
            bool ok = ytDeviceCode(device, code, url, interval, expires, error);
            return std::function<void()>([this, ok, device, code, url, interval, expires, error] {
                if (!ok) { message_ = L"Could not start the sign-in: " + error; return; }
                Screen s{L"SIGN IN TO YOUTUBE"};
                std::wstring site = url.rfind(L"https://", 0) == 0 ? url.substr(8) : url;
                s.entries.push_back({L"1.  On your phone (or any device), open:   " + site, L"Sign in to Google there if it asks", nullptr});
                s.entries.push_back({L"2.  Enter this code:   " + code, L"Letters and digits, dashes and all", nullptr});
                s.entries.push_back({L"Waiting for you...", L"This page notices by itself once you are done (the code is good for " + std::to_wstring(expires / 60) + L" minutes)", nullptr});
                push(s);
                hostLog("youtube: sign-in started (code %ls at %ls)", code.c_str(), url.c_str());
                ytPollSerial_++;
                uint64_t serial = ytPollSerial_;
                spawn([this, device, interval, expires, serial] {
                    int wait = interval;
                    uint64_t until = GetTickCount64() + uint64_t(expires) * 1000;
                    while (GetTickCount64() < until && serial == ytPollSerial_) {
                        Sleep(DWORD(wait * 1000));
                        if (serial != ytPollSerial_) return;
                        std::wstring refresh, access, err;
                        int expiresIn = 0;
                        int r = ytDevicePoll(device, refresh, access, expiresIn, err);
                        if (r == 2) { wait += 5; continue; }
                        if (r == 0) continue;
                        std::lock_guard<std::mutex> lock(pendingLock_);
                        pending_.push_back([this, r, refresh, access, expiresIn, err, serial] {
                            if (serial != ytPollSerial_) return;
                            if (r == 1) {
                                ytSaveLogin(refresh, access, expiresIn);
                                hostLog("youtube: signed in");
                                message_ = L"Signed in to YouTube. HOME is your own feed now.";
                                toLauncher();
                                openYouTube();
                            } else {
                                hostLog("youtube: sign-in failed: %ls", err.c_str());
                                for (auto& sc : screens_)
                                    if (sc.title == L"SIGN IN TO YOUTUBE" && sc.entries.size() >= 3) { sc.entries[2].label = L"Not signed in: " + err; sc.entries[2].sub = L"BACK, then SIGN IN TO YOUTUBE again for a new code"; }
                            }
                        });
                        return;
                    }
                });
            });
        }, L"Asking Google for a code");
    }
    void openYouTubeList(const YtLink& link) {
        std::wstring ytdlp = config_.ytdlp;
        async([this, link, ytdlp] {
            std::vector<YtVideo> found;
            std::wstring title, error;
            bool ok = ytPlaylist(ytdlp, link.url, title, found, error);
            return std::function<void()>([this, link, ok, found, title, error] {
                if (!ok) { message_ = L"Could not open " + link.name + L": " + error; return; }
                std::vector<std::wstring> urls;
                for (auto& v : found) urls.push_back(v.url);
                Screen s{link.name};
                if (!found.empty())
                    s.entries.push_back({L"\x25B6  PLAY ALL", std::to_wstring(found.size()) + L" videos" + (title.empty() ? L"" : L"   -   " + title),
                                         [this, urls, link] { playYouTube(urls, 0, link.name); }});
                for (size_t i = 0; i < found.size(); i++) {
                    auto v = found[i];
                    s.entries.push_back({v.title, v.info, [this, urls, i, v] { playYouTube(urls, int(i), v.title); }, v.thumb});
                }
                if (found.empty()) s.entries.push_back({L"This playlist is empty", L"", nullptr});
                push(s);
            });
        }, L"Opening " + link.name);
    }
    void searchYouTube() {
        std::wstring q = query_;
        while (!q.empty() && q.back() == L' ') q.pop_back();
        while (!q.empty() && q.front() == L' ') q.erase(q.begin());
        if (q.empty()) { message_ = L"Type something to search for."; return; }
        message_.clear();
        std::wstring ytdlp = config_.ytdlp;
        async([this, q, ytdlp] {
            std::vector<YtVideo> found;
            std::wstring error;
            bool ok = ytSearch(ytdlp, q, found, error);
            return std::function<void()>([this, q, ok, found, error] {
                if (!ok) { message_ = L"YouTube search failed: " + error; return; }
                Screen s{L"\"" + q + L"\""};
                for (auto& v : found) s.entries.push_back({v.title, v.info, [this, v] { playYouTube({v.url}, 0, v.title); }});
                if (s.entries.empty()) s.entries.push_back({L"Nothing found", L"Try other words", nullptr});
                push(s);
            });
        }, L"Searching YouTube");
    }
    void updateYtdlp() {
        std::wstring ytdlp = config_.ytdlp;
        async([this, ytdlp] {
            std::wstring said = ytUpdate(ytdlp);
            return std::function<void()>([this, said] { message_ = L"yt-dlp: " + said; });
        }, L"Updating yt-dlp");
    }
    void playYouTube(const std::vector<std::wstring>& urls, int index, const std::wstring& title) {
        message_.clear();
        if (urls.empty()) return;
        if (!session_.launch(AppId::YouTube, L"", 0, urls, index)) { message_ = L"Could not start mpv (see host.log)."; return; }
        playing_ = title;
        mode_ = Mode::Running;
        controlsUntil_ = 0;
        resetInput();
    }
    // ---- YouTube on this PC: the browser window, captured, used like YouTube on a tablet.
    // Taps and swipes become clicks and scrolling in the page, posted to the browser window: the
    // cursor never moves and Echo VR keeps focus. The browser plays the sound.
    void openScreen() {
        HWND window = findYouTubeWindow();
        if (!window) { message_ = L"Open YouTube in your browser on the PC first, then tap YOUTUBE."; return; }
        // A minimized window gives no picture: show it again, without taking focus from Echo VR.
        if (IsIconic(window)) ShowWindowAsync(window, SW_SHOWNOACTIVATE);
        if (!screenCap_.start(window)) { message_ = L"Could not capture the YouTube window (see host.log)."; return; }
        // Covered by Echo VR's window, Chrome counts itself hidden and leaves the page at its
        // blurry low-resolution rendering, which is what the capture then shows. On top of the
        // desktop (still without focus) it renders the page fully; put back when the tablet
        // lets go of it.
        screenWasTopmost_ = (GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        if (!screenWasTopmost_) SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        screenWin_ = window;
        screenTitle_ = youTubeWindowTitle(window);
        screenSerial_ = 0;
        scrW_ = scrH_ = 0;
        media_ = MediaInfo{};
        docRect_ = RECT{};
        frame_ = RECT{};
        pageShown_ = Rect{};
        fingerDown_ = scrolled_ = keyboardForScreen_ = false;
        tapAt_ = 0;
        lastMediaPoll_ = lastTitlePoll_ = lastDocPoll_ = 0;
        message_.clear();
        mode_ = Mode::Screen;
        resetInput();
        hostLog("youtube: showing window %p \"%ls\"", window, screenTitle_.c_str());
        routeScreenAudio();
    }
    // The browser's sound while the tablet shows its YouTube: to the headset, like the arcade's
    // own apps, or left on Windows' default device (the YouTube sound setting). Windows keeps the
    // choice per app, so it is forgotten again when the tablet lets go of the window.
    void routeScreenAudio() {
        if (!screenWin_) return;
        DWORD pid = 0;
        GetWindowThreadProcessId(screenWin_, &pid);
        if (!pid) return;
        if (config_.youtubeAudio != L"default") {
            AudioTarget audio = findAudioDevice(config_.audioDevice);
            if (audio.id.empty()) {
                hostLog("youtube: no headset device matching \"%ls\"; the browser keeps Windows' default", config_.audioDevice.c_str());
                return;
            }
            bool ok = routeProcessAudio(pid, audio);
            if (ok) screenRoutedPid_ = pid;
            hostLog("youtube: browser (pid %lu) sound -> %ls (%s)", pid, audio.name.c_str(), ok ? "ok" : "could not route");
        } else {
            unrouteScreenAudio();
        }
    }
    void unrouteScreenAudio() {
        if (!screenRoutedPid_) return;
        hostLog("youtube: browser (pid %lu) sound back to Windows' default (%s)", screenRoutedPid_, clearProcessAudio(screenRoutedPid_) ? "ok" : "could not");
        screenRoutedPid_ = 0;
    }
    void closeScreen(const char* why) {
        hostLog("youtube: screen off (%s)", why);
        screenCap_.stop();
        unrouteScreenAudio();
        if (screenWin_ && !screenWasTopmost_ && IsWindow(screenWin_))
            SetWindowPos(screenWin_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        screenWin_ = nullptr;
        keyboardForScreen_ = false;
        tapAt_ = 0;
        mode_ = screens_.empty() ? Mode::Launcher : Mode::Browse;
        resetInput();
    }
    void pollScreen(uint64_t now) {
        if (!screenWin_) return;  // also while its search keyboard is up
        if (!IsWindow(screenWin_)) { message_ = L"The YouTube window closed."; closeScreen("the window is gone"); return; }
        if (now - lastTitlePoll_ > 500) {
            lastTitlePoll_ = now;
            screenTitle_ = youTubeWindowTitle(screenWin_);
        }
        HWND window = screenWin_;
        if (now - lastMediaPoll_ > 1000 && !mediaBusy_) {
            lastMediaPoll_ = now;
            mediaBusy_ = true;
            std::wstring title = screenTitle_;
            spawn([this, title] {
                MediaInfo info = mediaInfo(title);
                std::lock_guard<std::mutex> lock(pendingLock_);
                pending_.push_back([this, info] { media_ = info; mediaBusy_ = false; });
            });
        }
        // Where the page is inside the window (it moves with full screen, zoom and resizing).
        if (now - lastDocPoll_ > 1500 && !docBusy_) {
            lastDocPoll_ = now;
            docBusy_ = true;
            spawn([this, window] {
                RECT r{};
                bool ok = youTubePageArea(window, r);
                std::lock_guard<std::mutex> lock(pendingLock_);
                pending_.push_back([this, ok, r] { docRect_ = ok ? r : RECT{}; docBusy_ = false; });
            });
        }
        // Ads: press YouTube's own Skip button as soon as it is offered (unskippable ads play out).
        if (now - lastAdCheck_ > 1200 && !adBusy_) {
            lastAdCheck_ = now;
            adBusy_ = true;
            spawn([this, window] {
                bool skipped = skipYouTubeAd(window);
                std::lock_guard<std::mutex> lock(pendingLock_);
                pending_.push_back([this, skipped] {
                    adBusy_ = false;
                    if (!skipped) return;
                    message_ = L"Ad skipped";
                    adToastAt_ = GetTickCount64();
                    lastMediaPoll_ = 0;
                });
            });
        }
        if (adToastAt_ && now - adToastAt_ > 3000) {
            if (message_ == L"Ad skipped") message_.clear();
            adToastAt_ = 0;
        }
        // A tap fires once it is clear the finger is not sliding.
        if (tapAt_ && now >= tapAt_) {
            tapAt_ = 0;
            POINT at;
            if (mode_ == Mode::Screen && tabletToScreen(fingerX0_, fingerY0_, at)) {
                hostLog("youtube: tap %d,%d -> screen %ld,%ld (window frame %ld,%ld-%ld,%ld, page %ld,%ld-%ld,%ld, capture %dx%d crop %d,%d %dx%d, shown %d,%d-%d,%d)",
                        fingerX0_, fingerY0_, at.x, at.y, frame_.left, frame_.top, frame_.right, frame_.bottom,
                        docRect_.left, docRect_.top, docRect_.right, docRect_.bottom, scrW_, scrH_, cropX_, cropY_, cropW_, cropH_,
                        pageShown_.x0, pageShown_.y0, pageShown_.x1, pageShown_.y1);
                pageTap(window, at);
            }
        }
    }
    // Ads skipped in the browser's YouTube whether the tablet shows it or not (the Skip button is
    // pressed as soon as YouTube offers it); SETTINGS > TABLET turns it off.
    void pollAds(uint64_t now) {
        if (!skipAds_ || mode_ == Mode::Screen || adBusy_ || now - lastAdCheck_ < 1200) return;  // the screen does its own
        lastAdCheck_ = now;
        HWND window = findYouTubeWindow();
        if (!window) return;
        adBusy_ = true;
        spawn([this, window] {
            bool skipped = skipYouTubeAd(window);
            std::lock_guard<std::mutex> lock(pendingLock_);
            pending_.push_back([this, skipped] { adBusy_ = false; if (skipped) hostLog("youtube: ad skipped in the browser (in the background)"); });
        });
    }
    void screenCommand(MediaCommand command, double to = 0) {
        std::wstring title = screenTitle_;
        spawn([this, command, title, to] {
            bool ok = mediaCommand(command, title, to);
            std::lock_guard<std::mutex> lock(pendingLock_);
            pending_.push_back([this, ok, command] {
                if (!ok && command == MediaCommand::Seek) message_ = L"Your browser does not let Windows seek this video.";
                lastMediaPoll_ = 0;  // show the change right away
            });
        });
    }
    // A point on the tablet's page picture -> the same point of the page on the PC's screen.
    bool tabletToScreen(int x, int y, POINT& out) const {
        if (!pageShown_.contains(x, y) || frame_.right <= frame_.left || scrW_ <= 0 || scrH_ <= 0) return false;
        double cx = cropX_ + (x - pageShown_.x0 + .5) * cropW_ / pageShown_.w();
        double cy = cropY_ + (y - pageShown_.y0 + .5) * cropH_ / pageShown_.h();
        out.x = frame_.left + LONG(cx * (frame_.right - frame_.left) / scrW_);
        out.y = frame_.top + LONG(cy * (frame_.bottom - frame_.top) / scrH_);
        return true;
    }
    double screenPerTablet() const {
        if (pageShown_.w() <= 0 || scrW_ <= 0) return 1;
        return double(cropW_) / pageShown_.w() * double(frame_.right - frame_.left) / scrW_;
    }
    void toolPressed(int i) {
        HWND window = screenWin_;
        switch (i) {
            case 0: spawn([window] { browserBack(window); }); break;
            case 1:
                spawn([this, window] {
                    bool ok = youTubeHome(window);
                    std::lock_guard<std::mutex> lock(pendingLock_);
                    if (!ok) pending_.push_back([this] { message_ = L"YouTube's home button is not on the page (leave full screen first)."; });
                });
                break;
            case 2: keyboardForScreen_ = true; mode_ = Mode::Keyboard; resetInput(); break;
            case 3: screenCommand(MediaCommand::Previous); break;
            case 4: screenCommand(MediaCommand::PlayPause); break;
            case 5: screenCommand(MediaCommand::Next); break;
            case 6: closeScreen("CLOSE tapped"); break;
        }
    }
    bool toolbarShown() const { return toolbar_; }
    Rect pageArea() const { return toolbar_ ? PAGE_AREA : Rect{0, 0, SCREEN_W, SCREEN_H}; }
    // The small button at the top-right: dim while the controls are away, lit while they're up.
    void controlsButton(bool on, bool paused = false) {
        canvas_.blend(CONTROLS_BUTTON, rgb(0, 0, 0), on ? 60 : 120);
        canvas_.frame(CONTROLS_BUTTON, on ? rgb(235, 235, 240) : rgb(120, 126, 150), 2);
        canvas_.text(CONTROLS_BUTTON, L"\x2261", 30, on ? rgb(255, 255, 255) : rgb(205, 210, 225));
        if (paused) {
            Rect mark{CONTROLS_BUTTON.x0 - 58, CONTROLS_BUTTON.y0, CONTROLS_BUTTON.x0 - 6, CONTROLS_BUTTON.y1};
            canvas_.blend(mark, rgb(0, 0, 0), 120);
            canvas_.text(mark, L"II", 24, rgb(235, 235, 240));
        }
    }
    void screenTouch(const Touch& t) {
        if (tapped(t, 810, CONTROLS_BUTTON)) { toolbar_ = !toolbar_; return; }
        if (CONTROLS_BUTTON.contains(t.x, t.y)) return;
        if (toolbar_) {
            for (int i = 0; i < TOOL_BUTTONS; i++)
                if (tapped(t, 800 + i, toolButton(i))) { toolPressed(i); return; }
            if (t.y < PAGE_AREA.y0) { if (!t.down) pressed_ = -1; return; }
        }
        pageGesture(t);
    }
    // One finger on the page: a tap clicks there; a slide scrolls the page with the finger.
    void pageGesture(const Touch& t) {
        uint64_t now = GetTickCount64();
        POINT at;
        if (t.down) {
            bool sliding = fingerDown_ || (tapAt_ && now < tapAt_ && std::abs(t.x - fingerX_) < 90 && std::abs(t.y - fingerY_) < 90);
            tapAt_ = 0;
            if (!sliding) {
                fingerDown_ = true;
                scrolled_ = false;
                fingerX0_ = fingerX_ = t.x;
                fingerY0_ = fingerY_ = t.y;
                if (tabletToScreen(t.x, t.y, at)) pageHover(screenWin_, at);
                return;
            }
            fingerDown_ = true;
            int dx = t.x - fingerX_, dy = t.y - fingerY_;
            fingerX_ = t.x;
            fingerY_ = t.y;
            if (!scrolled_ && std::abs(fingerX_ - fingerX0_) + std::abs(fingerY_ - fingerY0_) < TAP_SLOP) return;
            scrolled_ = true;
            // The page follows the finger: finger up scrolls down. 120 wheel units scroll ~100 px.
            double k = screenPerTablet() * 1.2;
            if (tabletToScreen(t.x, t.y, at)) pageScroll(screenWin_, at, int(-dx * k), int(dy * k));
            return;
        }
        for (auto& [cell, h] : held_)
            if (pageArea().contains(h.x, h.y)) return;  // another cell of the same finger is still down
        if (!fingerDown_) return;
        fingerDown_ = false;
        if (!scrolled_) tapAt_ = now + TAP_WAIT_MS;
    }
    void screenSearch() {
        std::wstring q = query_;
        while (!q.empty() && q.back() == L' ') q.pop_back();
        while (!q.empty() && q.front() == L' ') q.erase(q.begin());
        if (q.empty()) { message_ = L"Type something to search for."; return; }
        keyboardForScreen_ = false;
        mode_ = Mode::Screen;
        resetInput();
        HWND window = screenWin_;
        spawn([this, window, q] {
            bool ok = youTubeSearch(window, q);
            std::lock_guard<std::mutex> lock(pendingLock_);
            pending_.push_back([this, ok] {
                if (!ok) message_ = L"YouTube's search box is not on the page (leave full screen first).";
            });
        });
    }

    void keyboardTouch(const Touch& t) {
        if (tapped(t, 100, BACK_BUTTON)) {
            keyboardForWatch_ = false;
            if (keyboardForScreen_ && screenWin_) { keyboardForScreen_ = false; mode_ = Mode::Screen; }
            else mode_ = screens_.empty() ? Mode::Launcher : Mode::Browse;
            resetInput();
            return;
        }
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 10; col++)
                if (tapped(t, 700 + row * 10 + col, keyRect(row, col))) {
                    if (query_.size() < QUERY_MAX) query_ += wchar_t(towlower(KEY_ROWS[row][col]));
                    return;
                }
        if (tapped(t, 740, KEY_DEL)) { if (!query_.empty()) query_.pop_back(); return; }
        if (tapped(t, 741, KEY_SPACE)) { if (query_.size() < QUERY_MAX && !query_.empty() && query_.back() != L' ') query_ += L' '; return; }
        if (tapped(t, 742, KEY_CLEAR)) { query_.clear(); return; }
        if (tapped(t, 743, KEY_SEARCH)) {
            if (keyboardForWatch_) startWatch(query_);
            else if (keyboardForScreen_ && screenWin_) screenSearch();
            else searchYouTube();
            return;
        }
        if (!t.down) pressed_ = -1;
    }

    // ------------------------------------------------ touches
    void readTouches() {
        LONG write = shared_->touchWrite;
        if (write - readPos_ > LONG(arcade::TOUCH_RING)) readPos_ = write - LONG(arcade::TOUCH_RING);
        while (readPos_ != write) {
            auto& e = shared_->touches[uint32_t(readPos_) % arcade::TOUCH_RING];
            if (e.seq != uint32_t(readPos_) + 1) break;  // still being written
            MemoryBarrier();
            if (e.kind <= arcade::TouchDown) {
                if (shared_->cols && e.cell < shared_->cols * shared_->rows)
                    onTouch(touchFromCell(int(e.cell), e.kind == arcade::TouchDown, int(shared_->cols), int(shared_->rows)));
            } else {
                int x = std::clamp(int(e.xy & 0xffff), 0, SCREEN_W - 1), y = std::clamp(int(e.xy >> 16), 0, SCREEN_H - 1);
                if (e.kind == arcade::Shot) onShot(x, y);
                else if ((e.cell & 0xff) < 2) onTouch({POSTER_TOUCH + int(e.cell & 0xff), e.kind != arcade::PointUp, x, y, e.kind == arcade::PointMove, int(e.cell >> 8) - 1});
            }
            readPos_++;
        }
    }

    // ------------------------------------------------ dock (lobby poster) and light gun
    // quiet: an AUTO DOCK request, answered without a message on the tablet.
    void requestDock(bool dock, bool quiet = false) {
        shared_->dockWant = dock ? 1 : 0;
        MemoryBarrier();
        dockAsked_ = InterlockedIncrement(&shared_->dockSerial);
        dockAskedAt_ = GetTickCount64();
        quietAsked_ = quiet ? dockAsked_ : 0;
        if (quiet) return;
        message_ = dock ? L"Looking for the nearest lobby poster..." : L"Undocking...";
        hostLog("dock: asked to %s", dock ? "dock" : "undock");
    }
    bool docked() const { return shared_->dockState == arcade::Docked; }
    bool dockPending() const { return dockAsked_ && shared_->dockDone != dockAsked_ && GetTickCount64() - dockAskedAt_ < 6000; }
    void pollDock() {
        LONG done = shared_->dockDone;
        if (done == dockSeen_) return;
        dockSeen_ = done;
        char text[sizeof(shared_->dockText) + 1] = {};
        memcpy(text, const_cast<const char*>(shared_->dockText), sizeof(shared_->dockText));
        std::wstring w;
        for (const char* c = text; *c; c++) w += wchar_t(static_cast<unsigned char>(*c));
        if (!docked()) { held_.erase(POSTER_TOUCH); held_.erase(POSTER_TOUCH + 1); }
        // AUTO DOCK asks every couple of seconds: log only what changed, and keep the tablet quiet.
        if (quietAsked_ && done == quietAsked_) {
            if (w != lastQuietDock_) hostLog("dock (auto): state %ld: %s", shared_->dockState, text);
            lastQuietDock_ = w;
            return;
        }
        if (!w.empty()) message_ = w;
        hostLog("dock: state %ld: %s", shared_->dockState, text);
    }
    // AUTO DOCK: while on, the arcade goes on the lobby poster nearest you, and every poster it
    // has been on stays on (also after a match: the plugin puts it back when the lobby loads).
    void autoDock(uint64_t now) {
        if (shared_->levelSerial != dockLevel_) {  // a new level (an arena, the lobby): the posters are new, ask right away
            dockLevel_ = shared_->levelSerial;
            lastAutoDock_ = 0;
            dockAsked_ = 0;
        }
        if (!autoDock_ || dockPending() || now - lastAutoDock_ < 2000) return;
        lastAutoDock_ = now;
        requestDock(true, true);
    }
    std::wstring dockLabel() const {
        if (!autoDock_) return L"AUTO DOCK: OFF";
        return docked() ? L"AUTO DOCK: ON" : L"AUTO DOCK: ON (no poster)";
    }
    void dockTapped() {
        autoDock_ = !autoDock_;
        WritePrivateProfileStringW(L"dock", L"auto", autoDock_ ? L"1" : L"0", tweaksPath().c_str());
        hostLog("dock: auto dock %s", autoDock_ ? "on" : "off");
        if (autoDock_) {
            lastAutoDock_ = 0;
            message_ = L"Auto dock on: the arcade goes on the posters near you and stays on them.";
        } else {
            requestDock(false);
            message_ = L"Auto dock off: the posters show their own pictures again.";
        }
    }

    // ------------------------------------------------ FREEZE (Touch controllers -> RetroArch)
    // On: you're held still in Echo VR and your Touch controllers play RetroArch (started if it
    // isn't running; a game FREEZE paused carries on). Off (FREEZE again, the menu button held
    // 1 s, a new level, or RetroArch closing): Echo gets the controllers back as if nothing
    // happened, and a game in progress is paused.
    std::wstring freezeLabel() const { return freeze_ ? L"FREEZE: ON" : L"FREEZE: OFF"; }
    void freezeTapped() {
        if (freeze_) stopFreeze(L"Freeze off: you can move again. Your game is paused.");
        else startFreeze();
    }
    bool minecraftChooser() const { return !session_.alive() && current_.id == AppId::Minecraft && mode_ == Mode::Browse; }
    void startFreeze() {
        bool mc = (session_.alive() && isDesktop(session_.id())) || minecraftChooser();
        if (session_.alive() && !isRetro(session_.id()) && !mc) {
            message_ = L"Freeze plays RetroArch, Minecraft or Echo: quit " + current_.title + L" first.";
            return;
        }
        if (!session_.alive() && !mc) {
            auto it = std::find_if(apps_.begin(), apps_.end(), [](const AppInfo& a) { return a.id == AppId::RetroArch; });
            if (it == apps_.end()) { message_ = L"RetroArch isn't set up (tools\\setup_apps.py)."; return; }
            launch(*it);
            if (!session_.alive()) return;  // launch() said why
        } else if (pausedByFreeze_ && isRetro(session_.id())) {
            session_.resumeRetro();
        }
        pausedByFreeze_ = false;
        if (mc) {  // keyboard and mouse go to the window in front: Minecraft's, once it is up
            echoWindow_ = GetForegroundWindow();
            if (session_.alive() && !minecraft::focus(session_.window())) message_ = L"Minecraft's window would not come to the front; tap FREEZE again.";
            lastRefocus_ = GetTickCount64();
        }
        if (!minecraftChooser()) mode_ = Mode::Running;  // on the launcher page: stay, pick one
        resetInput();
        touchPad_.reset();
        padFrame_ = {};
        freezeLevel_ = shared_->levelSerial;
        shared_->controllerLevel = freezeLevel_;
        MemoryBarrier();
        shared_->controllerWant = 1;
        freeze_ = true;
        freezeAt_ = GetTickCount64();
        padHidden_ = true;  // the controllers play: the picture gets the whole tablet
        message_ = minecraftChooser() ? L"Frozen. Pick a launcher: your controllers play Minecraft once it is up."
                 : mc ? L"Frozen: your controllers play Minecraft. Both hands above your head for 5 s (or hold the menu button 1 s) to move again."
                      : L"Frozen: your controllers play RetroArch. Both hands above your head for 5 s (or hold the menu button 1 s) to move again.";
        hostLog("freeze: on (level #%ld)", freezeLevel_);
    }
    void stopFreeze(const std::wstring& why) {
        if (!freeze_) return;
        freeze_ = false;
        shared_->controllerWant = 0;
        padHidden_ = false;
        releaseController();
        if (session_.alive() && isRetro(session_.id()) && session_.pauseRetro()) pausedByFreeze_ = true;
        if (isDesktop(session_.id())) {  // Echo in front again: Minecraft pauses itself on losing the focus
            mcInput_.releaseAll();
            if (!minecraft::focus(echoWindow())) hostLog("freeze: could not give Echo VR the focus back");
        }
        message_ = why;
        hostLog("freeze: off");
    }
    void releaseController() {
        mcInput_.releaseAll();
        for (int p = 0; p < PadCount; p++) session_.pad(Pad(p), false);
        session_.analog(0, 0, 0);
        session_.analog(1, 0, 0);
        touchPad_.reset();
        padFrame_ = {};
        padFed_ = false;
    }
    // Every loop: ends FREEZE when it should, else plays the controllers on the RetroPad.
    void pollFreeze(uint64_t now) {
        if (!freeze_) return;
        if (shared_->levelSerial != freezeLevel_) { stopFreeze(L"New level: freeze is off and your game is paused."); return; }
        bool mc = (session_.alive() && isDesktop(session_.id())) || minecraftChooser();
        if (!mc && (!session_.alive() || !isRetro(session_.id()))) { stopFreeze(L"The game closed: freeze is off."); return; }
        if (!standalone_ && shared_->controllerState == arcade::ControllersReleased) {
            stopFreeze(L"Hands up: freeze is off and your game is paused.");
            return;
        }
        if (!standalone_ && shared_->controllerState == arcade::ControllersOff && now - freezeAt_ > 4000) {
            stopFreeze(L"Freeze couldn't take the controllers (see runtime.log).");
            return;
        }
        if (mode_ != Mode::Running) {  // the HOME menu is up: nothing held meanwhile
            if (padFed_) releaseController();
            return;
        }
        arcade::PadState s{};
        LONG64 tick = 0;
        if (!arcade::readPad(shared_, s, &tick) || now - uint64_t(tick) > 500) s = {};  // stale: let go of everything
        padFrame_ = touchPad_.step(s, now, stickMode_, config_.freezeButtons == L"labels");
        if (mc) {
            // The game window must be in front for keys and mouse to reach it (also after the
            // launcher hands over to the game, a new window).
            if (now - lastRefocus_ > 1000 && session_.hasWindow() && GetForegroundWindow() != session_.window()) {
                lastRefocus_ = now;
                minecraft::focus(session_.window());
            }
            mcInput_.apply(s, padFrame_, config_.minecraftLook * config_.minecraftLookScale, session_.window());
            padFed_ = true;
            if (padFrame_.release) stopFreeze(L"Freeze off: you can move again. Minecraft is paused.");
            return;
        }
        for (int p = 0; p < PadCount; p++) session_.pad(Pad(p), padFrame_.pad[p]);  // sends changes only
        auto steps = [](float v) { return std::round(v * 64) / 64; };               // fewer packets for tiny drifts
        session_.analog(0, steps(padFrame_.left[0]), steps(padFrame_.left[1]));
        session_.analog(1, steps(padFrame_.right[0]), steps(padFrame_.right[1]));
        padFed_ = true;
        if (padFrame_.menuToggle) session_.command("MENU_TOGGLE");
        if (padFrame_.release) stopFreeze(L"Freeze off: you can move again. Your game is paused.");
    }

    // A bullet hit the docked poster. On RetroArch's picture it is a light-gun shot at that
    // spot (lightgun.cpp); anywhere else a short tap exactly where it landed (a click in
    // Balatro, a button on the launcher, menus, player and on-screen RetroPad).
    void onShot(int x, int y) {
        float u, v;
        if (lightGun_.enabled() && mode_ == Mode::Running && isRetro(session_.id()) && session_.hasWindow() &&
            !settingsMode() && fit_.contains(x, y) && toApp(x, y, u, v)) {
            lightGun_.shoot(session_.window(), u, v);
            gunUsed_ = true;
            return;
        }
        if (shotUpAt_) onTouch({SHOT_TOUCH, false, shotX_, shotY_});
        shotX_ = x; shotY_ = y;
        onTouch({SHOT_TOUCH, true, x, y});
        shotUpAt_ = GetTickCount64() + SHOT_PRESS_MS;
    }

    // ------------------------------------------------ SETTINGS tab
    // Every switch of the mods, a page each. The tablet's own live in echo_tweaks.ini beside the
    // host (the plugin reads it when Echo starts; the tablet size also live). The HUD
    // (plugins\EchoHudPlus\settings.ini) and the disc button (plugins\DiscButton\settings.ini)
    // read their file again within a second. The HUD lines are NvrAssetPatches' manifest.json
    // entries, read when Echo starts. NORMAL turns all of it off at once (and back on), and can
    // take Echo Arcade off the tablet when Echo closes (tools\remove_on_exit.py).
    enum class Kind { Slider, Toggle, Choice, Action };
    struct Option {
        std::wstring label, note;
        Kind kind = Kind::Toggle;
        float min = 0, max = 1, step = 1;          // Slider
        const wchar_t* format = L"%.2f";           // Slider value
        std::vector<std::wstring> choices;         // Choice: value = index
        std::function<float()> get;
        std::function<void(float)> set;
        std::function<std::wstring(float)> text;   // Action: the button's label for a value
        float value = 0;
    };
    struct Page { std::wstring name; std::vector<Option> options; std::wstring footer; };
    static constexpr int SETTING_ROWS = 6, SETTING_Y = 148, SETTING_H = 64;
    // A page draws SETTING_ROWS rows; one with more is split into NAME, NAME 2, NAME 3 tabs.
    void addPage(Page p) {
        if (int(p.options.size()) <= SETTING_ROWS) { pages_.push_back(std::move(p)); return; }
        for (size_t at = 0, k = 1; at < p.options.size(); at += SETTING_ROWS, k++) {
            Page part{k == 1 ? p.name : p.name + L" " + std::to_wstring(k)};
            part.footer = p.footer;
            part.options.assign(p.options.begin() + long(at), p.options.begin() + long(std::min(at + size_t(SETTING_ROWS), p.options.size())));
            pages_.push_back(std::move(part));
        }
    }

    std::wstring tweaksPath() const { return config_.dir + L"\\echo_tweaks.ini"; }
    static std::wstring parent(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/")); }
    std::wstring pluginsDir() const { return parent(config_.dir); }
    std::wstring gameDir() const { return parent(parent(parent(pluginsDir()))); }  // plugins < win10 < bin < game
    std::wstring hudIni() const { return pluginsDir() + L"\\EchoHudPlus\\settings.ini"; }
    std::wstring discIni() const { return pluginsDir() + L"\\DiscButton\\settings.ini"; }
    std::wstring manifestPath() const { return parent(pluginsDir()) + L"\\asset_patches\\manifest.json"; }
    static bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

    static std::wstring iniText(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
        wchar_t buf[64];
        GetPrivateProfileStringW(section, key, fallback, buf, 64, file.c_str());
        return buf;
    }
    static float iniFloat(const std::wstring& file, const wchar_t* section, const wchar_t* key, float fallback) {
        std::wstring v = iniText(file, section, key, L"");
        return v.empty() ? fallback : float(_wtof(v.c_str()));
    }
    static void iniWrite(const std::wstring& file, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
        if (!WritePrivateProfileStringW(section, key, value.c_str(), file.c_str()))
            hostLog("settings: could not write [%ls] %ls in %ls (error %lu)", section, key, file.c_str(), GetLastError());
    }
    static std::wstring num(float v, const wchar_t* format = L"%.2f") {
        wchar_t buf[32];
        swprintf(buf, 32, format, v);
        return buf;
    }
    static std::string narrowUtf8(const std::wstring& w) {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }
    static std::wstring widenUtf8(const std::string& s) {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
        if (!w.empty() && w[0] == 0xFEFF) w.erase(0, 1);
        return w;
    }
    static bool readFile(const std::wstring& path, std::string& out) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
        char buf[65536];
        size_t n;
        out.clear();
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        fclose(f);
        return true;
    }

    // HUD lines: -1 not installed, 0 off, 1 on.
    int hudLinesState() const {
        std::string json;
        return readFile(manifestPath(), json) ? mods::hudLinesState(json) : -1;
    }
    bool setHudLines(bool on) {
        std::string json;
        if (!readFile(manifestPath(), json)) return false;
        std::string changed = mods::setHudLines(json, on);
        if (changed == json) return true;
        std::wstring tmp = manifestPath() + L".arcade-tmp";
        FILE* f = nullptr;
        bool ok = _wfopen_s(&f, tmp.c_str(), L"wb") == 0 && f && fwrite(changed.data(), 1, changed.size(), f) == changed.size();
        if (f) fclose(f);
        ok = ok && MoveFileExW(tmp.c_str(), manifestPath().c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!ok) { hostLog("settings: could not write manifest.json (error %lu)", GetLastError()); DeleteFileW(tmp.c_str()); }
        else hostLog("settings: HUD lines %s (from the next start)", on ? "on" : "off");
        return ok;
    }

    static Option toggle(std::wstring label, std::wstring note, std::wstring file, const wchar_t* section, const wchar_t* key, bool fallback) {
        Option o{label, note, Kind::Toggle};
        o.get = [=] { return iniFloat(file, section, key, fallback ? 1.f : 0.f) != 0 ? 1.f : 0.f; };
        o.set = [=](float v) { iniWrite(file, section, key, v != 0 ? L"1" : L"0"); };
        return o;
    }
    static Option slider(std::wstring label, std::wstring note, std::wstring file, const wchar_t* section, const wchar_t* key,
                         float min, float max, float step, float fallback, const wchar_t* format) {
        Option o{label, note, Kind::Slider, min, max, step, format};
        o.get = [=] { return std::clamp(iniFloat(file, section, key, fallback), min, max); };
        o.set = [=](float v) { iniWrite(file, section, key, num(v, step >= 1 ? L"%.0f" : step >= .1f ? L"%.1f" : L"%.2f")); };
        return o;
    }

    void loadSettings() {
        std::wstring t = tweaksPath();
        {
            std::wstring id = iniText(t, L"discord", L"client_id", L""), secret = iniText(t, L"discord", L"client_secret", L"");
            if (id != dClientId_ || secret != dClientSecret_) {
                dClientId_ = id;
                dClientSecret_ = secret;
                discord_.stop();
                discord_.configure(id, secret, localAppData() + L"\\EchoArcade\\discord_login.json");
                discord_.start();
                hostLog("discord: %s", id.empty() ? "no app id in echo_tweaks.ini [discord]" : "app id set; talking to Discord");
            }
        }
        shared_->tabletScale = LONG(std::clamp(iniFloat(t, L"tablet", L"scale", 1.f), .5f, 4.f) * 1000);
        autoDock_ = GetPrivateProfileIntW(L"dock", L"auto", 1, t.c_str()) != 0;  // on unless turned off
        posterHq_ = _wcsicmp(iniText(t, L"poster", L"quality", L"normal").c_str(), L"high") == 0;
        sidesOn_ = GetPrivateProfileIntW(L"sides", L"enabled", 1, t.c_str()) != 0;
        config_.minecraftSleep = GetPrivateProfileIntW(L"cpu", L"minecraft_sleep", 0, t.c_str()) != 0;
        config_.minecraftMinimize = GetPrivateProfileIntW(L"minecraft", L"minimize_hidden", 1, t.c_str()) != 0;
        config_.minecraftLookScale = std::clamp(iniFloat(t, L"minecraft", L"look_speed", 1.f), .25f, 4.f);
        bool sharing = GetPrivateProfileIntW(L"share", L"tiles", 0, t.c_str()) != 0;  // off unless turned on: nobody's tablet is shared by default
        if (sharing != config_.sharing) {
            config_.sharing = sharing;
            if (!sharing) { share_.stop(); watch_.disconnect(); }
            if (mode_ == Mode::Launcher) apps_ = listApps(config_);
        }
        skipAds_ = GetPrivateProfileIntW(L"youtube", L"skip_ads", 1, t.c_str()) != 0;
        session_.setWindowScale(posterHq_ ? 2 : 1);
        mcLight_ = GetPrivateProfileIntW(L"minecraft", L"light", 1, t.c_str()) != 0;
        session_.setMinecraftLight(mcLight_);
        std::wstring buttons = iniText(t, L"freeze", L"buttons", L"");          // the tablet's choice beats arcade.ini
        if (!buttons.empty()) config_.freezeButtons = buttons;
        std::wstring yt = iniText(t, L"youtube", L"audio", L"");
        if (!yt.empty()) config_.youtubeAudio = yt;
        if (removalScheduled()) startRemover(true);  // its helper went away (PC restarted?): again
    }

    void buildPages() {
        pages_.clear();
        std::wstring tweaks = tweaksPath(), hud = hudIni(), disc = discIni();
        bool hasHud = exists(hud);

        // Echo's own voice chat mode, one button each: the game's "voipmode" setting switched
        // live by the plugin (the game keeps it in memory) and written to settings_mp_v2.json
        // so it stays after a restart. The lit button is the mode the game reports.
        {
            Page vp{L"VOICE"};
            static const wchar_t* const labels[4] = {L"MUTED", L"OPEN MIC", L"PUSH TO TALK", L"PUSH TO MUTE"};
            static const wchar_t* const notes[4] = {L"Nobody hears you", L"Your mic is always on", L"Hold the button to talk",
                                                    L"Hold the button to go quiet"};
            for (int m = 0; m < 4; m++) {
                Option o{labels[m], notes[m], Kind::Action};
                o.get = [this, m] { return voiceMode() == m ? 1.f : 0.f; };
                o.set = [this, m](float) { setVoiceMode(m); };
                o.text = [m](float v) { return std::wstring(v != 0 ? L"\x25CF  " : L"") + labels[m]; };
                vp.options.push_back(o);
            }
            vp.footer = L"The lit one is what Echo uses now. Same as Echo's Settings > Audio; it stays after a restart.";
            addPage(vp);
        }

        Page tablet{L"TABLET"};
        Option size = slider(L"Tablet size", L"The hand tablet, bigger or smaller", tweaks, L"tablet", L"scale", .5f, 4.f, .05f, 1.f, L"%.2fx");
        size.set = [this](float v) { setTabletScale(v); };
        tablet.options.push_back(size);
        Option dock{L"Auto dock", L"The arcade goes on the lobby posters near you", Kind::Toggle};
        dock.get = [this] { return autoDock_ ? 1.f : 0.f; };
        dock.set = [this](float v) { if ((v != 0) != autoDock_) dockTapped(); };
        tablet.options.push_back(dock);
        Option quality{L"Poster quality", L"HIGH: the arcade at twice the detail on the posters (more CPU every frame; for a strong PC)", Kind::Choice};
        quality.choices = {L"NORMAL", L"HIGH"};
        quality.get = [this] { return posterHq_ ? 1.f : 0.f; };
        quality.set = [this, tweaks](float v) {
            posterHq_ = v != 0;
            iniWrite(tweaks, L"poster", L"quality", posterHq_ ? L"high" : L"normal");
            session_.setWindowScale(posterHq_ ? 2 : 1);
            message_ = posterHq_ ? L"Poster quality HIGH: from the next dock (undock and dock again); apps started from now on render bigger."
                                 : L"Poster quality NORMAL: from the next dock.";
        };
        tablet.options.push_back(quality);
        Option sharing{L"Sharing tiles", L"Off unless you turn it on: SPLIT SCREEN, SHARE MY TABLET and WATCH A FRIEND on the launcher (they need a port your router opens)", Kind::Toggle};
        sharing.get = [this] { return config_.sharing ? 1.f : 0.f; };
        sharing.set = [this, tweaks](float v) { iniWrite(tweaks, L"share", L"tiles", v != 0 ? L"1" : L"0"); loadSettings(); };
        Option sides{L"Side posters", L"Docked on the lobby's news board: the four posters beside it show listening mode, Discord, and cross play and friends (or the tablet, with the sharing tiles off)", Kind::Toggle};
        sides.get = [this] { return sidesOn_ ? 1.f : 0.f; };
        sides.set = [this, tweaks](float v) { sidesOn_ = v != 0; iniWrite(tweaks, L"sides", L"enabled", sidesOn_ ? L"1" : L"0"); };
        Option hidden{L"Hidden Minecraft", L"Not on the tablet: RUNS ON at low priority; MINIMIZED also minimizes its window (10 fps on 1.21.2+); ASLEEP freezes it (servers drop you)", Kind::Choice};
        hidden.choices = {L"RUNS ON", L"MINIMIZED", L"ASLEEP"};
        hidden.get = [this] { return config_.minecraftSleep ? 2.f : config_.minecraftMinimize ? 1.f : 0.f; };
        hidden.set = [this, tweaks](float v) {
            iniWrite(tweaks, L"cpu", L"minecraft_sleep", v == 2 ? L"1" : L"0");
            iniWrite(tweaks, L"minecraft", L"minimize_hidden", v == 1 ? L"1" : L"0");
            loadSettings();
        };
        tablet.options.push_back(hidden);
        Option light{L"Minecraft light settings", L"When Minecraft starts: render distance 8, simulation 6, no clouds or entity shadows (your own values come back when this is off)", Kind::Toggle};
        light.get = [this] { return mcLight_ ? 1.f : 0.f; };
        light.set = [this, tweaks](float v) {
            mcLight_ = v != 0;
            session_.setMinecraftLight(mcLight_);
            iniWrite(tweaks, L"minecraft", L"light", mcLight_ ? L"1" : L"0");
            message_ = mcLight_ ? L"Minecraft light settings: applied the next time Minecraft starts." : L"Minecraft keeps its own video settings from its next start.";
        };
        tablet.options.push_back(light);
        Option look = slider(L"Minecraft look speed", L"Right stick: turning in the game and the pointer in its menus (fine near the centre, fast at the edge)",
                             tweaks, L"minecraft", L"look_speed", .25f, 4.f, .25f, 1.f, L"%.2fx");
        look.set = [this, tweaks](float v) { iniWrite(tweaks, L"minecraft", L"look_speed", num(v, L"%.2f")); loadSettings(); };
        tablet.options.push_back(look);
        tablet.options.push_back(sharing);  // TABLET 2 from here (six rows a page)
        tablet.options.push_back(sides);
        Option ads{L"Skip YouTube ads", L"In your browser's YouTube, the Skip button is pressed for you as soon as it appears", Kind::Toggle};
        ads.get = [this] { return skipAds_ ? 1.f : 0.f; };
        ads.set = [this, tweaks](float v) { skipAds_ = v != 0; iniWrite(tweaks, L"youtube", L"skip_ads", skipAds_ ? L"1" : L"0"); };
        tablet.options.push_back(ads);
        Option buttons{L"FREEZE buttons", L"Touch A B X Y on the game pad: by place (like an Xbox pad) or by name", Kind::Choice};
        buttons.choices = {L"BY PLACE", L"BY NAME"};
        buttons.get = [this] { return config_.freezeButtons == L"labels" ? 1.f : 0.f; };
        buttons.set = [this, tweaks](float v) {
            config_.freezeButtons = v != 0 ? L"labels" : L"position";
            iniWrite(tweaks, L"freeze", L"buttons", config_.freezeButtons);
        };
        tablet.options.push_back(buttons);
        Option ytAudio{L"YouTube sound", L"Your browser's YouTube on the tablet: through the headset, or Windows' default device", Kind::Choice};
        ytAudio.choices = {L"HEADSET", L"DEFAULT"};
        ytAudio.get = [this] { return config_.youtubeAudio == L"default" ? 1.f : 0.f; };
        ytAudio.set = [this, tweaks](float v) {
            config_.youtubeAudio = v != 0 ? L"default" : L"headset";
            iniWrite(tweaks, L"youtube", L"audio", config_.youtubeAudio);
            routeScreenAudio();
        };
        tablet.options.push_back(ytAudio);
        tablet.footer = L"View (FOV) is set in echo_tweaks.ini.";
        addPage(tablet);

        if (hasHud) {
            Page p{L"HUD"};
            p.options.push_back(toggle(L"HUD", L"Everything at the bottom of your view, and the screen effects", hud, L"general", L"enabled", true));
            p.options.push_back(toggle(L"Speeds", L"Your speed, and your last run's top speed under it", hud, L"speed", L"enabled", true));
            p.options.push_back(slider(L"Speed size", L"How big your current speed is", hud, L"speed", L"current_font_px", 12, 36, 1, 20, L"%.0f"));
            p.options.push_back(toggle(L"Mute + network icons", L"Either side of the speeds", hud, L"thrusters", L"icons", true));
            p.options.push_back(toggle(L"Discord", L"Your call's people above the speed (green ring on whoever talks); a new message's sender with +N at the top for 3 s", hud, L"discord", L"enabled", true));
            Option passes{L"Pass counter", L"Top of your view: +1 a pass, the count from 6, big on a goal", Kind::Choice};
            passes.choices = {L"OFF", L"MY TEAM", L"BOTH"};
            passes.get = [hud] {
                if (iniFloat(hud, L"passes", L"enabled", 1) == 0) return 0.f;
                return _wcsicmp(iniText(hud, L"passes", L"teams", L"both").c_str(), L"mine") == 0 ? 1.f : 2.f;
            };
            passes.set = [hud](float v) {
                iniWrite(hud, L"passes", L"enabled", v != 0 ? L"1" : L"0");
                if (v != 0) iniWrite(hud, L"passes", L"teams", v == 1 ? L"mine" : L"both");
            };
            Option gauges{L"Gauges", L"The old thruster rings, speed rings and 7 s top speed", Kind::Toggle};
            gauges.get = [hud] {
                return iniFloat(hud, L"thrusters", L"rings", 0) != 0 || iniFloat(hud, L"speed", L"ring", 0) != 0 ||
                    iniFloat(hud, L"speed", L"top_speed", 0) != 0 ? 1.f : 0.f;
            };
            gauges.set = [hud](float v) {
                const wchar_t* s = v != 0 ? L"1" : L"0";
                iniWrite(hud, L"thrusters", L"rings", s);
                iniWrite(hud, L"speed", L"ring", s);
                iniWrite(hud, L"speed", L"top_speed", s);
            };
            p.options.push_back(gauges);
            addPage(p);

            Page pp{L"PASSES"};
            pp.options.push_back(passes);
            Option bounces{L"Bounce passes", L"How often the disc may bounce on its way and still be a pass (1: a bounce pass counts)", Kind::Choice};
            bounces.choices = {L"NO BOUNCE", L"1 BOUNCE", L"2 BOUNCES", L"3 BOUNCES"};
            bounces.get = [hud] { return std::clamp(iniFloat(hud, L"passes", L"max_bounces", 1), 0.f, 3.f); };
            bounces.set = [hud](float v) { iniWrite(hud, L"passes", L"max_bounces", num(v, L"%.0f")); };
            pp.options.push_back(bounces);
            pp.options.push_back(slider(L"Pass time limit", L"Seconds the disc may be loose and the catch still be a pass",
                                        hud, L"passes", L"max_seconds", 1, 15, .5f, 5, L"%.1f s"));
            pp.options.push_back(slider(L"Count from", L"The count itself shows from this many passes (\"+1\" before that)",
                                        hud, L"passes", L"count_from", 1, 20, 1, 6, L"%.0f"));
            pp.options.push_back(slider(L"Size", L"The counter's size", hud, L"passes", L"size_px", 10, 60, 2, 26, L"%.0f px"));
            pp.footer = L"A slower catch, or more bounces, is no pass: the chain starts over there.";
            addPage(pp);
        }

        Page screen{L"SCREEN"};
        if (hasHud) {
            screen.options.push_back(toggle(L"Thruster glow", L"Your view's edges glow in your team colour when your hands line up",
                                            hud, L"thrusters", L"screen_glow", true));
            screen.options.push_back(slider(L"Glow brightness", L"At full speed", hud, L"thrusters", L"screen_glow_peak", .1f, 1, .05f, .55f, L"%.2f"));
            Option from = slider(L"Glow starts above", L"m/s: nothing at or under it, full 1.3 faster",
                                 hud, L"thrusters", L"screen_glow_from", 1, 8, .1f, 3.5f, L"%.1f");
            from.set = [hud](float v) {
                iniWrite(hud, L"thrusters", L"screen_glow_from", num(v, L"%.1f"));
                iniWrite(hud, L"thrusters", L"screen_glow_to", num(v + 1.3f, L"%.1f"));
            };
            screen.options.push_back(from);
            screen.options.push_back(toggle(L"Stun flash", L"Green or red when you're stunned (green: your shield was up)", hud, L"stun", L"enabled", true));
        }
        if (hudLinesState() >= 0) {
            Option lines{L"Hide Echo's visor lines", L"With Echo's own speed and icons. From the next time Echo starts", Kind::Toggle};
            lines.get = [this] { return hudLinesState() > 0 ? 1.f : 0.f; };
            lines.set = [this](float v) { setHudLines(v != 0); };
            screen.options.push_back(lines);
        }
        if (!screen.options.empty()) addPage(screen);

        if (exists(disc)) {
            static const wchar_t* const names[5] = {L"A", L"B", L"X", L"Y", L"OFF"};
            Page p{L"DISC"};
            Option button{L"Summon disc button", L"Press it and your personal disc comes to your hand", Kind::Choice};
            button.choices = std::vector<std::wstring>(names, names + 5);
            button.get = [disc] {
                std::wstring b = iniText(disc, L"summon", L"button", L"X");
                for (int i = 0; i < 5; i++) if (_wcsicmp(b.c_str(), names[i]) == 0) return float(i);
                return 2.f;
            };
            button.set = [disc](float v) { iniWrite(disc, L"summon", L"button", names[std::clamp(int(v), 0, 4)]); };
            p.options.push_back(button);
            p.options.push_back(toggle(L"Hide it from Echo", L"Echo doesn't see that button at all", disc, L"summon", L"hide_from_echo", false));
            p.footer = L"The personal disc itself goes back with RESTORE.cmd in its own folder.";
            addPage(p);
        }

        Page normal{L"NORMAL"};
        Option off{L"Everything off", L"HUD, glow, visor lines, disc button, auto dock, tablet size", Kind::Action};
        off.get = [this] { return allOff() ? 1.f : 0.f; };
        off.set = [this](float v) { if (v != 0) allBackOn(); else everythingOff(); };
        off.text = [](float v) { return std::wstring(v != 0 ? L"ALL BACK ON" : L"ALL OFF"); };
        normal.options.push_back(off);
        Option plain{L"Normal tablet", L"All off, and Echo Arcade leaves the tablet when you close Echo VR", Kind::Action};
        plain.get = [this] { return removalScheduled() ? 2.f : armedUntil_ > GetTickCount64() ? 1.f : 0.f; };
        plain.set = [this](float) { normalTabletTapped(); };
        plain.text = [](float v) { return std::wstring(v == 2 ? L"CANCEL" : v == 1 ? L"TAP AGAIN: SURE?" : L"NORMAL TABLET"); };
        normal.options.push_back(plain);
        normal.footer = L"After NORMAL TABLET, INSTALL.cmd puts everything back.";
        addPage(normal);

        page_ = std::clamp(page_, 0, int(pages_.size()) - 1);
        refreshOptions();
    }
    static std::wstring voiceFile() {
        wchar_t base[MAX_PATH];
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return {};
        return std::wstring(base) + L"\\rad\\loneecho\\settings_mp_v2.json";
    }
    // Echo's voice chat mode: what the game reports while it runs, else the settings file's "voipmode".
    int voiceMode() const {
        LONG live = standalone_ ? -1 : shared_->voiceState;
        if (live >= 0 && live <= 3) return int(live);
        std::string text;
        if (!readFile(voiceFile(), text)) return -1;
        size_t at = text.find("\"voipmode\"");
        if (at == std::string::npos) return -1;
        at = text.find(':', at);
        if (at == std::string::npos) return -1;
        at++;
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) at++;
        return at < text.size() && text[at] >= '0' && text[at] <= '3' ? text[at] - '0' : -1;
    }
    void setVoiceMode(int mode) {
        static const wchar_t* const names[4] = {L"muted", L"open mic", L"push to talk", L"push to mute"};
        if (!standalone_) {  // the game switches it in memory
            shared_->voiceWant = mode + 1;
            MemoryBarrier();
            InterlockedIncrement(&shared_->voiceSerial);
        }
        // And the file, so it holds after a restart: the number after "voipmode": is replaced in place.
        std::string text;
        bool saved = false;
        if (readFile(voiceFile(), text)) {
            size_t at = text.find("\"voipmode\"");
            if (at != std::string::npos && (at = text.find(':', at)) != std::string::npos) {
                at++;
                while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) at++;
                size_t end = at;
                while (end < text.size() && text[end] >= '0' && text[end] <= '9') end++;
                if (end > at) {
                    text.replace(at, end - at, std::to_string(mode));
                    FILE* f = nullptr;
                    if (_wfopen_s(&f, voiceFile().c_str(), L"wb") == 0 && f) {
                        saved = fwrite(text.data(), 1, text.size(), f) == text.size();
                        fclose(f);
                    }
                }
            }
        }
        hostLog("voice: %ls (%d) asked%s", names[mode], mode, saved ? ", saved to settings_mp_v2.json" : "; the settings file was NOT updated");
        message_ = std::wstring(L"Voice chat: ") + names[mode] + (saved ? L"" : L" (could not write Echo's settings file)");
    }
    void refreshOptions() {
        for (auto& p : pages_)
            for (auto& o : p.options) o.value = o.get();
        refreshedAt_ = GetTickCount64();
    }

    void setTabletScale(float v) {
        iniWrite(tweaksPath(), L"tablet", L"scale", num(v));
        shared_->tabletScale = LONG(v * 1000);
    }

    // ---- NORMAL: everything off (what it was is kept in echo_tweaks.ini [before_off]) ----
    bool allOff() const { return GetPrivateProfileIntW(L"before_off", L"saved", 0, tweaksPath().c_str()) != 0; }
    void everythingOff() {
        std::wstring t = tweaksPath(), hud = hudIni(), disc = discIni();
        iniWrite(t, L"before_off", L"saved", L"1");
        iniWrite(t, L"before_off", L"tablet_scale", num(iniFloat(t, L"tablet", L"scale", 1.f)));
        iniWrite(t, L"before_off", L"auto_dock", autoDock_ ? L"1" : L"0");
        if (exists(hud)) {
            iniWrite(t, L"before_off", L"hud", iniText(hud, L"general", L"enabled", L"1"));
            iniWrite(hud, L"general", L"enabled", L"0");
        }
        int lines = hudLinesState();
        if (lines >= 0) {
            iniWrite(t, L"before_off", L"hud_lines", lines > 0 ? L"1" : L"0");
            setHudLines(false);
        }
        if (exists(disc)) {
            iniWrite(t, L"before_off", L"disc_button", iniText(disc, L"summon", L"button", L"X"));
            iniWrite(disc, L"summon", L"button", L"OFF");
        }
        if (autoDock_) dockTapped();
        setTabletScale(1.f);
        if (freeze_) stopFreeze(L"");
        hostLog("settings: everything off");
        message_ = L"Everything is off (Echo's own visor lines come back when Echo starts again).";
    }
    void allBackOn() {
        std::wstring t = tweaksPath(), hud = hudIni(), disc = discIni();
        auto saved = [&](const wchar_t* key) { return iniText(t, L"before_off", key, L""); };
        if (!saved(L"tablet_scale").empty()) setTabletScale(std::clamp(float(_wtof(saved(L"tablet_scale").c_str())), .5f, 4.f));
        if (saved(L"auto_dock") == L"1" && !autoDock_) dockTapped();
        if (!saved(L"hud").empty() && exists(hud)) iniWrite(hud, L"general", L"enabled", saved(L"hud"));
        if (!saved(L"hud_lines").empty() && hudLinesState() >= 0) setHudLines(saved(L"hud_lines") == L"1");
        if (!saved(L"disc_button").empty() && exists(disc)) iniWrite(disc, L"summon", L"button", saved(L"disc_button"));
        WritePrivateProfileStringW(L"before_off", nullptr, nullptr, t.c_str());
        hostLog("settings: all back on");
        message_ = L"Everything is back as it was (visor lines: when Echo starts again).";
    }

    // ---- NORMAL TABLET: Echo Arcade off the tablet once Echo VR closes ----
    bool removalScheduled() const { return GetPrivateProfileIntW(L"normal", L"remove_on_exit", 0, tweaksPath().c_str()) != 0; }
    void normalTabletTapped() {
        uint64_t now = GetTickCount64();
        if (removalScheduled()) {
            iniWrite(tweaksPath(), L"normal", L"remove_on_exit", L"0");  // the helper sees it and stops
            hostLog("settings: normal tablet cancelled");
            message_ = L"Cancelled: Echo Arcade stays. What's off stays off until ALL BACK ON.";
            return;
        }
        if (armedUntil_ < now) {
            armedUntil_ = now + 4000;
            message_ = L"Tap NORMAL TABLET again to be sure.";
            return;
        }
        armedUntil_ = 0;
        if (!allOff()) everythingOff();
        iniWrite(tweaksPath(), L"normal", L"remove_on_exit", L"1");
        hostLog("settings: normal tablet");
        startRemover(false);
    }
    // tools\remove_on_exit.py, with the arcade folder's own Python, waits for Echo VR to close and
    // then uninstalls. It must outlive Echo, so it can't stay in the job that closes the host
    // with the game: it breaks away (the runtime allows that), else Windows (WMI) starts it.
    void startRemover(bool quiet) {
        std::wstring root = config_.root, py = root + L"\\.venv\\Scripts\\pythonw.exe", script = root + L"\\tools\\remove_on_exit.py";
        if (root.empty() || !exists(py) || !exists(script)) {
            iniWrite(tweaksPath(), L"normal", L"remove_on_exit", L"0");
            hostLog("remover: missing %ls", root.empty() ? L"root= in arcade.ini" : exists(py) ? script.c_str() : py.c_str());
            message_ = L"Couldn't find the arcade's folder: close Echo and run UNINSTALL.cmd instead.";
            return;
        }
        std::wstring cmd = L"\"" + py + L"\" \"" + script + L"\" --game \"" + gameDir() + L"\" --tweaks \"" + tweaksPath() + L"\"";
        if (!quiet) message_ = L"Normal tablet: Echo Arcade comes off the tablet when you close Echo VR. INSTALL.cmd brings it back.";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        std::wstring line = cmd;
        if (CreateProcessW(py.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB | CREATE_SUSPENDED | CREATE_NO_WINDOW,
                           nullptr, root.c_str(), &si, &pi)) {
            BOOL inJob = TRUE;
            IsProcessInJob(pi.hProcess, nullptr, &inJob);
            if (!inJob) ResumeThread(pi.hThread); else TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            hostLog("remover: %s (pid %lu)", inJob ? "still in a job after breaking away; asking WMI" : "started", pi.dwProcessId);
            if (!inJob) return;
        } else {
            hostLog("remover: breakaway start failed (error %lu); asking WMI", GetLastError());
        }
        spawn([this, cmd, root] {
            SetEnvironmentVariableW(L"ECHO_ARCADE_REMOVER", cmd.c_str());
            SetEnvironmentVariableW(L"ECHO_ARCADE_ROOT", root.c_str());
            std::wstring ps = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"$r = Invoke-CimMethod "
                              L"-ClassName Win32_Process -MethodName Create -Arguments @{CommandLine=$env:ECHO_ARCADE_REMOVER; "
                              L"CurrentDirectory=$env:ECHO_ARCADE_ROOT}; exit [int]$r.ReturnValue\"";
            STARTUPINFOW s{sizeof(s)};
            PROCESS_INFORMATION p{};
            DWORD code = 1;
            if (CreateProcessW(nullptr, ps.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &s, &p)) {
                if (WaitForSingleObject(p.hProcess, 30000) == WAIT_OBJECT_0) GetExitCodeProcess(p.hProcess, &code);
                CloseHandle(p.hThread);
                CloseHandle(p.hProcess);
            }
            hostLog("remover: WMI start %s (%lu)", code == 0 ? "ok" : "failed", code);
            if (code == 0) return;
            std::lock_guard<std::mutex> lock(pendingLock_);
            pending_.push_back([this] {
                iniWrite(tweaksPath(), L"normal", L"remove_on_exit", L"0");
                message_ = L"Couldn't schedule it (see host.log): close Echo and run UNINSTALL.cmd instead.";
            });
        });
    }

    // ---- the page ----
    static Rect tabRect(int i, int n) {
        int w = (SCREEN_W - 24 - (n - 1) * 6) / n;
        return {12 + i * (w + 6), 84, 12 + i * (w + 6) + w, 136};
    }
    static int rowY(int i) { return SETTING_Y + i * SETTING_H; }
    static Rect toggleRect(int i) { return {834, rowY(i) + 5, 1004, rowY(i) + 55}; }
    static Rect actionRect(int i) { return {634, rowY(i) + 5, 1004, rowY(i) + 55}; }
    static Rect choiceRect(int i, int k, int n) {
        int w = n <= 2 ? 160 : n == 3 ? 130 : 84, x0 = 1004 - n * w - (n - 1) * 6 + k * (w + 6);
        return {x0, rowY(i) + 5, x0 + w, rowY(i) + 55};
    }
    static Rect minusRect(int i) { return {574, rowY(i) + 5, 638, rowY(i) + 55}; }
    static Rect plusRect(int i) { return {940, rowY(i) + 5, 1004, rowY(i) + 55}; }
    static Rect trackRect(int i) { return {664, rowY(i) + 22, 914, rowY(i) + 38}; }
    bool settingsMode() const { return shared_->pageMode == 1; }
    bool discordMode() const { return shared_->pageMode == 2; }

    void change(Option& o, float v) {
        if (o.kind == Kind::Slider) v = std::clamp(std::round(v / o.step) * o.step, o.min, o.max);
        if (o.kind != Kind::Action && std::fabs(v - o.value) < .0001f) return;
        o.set(v);
        refreshOptions();  // one change can move others (ALL OFF, auto dock)
    }

    void settingsTouch(const Touch& t) {
        int n = int(pages_.size());
        for (int i = 0; i < n; i++) {
            if (!tabRect(i, n).contains(t.x, t.y)) continue;
            if (tapped(t, 590 + i, tabRect(i, n))) { page_ = i; message_.clear(); refreshOptions(); }
            return;
        }
        auto& options = pages_[page_].options;
        for (int i = 0; i < int(options.size()) && i < SETTING_ROWS; i++) {
            Option& o = options[i];
            int id = 600 + i * 10;
            switch (o.kind) {
                case Kind::Toggle:
                    if (toggleRect(i).contains(t.x, t.y)) { if (tapped(t, id, toggleRect(i))) change(o, o.value != 0 ? 0.f : 1.f); return; }
                    break;
                case Kind::Action:
                    if (actionRect(i).contains(t.x, t.y)) { if (tapped(t, id, actionRect(i))) change(o, o.value); return; }
                    break;
                case Kind::Choice:
                    for (int k = 0; k < int(o.choices.size()); k++) {
                        Rect r = choiceRect(i, k, int(o.choices.size()));
                        if (r.contains(t.x, t.y)) { if (tapped(t, id + k, r)) change(o, float(k)); return; }
                    }
                    break;
                case Kind::Slider: {
                    if (minusRect(i).contains(t.x, t.y)) { if (tapped(t, id, minusRect(i))) change(o, o.value - o.step); return; }
                    if (plusRect(i).contains(t.x, t.y)) { if (tapped(t, id + 1, plusRect(i))) change(o, o.value + o.step); return; }
                    Rect track = trackRect(i);
                    Rect area{track.x0 - 20, rowY(i), track.x1 + 20, rowY(i) + SETTING_H};
                    if (t.down && area.contains(t.x, t.y)) {  // tap or slide along the bar
                        float f = std::clamp(float(t.x - track.x0) / track.w(), 0.f, 1.f);
                        change(o, o.min + f * (o.max - o.min));
                        return;
                    }
                    break;
                }
            }
        }
        if (!t.down) pressed_ = -1;
    }

    void drawSettings() {
        if (GetTickCount64() - refreshedAt_ > 1000) refreshOptions();  // files changed by hand, a confirm timing out
        canvas_.clear(rgb(14, 16, 24));
        header(L"SETTINGS", false);
        canvas_.text({600, 0, 1004, 76}, L"changes apply right away", 18, rgb(150, 160, 180), false, 2);
        int n = int(pages_.size());
        for (int i = 0; i < n; i++)
            button(tabRect(i, n), pages_[i].name, pressed_ == 590 + i, i == page_ ? rgb(40, 96, 140) : rgb(36, 42, 62), 22);
        const Page& page = pages_[page_];
        const uint32_t onColor = rgb(40, 110, 60), offColor = rgb(44, 50, 72), pick = rgb(40, 96, 140);
        for (int i = 0; i < int(page.options.size()) && i < SETTING_ROWS; i++) {
            const Option& o = page.options[i];
            int y = rowY(i), id = 600 + i * 10;
            if (i) canvas_.fill({24, y - 1, 1000, y}, rgb(30, 34, 48));
            int textRight = o.kind == Kind::Choice ? choiceRect(i, 0, int(o.choices.size())).x0 - 12
                          : o.kind == Kind::Slider ? 470 : o.kind == Kind::Action ? 620 : 820;
            canvas_.text({24, y + 4, textRight, y + 32}, o.label, 22, rgb(235, 235, 240), true, 0);
            canvas_.text({24, y + 32, textRight, y + 58}, o.note, 15, rgb(140, 150, 170), false, 0);
            switch (o.kind) {
                case Kind::Toggle:
                    button(toggleRect(i), o.value != 0 ? L"ON" : L"OFF", pressed_ == id, o.value != 0 ? onColor : offColor, 24);
                    break;
                case Kind::Action:
                    button(actionRect(i), o.text(o.value), pressed_ == id, o.value != 0 ? rgb(130, 70, 30) : rgb(110, 40, 40), 22);
                    break;
                case Kind::Choice:
                    for (int k = 0; k < int(o.choices.size()); k++)
                        button(choiceRect(i, k, int(o.choices.size())), o.choices[k], pressed_ == id + k, int(o.value) == k ? pick : offColor, 20);
                    break;
                case Kind::Slider: {
                    canvas_.text({470, y, 562, y + 60}, num(o.value, o.format), 22, rgb(90, 200, 255), true, 2);
                    button(minusRect(i), L"\x2212", pressed_ == id, offColor, 30);
                    button(plusRect(i), L"+", pressed_ == id + 1, offColor, 30);
                    Rect track = trackRect(i);
                    canvas_.fill(track, rgb(40, 44, 62));
                    int fill = track.x0 + int(track.w() * (o.value - o.min) / (o.max - o.min));
                    canvas_.fill({track.x0, track.y0, fill, track.y1}, rgb(70, 150, 190));
                    canvas_.fill({fill - 10, track.y0 - 12, fill + 10, track.y1 + 12}, rgb(235, 240, 250));
                    break;
                }
            }
        }
        if (!message_.empty()) messageBar();
        else if (!page.footer.empty()) canvas_.text({24, SCREEN_H - 40, 1000, SCREEN_H}, page.footer, 17, rgb(150, 160, 180), false, 0);
    }

    // Stop whatever the game was being fed when the tab changes under a held finger.
    void releaseGameInput() {
        if (!freeze_) {
            for (auto& b : PAD) session_.pad(b.pad, false);
            session_.analog(0, 0);
        }
        if (pointerDown_) session_.pointer("up", lastU_, lastV_);
        resetInput();
    }

    void onTouch(const Touch& t) {
        if (t.down) held_[t.cell] = t; else held_.erase(t.cell);
        // A landing (tapped() fires on it): the first cell down while none was held, and not
        // within a blink of the last one (a finger on the line between two buttons presses
        // both; the release of one cell can come before the press of the next).
        uint64_t touchNow = GetTickCount64();
        if (!t.move) {
            if (t.down) {
                if (heldCount_ && touchNow - lastTouchAt_ > 1500) heldCount_ = 0;  // a release that never came
                landing_ = heldCount_ == 0 && touchNow - firedAt_ > 150;
                heldCount_++;
            } else {
                heldCount_ = std::max(0, heldCount_ - 1);
                landing_ = false;
            }
            lastTouchAt_ = touchNow;
        }
        if (!loading_.empty() && !settingsMode()) return;
        if (t.side >= 0) { sideTouch(t); return; }  // a side poster: only the cross-play one has a button
        if (dstate_.call.at && callTouch(t)) return;
        if (discordMode()) { discordTouch(t); return; }
        if (settingsMode()) { settingsTouch(t); return; }
        switch (mode_) {
            case Mode::Launcher: launcherTouch(t); break;
            case Mode::Browse: browseTouch(t); break;
            case Mode::PlexLink: if (!t.down && BACK_BUTTON.contains(t.x, t.y)) toLauncher(); break;
            case Mode::Menu: menuTouch(t); break;
            case Mode::Keyboard: keyboardTouch(t); break;
            case Mode::Screen: screenTouch(t); break;
            case Mode::Media: mediaTouch(t); break;
            case Mode::Watch: watchTouch(t); break;
            case Mode::Split: splitTouch(t); break;
            case Mode::Running:
                if (isVideo(session_.id())) playerTouch(t);
                else if (isRetro(session_.id())) retroTouch(t);
                else if (isDesktop(session_.id())) minecraftTouch(t);
                else balatroTouch(t);
                break;
        }
    }

    // A tap = press and release on the same control.
    // A button fires the moment the finger lands on it: a poke in VR often lifts off somewhere
    // else, or its release comes late or not at all, and waiting for it lost taps. One landing
    // fires once (the fingertip covers several touch cells and each reports a press; onTouch
    // counts the cells held, and a landing is the first cell down when none was). Half a touch
    // cell of slack around the button: a cell's centre may sit just outside a small one.
    bool tapped(const Touch& t, int index, Rect r) {
        Rect wide{r.x0 - 8, r.y0 - 8, r.x1 + 8, r.y1 + 8};
        if (!wide.contains(t.x, t.y) || !t.down || t.move) return false;
        if (!landing_) { if (pressed_ < 0) pressed_ = index; return false; }  // the same poke, another cell under the fingertip
        landing_ = false;
        pressed_ = index;
        firedAt_ = GetTickCount64();
        return true;
    }

    void launcherTouch(const Touch& t) {
        if (tapped(t, 500, DOCK_BUTTON)) { dockTapped(); return; }
        if (tapped(t, 501, FREEZE_BUTTON)) { freezeTapped(); return; }
        for (size_t i = 0; i < apps_.size(); i++)
            if (tapped(t, int(i), tileRect(i))) { launch(apps_[i]); return; }
        if (!t.down) pressed_ = -1;
    }

    void browseTouch(const Touch& t) {
        auto& s = screens_.back();
        int pages = std::max(1, (int(s.entries.size()) + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
        if (tapped(t, 100, BACK_BUTTON)) { back(); return; }
        if (minecraftChooser() && tapped(t, 501, DOCK_BUTTON)) { freezeTapped(); return; }  // FREEZE on the Minecraft page only
        if (tapped(t, 101, PAGE_UP)) { s.page = std::max(0, s.page - 1); return; }
        if (tapped(t, 102, PAGE_DOWN)) { s.page = std::min(pages - 1, s.page + 1); return; }
        for (int i = 0; i < ROWS_PER_PAGE; i++) {
            size_t index = size_t(s.page * ROWS_PER_PAGE + i);
            if (index >= s.entries.size()) break;
            if (tapped(t, i, rowRect(i))) {
                auto open = s.entries[index].open;  // may push/pop screens
                if (open) open();
                return;
            }
        }
        if (!t.down) pressed_ = -1;
    }

    void openMenu() {
        for (auto& b : PAD) session_.pad(b.pad, false);
        session_.analog(0, 0);
        if (pointerDown_) session_.pointer("up", lastU_, lastV_);
        pointerDown_ = false;
        gameCells_.clear();
        mode_ = Mode::Menu;
    }

    void menuTouch(const Touch& t) {
        if (tapped(t, 200, RESUME_BUTTON)) mode_ = Mode::Running;
        else if (tapped(t, 201, QUIT_BUTTON)) { session_.quit(); message_ = L"Closing " + current_.title + L"..."; }
        else if (tapped(t, 202, MENU_DOCK_BUTTON)) dockTapped();
        else if (tapped(t, 203, MENU_FREEZE_BUTTON)) freezeTapped();
        else if (tapped(t, 204, MENU_HOME_BUTTON)) goHome();
    }
    // HOME: back to the launcher with the app still running (a video keeps playing; Minecraft
    // sleeps until you come back). Its tile says so and brings it back; starting another app
    // ends it.
    void goHome() {
        if (freeze_) stopFreeze(L"");
        background_ = true;
        backgroundApp_ = current_;
        toLauncher();
        message_ = current_.title + L" keeps running: its tile brings it back.";
    }
    bool sameApp(const AppInfo& a, const AppInfo& b) const { return a.id == b.id && a.rom == b.rom && a.title == b.title; }
    void pollBackground() {
        if (!background_) return;
        if (!session_.alive()) {
            background_ = false;
            if (mode_ == Mode::Launcher || mode_ == Mode::Browse) message_ = backgroundApp_.title + L" closed.";
            lightGun_.appClosed();
            finishPlex();
        }
    }

    void playerTouch(const Touch& t) {
        uint64_t now = GetTickCount64();
        bool shown = now < controlsUntil_;
        if (tapped(t, 320, CONTROLS_BUTTON)) { controlsUntil_ = shown ? 0 : now + 8000; return; }  // the small button only
        if (CONTROLS_BUTTON.contains(t.x, t.y)) return;
        if (!shown) { if (!t.down) pressed_ = -1; return; }
        controlsUntil_ = now + 8000;
        auto& mpv = session_.mpv();
        bool youTube = current_.id == AppId::YouTube;  // PREV / NEXT instead of SUBS / AUDIO
        if (!t.down && SEEK_BAR.contains(t.x, t.y) && pressed_ < 0) {
            char cmd[96];
            snprintf(cmd, sizeof(cmd), R"(["seek",%.2f,"absolute-percent"])", 100.0 * (t.x - SEEK_BAR.x0) / SEEK_BAR.w());
            mpv.command(cmd);
            return;
        }
        for (int i = 0; i < PLAYER_BUTTONS; i++) {
            if (!tapped(t, 300 + i, playerButton(i))) continue;
            switch (i) {
                case 0: mpv.command(R"(["seek",-30])"); break;
                case 1: mpv.command(R"(["seek",-10])"); break;
                case 2: mpv.command(R"(["cycle","pause"])"); break;
                case 3: mpv.command(R"(["seek",10])"); break;
                case 4: mpv.command(R"(["seek",30])"); break;
                case 5: mpv.command(R"(["add","volume",-10])"); break;
                case 6: mpv.command(R"(["add","volume",10])"); break;
                case 7: mpv.command(youTube ? R"(["playlist-prev"])" : R"(["cycle","sub"])"); break;
                case 8: mpv.command(youTube ? R"(["playlist-next"])" : R"(["cycle","audio"])"); break;
                case 9: finishPlex(); session_.quit(); break;
            }
            return;
        }
        if (!t.down && t.y < SEEK_BAR.y0 - 8 && pressed_ < 0) controlsUntil_ = 0;  // tap the picture to hide
        if (!t.down) pressed_ = -1;
    }

    void hidePad(bool hide) {
        padHidden_ = hide;
        if (hide && !freeze_) {  // whatever a finger was holding is let go
            for (auto& b : PAD) session_.pad(b.pad, false);
            session_.analog(0, 0);
            std::fill(std::begin(padHeld_), std::end(padHeld_), false);
        }
    }
    void retroTouch(const Touch& t) {
        if (padHidden_) {  // only the small button at the top-right brings the pad back
            if (tapped(t, 404, CONTROLS_BUTTON)) hidePad(false);
            else if (!t.down) pressed_ = -1;
            return;
        }
        if (tapped(t, 404, RETRO_FULL)) { hidePad(true); return; }
        if (tapped(t, 400, RETRO_HOME)) { openMenu(); return; }
        if (tapped(t, 401, RETRO_MENU)) { session_.command("MENU_TOGGLE"); return; }
        if (tapped(t, 403, RETRO_FREEZE)) { freezeTapped(); return; }
        if (tapped(t, 402, STICK_TOGGLE)) {
            stickMode_ = !stickMode_;
            for (auto p : {PadUp, PadDown, PadLeft, PadRight}) session_.pad(p, false);
            session_.analog(0, 0);
            return;
        }
        if (freeze_) { if (!t.down) pressed_ = -1; return; }  // the controllers have the pad
        for (auto& b : PAD) {
            bool dpad = b.pad == PadUp || b.pad == PadDown || b.pad == PadLeft || b.pad == PadRight;
            if (dpad && stickMode_) continue;
            bool on = false;
            for (auto& [cell, h] : held_) on |= grow(b.rect, 6).contains(h.x, h.y);
            session_.pad(b.pad, on);
            if (on && !padHeld_[b.pad]) gunButton(b.pad);
            padHeld_[b.pad] = on;
        }
        if (stickMode_) {
            // Newest finger in the stick area sets the direction; release recentres.
            float x = 0, y = 0;
            for (auto& [cell, h] : held_) {
                if (!STICK_AREA.contains(h.x, h.y)) continue;
                float cx = (STICK_AREA.x0 + STICK_AREA.x1) / 2.f, cy = (STICK_AREA.y0 + STICK_AREA.y1) / 2.f;
                x = (h.x - cx) / (STICK_AREA.w() / 2.f - 16);
                y = (h.y - cy) / (STICK_AREA.h() / 2.f - 16);
            }
            float len = std::sqrt(x * x + y * y);
            if (len > 1) { x /= len; y /= len; }
            stick_[0] = x; stick_[1] = y;
            session_.analog(x, y);
        }
    }

    // Once you have shot at the game, the pad's A/B/X/START/SELECT also press the light gun's
    // own buttons (GunCon A/B, Justifier special...), which the RetroPad can't reach: the gun
    // replaces the pad on its port.
    void gunButton(Pad pad) {
        if (!gunUsed_ || !session_.hasWindow()) return;
        GunButton b = pad == PadA ? GunAuxA : pad == PadB ? GunAuxB : pad == PadX ? GunAuxC :
                      pad == PadStart ? GunStart : pad == PadSelect ? GunSelect : GunButtonCount;
        if (b != GunButtonCount) lightGun_.press(session_.window(), b);
    }

    bool toApp(int x, int y, float& u, float& v) {
        if (!fit_.w() || !fit_.h()) return false;
        u = std::clamp((x - fit_.x0 + .5f) / fit_.w(), 0.f, 1.f);
        v = std::clamp((y - fit_.y0 + .5f) / fit_.h(), 0.f, 1.f);
        return true;
    }

    void balatroTouch(const Touch& t) {
        if (BALATRO_HOME.contains(t.x, t.y)) { if (!t.down) openMenu(); return; }
        auto same = std::find_if(gameCells_.begin(), gameCells_.end(), [&](const Touch& g) { return g.cell == t.cell; });
        if (t.move && same != gameCells_.end()) *same = t;
        else if (t.down) gameCells_.push_back(t);
        else gameCells_.erase(std::remove_if(gameCells_.begin(), gameCells_.end(), [&](const Touch& g) { return g.cell == t.cell; }), gameCells_.end());
        float u, v;
        if (!gameCells_.empty()) {
            // The newest held cell is the finger's position; sliding across cells drags.
            if (!toApp(gameCells_.back().x, gameCells_.back().y, u, v)) return;
            session_.pointer("move", u, v);
            if (!pointerDown_) session_.pointer("down", u, v);
            pointerDown_ = true;
            lastU_ = u; lastV_ = v;
        } else if (pointerDown_) {
            session_.pointer("up", lastU_, lastV_);
            pointerDown_ = false;
        }
    }

    // ------------------------------------------------ drawing
    bool compactTiles() const { return apps_.size() > 6; }  // seven or more tiles: three rows of shorter ones
    Rect tileRect(size_t i) const {
        int col = int(i % 3), row = int(i / 3);
        int h = compactTiles() ? 150 : 210, step = compactTiles() ? 162 : 222;
        return {20 + col * 332, 88 + row * step, 20 + col * 332 + 320, 88 + row * step + h};
    }

    void header(const std::wstring& title, bool backButton) {
        canvas_.fill({0, 0, SCREEN_W, 76}, rgb(22, 26, 40));
        if (backButton) {
            button(BACK_BUTTON, L"\x25C0  BACK", pressed_ == 100, rgb(50, 58, 84));
            canvas_.text({170, 0, 1004, 76}, title, 32, rgb(90, 200, 255), true, 0);
        } else {
            canvas_.text({24, 0, 600, 76}, title, 40, rgb(90, 200, 255), true, 0);
        }
    }

    void button(Rect r, const std::wstring& label, bool on, uint32_t color, int size = 0) {
        canvas_.fill(r, on ? rgb(250, 250, 250) : color);
        canvas_.frame(r, rgb(70, 76, 100), 2);
        canvas_.text(r, label, size ? size : (r.h() > 60 ? 28 : 20), on ? rgb(20, 20, 20) : rgb(235, 235, 240));
    }

    void messageBar() {
        if (message_.empty()) return;
        canvas_.fill({0, SCREEN_H - 40, SCREEN_W, SCREEN_H}, rgb(60, 30, 30));
        canvas_.text({20, SCREEN_H - 40, SCREEN_W - 20, SCREEN_H}, message_, 20, rgb(255, 220, 200));
    }

    void loadingOverlay() {
        if (loading_.empty()) return;
        canvas_.blend({0, 0, SCREEN_W, SCREEN_H}, rgb(0, 0, 0), 170);
        unsigned dots = unsigned(GetTickCount64() / 400 % 4);
        canvas_.text({0, 240, SCREEN_W, 330}, loading_ + std::wstring(dots, L' '), 32, rgb(255, 255, 255));
    }

    void drawLauncher() {
        canvas_.clear(rgb(14, 16, 24));
        header(L"ECHO ARCADE", false);
        button(FREEZE_BUTTON, freezeLabel(), pressed_ == 501, freeze_ ? rgb(40, 110, 60) : rgb(50, 58, 84), 22);
        button(DOCK_BUTTON, dockLabel(), pressed_ == 500, docked() ? rgb(40, 110, 60) : rgb(50, 58, 84), 22);
        for (size_t i = 0; i < apps_.size(); i++) {
            auto& a = apps_[i];
            Rect r = tileRect(i);
            bool on = pressed_ == int(i);
            canvas_.fill(r, on ? rgb(48, 56, 80) : rgb(30, 34, 50));
            canvas_.fill({r.x0, r.y0, r.x0 + 12, r.y1}, a.accent);
            canvas_.frame(r, on ? rgb(255, 255, 255) : rgb(50, 58, 84), on ? 4 : 2);
            size_t split = a.title.size() > 13 ? a.title.rfind(L' ', a.title.size() / 2 + 3) : std::wstring::npos;  // long names: two lines
            int up = compactTiles() ? 12 : 0;  // shorter tiles: everything a little higher and tighter
            if (split == std::wstring::npos || split == 0) {
                canvas_.text({r.x0 + 30, r.y0 + 26 - up, r.x1 - 12, r.y0 + 96 - up}, a.title, a.title.size() > 13 ? 28 : 40, rgb(245, 245, 250), true, 0);
            } else {
                canvas_.text({r.x0 + 30, r.y0 + 22 - up, r.x1 - 12, r.y0 + 60 - up}, a.title.substr(0, split), 28, rgb(245, 245, 250), true, 0);
                canvas_.text({r.x0 + 30, r.y0 + 58 - up, r.x1 - 12, r.y0 + 96 - up}, a.title.substr(split + 1), 28, rgb(245, 245, 250), true, 0);
            }
            std::wstring sub = tileSub(a), note = tileNote(a);
            bool codeNote = (a.id == AppId::Share || a.id == AppId::Split) && share_.running();
            canvas_.text({r.x0 + 30, r.y0 + 96 - up, r.x1 - 12, r.y0 + 134 - up}, sub, compactTiles() ? 19 : 21, rgb(180, 190, 210), false, 0);
            if (!note.empty())
                canvas_.text({r.x0 + 30, r.y0 + (compactTiles() ? 118 : 140), r.x1 - 12, r.y0 + (compactTiles() ? 148 : 180)}, note,
                             codeNote ? 20 : 17, codeNote ? rgb(90, 200, 255) : rgb(255, 170, 90), codeNote, 0);
        }
        messageBar();
    }

    void drawBrowse() {
        auto& s = screens_.back();
        canvas_.clear(rgb(14, 16, 24));
        header(s.title, true);
        if (minecraftChooser()) {  // FREEZE here: hold still, pick the launcher, the game gets the controllers
            canvas_.fill({730, 0, SCREEN_W, 76}, rgb(22, 26, 40));
            button(DOCK_BUTTON, freezeLabel(), pressed_ == 501, freeze_ ? rgb(40, 110, 60) : rgb(50, 58, 84), 22);
        }
        int pages = std::max(1, (int(s.entries.size()) + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
        for (int i = 0; i < ROWS_PER_PAGE; i++) {
            size_t index = size_t(s.page * ROWS_PER_PAGE + i);
            if (index >= s.entries.size()) break;
            auto& e = s.entries[index];
            Rect r = rowRect(i);
            bool on = pressed_ == i;
            canvas_.fill(r, on ? rgb(56, 64, 92) : rgb(30, 34, 50));
            canvas_.fill({r.x0, r.y0, r.x0 + 8, r.y1}, current_.accent);
            int tx = r.x0 + 24;
            if (!e.thumb.empty()) {  // a video: its picture at the left (fetched while the page is up)
                Rect box{r.x0 + 14, r.y0 + 3, r.x0 + 14 + Thumbs::W, r.y0 + 3 + Thumbs::H};
                auto pic = thumbs_.get(e.thumb);
                if (pic && pic->w) canvas_.blit(pic->bgra.data(), pic->w, pic->h, pic->w * 4, box);
                else canvas_.fill(box, rgb(40, 44, 60));
                tx = box.x1 + 14;
            }
            if (e.sub.empty()) canvas_.text({tx, r.y0, r.x1 - 12, r.y1}, e.label, 26, rgb(240, 240, 245), true, 0);
            else {
                canvas_.text({tx, r.y0 + 4, r.x1 - 12, r.y0 + 40}, e.label, 25, rgb(240, 240, 245), true, 0);
                canvas_.text({tx, r.y0 + 38, r.x1 - 12, r.y1 - 2}, e.sub, 18, rgb(160, 170, 190), false, 0);
            }
        }
        button(PAGE_UP, L"\x25B2", pressed_ == 101, s.page > 0 ? rgb(50, 58, 84) : rgb(28, 30, 40), 36);
        button(PAGE_DOWN, L"\x25BC", pressed_ == 102, s.page < pages - 1 ? rgb(50, 58, 84) : rgb(28, 30, 40), 36);
        canvas_.text({12, 528, 904, 574}, L"page " + std::to_wstring(s.page + 1) + L" of " + std::to_wstring(pages) + L"   -   " +
                     std::to_wstring(s.entries.size()) + L" items", 18, rgb(130, 140, 160), false, 0);
        messageBar();
    }

    // ---- MEDIA: whatever plays on this PC (Spotify, a browser, mpv...), through Windows' media
    // sessions, and the master volume.
    void openMedia() {
        mode_ = Mode::Media;
        lastMediaPage_ = 0;
        resetInput();
    }
    // ---- album art for the listening mode: the real cover from iTunes' catalogue (600 px,
    // no video bars) for the title and artist, else the media session's own picture with
    // its side bars cut off (YouTube's auto-made videos pad the cover with a colour).
    static std::wstring cleanMediaText(std::wstring s) {
        for (;;) {  // "(Official Video)", "[Lyrics]"...
            size_t a = s.find_first_of(L"([");
            if (a == std::wstring::npos) break;
            size_t b = s.find_first_of(L")]", a);
            if (b == std::wstring::npos) { s.erase(a); break; }
            s.erase(a, b - a + 1);
        }
        static const wchar_t* const drop[] = {L" - Topic", L"- Topic", L"VEVO", L"Official", L"official", L"ft.", L"feat."};
        for (auto d : drop) { size_t p; while ((p = s.find(d)) != std::wstring::npos) s.erase(p, wcslen(d)); }
        std::wstring out;
        for (wchar_t c : s) { if (c == L' ' && (out.empty() || out.back() == L' ')) continue; out += c; }
        while (!out.empty() && out.back() == L' ') out.pop_back();
        return out;
    }
    static std::string urlEncodeUtf8(const std::wstring& w) {
        std::string s = narrowUtf8(w), out;
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
            else { char b[4]; sprintf_s(b, "%%%02X", c); out += b; }
        }
        return out;
    }
    static std::shared_ptr<const SharePicture> cropBars(std::shared_ptr<const SharePicture> pic) {
        if (!pic || pic->w < 16 || pic->h < 8) return pic;
        auto px = [&](int x, int y) { return pic->bgra.data() + (size_t(y) * pic->w + x) * 4; };
        auto similar = [](const uint8_t* a, const uint8_t* b) { return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) < 60; };
        auto bar = [&](int x, const uint8_t* ref) {
            int bad = 0, n = 0;
            for (int y = 0; y < pic->h; y += 2) { n++; if (!similar(px(x, y), ref)) bad++; }
            return bad * 12 < n;
        };
        int left = 0, right = pic->w - 1;
        while (left < pic->w / 2 && bar(left, px(0, 0))) left++;
        while (right > pic->w / 2 && bar(right, px(pic->w - 1, 0))) right--;
        if (left < 3 && right > pic->w - 4) return pic;
        auto out = std::make_shared<SharePicture>();
        out->w = right - left + 1;
        out->h = pic->h;
        out->bgra.resize(size_t(out->w) * out->h * 4);
        for (int y = 0; y < pic->h; y++) memcpy(out->bgra.data() + size_t(y) * out->w * 4, px(left, y), size_t(out->w) * 4);
        return out;
    }
    static std::shared_ptr<const SharePicture> itunesArt(const std::wstring& title, const std::wstring& artist) {
        std::wstring t = cleanMediaText(title), a = cleanMediaText(artist);
        if (t.empty()) return nullptr;
        auto body = httpGetBytes(L"https://itunes.apple.com/search?entity=song&limit=6&term=" + widenUtf8(urlEncodeUtf8(a + L" " + t)));
        if (body.empty()) return nullptr;
        std::wstring url;
        try {
            winrt::Windows::Data::Json::JsonObject o;
            if (!winrt::Windows::Data::Json::JsonObject::TryParse(widenUtf8(std::string(body.begin(), body.end())), o)) return nullptr;
            auto results = o.GetNamedArray(L"results", nullptr);
            if (!results) return nullptr;
            std::wstring firstWord = a.substr(0, a.find(L' '));
            for (auto& c : firstWord) c = wchar_t(towlower(c));
            std::wstring lowTitle = t;
            for (auto& c : lowTitle) c = wchar_t(towlower(c));
            for (auto v : results) {
                if (v.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) continue;
                auto r = v.GetObject();
                std::wstring artistName = std::wstring(r.GetNamedString(L"artistName", L"")), trackName = std::wstring(r.GetNamedString(L"trackName", L"")), art = std::wstring(r.GetNamedString(L"artworkUrl100", L""));
                for (auto& c : artistName) c = wchar_t(towlower(c));
                for (auto& c : trackName) c = wchar_t(towlower(c));
                bool artistOk = firstWord.empty() || artistName.find(firstWord) != std::wstring::npos;
                bool titleOk = trackName.find(lowTitle) != std::wstring::npos || lowTitle.find(trackName) != std::wstring::npos;
                if (!art.empty() && (artistOk || titleOk)) { url = art; break; }
            }
        } catch (...) { return nullptr; }
        if (url.empty()) return nullptr;
        size_t p = url.find(L"100x100");
        if (p != std::wstring::npos) url.replace(p, 7, L"600x600");
        auto bytes = httpGetBytes(url);
        if (bytes.empty()) return nullptr;
        auto pic = decodeImage(bytes.data(), bytes.size());
        return pic && pic->w ? pic : nullptr;
    }
    // A soft, dark wash of the cover's colours for the whole screen: a 24 x 14 average, blown up.
    static std::shared_ptr<const SharePicture> makeBackdrop(const SharePicture* art) {
        auto out = std::make_shared<SharePicture>();
        out->w = SCREEN_W;
        out->h = SCREEN_H;
        out->bgra.assign(size_t(SCREEN_W) * SCREEN_H * 4, 0);
        const int GW = 24, GH = 14;
        float grid[GH][GW][3] = {};
        if (art && art->w && art->h) {
            for (int gy = 0; gy < GH; gy++)
                for (int gx = 0; gx < GW; gx++) {
                    int x0 = gx * art->w / GW, x1 = std::max(x0 + 1, (gx + 1) * art->w / GW), y0 = gy * art->h / GH, y1 = std::max(y0 + 1, (gy + 1) * art->h / GH);
                    double s[3] = {0, 0, 0};
                    int n = 0;
                    for (int y = y0; y < y1; y++)
                        for (int x = x0; x < x1; x++) { const uint8_t* p = art->bgra.data() + (size_t(y) * art->w + x) * 4; s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; n++; }
                    for (int c = 0; c < 3; c++) grid[gy][gx][c] = float(s[c] / n);
                }
        } else {
            for (int gy = 0; gy < GH; gy++) for (int gx = 0; gx < GW; gx++) { grid[gy][gx][0] = 40; grid[gy][gx][1] = 30; grid[gy][gx][2] = 30; }
        }
        for (int y = 0; y < SCREEN_H; y++) {
            float fy = float(y) * (GH - 1) / (SCREEN_H - 1);
            int iy = int(fy), iy1 = std::min(GH - 1, iy + 1);
            float ty = fy - iy;
            for (int x = 0; x < SCREEN_W; x++) {
                float fx = float(x) * (GW - 1) / (SCREEN_W - 1);
                int ix = int(fx), ix1 = std::min(GW - 1, ix + 1);
                float tx = fx - ix;
                uint8_t* o = out->bgra.data() + (size_t(y) * SCREEN_W + x) * 4;
                for (int c = 0; c < 3; c++) {
                    float v = (grid[iy][ix][c] * (1 - tx) + grid[iy][ix1][c] * tx) * (1 - ty) + (grid[iy1][ix][c] * (1 - tx) + grid[iy1][ix1][c] * tx) * ty;
                    o[c] = uint8_t(std::min(255.f, v * 0.34f));
                }
                o[3] = 255;
            }
        }
        return out;
    }
    void renderMarquee(const std::wstring& text) {
        marquee_.clear(rgb(0, 0, 0));
        marqueeWidth_ = 0;
        if (text.empty()) return;
        marquee_.text({0, 0, marquee_.width(), marquee_.height()}, text, 36, rgb(255, 255, 255), true, 0);
        const uint32_t* px = marquee_.pixels();
        for (int x = marquee_.width() - 1; x >= 0 && !marqueeWidth_; x--)
            for (int y = 0; y < marquee_.height(); y++)
                if ((px[size_t(y) * marquee_.width() + x] & 0xff) > 40) { marqueeWidth_ = x + 1; break; }
    }
    // The strip (white text on black) over the screen with `opacity`, moving right: its
    // text repeats every (width + gap) px; `yTop` in tablet px.
    void blendMarquee(int yTop, int offset, float opacity) {
        if (marqueeWidth_ <= 0) return;
        Canvas& c = canvas_;
        float k = float(c.width()) / SCREEN_W;
        int period = marqueeWidth_ + 180;
        const uint32_t* src = marquee_.pixels();
        uint32_t* dst = c.pixels();
        int y0 = std::max(0, int(yTop * k)), y1 = std::min(c.height(), int((yTop + marquee_.height()) * k));
        for (int y = y0; y < y1; y++) {
            int sy = std::clamp(int(y / k) - yTop, 0, marquee_.height() - 1);
            for (int x = 0; x < c.width(); x++) {
                int sx = ((int(x / k) - offset) % period + period) % period;
                if (sx >= marqueeWidth_) continue;
                int lum = int(src[size_t(sy) * marquee_.width() + sx] & 0xff);
                if (lum < 12) continue;
                float a = opacity * lum / 255.f;
                uint32_t& d = dst[size_t(y) * c.width() + x];
                int r = int(((d >> 16) & 255) * (1 - a) + 255 * a), g = int(((d >> 8) & 255) * (1 - a) + 255 * a), b = int((d & 255) * (1 - a) + 255 * a);
                d = 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
            }
        }
    }
    void pollMedia(uint64_t now) {
        if ((mode_ != Mode::Media && !sidesLive_) || mediaPageBusy_ || now - lastMediaPage_ < 1000) return;
        lastMediaPage_ = now;
        mediaPageBusy_ = true;
        std::wstring artKey = mediaArtKey_;
        spawn([this, artKey] {
            std::wstring app;
            MediaInfo peek = mediaNowPlaying(app, false);
            std::wstring key = peek.valid ? peek.title + L"|" + peek.artist + L"|" + app : L"";
            std::shared_ptr<const SharePicture> art, back;
            bool newArt = key != artKey;
            if (newArt && peek.valid) {  // the cover, once per track: iTunes first, the session's picture second
                art = itunesArt(peek.title, peek.artist);
                if (!art) {
                    MediaInfo withArt = mediaNowPlaying(app, true);
                    if (!withArt.art.empty()) art = cropBars(decodeImage(withArt.art.data(), withArt.art.size()));
                    if (art && !art->w) art = nullptr;
                }
                back = makeBackdrop(art.get());
                hostLog("media: %ls - %ls: cover %s", peek.artist.c_str(), peek.title.c_str(), art ? (art->w >= 400 ? "from iTunes" : "from the player") : "none");
            }
            float volume = masterVolume();
            bool muted = masterMuted();
            std::lock_guard<std::mutex> lock(pendingLock_);
            pending_.push_back([this, peek, app, volume, muted, key, newArt, art, back] {
                mediaNow_ = peek; mediaApp_ = app; mediaVolume_ = volume; mediaMuted_ = muted; mediaPageBusy_ = false;
                if (newArt) {
                    mediaArtKey_ = key;
                    mediaArt_ = art;
                    mediaBack_ = back;
                    std::wstring artist = cleanMediaText(peek.artist), title = cleanMediaText(peek.title);
                    renderMarquee(peek.valid ? (artist.empty() ? title : artist + L"   \x2022   " + title) : L"");
                }
            });
        });
    }
    void mediaTouch(const Touch& t) {
        if (mediaListen_) {  // listening mode: any tap brings the controls back
            if (t.down && !t.move && landing_) { landing_ = false; mediaListen_ = false; }
            if (!t.down) pressed_ = -1;
            return;
        }
        if (tapped(t, 100, BACK_BUTTON)) { toLauncher(); return; }
        if (tapped(t, 906, MEDIA_LISTEN)) { mediaListen_ = true; return; }
        MediaCommand command;
        bool control = false;
        if (tapped(t, 900, MEDIA_PREV)) { command = MediaCommand::Previous; control = true; }
        else if (tapped(t, 901, MEDIA_PLAY)) { command = MediaCommand::PlayPause; control = true; }
        else if (tapped(t, 902, MEDIA_NEXT)) { command = MediaCommand::Next; control = true; }
        else if (tapped(t, 903, MEDIA_VOLDOWN)) { setMasterVolume(std::max(0.f, mediaVolume_ - .05f)); lastMediaPage_ = 0; }
        else if (tapped(t, 904, MEDIA_MUTE)) { toggleMasterMute(); lastMediaPage_ = 0; }
        else if (tapped(t, 905, MEDIA_VOLUP)) { setMasterVolume(std::min(1.f, mediaVolume_ + .05f)); lastMediaPage_ = 0; }
        else if (!t.down) pressed_ = -1;
        if (control) spawn([this, command] {
            bool ok = mediaControl(command);
            std::lock_guard<std::mutex> lock(pendingLock_);
            pending_.push_back([this, ok] { if (!ok) message_ = L"Nothing is playing that Windows can control."; lastMediaPage_ = 0; });
        });
    }
    // Listening mode: the cover front and centre, a soft wash of its colours around it, the
    // track scrolling by (the MEDIA page's, and the first side poster's).
    void drawListening() {
        if (mediaBack_) canvas_.blit(mediaBack_->bgra.data(), mediaBack_->w, mediaBack_->h, mediaBack_->w * 4, {0, 0, SCREEN_W, SCREEN_H});
        else canvas_.clear(rgb(6, 6, 10));
        if (mediaArt_) canvas_.blit(mediaArt_->bgra.data(), mediaArt_->w, mediaArt_->h, mediaArt_->w * 4, {0, 0, SCREEN_W, SCREEN_H});
        else canvas_.text({0, 150, SCREEN_W, 330}, L"\x266B", 150, rgb(60, 70, 100));
        if (!mediaNow_.valid) canvas_.text({0, SCREEN_H - 70, SCREEN_W, SCREEN_H - 30}, L"Nothing is playing", 24, rgb(200, 205, 220), false);
        else blendMarquee(SCREEN_H - 84, int((GetTickCount64() / 12) % 100000), mediaNow_.playing ? 0.55f : 0.3f);
    }
    // ---- the side posters: four pictures of their own while the arcade is docked on the
    // lobby's news board (its four small neighbours each sample a slice of one texture).
    void renderSides(uint64_t now) {
        bool live = sidesOn_ && docked();
        if (live != sidesLive_) {
            sidesLive_ = live;
            shared_->sideWanted = live ? (1 << arcade::SIDES) - 1 : 0;
            if (live) { for (auto& h : sideHash_) h = 0; lastMediaPage_ = 0; }
            hostLog("sides: %s", live ? "on (the four posters beside the board get their own pictures)" : "off");
        }
        if (!live) return;
        for (int i = 0; i < arcade::SIDES; i++) {
            uint64_t every = i == SIDE_LISTEN ? 125 : 500;  // the scrolling line moves; the rest hardly changes
            if (now - lastSide_[i] < every) continue;
            lastSide_[i] = now;
            target_ = &side_[i];
            if (config_.sharing) {
                switch (i) {
                    case SIDE_LISTEN: drawListening(); break;
                    case SIDE_CROSS: drawCrossPlay(); break;
                    case SIDE_DISCORD: drawDiscordPoster(); break;
                    default: drawFriendsPoster(); break;
                }
            } else {  // no sharing: listening, Discord, and the tablet on the two below
                switch (i) {
                    case SIDE_LISTEN: drawListening(); break;
                    case SIDE_CROSS: drawDiscordPoster(); break;
                    default: canvas_.blit(reinterpret_cast<const uint8_t*>(screen_.pixels()), SCREEN_W, SCREEN_H, SCREEN_W * 4, {0, 0, SCREEN_W, SCREEN_H}); break;
                }
            }
            canvas_.opaque();
            target_ = &screen_;
            publishSide(i);
        }
    }
    void publishSide(int i) {  // only when the picture changed (a sample of its pixels)
        const uint32_t* px = side_[i].pixels();
        uint64_t hash = 1469598103934665603ull;
        for (size_t k = 0; k < size_t(SCREEN_W) * SCREEN_H; k += 61) { hash ^= px[k]; hash *= 1099511628211ull; }
        if (hash == sideHash_[i]) return;
        sideHash_[i] = hash;
        InterlockedIncrement(&shared_->sideSerial[i]);  // odd: being written
        MemoryBarrier();
        memcpy(shared_->sideFrames[i], px, arcade::FRAME_BYTES);
        MemoryBarrier();
        InterlockedIncrement(&shared_->sideSerial[i]);  // even: whole
    }
    void drawCrossPlay() {
        canvas_.clear(rgb(8, 8, 12));
        if (share_.running() && share_.split()) {
            canvas_.blit(reinterpret_cast<const uint8_t*>(split_.pixels()), SCREEN_W, SCREEN_H, SCREEN_W * 4, {0, 0, SCREEN_W, SCREEN_H});
        } else if (watch_.active()) {
            auto pic = watch_.latest();
            if (pic && pic->w) canvas_.blit(pic->bgra.data(), pic->w, pic->h, pic->w * 4, {0, 0, SCREEN_W, SCREEN_H});
            else canvas_.text({0, 240, SCREEN_W, 290}, watch_.status(), 28, rgb(200, 205, 220));
        } else {
            canvas_.text({0, 150, SCREEN_W, 210}, L"CROSS PLAY", 44, rgb(230, 120, 220));
            canvas_.text({0, 222, SCREEN_W, 262}, L"Your friends' tablets appear here", 24, rgb(200, 205, 220), false);
            canvas_.text({0, 268, SCREEN_W, 306}, L"Tablet: SPLIT SCREEN, then they join with your code in WATCH A FRIEND", 20, rgb(150, 160, 180), false);
            if (share_.running() && !share_.code().empty()) canvas_.text({0, 330, SCREEN_W, 400}, share_.code(), 56, rgb(90, 200, 255));
        }
        button(SIDE_BIG_BUTTON, L"\x25B2  TO THE BIG SCREEN", pressed_ == 840, rgb(50, 58, 84), 22);
    }
    void drawDiscordPoster() {
        canvas_.clear(rgb(18, 19, 26));
        canvas_.fill({0, 0, SCREEN_W, 70}, rgb(88, 101, 242));
        canvas_.text({24, 0, 600, 70}, L"DISCORD", 40, rgb(255, 255, 255), true, 0);
        const DiscordState& s = dstate_;
        if (!discord_.configured() || !s.authed) {
            canvas_.text({0, 240, SCREEN_W, 290}, discord_.configured() ? (s.status.empty() ? L"Connecting..." : s.status) : L"Not set up (DISCORD tab on the tablet)", 26, rgb(200, 205, 220), false);
            return;
        }
        canvas_.text({SCREEN_W - 420, 0, SCREEN_W - 24, 70}, s.me.name, 24, rgb(230, 235, 255), false, 2);
        if (s.channelId.empty()) {
            canvas_.text({0, 150, SCREEN_W, 200}, L"Not in a voice call", 32, rgb(200, 205, 220));
        } else {
            canvas_.text({24, 80, SCREEN_W - 24, 112}, L"In  " + s.channelName + L"   -   " + std::to_wstring(s.members.size()) + (s.members.size() == 1 ? L" person" : L" people"), 22, rgb(200, 210, 230), false, 0);
            int n = std::min<int>(8, int(s.members.size()));
            int cols = n <= 4 ? n : 4, rows = n <= 4 ? 1 : 2;
            int cell = cols <= 2 ? 300 : 244, avatar = rows == 1 ? 150 : 110;
            int x0 = (SCREEN_W - cols * cell) / 2, y0 = rows == 1 ? 150 : 118;
            for (int i = 0; i < n; i++) {
                const DiscordMember& m = s.members[size_t(i)];
                int x = x0 + (i % cols) * cell, y = y0 + (i / cols) * (avatar + 70);
                Rect box{x + (cell - avatar) / 2, y, x + (cell - avatar) / 2 + avatar, y + avatar};
                drawAvatar(m.user, box, m.speaking ? rgb(60, 220, 110) : rgb(50, 54, 72), m.speaking ? 8 : 3);
                canvas_.text({x, y + avatar + 4, x + cell, y + avatar + 34}, m.user.name, 22, m.self ? rgb(90, 200, 255) : rgb(240, 240, 245), true);
                std::wstring tag = m.deaf ? L"deafened" : m.mute ? L"muted" : m.speaking ? L"talking" : L"";
                if (!tag.empty()) canvas_.text({x, y + avatar + 34, x + cell, y + avatar + 58}, tag, 17, m.speaking ? rgb(60, 220, 110) : rgb(255, 120, 120), false);
            }
        }
        // the newest messages along the bottom
        int y = SCREEN_H - 30;
        int shown = 0;
        for (auto& note : s.notes) {
            if (shown++ >= 3) break;
            canvas_.fill({0, y - 36, SCREEN_W, y}, rgb(24, 26, 36));
            drawAvatar(note.from, {12, y - 33, 42, y - 3}, rgb(50, 54, 72), 1);
            canvas_.text({52, y - 36, SCREEN_W - 12, y}, note.from.name + L":  " + note.text, 18, rgb(220, 225, 235), false, 0);
            y -= 38;
        }
    }
    void drawFriendsPoster() {
        canvas_.clear(rgb(12, 14, 22));
        canvas_.text({24, 10, 600, 70}, L"FRIENDS", 40, rgb(90, 200, 255), true, 0);
        int y = 90;
        if (share_.running()) {
            std::wstring c = share_.code();
            canvas_.text({24, y, SCREEN_W - 24, y + 34}, share_.split() ? L"Split screen is on. Friends join with:" : L"Your tablet is shared. Friends watch with:", 24, rgb(200, 210, 230), false, 0);
            canvas_.text({24, y + 40, SCREEN_W - 24, y + 110}, c.empty() ? share_.lanCode() : c, 56, rgb(255, 255, 255), true, 0);
            {
                std::wstring more;
                if (!share_.vpnCode().empty()) more = share_.vpnName() + L" code " + share_.vpnCode();
                if (!c.empty() && !share_.lanCode().empty()) more += (more.empty() ? L"" : L"   -   ") + std::wstring(L"LAN code ") + share_.lanCode();
                if (!more.empty()) canvas_.text({24, y + 112, SCREEN_W - 24, y + 142}, more, 20, rgb(150, 160, 180), false, 0);
            }
            int n = share_.viewers();
            canvas_.text({24, y + 150, SCREEN_W - 24, y + 184}, std::to_wstring(n) + (n == 1 ? L" friend connected" : L" friends connected"), 24, rgb(200, 210, 230), false, 0);
            y += 200;
        } else {
            canvas_.text({24, y, SCREEN_W - 24, y + 34}, L"Not sharing. Tablet: SHARE MY TABLET or SPLIT SCREEN gives friends a code.", 22, rgb(200, 210, 230), false, 0);
            y += 50;
        }
        auto list = friends();
        if (!list.empty()) {
            canvas_.text({24, y, SCREEN_W - 24, y + 30}, L"Friends you have watched:", 20, rgb(150, 160, 180), false, 0);
            y += 36;
            for (auto& f : list) {
                if (y > SCREEN_H - 40) break;
                canvas_.text({40, y, SCREEN_W - 24, y + 30}, f.first + L"    " + f.second, 22, rgb(220, 225, 235), false, 0);
                y += 34;
            }
        }
        if (watch_.active()) canvas_.text({24, SCREEN_H - 40, SCREEN_W - 24, SCREEN_H - 8}, L"Watching " + watch_.friendName(), 20, rgb(90, 200, 255), false, 0);
    }
    void sideTouch(const Touch& t) {
        if (t.side != SIDE_CROSS || !config_.sharing) { if (!t.down) pressed_ = -1; return; }
        if (tapped(t, 840, SIDE_BIG_BUTTON)) {
            if (share_.running() && share_.split()) { mode_ = Mode::Split; splitBarUntil_ = GetTickCount64() + 6000; }
            else if (watch_.active()) { mode_ = Mode::Watch; watchBarUntil_ = GetTickCount64() + 6000; }
            else splitTapped();
            resetInput();
            return;
        }
        if (!t.down) pressed_ = -1;
    }

    void drawMedia() {
        if (mediaListen_) { drawListening(); return; }
        canvas_.clear(rgb(14, 16, 24));
        header(L"MEDIA", true);
        if (mediaNow_.valid) {
            canvas_.text({40, 96, SCREEN_W - 40, 140}, mediaNow_.title.empty() ? L"(no title)" : mediaNow_.title, 30, rgb(245, 245, 250), true, 0);
            canvas_.text({40, 140, SCREEN_W - 40, 172}, mediaNow_.artist, 22, rgb(180, 190, 210), false, 0);
            canvas_.text({40, 172, SCREEN_W - 40, 200}, (mediaNow_.playing ? L"playing  -  " : L"paused  -  ") + mediaApp_, 18, rgb(130, 140, 160), false, 0);
        } else {
            canvas_.text({40, 96, SCREEN_W - 40, 140}, L"Nothing is playing", 30, rgb(200, 205, 220), true, 0);
            canvas_.text({40, 140, SCREEN_W - 40, 172}, L"Start Spotify, a YouTube video in your browser, or a video here, and it shows up.", 20, rgb(150, 160, 180), false, 0);
        }
        button(MEDIA_PREV, L"\x25C0\x25C0  PREV", pressed_ == 900, rgb(50, 58, 84), 26);
        button(MEDIA_PLAY, mediaNow_.valid && mediaNow_.playing ? L"PAUSE" : L"PLAY", pressed_ == 901, rgb(40, 110, 60), 26);
        button(MEDIA_NEXT, L"NEXT  \x25B6\x25B6", pressed_ == 902, rgb(50, 58, 84), 26);
        button(MEDIA_VOLDOWN, L"VOL \x2212", pressed_ == 903, rgb(50, 58, 84), 26);
        button(MEDIA_MUTE, mediaMuted_ ? L"UNMUTE" : L"MUTE", pressed_ == 904, mediaMuted_ ? rgb(150, 50, 50) : rgb(50, 58, 84), 26);
        button(MEDIA_VOLUP, L"VOL +", pressed_ == 905, rgb(50, 58, 84), 26);
        std::wstring vol = mediaVolume_ < 0 ? L"Volume: ?" : L"Volume " + std::to_wstring(int(mediaVolume_ * 100 + .5f)) + L"%" + (mediaMuted_ ? L"  (muted)" : L"");
        canvas_.text({40, 476, 640, 516}, vol, 24, rgb(90, 200, 255), true, 0);
        button(MEDIA_LISTEN, L"LISTENING MODE", pressed_ == 906, rgb(60, 50, 110), 20);
        messageBar();
    }

    // ---- SHARE MY TABLET / WATCH A FRIEND (share.h): this screen streamed to friends, a
    // friend's screen shown here.
    static std::wstring upper(std::wstring s) { for (auto& ch : s) ch = wchar_t(towupper(ch)); return s; }
    std::wstring shareName() const {
        std::wstring n = iniText(tweaksPath(), L"share", L"name", L"");
        if (n.empty()) {
            wchar_t pc[MAX_COMPUTERNAME_LENGTH + 1];
            DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
            if (GetComputerNameW(pc, &len)) n = pc;
        }
        return n.empty() ? L"a friend" : n;
    }
    std::wstring tileSub(const AppInfo& a) const {
        if (background_ && session_.alive() && sameApp(a, backgroundApp_)) return L"RUNNING - tap to go back to it";
        if (a.id == AppId::Split && share_.running() && share_.split()) {
            int n = share_.viewers();
            return L"ON - " + std::to_wstring(n) + (n == 1 ? L" friend connected" : L" friends connected") + L" - tap to look";
        }
        if (a.id == AppId::Share && share_.running() && share_.split()) return L"SPLIT SCREEN is sharing - tap for details";
        if (a.id == AppId::Share && share_.running()) {
            int n = share_.viewers();
            if (share_.code().empty() && share_.lanCode().empty()) return share_.status().empty() ? L"SHARING - finding your address..." : share_.status();
            return n == 0 ? L"SHARING - nobody watching yet; tap for details" : L"SHARING - " + std::to_wstring(n) + (n == 1 ? L" friend watching" : L" friends watching");
        }
        if (a.id == AppId::Watch && watch_.active()) return L"Watching " + watch_.friendName() + L" - tap to go back to it";
        return a.subtitle;
    }
    std::wstring tileNote(const AppInfo& a) const {
        if ((a.id == AppId::Share || a.id == AppId::Split) && share_.running()) {
            std::wstring c = share_.code(), vpn = share_.vpnCode();
            if (!vpn.empty() && !share_.status().empty()) return vpn;  // the router did not open the port: the VPN code is the one that works
            return c.empty() ? share_.lanCode() : c;
        }
        return a.missing;
    }
    void shareTapped() {
        if (!share_.running()) {
            share_.start(uint16_t(config_.sharePort), shareName(), false);
            message_ = share_.running() ? L"Sharing your tablet. The code appears on the tile in a moment; tap the tile again for the LAN code or to stop."
                                        : share_.status();
            return;
        }
        Screen s{L"SHARE MY TABLET"};
        std::wstring c = share_.code(), lan = share_.lanCode(), st = share_.status();
        s.entries.push_back({c.empty() ? L"Finding your internet address..." : L"Your code:   " + c,
                             L"Friends type it in WATCH A FRIEND > Enter a code (from anywhere on the internet)", nullptr});
        if (!share_.vpnCode().empty()) s.entries.push_back({L"VPN code:   " + share_.vpnCode(), L"For friends on your " + share_.vpnName() + L" network: works from anywhere, no router setup", nullptr});
        if (!lan.empty()) s.entries.push_back({L"LAN code:   " + lan, L"For a friend on the same Wi-Fi", nullptr});
        if (!st.empty()) s.entries.push_back({st, L"The router did not open the port by itself: forward it, or friends use the LAN code", nullptr});
        int n = share_.viewers();
        s.entries.push_back({std::to_wstring(n) + (n == 1 ? L" friend watching" : L" friends watching"),
                             L"Any number can watch at once; they see this screen and hear the app on it (Windows 11)", nullptr});
        s.entries.push_back({L"STOP SHARING", L"Nobody can watch after this", [this] { share_.stop(); message_ = L"Sharing stopped."; toLauncher(); }});
        push(s);
    }
    DWORD shareAudioPid() const {
        if (mode_ == Mode::Screen && screenWin_) {
            DWORD pid = 0;
            GetWindowThreadProcessId(screenWin_, &pid);
            return pid;
        }
        if ((mode_ == Mode::Running || mode_ == Mode::Menu) && session_.alive()) return session_.audioPid();
        return 0;
    }
    std::wstring friendsPath() const { return localAppData() + L"\\EchoArcade\\friends.txt"; }
    std::vector<std::pair<std::wstring, std::wstring>> friends() const {  // name, code
        std::vector<std::pair<std::wstring, std::wstring>> out;
        std::string text;
        if (!readFile(friendsPath(), text)) return out;
        std::wstring all = widenUtf8(text), line;
        auto trim = [](std::wstring s) {
            while (!s.empty() && (s.back() == L' ' || s.back() == L'\r' || s.back() == L'\t')) s.pop_back();
            size_t i = 0;
            while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) i++;
            return s.substr(i);
        };
        for (size_t i = 0; i <= all.size(); i++) {
            if (i < all.size() && all[i] != L'\n') { line += all[i]; continue; }
            size_t bar = line.find(L'|');
            if (bar != std::wstring::npos && line[0] != L'#') {
                std::wstring name = trim(line.substr(0, bar)), code = trim(line.substr(bar + 1));
                if (!code.empty()) out.push_back({name.empty() ? code : name, code});
            }
            line.clear();
        }
        return out;
    }
    void rememberFriend(const std::wstring& name, const std::wstring& code) {
        auto list = friends();
        list.erase(std::remove_if(list.begin(), list.end(), [&](auto& f) { return f.second == code; }), list.end());
        list.insert(list.begin(), {name, code});
        if (list.size() > 8) list.resize(8);
        std::string out = "# WATCH A FRIEND remembers the last codes here, one a line:  Name | code\n";
        for (auto& f : list) out += narrowUtf8(f.first) + " | " + narrowUtf8(f.second) + "\n";
        FILE* f = nullptr;
        if (_wfopen_s(&f, friendsPath().c_str(), L"wb") == 0 && f) { fwrite(out.data(), 1, out.size(), f); fclose(f); }
    }
    void openWatch() {
        if (watch_.active()) { mode_ = Mode::Watch; watchBarUntil_ = GetTickCount64() + 6000; resetInput(); return; }
        Screen s{L"WATCH A FRIEND"};
        s.entries.push_back({L"Enter a code", L"The code on your friend's SHARE MY TABLET tile", [this] { keyboardForWatch_ = true; mode_ = Mode::Keyboard; resetInput(); }});
        for (auto& f : friends()) s.entries.push_back({f.first, f.second, [this, f] { startWatch(f.second); }});
        if (share_.running() && !share_.code().empty()) s.entries.push_back({L"Your own code:   " + share_.code(), L"What friends type to watch you", nullptr});
        push(s);
    }
    void startWatch(const std::wstring& code) {
        AudioTarget audio = findAudioDevice(config_.audioDevice);
        watch_.connect(code, shareName(), audio.id);
        if (!watch_.active()) { message_ = watch_.status(); return; }
        keyboardForWatch_ = false;
        query_.clear();
        watchRemembered_ = false;
        mode_ = Mode::Watch;
        watchBarUntil_ = GetTickCount64() + 6000;
        resetInput();
    }
    void pollWatch() {
        if (watch_.active()) watch_.setState(mode_ == Mode::Watch ? ShareState::Looking : ShareState::Away);
        if (mode_ != Mode::Watch) return;
        if (watch_.connected() && !watchRemembered_) {
            watchRemembered_ = true;
            rememberFriend(watch_.friendName(), watch_.code());
        }
        if (!watch_.active()) watchBarUntil_ = UINT64_MAX;  // over: the bar stays, with what happened and the way back
    }
    void watchTouch(const Touch& t) {
        if (tapped(t, 820, WATCH_BACK)) { if (!watch_.active()) watch_.disconnect(); toLauncher(); return; }  // still connected: the WATCH tile comes back here
        if (tapped(t, 821, WATCH_LEAVE)) { watch_.disconnect(); toLauncher(); return; }
        if (t.down && pressed_ < 0) watchBarUntil_ = GetTickCount64() + 6000;  // a tap anywhere brings the bar
        if (!t.down) pressed_ = -1;
    }
    void drawWatch() {
        canvas_.clear(rgb(10, 10, 14));
        auto pic = watch_.latest();
        if (pic && pic->w) {
            canvas_.blit(pic->bgra.data(), pic->w, pic->h, pic->w * 4, {0, 0, SCREEN_W, SCREEN_H});
        } else {
            canvas_.text({40, 220, SCREEN_W - 40, 280}, watch_.status(), 28, rgb(220, 225, 235));
            if (watch_.active()) canvas_.text({0, 290, SCREEN_W, 330}, watch_.connected() ? L"Waiting for the first picture..." : L"", 20, rgb(140, 150, 170), false);
        }
        if (GetTickCount64() < watchBarUntil_) {
            canvas_.fill({0, 0, SCREEN_W, 58}, rgb(24, 24, 28));
            button(WATCH_BACK, L"\x25C0  MY TABLET", pressed_ == 820, rgb(50, 58, 84), 22);
            button(WATCH_LEAVE, L"LEAVE", pressed_ == 821, rgb(120, 50, 50), 22);
            canvas_.text({436, 0, SCREEN_W - 12, 58}, watch_.status() + (watch_.split() && watch_.connected() ? L"  (split screen)" : L""), 22, rgb(200, 210, 230), false, 0);
        }
    }

    // ---- SPLIT SCREEN: a share whose viewers send their tablets back. The layout of them
    // (2 halves, 3 three ways, 4 quadrants), with my own tablet while I am on it, is the
    // shared picture; in Mode::Split I look at it myself.
    void splitTapped() {
        if (share_.running() && !share_.split()) share_.stop();  // a plain share becomes the split screen
        if (!share_.running()) {
            share_.start(uint16_t(config_.sharePort), shareName(), true);
            if (!share_.running()) { message_ = share_.status(); return; }
            message_ = L"Split screen on. Friends join with the code (WATCH A FRIEND > Enter a code) and their tablets appear here.";
        }
        mode_ = Mode::Split;
        splitBarUntil_ = GetTickCount64() + 8000;
        resetInput();
    }
    static std::vector<Rect> splitRects(size_t n) {
        const int W = SCREEN_W, H = SCREEN_H;
        if (n <= 1) return {{0, 0, W, H}};
        if (n == 2) return {{0, 0, W / 2, H}, {W / 2, 0, W, H}};
        if (n == 3) return {{0, 0, W / 2, H / 2}, {W / 2, 0, W, H / 2}, {W / 4, H / 2, W * 3 / 4, H}};
        return {{0, 0, W / 2, H / 2}, {W / 2, 0, W, H / 2}, {0, H / 2, W / 2, H}, {W / 2, H / 2, W, H}};
    }
    void composeSplit() {
        if (!share_.running() || !share_.split()) return;
        struct Pane { std::wstring label; const uint8_t* pixels; int w, h, pitch; bool looking; };
        std::vector<Pane> panes;
        if (mode_ != Mode::Split) panes.push_back({L"You", reinterpret_cast<const uint8_t*>(screen_.pixels()), SCREEN_W, SCREEN_H, SCREEN_W * 4, false});
        auto parts = share_.participants();
        for (auto& p : parts) {
            if (panes.size() >= 4) break;
            if (p.picture && p.picture->w) panes.push_back({p.name, p.picture->bgra.data(), p.picture->w, p.picture->h, p.picture->w * 4, p.state == ShareState::Looking});
            else panes.push_back({p.name, nullptr, 0, 0, 0, p.state == ShareState::Looking});
        }
        split_.clear(rgb(8, 8, 12));
        if (panes.empty()) {
            split_.text({0, 140, SCREEN_W, 200}, L"SPLIT SCREEN", 44, rgb(230, 120, 220));
            split_.text({0, 212, SCREEN_W, 252}, L"Friends join in WATCH A FRIEND > Enter a code:", 24, rgb(200, 205, 220), false);
            std::wstring c = share_.code(), lan = share_.lanCode(), vpn = share_.vpnCode();
            bool vpnFirst = !vpn.empty() && !share_.status().empty();
            split_.text({0, 262, SCREEN_W, 332}, vpnFirst ? vpn : c.empty() ? (lan.empty() ? L"finding your address..." : lan) : c, 56, rgb(90, 200, 255));
            std::wstring more;
            if (vpnFirst) more = L"(" + share_.vpnName() + L" code)   internet code " + c;
            else if (!vpn.empty()) more = L"VPN code " + vpn;
            if (!lan.empty()) more += (more.empty() ? L"" : L"   -   ") + std::wstring(L"LAN code ") + lan;
            if (!more.empty()) split_.text({0, 342, SCREEN_W, 377}, more, 22, rgb(150, 160, 180), false);
            std::wstring st = share_.status();
            if (!st.empty()) split_.text({0, 392, SCREEN_W, 427}, st, 22, rgb(255, 170, 90), false);
            return;
        }
        auto rects = splitRects(panes.size());
        for (size_t i = 0; i < panes.size(); i++) {
            Rect r = rects[i];
            Rect inner{r.x0 + 2, r.y0 + 2, r.x1 - 2, r.y1 - 2};
            if (panes[i].pixels) {
                split_.blit(panes[i].pixels, panes[i].w, panes[i].h, panes[i].pitch, inner);
            } else {
                split_.fill(inner, rgb(22, 24, 34));
                int mid = (inner.y0 + inner.y1) / 2;
                split_.text({inner.x0, mid - 24, inner.x1, mid + 24}, panes[i].label + (panes[i].looking ? L" is looking at the split screen" : L""), 22, rgb(170, 180, 200), false);
            }
            split_.fill({inner.x0, inner.y1 - 26, std::min(inner.x1, inner.x0 + 20 + int(panes[i].label.size()) * 11), inner.y1}, rgb(0, 0, 0));
            split_.text({inner.x0 + 8, inner.y1 - 26, inner.x1, inner.y1}, panes[i].label, 18, rgb(240, 240, 245), true, 0);
        }
    }
    void drawSplit() {
        canvas_.blit(reinterpret_cast<const uint8_t*>(split_.pixels()), SCREEN_W, SCREEN_H, SCREEN_W * 4, {0, 0, SCREEN_W, SCREEN_H});
        if (GetTickCount64() < splitBarUntil_) {
            canvas_.fill({0, 0, SCREEN_W, 58}, rgb(24, 24, 28));
            button(SPLIT_STOP, L"STOP", pressed_ == 830, rgb(140, 50, 50), 22);
            button(SPLIT_MINE, L"\x25C0  MY TABLET", pressed_ == 831, rgb(50, 58, 84), 22);
            int n = share_.viewers();
            std::wstring c = share_.code();
            canvas_.text({430, 0, SCREEN_W - 12, 58}, (c.empty() ? share_.lanCode() : c) + L"   -   " + std::to_wstring(n) + (n == 1 ? L" friend connected" : L" friends connected"),
                         22, rgb(200, 210, 230), false, 0);
        }
    }
    void splitTouch(const Touch& t) {
        if (tapped(t, 830, SPLIT_STOP)) { share_.stop(); message_ = L"Split screen off."; toLauncher(); return; }
        if (tapped(t, 831, SPLIT_MINE)) { toLauncher(); return; }  // it goes on; the SPLIT SCREEN tile comes back here
        if (t.down && pressed_ < 0) splitBarUntil_ = GetTickCount64() + 6000;  // a tap anywhere brings the bar
        if (!t.down) pressed_ = -1;
    }
    // My tablet for a friend's split screen: my screen, or, while I look at theirs, the app
    // running under it (a video keeps playing); nothing while I only look.
    void offerMyTablet() {
        if (!watch_.split() || !watch_.connected()) return;
        if (mode_ != Mode::Watch) { watch_.offerUp(reinterpret_cast<const uint8_t*>(screen_.pixels()), SCREEN_W, SCREEN_H, SCREEN_W * 4); return; }
        if (session_.alive() && session_.capture().latest(upPixels_, upW_, upH_, upSerial_)) watch_.offerUp(upPixels_.data(), upW_, upH_, upW_ * 4);
    }

    // ---- DISCORD (discord.h): the page on the DISCORD tab, an incoming call over any page,
    // and the block the HUD reads.
    void pollDiscord(uint64_t now) {
        uint64_t serial = discord_.serial();
        if (serial != dstateSerial_) {
            dstate_ = discord_.state();
            dstateSerial_ = serial;
            if (dstate_.call.at && now - dstate_.call.at > 45000) discord_.dismissCall();  // nobody answered
            writeDiscordShared(now);
        } else if (dstate_.call.at && now - dstate_.call.at > 45000) {
            discord_.dismissCall();
        }
        if (dshared_ && now - dsharedBeat_ > 1000) { dshared_->heartbeat = LONG64(now); dsharedBeat_ = now; }
    }
    void openDiscordShared() {
        dmap_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(arcade::DiscordShared), arcade::DISCORD_SHARED_NAME);
        if (!dmap_) return;
        dshared_ = static_cast<arcade::DiscordShared*>(MapViewOfFile(dmap_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(arcade::DiscordShared)));
        if (!dshared_) return;
        dshared_->magic = arcade::DISCORD_MAGIC;
        dshared_->version = arcade::DISCORD_VERSION;
    }
    static void avatarTo48(const SharePicture* pic, uint8_t* out, LONG& has) {
        has = pic && pic->w ? 1 : 0;
        if (!has) { memset(out, 0, size_t(arcade::DISCORD_AVATAR) * arcade::DISCORD_AVATAR * 4); return; }
        const int N = arcade::DISCORD_AVATAR;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                int sx = x * pic->w / N, sy = y * pic->h / N;
                const uint8_t* p = pic->bgra.data() + (size_t(sy) * pic->w + sx) * 4;
                uint8_t* o = out + (size_t(y) * N + x) * 4;
                o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255;
            }
    }
    static void copyName(wchar_t* out, const std::wstring& s) {
        size_t n = std::min<size_t>(31, s.size());
        memcpy(out, s.data(), n * sizeof(wchar_t));
        out[n] = 0;
    }
    void writeDiscordShared(uint64_t now) {
        if (!dshared_) return;
        auto& d = *dshared_;
        InterlockedIncrement(&d.serial);  // odd: being written
        MemoryBarrier();
        d.connected = dstate_.authed ? 1 : 0;
        d.inVoice = dstate_.channelId.empty() ? 0 : 1;
        int n = 0;
        for (auto& m : dstate_.members) {
            if (n >= arcade::DISCORD_MAX_MEMBERS) break;
            auto& o = d.members[n++];
            copyName(o.name, m.user.name);
            o.speaking = m.speaking; o.self = m.self; o.mute = m.mute; o.deaf = m.deaf;
            avatarTo48(discord_.avatar(m.user, 64).get(), o.picture.rgba, o.hasPicture);
        }
        d.memberCount = n;
        d.unread = dstate_.unread;
        d.lastMessageAt = dstate_.notes.empty() ? 0 : LONG64(dstate_.notes.front().at);
        if (!dstate_.notes.empty()) {
            copyName(d.lastFrom, dstate_.notes.front().from.name);
            avatarTo48(discord_.avatar(dstate_.notes.front().from, 64).get(), d.lastPicture.rgba, d.lastHasPicture);
        }
        d.ringing = dstate_.call.at ? 1 : 0;
        if (dstate_.call.at) {
            copyName(d.caller, dstate_.call.from.name);
            avatarTo48(discord_.avatar(dstate_.call.from, 64).get(), d.callerPicture.rgba, d.callerHasPicture);
        }
        d.heartbeat = LONG64(now);
        MemoryBarrier();
        InterlockedIncrement(&d.serial);  // even: done
        dsharedBeat_ = now;
    }
    void drawAvatar(const DiscordUser& u, Rect box, uint32_t ring, int ringWidth) {
        auto pic = discord_.avatar(u, 128);
        Rect inner{box.x0 + ringWidth, box.y0 + ringWidth, box.x1 - ringWidth, box.y1 - ringWidth};
        if (pic && pic->w) canvas_.blit(pic->bgra.data(), pic->w, pic->h, pic->w * 4, inner);
        else {
            canvas_.fill(inner, rgb(60, 64, 90));
            canvas_.text(inner, u.name.empty() ? L"?" : u.name.substr(0, 1), inner.h() / 2, rgb(230, 230, 240));
        }
        if (ringWidth > 0) canvas_.frame(box, ring, ringWidth);
    }
    Rect dRowRect(int i) const { return {12, 84 + i * 72, 904, 84 + i * 72 + 66}; }
    void drawDiscord() {
        canvas_.clear(rgb(18, 19, 26));
        const DiscordState& s = dstate_;
        if (!discord_.configured()) {
            header(L"DISCORD", false);
            const wchar_t* lines[] = {
                L"One-time setup on the PC (Discord only talks to an app you made yourself):",
                L"1.  discord.com/developers/applications  >  New Application (any name, e.g. Echo Arcade)",
                L"2.  OAuth2  >  Redirects  >  add   http://localhost   and save",
                L"3.  Copy the Application ID and the Client Secret (OAuth2 page) into",
                L"     plugins\\EchoArcade\\echo_tweaks.ini  under  [discord]  as  client_id=  and  client_secret=",
                L"The tablet notices the file within a second. Discord must be running on this PC."};
            int y = 110;
            for (auto l : lines) { canvas_.text({40, y, SCREEN_W - 40, y + 40}, l, 21, rgb(210, 215, 230), false, 0); y += 54; }
            return;
        }
        if (!s.authed) {
            header(L"DISCORD", false);
            canvas_.text({40, 110, SCREEN_W - 40, 150}, s.status.empty() ? L"Connecting to Discord..." : s.status, 24, rgb(220, 225, 235), false, 0);
            if (s.pipe && !s.busy) {
                button(DC_CONNECT, L"CONNECT", pressed_ == 950, rgb(88, 101, 242), 34);
                canvas_.text({40, 400, SCREEN_W - 40, 440}, L"Discord opens a small Authorize box on your desktop the first time; click it once (take the headset off for that).", 19, rgb(150, 160, 180), false, 0);
            }
            if (!s.me.name.empty()) canvas_.text({40, 150, SCREEN_W - 40, 185}, L"Discord's user on this PC: " + s.me.name, 20, rgb(150, 160, 180), false, 0);
            return;
        }
        if (discordView_ == 1) { drawDiscordList(L"MESSAGES", int(s.notes.size()), [&](int i, Rect r) {
                const DiscordNote& n = s.notes[size_t(i)];
                drawAvatar(n.from, {r.x0 + 8, r.y0 + 5, r.x0 + 64, r.y0 + 61}, rgb(60, 64, 90), 2);
                canvas_.text({r.x0 + 76, r.y0 + 2, r.x1 - 12, r.y0 + 34}, n.from.name + (n.title.empty() || n.title == n.from.name ? L"" : L"  -  " + n.title), 22, rgb(240, 240, 245), true, 0);
                canvas_.text({r.x0 + 76, r.y0 + 34, r.x1 - 12, r.y1 - 2}, n.text, 18, rgb(170, 180, 200), false, 0);
            }); return; }
        if (discordView_ == 2) { drawDiscordList(L"JOIN A CALL - SERVERS", int(dGuilds_.size()), [&](int i, Rect r) {
                canvas_.text({r.x0 + 24, r.y0, r.x1 - 12, r.y1}, dGuilds_[size_t(i)].name, 26, rgb(240, 240, 245), true, 0);
            }); return; }
        if (discordView_ == 3) { drawDiscordList(L"JOIN A CALL - " + dGuildName_, int(dChannels_.size()), [&](int i, Rect r) {
                canvas_.text({r.x0 + 24, r.y0, r.x1 - 12, r.y1}, L"\x25CF  " + dChannels_[size_t(i)].name, 26, rgb(240, 240, 245), true, 0);
            }); return; }
        // home: the channel and its people
        header(L"DISCORD", false);
        if (!s.me.name.empty()) {
            drawAvatar(s.me, {SCREEN_W - 236, 14, SCREEN_W - 188, 62}, rgb(60, 64, 90), 2);
            canvas_.text({SCREEN_W - 180, 14, SCREEN_W - 20, 62}, s.me.name, 20, rgb(200, 210, 230), false, 0);
        }
        button(DC_SIGNOUT, L"SIGN OUT", pressed_ == 951, rgb(50, 58, 84), 16);
        if (!s.status.empty()) canvas_.text({24, 78, SCREEN_W - 24, 106}, s.status, 18, rgb(255, 170, 90), false, 0);
        if (s.channelId.empty()) {
            canvas_.text({0, 180, SCREEN_W, 230}, L"Not in a voice call", 32, rgb(200, 205, 220));
            button({312, 260, 712, 340}, L"JOIN A CALL", pressed_ == 952, rgb(88, 101, 242), 30);
        } else {
            canvas_.text({24, 104, SCREEN_W - 24, 136}, L"In  " + s.channelName + L"   -   " + std::to_wstring(s.members.size()) + (s.members.size() == 1 ? L" person" : L" people"), 22, rgb(200, 210, 230), false, 0);
            int n = std::min<int>(8, int(s.members.size()));
            for (int i = 0; i < n; i++) {
                const DiscordMember& m = s.members[size_t(i)];
                int col = i % 4, row = i / 4;
                int x = 24 + col * 244, y = 146 + row * 168;
                Rect box{x + 66, y, x + 66 + 112, y + 112};
                drawAvatar(m.user, box, m.speaking ? rgb(60, 220, 110) : rgb(50, 54, 72), m.speaking ? 6 : 3);
                canvas_.text({x, y + 116, x + 244, y + 142}, m.user.name, 20, m.self ? rgb(90, 200, 255) : rgb(240, 240, 245), true);
                std::wstring tag = m.deaf ? L"deafened" : m.mute ? L"muted" : m.speaking ? L"talking" : L"";
                if (!tag.empty()) canvas_.text({x, y + 142, x + 244, y + 164}, tag, 16, m.speaking ? rgb(60, 220, 110) : rgb(255, 120, 120), false);
            }
        }
        button(DC_MUTE, s.selfMute ? L"UNMUTE" : L"MUTE", pressed_ == 953, s.selfMute ? rgb(150, 50, 50) : rgb(50, 58, 84), 22);
        button(DC_DEAF, s.selfDeaf ? L"UNDEAFEN" : L"DEAFEN", pressed_ == 954, s.selfDeaf ? rgb(150, 50, 50) : rgb(50, 58, 84), 22);
        button(DC_LEAVE, s.channelId.empty() ? L"JOIN A CALL" : L"LEAVE CALL", pressed_ == 955, s.channelId.empty() ? rgb(88, 101, 242) : rgb(120, 50, 50), 22);
        button(DC_MSGS, s.unread ? L"MESSAGES  +" + std::to_wstring(s.unread) : L"MESSAGES", pressed_ == 956, s.unread ? rgb(88, 101, 242) : rgb(50, 58, 84), 22);
    }
    void drawDiscordList(const std::wstring& title, int count, const std::function<void(int, Rect)>& row) {
        header(title, true);
        int pages = std::max(1, (count + DC_ROWS - 1) / DC_ROWS);
        if (dPage_ >= pages) dPage_ = pages - 1;
        if (dPage_ < 0) dPage_ = 0;
        for (int i = 0; i < DC_ROWS; i++) {
            int index = dPage_ * DC_ROWS + i;
            if (index >= count) break;
            Rect r = dRowRect(i);
            canvas_.fill(r, pressed_ == 960 + i ? rgb(48, 56, 80) : rgb(30, 34, 50));
            canvas_.fill({r.x0, r.y0, r.x0 + 6, r.y1}, rgb(88, 101, 242));
            row(index, r);
        }
        if (count == 0) canvas_.text({0, 240, 904, 290}, dBusy_ ? L"Loading..." : L"Nothing here", 26, rgb(150, 160, 180));
        button(DC_UP, L"\x25B2", pressed_ == 970, rgb(40, 44, 62), 40);
        button(DC_DOWN, L"\x25BC", pressed_ == 971, rgb(40, 44, 62), 40);
        canvas_.text({12, 536, 500, 568}, L"page " + std::to_wstring(dPage_ + 1) + L" of " + std::to_wstring(pages) + L"  -  " + std::to_wstring(count) + L" items", 18, rgb(130, 140, 160), false, 0);
    }
    void discordTouch(const Touch& t) {
        const DiscordState& s = dstate_;
        if (!discord_.configured()) { if (!t.down) pressed_ = -1; return; }
        if (!s.authed) {
            if (tapped(t, 950, DC_CONNECT) && s.pipe && !s.busy) discord_.authorize();
            if (!t.down) pressed_ = -1;
            return;
        }
        if (discordView_ != 0) {
            if (tapped(t, 100, BACK_BUTTON)) { discordView_ = discordView_ == 3 ? 2 : 0; dPage_ = 0; return; }
            if (tapped(t, 970, DC_UP)) { if (dPage_ > 0) dPage_--; return; }
            if (tapped(t, 971, DC_DOWN)) { dPage_++; return; }
            for (int i = 0; i < DC_ROWS; i++)
                if (tapped(t, 960 + i, dRowRect(i))) {
                    int index = dPage_ * DC_ROWS + i;
                    if (discordView_ == 2 && index < int(dGuilds_.size())) { openDiscordChannels(dGuilds_[size_t(index)]); }
                    else if (discordView_ == 3 && index < int(dChannels_.size())) { discord_.joinChannel(dChannels_[size_t(index)].id); discordView_ = 0; dPage_ = 0; }
                    return;
                }
            if (!t.down) pressed_ = -1;
            return;
        }
        if (tapped(t, 951, DC_SIGNOUT)) { discord_.signOut(); return; }
        if (tapped(t, 953, DC_MUTE)) { discord_.setMute(!s.selfMute); return; }
        if (tapped(t, 954, DC_DEAF)) { discord_.setDeaf(!s.selfDeaf); return; }
        if (tapped(t, 955, DC_LEAVE) || (s.channelId.empty() && tapped(t, 952, {312, 260, 712, 340}))) {
            if (s.channelId.empty()) openDiscordGuilds(); else discord_.leaveChannel();
            return;
        }
        if (tapped(t, 956, DC_MSGS)) { discordView_ = 1; dPage_ = 0; discord_.markRead(); return; }
        if (!t.down) pressed_ = -1;
    }
    void openDiscordGuilds() {
        discordView_ = 2;
        dPage_ = 0;
        dGuilds_.clear();
        dBusy_ = true;
        discord_.listGuilds([this](std::vector<DiscordRef> guilds, std::wstring error) {
            std::lock_guard<std::mutex> lock(pendingLock_);
            pending_.push_back([this, guilds, error] { dGuilds_ = guilds; dBusy_ = false; if (!error.empty()) message_ = L"Discord: " + error; });
        });
    }
    void openDiscordChannels(const DiscordRef& guild) {
        discordView_ = 3;
        dPage_ = 0;
        dGuildName_ = guild.name;
        dChannels_.clear();
        dBusy_ = true;
        discord_.listVoiceChannels(guild.id, [this](std::vector<DiscordRef> channels, std::wstring error) {
            std::lock_guard<std::mutex> lock(pendingLock_);
            pending_.push_back([this, channels, error] { dChannels_ = channels; dBusy_ = false; if (!error.empty()) message_ = L"Discord: " + error; });
        });
    }
    // An incoming call, over whatever the tablet shows: who, ANSWER (join their call), IGNORE.
    void drawCallOverlay() {
        if (!dstate_.call.at) return;
        const DiscordNote& c = dstate_.call;
        canvas_.fill({0, 0, SCREEN_W, SCREEN_H}, rgb(10, 11, 16));
        drawAvatar(c.from, {432, 70, 592, 230}, rgb(88, 101, 242), 6);
        canvas_.text({0, 244, SCREEN_W, 294}, c.from.name + L" is calling", 36, rgb(245, 245, 250));
        canvas_.text({0, 296, SCREEN_W, 326}, L"Discord", 20, rgb(150, 160, 180), false);
        button(CALL_ANSWER, L"ANSWER", pressed_ == 980, rgb(40, 160, 80), 30);
        button(CALL_IGNORE, L"IGNORE", pressed_ == 981, rgb(170, 40, 50), 30);
    }
    bool callTouch(const Touch& t) {
        if (tapped(t, 980, CALL_ANSWER)) { discord_.joinChannel(dstate_.call.channelId); discord_.dismissCall(); return true; }
        if (tapped(t, 981, CALL_IGNORE)) { discord_.dismissCall(); return true; }
        if (!t.down) pressed_ = -1;
        return true;  // nothing else while it rings
    }

    void drawPlexLink() {
        canvas_.clear(rgb(14, 16, 24));
        header(L"LINK PLEX", true);
        canvas_.text({0, 110, SCREEN_W, 160}, L"On your phone or PC, go to", 28, rgb(200, 205, 220), false);
        canvas_.text({0, 160, SCREEN_W, 230}, L"plex.tv/link", 52, rgb(229, 160, 13));
        canvas_.text({0, 240, SCREEN_W, 280}, L"and enter this code:", 28, rgb(200, 205, 220), false);
        canvas_.fill({312, 300, 712, 420}, rgb(30, 34, 50));
        canvas_.text({312, 300, 712, 420}, linkCode_, 80, rgb(255, 255, 255));
        canvas_.text({0, 450, SCREEN_W, 500}, L"This screen continues by itself once you approve.", 22, rgb(150, 160, 180), false);
    }

    void drawScreen() {
        canvas_.clear(rgb(15, 15, 15));
        screenCap_.latest(screenPixels_, scrW_, scrH_, screenSerial_);
        pageShown_ = Rect{};
        if (scrW_ > 0 && scrH_ > 0) {
            RECT f{};
            frame_ = windowFrame(screenWin_, f) ? f : RECT{};
            // Only the web page: the browser's tabs and address bar are cut off (whole window until known).
            int cx = 0, cy = 0, cw = scrW_, ch = scrH_;
            if (frame_.right > frame_.left && docRect_.right > docRect_.left) {
                double sx = double(scrW_) / (frame_.right - frame_.left), sy = double(scrH_) / (frame_.bottom - frame_.top);
                cx = std::clamp(int((docRect_.left - frame_.left) * sx), 0, scrW_ - 1);
                cy = std::clamp(int((docRect_.top - frame_.top) * sy), 0, scrH_ - 1);
                cw = std::clamp(int((docRect_.right - docRect_.left) * sx), 1, scrW_ - cx);
                ch = std::clamp(int((docRect_.bottom - docRect_.top) * sy), 1, scrH_ - cy);
            }
            cropX_ = cx; cropY_ = cy; cropW_ = cw; cropH_ = ch;
            Rect area = pageArea();
            float scale = std::min(float(area.w()) / cw, float(area.h()) / ch);
            int w = int(cw * scale), h = int(ch * scale);
            pageShown_ = {area.x0 + (area.w() - w) / 2, area.y0 + (area.h() - h) / 2, 0, 0};
            pageShown_.x1 = pageShown_.x0 + w;
            pageShown_.y1 = pageShown_.y0 + h;
            canvas_.blit(screenPixels_.data() + (size_t(cy) * scrW_ + cx) * 4, cw, ch, scrW_ * 4, area);
        } else {
            canvas_.text({0, 220, SCREEN_W, 280}, L"Waiting for the YouTube window...", 32, rgb(230, 230, 235));
            canvas_.text({0, 290, SCREEN_W, 330}, L"It must stay open on the PC (not minimized).", 22, rgb(160, 170, 190), false);
        }
        if (toolbar_) {
            canvas_.fill({0, 0, SCREEN_W, PAGE_AREA.y0}, rgb(24, 24, 28));
            for (int i = 0; i < TOOL_BUTTONS; i++) {
                std::wstring label = i == 4 ? (media_.valid && !media_.playing ? L"PLAY" : L"PAUSE") : TOOL_LABELS[i];
                button(toolButton(i), label, pressed_ == 800 + i, i == 6 ? rgb(120, 40, 40) : i == 2 ? rgb(150, 30, 40) : rgb(44, 50, 72), 20);
            }
        }
        controlsButton(toolbar_);
        messageBar();
    }

    void drawKeyboard() {
        canvas_.clear(rgb(14, 16, 24));
        header(keyboardForWatch_ ? L"WATCH A FRIEND" : L"SEARCH YOUTUBE", true);
        canvas_.fill(QUERY_BOX, rgb(30, 34, 50));
        canvas_.frame(QUERY_BOX, current_.accent, 2);
        bool blink = GetTickCount64() / 500 % 2 == 0;
        canvas_.text({QUERY_BOX.x0 + 20, QUERY_BOX.y0, QUERY_BOX.x1 - 20, QUERY_BOX.y1},
                     query_.empty() ? std::wstring(keyboardForWatch_ ? L"type your friend's code (12 letters and digits)..." : L"type a song, artist or video...")
                                    : (keyboardForWatch_ ? upper(query_) : query_) + (blink ? L"_" : L" "),
                     30, query_.empty() ? rgb(110, 118, 140) : rgb(250, 250, 255), !query_.empty(), 0);
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 10; col++)
                button(keyRect(row, col), std::wstring(1, KEY_ROWS[row][col]), pressed_ == 700 + row * 10 + col, rgb(44, 50, 72), 28);
        button(KEY_DEL, L"\x25C0 DEL", pressed_ == 740, rgb(70, 50, 60), 24);
        button(KEY_SPACE, L"SPACE", pressed_ == 741, rgb(44, 50, 72), 24);
        button(KEY_CLEAR, L"CLEAR", pressed_ == 742, rgb(70, 50, 60), 24);
        button(KEY_SEARCH, keyboardForWatch_ ? L"CONNECT" : L"SEARCH", pressed_ == 743, keyboardForWatch_ ? rgb(40, 130, 200) : rgb(170, 30, 40), 26);
        messageBar();
    }

    void drawApp() {
        Rect content = padHidden_ && isRetro(session_.id()) ? Rect{0, 0, SCREEN_W, SCREEN_H} : session_.contentRect();
        canvas_.clear(rgb(10, 10, 14));
        uint64_t before = captureSerial_;
        session_.capture().latest(pixels_, capW_, capH_, captureSerial_);
        if (captureSerial_ != before && captureSerial_ == 1) hostLog("first captured frame %dx%d", capW_, capH_);
        if (session_.hasWindow() && capW_ > 0) {
            canvas_.blit(pixels_.data(), capW_, capH_, capW_ * 4, content);
            float scale = std::min(float(content.w()) / capW_, float(content.h()) / capH_);
            int w = int(capW_ * scale), h = int(capH_ * scale);
            fit_ = {content.x0 + (content.w() - w) / 2, content.y0 + (content.h() - h) / 2, 0, 0};
            fit_.x1 = fit_.x0 + w; fit_.y1 = fit_.y0 + h;
        } else {
            unsigned dots = unsigned(session_.age() / 400 % 4);
            std::wstring name = isVideo(session_.id()) ? playing_ : current_.title;
            canvas_.text(content, L"Starting " + name + std::wstring(dots, L'.'), 32, rgb(220, 220, 230));
        }
        if (isDesktop(session_.id()) && session_.hasWindow() && capW_ > 0) drawCursor();
        if (isVideo(session_.id())) drawPlayerControls();
        else if (isRetro(session_.id())) drawPad();
        else {
            canvas_.blend(BALATRO_HOME, rgb(0, 0, 0), 150);
            canvas_.text(BALATRO_HOME, L"\x2261", 40, rgb(255, 255, 255));
        }
    }

    // The mouse pointer over a desktop app's picture (the capture leaves it out): where the right
    // stick points during FREEZE, and where the last tap landed. Only while the app has the mouse.
    void drawCursor() {
        HWND window = session_.window();
        if (!window || (GetForegroundWindow() != window && !freeze_)) return;
        CURSORINFO info{sizeof info};
        if (GetForegroundWindow() == window && GetCursorInfo(&info) && !(info.flags & CURSOR_SHOWING)) return;  // in the world: the game hides it
        POINT p;
        RECT client{};
        POINT origin{0, 0};
        if (!GetCursorPos(&p) || !GetClientRect(window, &client) || !ClientToScreen(window, &origin)) return;
        if (client.right <= 0 || client.bottom <= 0) return;
        float u = float(p.x - origin.x) / client.right, v = float(p.y - origin.y) / client.bottom;
        if (u < 0 || v < 0 || u > 1 || v > 1) return;
        int x = fit_.x0 + int(u * fit_.w()), y = fit_.y0 + int(v * fit_.h());
        canvas_.fill({x - 9, y - 1, x + 10, y + 2}, rgb(0, 0, 0));
        canvas_.fill({x - 1, y - 9, x + 2, y + 10}, rgb(0, 0, 0));
        canvas_.fill({x - 8, y, x + 9, y + 1}, rgb(255, 255, 255));
        canvas_.fill({x, y - 8, x + 1, y + 9}, rgb(255, 255, 255));
    }

    void drawPlayerControls() {
        auto& mpv = session_.mpv();
        if (mode_ != Mode::Running) return;
        if (GetTickCount64() >= controlsUntil_) { controlsButton(false, mpv.paused()); return; }
        bool youTube = current_.id == AppId::YouTube;
        std::wstring title = playing_;
        if (youTube) {
            // mpv names the video once yt-dlp has found it; until then it shows the link.
            std::wstring now = mpv.mediaTitle();
            if (!now.empty() && now.find(L"watch?v=") == std::wstring::npos) title = now;
            int n = mpv.playlistCount(), p = mpv.playlistPos();
            if (n > 1 && p >= 0) title = L"(" + std::to_wstring(p + 1) + L"/" + std::to_wstring(n) + L")   " + title;
        }
        canvas_.blend({0, 0, SCREEN_W, 64}, rgb(0, 0, 0), 170);
        canvas_.text({20, 0, 950, 64}, title, 24, rgb(255, 255, 255), true, 0);
        controlsButton(true);
        canvas_.blend({0, 408, SCREEN_W, SCREEN_H}, rgb(0, 0, 0), 190);
        double pos = mpv.position(), dur = mpv.duration();
        canvas_.text({20, 410, 500, 446}, clock(pos) + L" / " + clock(dur), 20, rgb(230, 230, 235), false, 0);
        canvas_.text({500, 410, 1004, 446}, L"volume " + std::to_wstring(int(mpv.volume())) + L"%", 20, rgb(180, 185, 200), false, 2);
        canvas_.fill(SEEK_BAR, rgb(60, 64, 80));
        if (dur > 0) canvas_.fill({SEEK_BAR.x0, SEEK_BAR.y0, SEEK_BAR.x0 + int(SEEK_BAR.w() * std::clamp(pos / dur, 0.0, 1.0)), SEEK_BAR.y1}, current_.accent);
        for (int i = 0; i < PLAYER_BUTTONS; i++) {
            std::wstring label = i == 2 ? (mpv.paused() ? L"PLAY" : L"PAUSE") : PLAYER_LABELS[i];
            if (youTube && i == 7) label = L"PREV";
            if (youTube && i == 8) label = L"NEXT";
            button(playerButton(i), label, pressed_ == 300 + i, i == 9 ? rgb(120, 40, 40) : rgb(44, 50, 72), 20);
        }
    }

    void drawPad() {
        if (padHidden_) { controlsButton(false); return; }
        canvas_.fill({0, 0, 200, SCREEN_H}, rgb(20, 22, 30));
        canvas_.fill({824, 0, SCREEN_W, SCREEN_H}, rgb(20, 22, 30));
        bool live = mode_ == Mode::Running;
        for (auto& b : PAD) {
            bool dpad = b.pad == PadUp || b.pad == PadDown || b.pad == PadLeft || b.pad == PadRight;
            if (dpad && stickMode_) continue;
            bool on = false;
            if (freeze_) on = padFrame_.pad[b.pad];  // what the controllers are pressing
            else for (auto& [cell, h] : held_) on |= grow(b.rect, 6).contains(h.x, h.y);
            button(b.rect, b.label, on && live, rgb(44, 50, 72));
        }
        if (stickMode_) {
            canvas_.fill(STICK_AREA, rgb(34, 38, 54));
            canvas_.frame(STICK_AREA, rgb(70, 76, 100), 2);
            int cx = (STICK_AREA.x0 + STICK_AREA.x1) / 2, cy = (STICK_AREA.y0 + STICK_AREA.y1) / 2;
            float sx = freeze_ ? padFrame_.left[0] : stick_[0], sy = freeze_ ? padFrame_.left[1] : stick_[1];
            int kx = cx + int(sx * (STICK_AREA.w() / 2 - 30)), ky = cy + int(sy * (STICK_AREA.h() / 2 - 30));
            canvas_.fill({cx - 2, STICK_AREA.y0 + 8, cx + 2, STICK_AREA.y1 - 8}, rgb(60, 66, 90));
            canvas_.fill({STICK_AREA.x0 + 8, cy - 2, STICK_AREA.x1 - 8, cy + 2}, rgb(60, 66, 90));
            canvas_.fill({kx - 28, ky - 28, kx + 28, ky + 28}, rgb(200, 205, 220));
        }
        button(STICK_TOGGLE, stickMode_ ? L"D-PAD" : L"STICK", pressed_ == 402, rgb(40, 70, 90), 20);
        button(RETRO_HOME, L"HOME", pressed_ == 400, rgb(90, 40, 40));
        button(RETRO_FREEZE, freeze_ ? L"FREEZE: ON" : L"FREEZE", pressed_ == 403, freeze_ ? rgb(40, 110, 60) : rgb(50, 58, 84), 22);
        if (freeze_) canvas_.text({0, 162, 200, 196}, L"hold \x2261 1 s to move", 15, rgb(170, 220, 170), false);
        button(RETRO_MENU, L"MENU", pressed_ == 401, rgb(60, 50, 110));
        button(RETRO_FULL, L"FULL SCREEN", pressed_ == 404, rgb(40, 70, 90), 20);
    }

    void drawMenu() {
        drawApp();
        canvas_.blend({0, 0, SCREEN_W, SCREEN_H}, rgb(0, 0, 0), 190);
        canvas_.text({0, 90, SCREEN_W, 160}, current_.title + L" - " + current_.subtitle, 34, rgb(255, 255, 255));
        button(RESUME_BUTTON, L"RESUME", pressed_ == 200, rgb(40, 110, 60), 32);
        button(QUIT_BUTTON, L"QUIT TO LAUNCHER", pressed_ == 201, rgb(130, 40, 40), 32);
        button(MENU_DOCK_BUTTON, dockLabel(), pressed_ == 202, docked() ? rgb(40, 110, 60) : rgb(50, 58, 84), 28);
        button(MENU_FREEZE_BUTTON, freezeLabel(), pressed_ == 203, freeze_ ? rgb(40, 110, 60) : rgb(50, 58, 84), 28);
        button(MENU_HOME_BUTTON, L"HOME  (keeps it running)", pressed_ == 204, rgb(50, 58, 84), 26);
        if (!message_.empty()) canvas_.text({0, 548, SCREEN_W, 574}, message_, 18, rgb(255, 220, 200));
    }

    bool hqWanted() const { return posterHq_ && shared_->dockState == arcade::Docked; }  // only while docked (the tablet never needs it)
    void render() {
        target_ = &screen_;
        draw();
        drawCallOverlay();
        if (hqWanted()) { target_ = &hq_; draw(); drawCallOverlay(); target_ = &screen_; }
    }
    void draw() {
        bool settings = settingsMode();
        if (settings != lastSettings_) {
            if (session_.alive()) releaseGameInput(); else resetInput();
            lastSettings_ = settings;
            if (settings) { buildPages(); message_.clear(); }  // a mod installed or removed meanwhile
        }
        bool discord = discordMode();
        if (discord != lastDiscordMode_) {
            if (session_.alive()) releaseGameInput(); else resetInput();
            lastDiscordMode_ = discord;
            discordView_ = 0;
            dPage_ = 0;
        }
        if (discord) { drawDiscord(); canvas_.opaque(); return; }
        if (settings) { drawSettings(); canvas_.opaque(); return; }
        switch (mode_) {
            case Mode::Launcher: drawLauncher(); break;
            case Mode::Browse: drawBrowse(); break;
            case Mode::PlexLink: drawPlexLink(); break;
            case Mode::Running: drawApp(); break;
            case Mode::Menu: drawMenu(); break;
            case Mode::Keyboard: drawKeyboard(); break;
            case Mode::Screen: drawScreen(); break;
            case Mode::Media: drawMedia(); break;
            case Mode::Watch: drawWatch(); break;
            case Mode::Split: drawSplit(); break;
        }
        loadingOverlay();
        canvas_.opaque();
    }

    void publish() {
        // Only a changed picture goes out: the game's upload thread copies every published
        // frame to the GPU, and a tablet showing the same thing 60 times a second (the
        // launcher, a paused game) would keep it copying for nothing.
        bool hq = hqWanted();
        bool same = lastPublished_.size() == arcade::FRAME_BYTES && lastPublishedHq_ == hq &&
                    memcmp(lastPublished_.data(), screen_.pixels(), arcade::FRAME_BYTES) == 0;
        if (!same) {
            lastPublished_.resize(arcade::FRAME_BYTES);
            memcpy(lastPublished_.data(), screen_.pixels(), arcade::FRAME_BYTES);
            lastPublishedHq_ = hq;
            uint32_t index = arcade::writableFrame(shared_);
            memcpy(shared_->frames[index], screen_.pixels(), arcade::FRAME_BYTES);
            arcade::publishFrame(shared_, index);
        }
        if (hq) {
            if (!same) {
                uint32_t hqIndex = arcade::writableHqFrame(shared_);
                memcpy(shared_->hqFrames[hqIndex], hq_.pixels(), arcade::HQ_FRAME_BYTES);
                arcade::publishHqFrame(shared_, hqIndex);
            }
        } else if (shared_->hqLatestFrame != LONG(arcade::NO_FRAME)) {
            InterlockedExchange(&shared_->hqLatestFrame, LONG(arcade::NO_FRAME));
        }
        shared_->posterHq = posterHq_ ? 1 : 0;
    }

    arcade::Shared* shared_;
    bool standalone_;
    Config config_;
    Session session_;
    LightGun lightGun_;
    Canvas screen_;                     // the 1024x574 tablet screen
    Canvas hq_{int(arcade::HQ_WIDTH), int(arcade::HQ_HEIGHT), 2.f};  // the same at 2x (poster quality HIGH, while docked)
    Canvas* target_ = &screen_;
    std::vector<uint8_t> lastPublished_;  // the picture the game has (publish() skips an unchanged one)
    bool lastPublishedHq_ = false;
    bool posterHq_ = false;             // SETTINGS > TABLET > Poster quality
    bool mcLight_ = true;               // SETTINGS > TABLET > Minecraft light settings
    bool skipAds_ = true;               // SETTINGS > TABLET > Skip YouTube ads (in the browser, in the background too)
    Thumbs thumbs_;                     // video pictures for the list pages
    Discord discord_;                   // the DISCORD tab (discord.h)
    DiscordState dstate_;               // what the page and the HUD block show
    uint64_t dstateSerial_ = 0, dsharedBeat_ = 0;
    std::wstring dClientId_, dClientSecret_;
    int discordView_ = 0, dPage_ = 0;  // 0 home, 1 messages, 2 servers, 3 voice channels
    std::vector<DiscordRef> dGuilds_, dChannels_;
    std::wstring dGuildName_;
    bool dBusy_ = false, lastDiscordMode_ = false;
    arcade::DiscordShared* dshared_ = nullptr;  // the HUD's block
    HANDLE dmap_ = nullptr;
    ShareServer share_;                 // SHARE MY TABLET / SPLIT SCREEN
    ShareClient watch_;                 // WATCH A FRIEND
    Canvas split_;                      // the split screen's picture (composeSplit)
    Canvas side_[arcade::SIDES];        // the side posters' pictures
    uint64_t sideHash_[arcade::SIDES] = {};
    uint64_t lastSide_[arcade::SIDES] = {};
    bool sidesOn_ = true, sidesLive_ = false;  // SETTINGS > TABLET > Side posters; on right now (docked)
    uint64_t splitBarUntil_ = 0;
    bool landing_ = false;              // tapped(): this touch is a new poke (see onTouch)
    bool background_ = false;           // HOME: the app runs on behind the launcher
    AppInfo backgroundApp_{};
    LONG dockLevel_ = -1;               // the level the last auto dock was asked in
    int heldCount_ = 0;                 // touch cells down right now
    uint64_t firedAt_ = 0, lastTouchAt_ = 0;
    std::vector<uint8_t> upPixels_;     // my app's picture for a friend's split screen
    int upW_ = 0, upH_ = 0;
    uint64_t upSerial_ = 0;
    bool keyboardForWatch_ = false, watchRemembered_ = false;
    uint64_t watchBarUntil_ = 0;
    MediaInfo mediaNow_;                // the MEDIA page
    std::wstring mediaApp_;
    float mediaVolume_ = -1;
    bool mediaMuted_ = false, mediaPageBusy_ = false, mediaListen_ = false;
    std::shared_ptr<const SharePicture> mediaArt_;  // the album art of the current track
    std::shared_ptr<const SharePicture> mediaBack_; // the wash behind it
    Canvas marquee_{2048, 72, 1.f};                 // "artist - song", white on black, for the scrolling line
    int marqueeWidth_ = 0;
    std::wstring mediaArtKey_;
    uint64_t ytPollSerial_ = 0;         // the sign-in poll that is current (an older one gives up)
    uint64_t lastMediaPage_ = 0;
    Plex plex_;
    std::vector<AppInfo> apps_;
    AppInfo current_{};
    Mode mode_ = Mode::Launcher;
    std::vector<Screen> screens_;
    LONG readPos_ = 0;
    std::map<int, Touch> held_;
    std::vector<Touch> gameCells_;
    int pressed_ = -1;
    bool pointerDown_ = false, stickMode_ = false, gunUsed_ = false, padHeld_[PadCount] = {};
    float lastU_ = 0, lastV_ = 0, stick_[2] = {};
    std::wstring message_, loading_, playing_, linkCode_, query_, screenTitle_;
    WindowCapture screenCap_;
    HWND screenWin_ = nullptr;
    DWORD screenRoutedPid_ = 0;        // the browser whose sound went to the headset
    uint64_t lastUnminimize_ = 0;
    MediaInfo media_;
    bool mediaBusy_ = false, adBusy_ = false;
    uint64_t lastAdCheck_ = 0, adToastAt_ = 0;
    std::vector<uint8_t> screenPixels_;
    int scrW_ = 0, scrH_ = 0;
    uint64_t screenSerial_ = 0, lastMediaPoll_ = 0, lastTitlePoll_ = 0, lastDocPoll_ = 0, tapAt_ = 0;
    RECT docRect_{}, frame_{};          // the web page and the captured window, in screen pixels
    Rect pageShown_{};                  // where the page picture is on the tablet
    int cropX_ = 0, cropY_ = 0, cropW_ = 0, cropH_ = 0;  // the page's part of the capture
    int fingerX0_ = 0, fingerY0_ = 0, fingerX_ = 0, fingerY_ = 0;
    bool docBusy_ = false, fingerDown_ = false, scrolled_ = false, keyboardForScreen_ = false, toolbar_ = false, screenWasTopmost_ = false;
    bool padHidden_ = false;            // RetroArch: FULL SCREEN, the on-screen pad away
    minecraft::Input mcInput_;          // FREEZE in Minecraft: the controllers as keyboard and mouse
    HWND echoWindow_ = nullptr;         // Echo VR's window, to give the focus back to
    bool mcFinger_ = false, mcScrolled_ = false;  // a finger on Minecraft's picture, and whether it slid
    int mcX0_ = 0, mcY0_ = 0, mcX_ = 0, mcY_ = 0;
    POINT mcAt_{};
    uint64_t mcFocusBackAt_ = 0;        // when Echo gets the focus back after a tap on Minecraft
    uint64_t lastRefocus_ = 0;
    std::string linkPin_;
    uint64_t linkStarted_ = 0, lastLinkPoll_ = 0, controlsUntil_ = 0, lastTimeline_ = 0;
    PlexItem plexItem_;
    bool plexPlaying_ = false;
    std::mutex pendingLock_;
    std::vector<std::function<void()>> pending_;
    std::vector<std::future<void>> workers_;
    std::vector<uint8_t> pixels_;
    int capW_ = 0, capH_ = 0;
    uint64_t captureSerial_ = 0;
    Rect fit_{};
    std::vector<Page> pages_;
    int page_ = 0;
    uint64_t refreshedAt_ = 0, armedUntil_ = 0;
    bool lastSettings_ = false;
    LONG dockAsked_ = 0, dockSeen_ = 0, quietAsked_ = 0;
    bool autoDock_ = true;
    bool freeze_ = false, pausedByFreeze_ = false, padFed_ = false;
    LONG freezeLevel_ = 0;
    uint64_t freezeAt_ = 0;
    TouchPad touchPad_;
    PadFrame padFrame_;
    uint64_t lastAutoDock_ = 0;
    std::wstring lastQuietDock_;
    uint64_t dockAskedAt_ = 0, shotUpAt_ = 0;
    int shotX_ = 0, shotY_ = 0;
};

}  // namespace

// ArcadeHost.exe --watchdog <pid>: waits for that host to end, then ends every process it had
// put on the tablet (owned.txt). Started outside Echo VR's job, so it outlives the host when
// Echo closes and takes the host with it.
int watchdog(DWORD hostPid) {
    HANDLE host = OpenProcess(SYNCHRONIZE, FALSE, hostPid);
    if (!host) return 1;
    WaitForSingleObject(host, INFINITE);
    CloseHandle(host);
    Sleep(1500);  // a host closing its apps itself empties the list first
    std::vector<DWORD> pids = minecraft::readOwned();
    for (DWORD pid : pids) minecraft::killTree(pid);
    minecraft::writeOwned({});
    hostLog("watchdog: host %lu gone; ended %zu process(es) it had started", hostPid, pids.size());
    return 0;
}

void startWatchdog() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" --watchdog " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    for (DWORD flags : {DWORD(CREATE_BREAKAWAY_FROM_JOB | CREATE_NO_WINDOW), DWORD(CREATE_NO_WINDOW)}) {
        std::wstring line = cmd;
        if (CreateProcessW(self, line.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            hostLog("watchdog: pid %lu%s", pi.dwProcessId, flags & CREATE_BREAKAWAY_FROM_JOB ? "" : " (could not leave the job: it ends with Echo too)");
            return;
        }
    }
    hostLog("watchdog: could not start (%lu)", GetLastError());
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int) {
    if (const wchar_t* w = wcsstr(cmdLine, L"--watchdog ")) return watchdog(DWORD(_wtoi(w + 11)));
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\EchoArcade.Host");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    bool standalone = wcsstr(cmdLine, L"--standalone") != nullptr;
    HANDLE mapping = nullptr;
    arcade::Shared* shared = arcade::open(&mapping);
    if (!shared) { hostLog("shared memory unavailable (%lu)", GetLastError()); return 1; }
    hostLog("ArcadeHost started pid=%lu standalone=%d", GetCurrentProcessId(), int(standalone));
    for (DWORD pid : minecraft::readOwned()) minecraft::killTree(pid);  // left from a host that died without its watchdog
    minecraft::writeOwned({});
    startWatchdog();
    {
        Host host(shared, standalone);
        host.run();
    }
    shared->hostHeartbeat = 0;
    shared->hostPid = 0;
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    CloseHandle(single);
    return 0;
}
