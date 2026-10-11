// ArcadeHost.exe -- started by EchoArcade.dll when the PLAY tab is first opened.
// Owns the tablet UI (launcher, browsers, touch controls) and the running app;
// exits when Echo VR goes away.
#include "apps.h"
#include "touchpad.h"
#include <winrt/base.h>
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
const Rect BALATRO_HOME{0, 0, 64, 64};
const Rect RESUME_BUTTON{312, 170, 712, 240}, QUIT_BUTTON{312, 252, 712, 322};
const Rect BACK_BUTTON{12, 10, 150, 66};
const Rect DOCK_BUTTON{744, 12, 1008, 64};                  // launcher header
const Rect FREEZE_BUTTON{470, 12, 734, 64};                 // launcher header
const Rect MENU_DOCK_BUTTON{312, 334, 712, 396};            // in-app menu
const Rect MENU_FREEZE_BUTTON{312, 408, 712, 470};          // in-app menu
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
// YouTube on this PC: a toolbar on top, the web page under it (touch it like a tablet).
constexpr int TOOL_BUTTONS = 7;
const wchar_t* const TOOL_LABELS[TOOL_BUTTONS] = {L"\x25C0 BACK", L"HOME", L"SEARCH", L"PREV", L"PAUSE", L"NEXT", L"CLOSE"};
Rect toolButton(int i) { return {6 + i * 146, 4, 6 + i * 146 + 140, 54}; }
const Rect PAGE_AREA{0, 58, SCREEN_W, SCREEN_H};
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

enum class Mode { Launcher, Browse, PlexLink, Running, Menu, Keyboard, Screen };

struct Entry {
    std::wstring label, sub;
    std::function<void()> open;
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
        apps_ = listApps(config_);
        loadSettings();
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
            if ((mode_ == Mode::Running || mode_ == Mode::Menu) && !session_.alive()) appClosed();
            pollFreeze(now);
            if (mode_ == Mode::Running && session_.windowless() > 30000) {
                message_ = L"It did not open a window in 30 s (see host.log).";
                session_.kill();
                appClosed();
            }
            if (mode_ == Mode::PlexLink) pollPlexLink(now);
            reportPlexProgress(now);
            pollScreen(now);
            pollDock();
            autoDock(now);
            if (shotUpAt_ && now >= shotUpAt_) { shotUpAt_ = 0; onTouch({SHOT_TOUCH, false, shotX_, shotY_}); }
            bool visible = shared_->pageVisible != 0 || standalone_ || shared_->dockState == arcade::Docked;
            if (now - lastPublish >= (visible ? 16u : 200u)) {
                render();
                publish();
                lastPublish = now;
            }
            Sleep(4);
        }
        shared_->controllerWant = 0;  // Echo gets the controllers back
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
        current_ = app;
        if (app.id == AppId::Movies) { openMovies(); return; }
        if (app.id == AppId::Plex) { openPlex(); return; }
        if (app.id == AppId::YouTube) { openYouTube(); return; }
        if (!session_.launch(app.id)) { message_ = L"Could not start " + app.title + L" (see host.log)."; return; }
        gunUsed_ = false;
        mode_ = Mode::Running;
        resetInput();
    }

    void play(const std::wstring& target, const std::wstring& title, double start = 0) {
        message_.clear();
        if (!session_.launch(current_.id, target, start)) { message_ = L"Could not start mpv (see host.log)."; return; }
        playing_ = title;
        mode_ = Mode::Running;
        controlsUntil_ = GetTickCount64() + 4000;
        resetInput();
    }

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
        s.entries.push_back({L"YOUTUBE ON THIS PC", L"The YouTube page open in your browser, with touch controls", [this] { openScreen(); }});
        s.entries.push_back({L"SEARCH YOUTUBE", L"Type with the on-screen keyboard", [this] { mode_ = Mode::Keyboard; resetInput(); }});
        for (auto& link : loadYouTubeLinks(config_.youtubeLinks)) {
            if (link.playlist) s.entries.push_back({L"\x25B8  " + link.name, L"Playlist / mix - tap to see the videos", [this, link] { openYouTubeList(link); }});
            else s.entries.push_back({link.name, L"Video", [this, link] { playYouTube({link.url}, 0, link.name); }});
        }
        s.entries.push_back({L"Update yt-dlp", L"Tap this if videos stop playing (YouTube changes often)", [this] { updateYtdlp(); }});
        std::wstring file = config_.youtubeLinks;  // shown as its folder and name, e.g. apps\youtube.txt
        size_t cut = file.find_last_of(L"\\/");
        if (cut != std::wstring::npos && cut > 0) cut = file.find_last_of(L"\\/", cut - 1);
        if (cut != std::wstring::npos) file = file.substr(cut + 1);
        s.entries.push_back({L"Add your own links", L"On the PC, edit " + file + L"  -  one per line:  Name | link", nullptr});
        push(s);
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
                    s.entries.push_back({v.title, v.info, [this, urls, i, v] { playYouTube(urls, int(i), v.title); }});
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
        controlsUntil_ = GetTickCount64() + 4000;
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
    }
    void closeScreen() {
        screenCap_.stop();
        screenWin_ = nullptr;
        keyboardForScreen_ = false;
        tapAt_ = 0;
        mode_ = screens_.empty() ? Mode::Launcher : Mode::Browse;
        resetInput();
    }
    void pollScreen(uint64_t now) {
        if (!screenWin_) return;  // also while its search keyboard is up
        if (!IsWindow(screenWin_)) { message_ = L"The YouTube window closed."; closeScreen(); return; }
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
            if (mode_ == Mode::Screen && tabletToScreen(fingerX0_, fingerY0_, at)) pageTap(window, at);
        }
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
            case 6: closeScreen(); break;
        }
    }
    void screenTouch(const Touch& t) {
        for (int i = 0; i < TOOL_BUTTONS; i++)
            if (tapped(t, 800 + i, toolButton(i))) { toolPressed(i); return; }
        if (t.y < PAGE_AREA.y0) { if (!t.down) pressed_ = -1; return; }
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
            if (PAGE_AREA.contains(h.x, h.y)) return;  // another cell of the same finger is still down
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
        if (tapped(t, 743, KEY_SEARCH)) { if (keyboardForScreen_ && screenWin_) screenSearch(); else searchYouTube(); return; }
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
                else if (e.cell < 2) onTouch({POSTER_TOUCH + int(e.cell), e.kind != arcade::PointUp, x, y, e.kind == arcade::PointMove});
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
    bool dockPending() const { return dockAsked_ && shared_->dockDone != dockAsked_; }
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
    void startFreeze() {
        if (session_.alive() && !isRetro(session_.id())) {
            message_ = L"Freeze plays RetroArch: quit " + current_.title + L" first.";
            return;
        }
        if (!session_.alive()) {
            auto it = std::find_if(apps_.begin(), apps_.end(), [](const AppInfo& a) { return a.id == AppId::RetroArch; });
            if (it == apps_.end()) { message_ = L"RetroArch isn't set up (tools\\setup_apps.py)."; return; }
            launch(*it);
            if (!session_.alive()) return;  // launch() said why
        } else if (pausedByFreeze_) {
            session_.resumeRetro();
        }
        pausedByFreeze_ = false;
        mode_ = Mode::Running;
        resetInput();
        touchPad_.reset();
        padFrame_ = {};
        freezeLevel_ = shared_->levelSerial;
        shared_->controllerLevel = freezeLevel_;
        MemoryBarrier();
        shared_->controllerWant = 1;
        freeze_ = true;
        freezeAt_ = GetTickCount64();
        message_ = L"Frozen: your controllers play RetroArch. Hold the menu button 1 s (or tap FREEZE) to move again.";
        hostLog("freeze: on (level #%ld)", freezeLevel_);
    }
    void stopFreeze(const std::wstring& why) {
        if (!freeze_) return;
        freeze_ = false;
        shared_->controllerWant = 0;
        releaseController();
        if (session_.alive() && isRetro(session_.id()) && session_.pauseRetro()) pausedByFreeze_ = true;
        message_ = why;
        hostLog("freeze: off");
    }
    void releaseController() {
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
        if (!session_.alive() || !isRetro(session_.id())) { stopFreeze(L"RetroArch closed: freeze is off."); return; }
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
    // Saved in echo_tweaks.ini beside the host, which the plugin reads when Echo starts.
    struct Setting { const wchar_t* label; const wchar_t* section; const wchar_t* key; float min, max, step, value; };
    std::wstring tweaksPath() const {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring p = path;
        return p.substr(0, p.find_last_of(L"\\/")) + L"\\echo_tweaks.ini";
    }
    void loadSettings() {
        // FOV is not here on purpose: it is set by hand in echo_tweaks.ini [fov].
        settings_ = {{L"TABLET SIZE", L"tablet", L"scale", .5f, 4.f, .05f, 1.f}};
        for (auto& s : settings_) {
            wchar_t buf[32];
            GetPrivateProfileStringW(s.section, s.key, L"1.0", buf, 32, tweaksPath().c_str());
            s.value = std::clamp(float(_wtof(buf)), s.min, s.max);
        }
        shared_->tabletScale = LONG(settings_[0].value * 1000);
        autoDock_ = GetPrivateProfileIntW(L"dock", L"auto", 1, tweaksPath().c_str()) != 0;  // on unless turned off
    }
    void setSetting(size_t i, float v) {
        auto& s = settings_[i];
        v = std::clamp(std::round(v / s.step) * s.step, s.min, s.max);
        if (std::fabs(v - s.value) < .001f) return;
        s.value = v;
        wchar_t buf[16];
        swprintf(buf, 16, L"%.2f", v);
        WritePrivateProfileStringW(s.section, s.key, buf, tweaksPath().c_str());
        if (i == 0) shared_->tabletScale = LONG(v * 1000);
    }
    static int settingY(size_t i) { return 96 + int(i) * 150; }
    static Rect settingMinus(size_t i) { return {20, settingY(i) + 46, 110, settingY(i) + 116}; }
    static Rect settingPlus(size_t i) { return {914, settingY(i) + 46, 1004, settingY(i) + 116}; }
    static Rect settingTrack(size_t i) { return {140, settingY(i) + 66, 884, settingY(i) + 96}; }
    bool settingsMode() const { return shared_->pageMode == 1; }

    void settingsTouch(const Touch& t) {
        for (size_t i = 0; i < settings_.size(); i++) {
            auto& s = settings_[i];
            if (tapped(t, 600 + int(i) * 2, settingMinus(i))) { setSetting(i, s.value - s.step); return; }
            if (tapped(t, 601 + int(i) * 2, settingPlus(i))) { setSetting(i, s.value + s.step); return; }
            Rect track = settingTrack(i);
            if (t.down && grow(track, 26).contains(t.x, t.y)) {  // tap or slide along the bar
                float f = std::clamp(float(t.x - track.x0) / track.w(), 0.f, 1.f);
                setSetting(i, s.min + f * (s.max - s.min));
                return;
            }
        }
        if (!t.down) pressed_ = -1;
    }

    void drawSettings() {
        canvas_.clear(rgb(14, 16, 24));
        header(L"SETTINGS", false);
        for (size_t i = 0; i < settings_.size(); i++) {
            auto& s = settings_[i];
            int y = settingY(i);
            wchar_t value[16];
            swprintf(value, 16, L"%.2fx", s.value);
            canvas_.text({24, y, 700, y + 44}, s.label, 26, rgb(235, 235, 240), true, 0);
            canvas_.text({700, y, 1004, y + 44}, value, 26, rgb(90, 200, 255), true, 2);
            button(settingMinus(i), L"\x2212", pressed_ == 600 + int(i) * 2, rgb(44, 50, 72), 36);
            button(settingPlus(i), L"+", pressed_ == 601 + int(i) * 2, rgb(44, 50, 72), 36);
            Rect track = settingTrack(i);
            canvas_.fill(track, rgb(40, 44, 62));
            int fill = track.x0 + int(track.w() * (s.value - s.min) / (s.max - s.min));
            canvas_.fill({track.x0, track.y0, fill, track.y1}, rgb(70, 150, 190));
            canvas_.fill({fill - 14, track.y0 - 16, fill + 14, track.y1 + 16}, rgb(235, 240, 250));
        }
        canvas_.text({24, 516, 1004, 572}, L"Saved instantly; the tablet size changes live. "
                     L"View (FOV) is set in echo_tweaks.ini.", 17, rgb(150, 160, 180), false, 0);
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
        if (!loading_.empty() && !settingsMode()) return;
        if (settingsMode()) { settingsTouch(t); return; }
        switch (mode_) {
            case Mode::Launcher: launcherTouch(t); break;
            case Mode::Browse: browseTouch(t); break;
            case Mode::PlexLink: if (!t.down && BACK_BUTTON.contains(t.x, t.y)) toLauncher(); break;
            case Mode::Menu: menuTouch(t); break;
            case Mode::Keyboard: keyboardTouch(t); break;
            case Mode::Screen: screenTouch(t); break;
            case Mode::Running:
                if (isVideo(session_.id())) playerTouch(t);
                else if (isRetro(session_.id())) retroTouch(t);
                else balatroTouch(t);
                break;
        }
    }

    // A tap = press and release on the same control.
    bool tapped(const Touch& t, int index, Rect r) {
        if (!r.contains(t.x, t.y)) return false;
        if (t.down) { pressed_ = index; return false; }
        bool hit = pressed_ == index;
        pressed_ = -1;
        return hit;
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
    }

    void playerTouch(const Touch& t) {
        uint64_t now = GetTickCount64();
        bool shown = now < controlsUntil_ || session_.mpv().paused();  // paused: controls stay up
        if (!shown) { if (!t.down) controlsUntil_ = now + 5000; return; }  // first tap just reveals the controls
        controlsUntil_ = now + 5000;
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

    void retroTouch(const Touch& t) {
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
    Rect tileRect(size_t i) const {
        int col = int(i % 3), row = int(i / 3);
        return {20 + col * 332, 88 + row * 222, 20 + col * 332 + 320, 88 + row * 222 + 210};
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
            canvas_.text({r.x0 + 30, r.y0 + 26, r.x1 - 12, r.y0 + 96}, a.title, 40, rgb(245, 245, 250), true, 0);
            canvas_.text({r.x0 + 30, r.y0 + 96, r.x1 - 12, r.y0 + 134}, a.subtitle, 21, rgb(180, 190, 210), false, 0);
            if (!a.missing.empty())
                canvas_.text({r.x0 + 30, r.y0 + 140, r.x1 - 12, r.y0 + 180}, a.missing, 17, rgb(255, 170, 90), false, 0);
        }
        messageBar();
    }

    void drawBrowse() {
        auto& s = screens_.back();
        canvas_.clear(rgb(14, 16, 24));
        header(s.title, true);
        int pages = std::max(1, (int(s.entries.size()) + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
        for (int i = 0; i < ROWS_PER_PAGE; i++) {
            size_t index = size_t(s.page * ROWS_PER_PAGE + i);
            if (index >= s.entries.size()) break;
            auto& e = s.entries[index];
            Rect r = rowRect(i);
            bool on = pressed_ == i;
            canvas_.fill(r, on ? rgb(56, 64, 92) : rgb(30, 34, 50));
            canvas_.fill({r.x0, r.y0, r.x0 + 8, r.y1}, current_.accent);
            if (e.sub.empty()) canvas_.text({r.x0 + 24, r.y0, r.x1 - 12, r.y1}, e.label, 26, rgb(240, 240, 245), true, 0);
            else {
                canvas_.text({r.x0 + 24, r.y0 + 4, r.x1 - 12, r.y0 + 40}, e.label, 25, rgb(240, 240, 245), true, 0);
                canvas_.text({r.x0 + 24, r.y0 + 38, r.x1 - 12, r.y1 - 2}, e.sub, 18, rgb(160, 170, 190), false, 0);
            }
        }
        button(PAGE_UP, L"\x25B2", pressed_ == 101, s.page > 0 ? rgb(50, 58, 84) : rgb(28, 30, 40), 36);
        button(PAGE_DOWN, L"\x25BC", pressed_ == 102, s.page < pages - 1 ? rgb(50, 58, 84) : rgb(28, 30, 40), 36);
        canvas_.text({12, 528, 904, 574}, L"page " + std::to_wstring(s.page + 1) + L" of " + std::to_wstring(pages) + L"   -   " +
                     std::to_wstring(s.entries.size()) + L" items", 18, rgb(130, 140, 160), false, 0);
        messageBar();
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
            float scale = std::min(float(PAGE_AREA.w()) / cw, float(PAGE_AREA.h()) / ch);
            int w = int(cw * scale), h = int(ch * scale);
            pageShown_ = {PAGE_AREA.x0 + (PAGE_AREA.w() - w) / 2, PAGE_AREA.y0 + (PAGE_AREA.h() - h) / 2, 0, 0};
            pageShown_.x1 = pageShown_.x0 + w;
            pageShown_.y1 = pageShown_.y0 + h;
            canvas_.blit(screenPixels_.data() + (size_t(cy) * scrW_ + cx) * 4, cw, ch, scrW_ * 4, PAGE_AREA);
        } else {
            canvas_.text({0, 220, SCREEN_W, 280}, L"Waiting for the YouTube window...", 32, rgb(230, 230, 235));
            canvas_.text({0, 290, SCREEN_W, 330}, L"It must stay open on the PC (not minimized).", 22, rgb(160, 170, 190), false);
        }
        canvas_.fill({0, 0, SCREEN_W, PAGE_AREA.y0}, rgb(24, 24, 28));
        for (int i = 0; i < TOOL_BUTTONS; i++) {
            std::wstring label = i == 4 ? (media_.valid && !media_.playing ? L"PLAY" : L"PAUSE") : TOOL_LABELS[i];
            button(toolButton(i), label, pressed_ == 800 + i, i == 6 ? rgb(120, 40, 40) : i == 2 ? rgb(150, 30, 40) : rgb(44, 50, 72), 20);
        }
        messageBar();
    }

    void drawKeyboard() {
        canvas_.clear(rgb(14, 16, 24));
        header(L"SEARCH YOUTUBE", true);
        canvas_.fill(QUERY_BOX, rgb(30, 34, 50));
        canvas_.frame(QUERY_BOX, current_.accent, 2);
        bool blink = GetTickCount64() / 500 % 2 == 0;
        canvas_.text({QUERY_BOX.x0 + 20, QUERY_BOX.y0, QUERY_BOX.x1 - 20, QUERY_BOX.y1},
                     query_.empty() ? std::wstring(L"type a song, artist or video...") : query_ + (blink ? L"_" : L" "),
                     30, query_.empty() ? rgb(110, 118, 140) : rgb(250, 250, 255), !query_.empty(), 0);
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 10; col++)
                button(keyRect(row, col), std::wstring(1, KEY_ROWS[row][col]), pressed_ == 700 + row * 10 + col, rgb(44, 50, 72), 28);
        button(KEY_DEL, L"\x25C0 DEL", pressed_ == 740, rgb(70, 50, 60), 24);
        button(KEY_SPACE, L"SPACE", pressed_ == 741, rgb(44, 50, 72), 24);
        button(KEY_CLEAR, L"CLEAR", pressed_ == 742, rgb(70, 50, 60), 24);
        button(KEY_SEARCH, L"SEARCH", pressed_ == 743, rgb(170, 30, 40), 26);
        messageBar();
    }

    void drawApp() {
        Rect content = session_.contentRect();
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
        if (isVideo(session_.id())) drawPlayerControls();
        else if (isRetro(session_.id())) drawPad();
        else {
            canvas_.blend(BALATRO_HOME, rgb(0, 0, 0), 150);
            canvas_.text(BALATRO_HOME, L"\x2261", 40, rgb(255, 255, 255));
        }
    }

    void drawPlayerControls() {
        auto& mpv = session_.mpv();
        if ((GetTickCount64() >= controlsUntil_ && !mpv.paused()) || mode_ != Mode::Running) return;
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
        canvas_.text({20, 0, 1004, 64}, title, 24, rgb(255, 255, 255), true, 0);
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
    }

    void drawMenu() {
        drawApp();
        canvas_.blend({0, 0, SCREEN_W, SCREEN_H}, rgb(0, 0, 0), 190);
        canvas_.text({0, 90, SCREEN_W, 160}, current_.title + L" - " + current_.subtitle, 34, rgb(255, 255, 255));
        button(RESUME_BUTTON, L"RESUME", pressed_ == 200, rgb(40, 110, 60), 32);
        button(QUIT_BUTTON, L"QUIT TO LAUNCHER", pressed_ == 201, rgb(130, 40, 40), 32);
        button(MENU_DOCK_BUTTON, dockLabel(), pressed_ == 202, docked() ? rgb(40, 110, 60) : rgb(50, 58, 84), 28);
        button(MENU_FREEZE_BUTTON, freezeLabel(), pressed_ == 203, freeze_ ? rgb(40, 110, 60) : rgb(50, 58, 84), 28);
        if (!message_.empty()) canvas_.text({0, 486, SCREEN_W, 540}, message_, 22, rgb(255, 220, 200));
    }

    void render() {
        bool settings = settingsMode();
        if (settings != lastSettings_) { if (session_.alive()) releaseGameInput(); else resetInput(); lastSettings_ = settings; }
        if (settings) { drawSettings(); canvas_.opaque(); return; }
        switch (mode_) {
            case Mode::Launcher: drawLauncher(); break;
            case Mode::Browse: drawBrowse(); break;
            case Mode::PlexLink: drawPlexLink(); break;
            case Mode::Running: drawApp(); break;
            case Mode::Menu: drawMenu(); break;
            case Mode::Keyboard: drawKeyboard(); break;
            case Mode::Screen: drawScreen(); break;
        }
        loadingOverlay();
        canvas_.opaque();
    }

    void publish() {
        uint32_t index = arcade::writableFrame(shared_);
        memcpy(shared_->frames[index], canvas_.pixels(), arcade::FRAME_BYTES);
        arcade::publishFrame(shared_, index);
    }

    arcade::Shared* shared_;
    bool standalone_;
    Config config_;
    Session session_;
    LightGun lightGun_;
    Canvas canvas_;
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
    bool docBusy_ = false, fingerDown_ = false, scrolled_ = false, keyboardForScreen_ = false;
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
    std::vector<Setting> settings_;
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

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\EchoArcade.Host");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    bool standalone = wcsstr(cmdLine, L"--standalone") != nullptr;
    HANDLE mapping = nullptr;
    arcade::Shared* shared = arcade::open(&mapping);
    if (!shared) { hostLog("shared memory unavailable (%lu)", GetLastError()); return 1; }
    hostLog("ArcadeHost started pid=%lu standalone=%d", GetCurrentProcessId(), int(standalone));
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
