// EchoArcade.dll -- EchoLoader plugin that adds the ARCADE tab to Echo VR's hand tablet.
//
// Tablet integration (canvas discovery, page switching, native button gating and
// event dispatch) follows the approach proven by heisthecat31/Doom-on-EchoVR,
// re-implemented here as a standalone plugin with no script-DLL wrappers.
// Addresses are for echovr.exe timestamp 1683152886 and are verified at startup.
#include "../shared/arcade_ipc.h"
#include "../generated/arcade_tab.h"
#include "../vendor/minhook/include/MinHook.h"
#include "controllers.h"
#include "d3d12_stream.h"
#include "ovr_tweaks.h"
#include "posters.h"
#include "voice.h"
#include "tablet_scale.h"
#include "log.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using U = uint64_t;
using P = unsigned char*;
template <class T> static T& at(void* p, size_t n) { return *reinterpret_cast<T*>(static_cast<P>(p) + n); }
template <class T> static T fn(void* p, size_t n) { return reinterpret_cast<T>(static_cast<P>(p) + n); }

namespace {

constexpr unsigned EXE_TIMESTAMP = 1683152886, EXE_IMAGE_SIZE = 35852288;
constexpr U PRESS = 0xfdb213d9dc5e4826, RELEASE = 0xd7ded077f321f69b;
constexpr U TABLET_ACTOR = 0x6c1f6ff04e070923;
constexpr U STOCK_TAB_FIRST = 0x275876572b742791, STOCK_TAB_LAST = 0x275876572b742794;
// Stock page-content buttons that must not react while ARCADE covers the page.
constexpr U CONTENT[] = {0x5071f3ac46630320, 0x645c2234c519a275, 0x645c2234c519a276, 0x645c2234c519a277,
    0x6567539d25efbdc0, 0xfc4ee470b38fb49d, 0xfc4ee470b38fb49e, 0xfc4ee470b38fb49f,
    0x2755735736742790, 0x2755735736742791, 0x2755735736742792, 0x2755735736742793,
    0x2755735736742794, 0x2755735736742795, 0x2755735736742796, 0x2755735736742797,
    0x556819fa696d48c4};

struct Signature { unsigned rva; unsigned char bytes[16]; };
constexpr Signature SIGNATURES[] = {
    {0x71fc90, {0x48,0x89,0x5c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57}},  // canvas loaded
    {0x7287b0, {0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xec}},  // canvas unload
    {0x71c820, {0x89,0x54,0x24,0x10,0x57,0x41,0x57,0x48,0x83,0xec,0x38,0x48,0x8b,0x81,0xc0,0x00}},  // element show
    {0x727f10, {0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x89,0x54,0x24,0x10,0x57,0x48}},  // element text
    {0x726f00, {0xf3,0x0f,0x10,0x81,0x58,0x03,0x00,0x00,0xb8,0x01,0x00,0x00,0x00,0xf3,0x0f,0x5c}},  // canvas alpha
    {0x92f3f0, {0x40,0x55,0x57,0x41,0x54,0x48,0x8d,0xac,0x24,0x40,0xfe,0xff,0xff,0x48,0x81,0xec}},  // button update
    {0x92b9e0, {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48}},  // button disable
    {0x92bd10, {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57}},  // button enable
    {0x510060, {0x48,0x83,0xec,0x58,0x48,0x8b,0x41,0x58,0x48,0x89,0x54,0x24,0x20,0x48,0x8d,0x54}},  // event dispatch
};

using Show = U (*)(void*, unsigned, int);
using Text = U (*)(void*, unsigned, const char*);
using Alpha = U (*)(void*, float);
using Mask = void (*)(void*, U, unsigned short);
using Dispatch = void (*)(void*, U, U, U, int);
using Loaded = U (*)(void*, U, U, U);
using Unload = void (*)(void*, U, U, U);
using ButtonUpdate = void (*)(void*);

P exe = nullptr;
Show show; Text text; Alpha alpha; Mask enableButton, disableButton;
Dispatch dispatchOriginal; Loaded loadedOriginal; Unload unloadOriginal; ButtonUpdate buttonOriginal;

std::recursive_mutex mutex;
std::atomic<bool> fault{false};
arcade::Shared* shared = nullptr;
std::unordered_map<U, unsigned> cellIndex;
std::atomic<U> lastTabletTick{0};
std::atomic<bool> arcadeSelected{false};
std::atomic<int> pageMode{0};
HANDLE job = nullptr;
std::wstring pluginDir;
std::wstring startupEnvironment;  // Echo's environment at plugin load (see startHost)

void failRuntime(const char* why) { fault = true; logf("FAULT: %s -- ARCADE disabled for this session", why); }

// Canvas kinds: 0 root, 1 nav, 2 ARCADE page, 3-5 stock page canvases hidden under ours.
constexpr unsigned KINDS = 6;
constexpr unsigned COUNTS[KINDS] = {ARCADE_ROOT_ELEMENTS, ARCADE_NAV_ELEMENTS, ARCADE_PAGE_ELEMENTS, 24, 59, 7};
constexpr unsigned MARKER_INDEX[KINDS] = {ARCADE_ROOT_CHILD, ARCADE_NAV_ICON, 0, 0, 0, 0};
constexpr U MARKERS[KINDS] = {ARCADE_ROOT_MARKER, ARCADE_NAV_MARKER, ARCADE_PAGE_MARKER,
                              0x41d2cf3808220b1a, 0x2fd5888f5f7a5286, 0xf79f5459a0f0cf34};

struct View { void* p = nullptr; bool saved = false; float opacity = 0; std::array<bool, 3> visible{}; };
enum class Page { Stock, Arcade };
enum class Mode { Arcade = 0, Settings = 1 };  // both tabs show our page; the host draws either
struct Context {
    Page page = Page::Stock;
    Mode mode = Mode::Arcade;
    int navState = -1;   // highlighted tab we last drew: 0 stock, 1 ARCADE, 2 SETTINGS
    std::array<View, KINDS> views{};
    std::set<unsigned> masked;
    int statusShown = -1;
    std::string statusText;
};
std::map<void*, Context> contexts;                          // keyed by gamespace
std::map<void*, std::pair<void*, unsigned>> canvases;       // canvas -> (gamespace, kind)
std::set<void*> pending;
std::set<std::array<U, 3>> held;                            // (gamespace, actor, component)

bool isContent(U n) { return std::find(std::begin(CONTENT), std::end(CONTENT), n) != std::end(CONTENT); }
bool isStockTab(U n) { return n >= STOCK_TAB_FIRST && n <= STOCK_TAB_LAST; }
bool isOurTab(U n) { return n == ARCADE_TAB || n == SETTINGS_TAB; }
int cellOf(U n) { auto it = cellIndex.find(n); return it == cellIndex.end() ? -1 : int(it->second); }

void remember(void* p, unsigned depth = 0) {
    if (!p || depth > 8 || canvases.count(p)) return;
    U n = at<U>(p, 0xc0);
    auto elements = at<P>(p, 0x90);
    for (unsigned kind = 0; kind < KINDS; kind++) {
        if (n != COUNTS[kind] || !elements || at<U>(elements, MARKER_INDEX[kind] * 224) != MARKERS[kind]) continue;
        auto cs = at<void*>(p, 0x370);
        if (!cs) { pending.insert(p); return; }
        pending.erase(p);
        auto gs = at<void*>(cs, 0x80);
        contexts[gs].views[kind] = View{p};
        canvases[p] = {gs, kind};
        logf("canvas kind=%u gs=%p ptr=%p", kind, gs, p);
        if (kind == 0) tablet::tabletPresent(true);
        remember(at<void*>(p, 0x378), depth + 1);
        return;
    }
}

bool hostAlive() { return shared && arcade::alive(shared->hostHeartbeat, GetTickCount64()); }

void releaseAllTouches(void* gs) {
    for (auto it = held.begin(); it != held.end();) {
        if (reinterpret_cast<void*>((*it)[0]) == gs) {
            int cell = cellOf((*it)[2]);
            if (cell >= 0 && shared) arcade::pushTouch(shared, unsigned(cell), arcade::TouchUp);
            it = held.erase(it);
        } else ++it;
    }
}

void startHost() {
    if (hostAlive()) return;
    static U lastAttempt = 0;
    U now = GetTickCount64();
    if (lastAttempt && now - lastAttempt < 5000) return;
    lastAttempt = now;
    std::wstring exePath = pluginDir + L"\\EchoArcade\\ArcadeHost.exe";
    std::wstring cmd = L"\"" + exePath + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    // Echo edits its own environment after start-up (the in-game host lost
    // %LOCALAPPDATA%), so the host and its apps get the environment Echo was
    // launched with instead.
    wchar_t probe[8];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", probe, 8)) logf("note: Echo's environment has no LOCALAPPDATA now; using the start-up copy");
    void* env = startupEnvironment.empty() ? nullptr : startupEnvironment.data();
    if (!CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                        env, (pluginDir + L"\\EchoArcade").c_str(), &si, &pi)) {
        logf("could not start ArcadeHost.exe (error %lu)", GetLastError());
        return;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);  // host and its games close with Echo
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logf("started ArcadeHost.exe pid=%lu", pi.dwProcessId);
}

void setStatus(Context& c, void* page, bool showIt, const char* message) {
    if (c.statusShown != int(showIt)) { show(page, ARCADE_PAGE_STATUS, showIt); c.statusShown = int(showIt); }
    if (showIt && c.statusText != message) { text(page, ARCADE_PAGE_STATUS, message); c.statusText = message; }
}

void render(Context& c) {
    bool active = c.page == Page::Arcade && !fault;
    for (unsigned i = 3; i < 6; i++) {
        auto& v = c.views[i];
        if (!v.p) continue;
        if (active) { if (!v.saved) { v.opacity = at<float>(v.p, 0x358); v.saved = true; } alpha(v.p, 0); }
        else if (v.saved) { alpha(v.p, v.opacity); v.saved = false; }
    }
    auto& root = c.views[0];
    if (root.p) {
        const unsigned hide[] = {0, 1, 3};
        if (active && !root.saved) {
            auto rows = at<P>(root.p, 0x90);
            for (unsigned i = 0; i < 3; i++) root.visible[i] = at<unsigned>(rows, hide[i] * 224 + 0x10) == 0;
            root.saved = true;
        }
        if (active) for (auto i : hide) show(root.p, i, 0);
        else if (root.saved) { for (unsigned i = 0; i < 3; i++) show(root.p, hide[i], root.visible[i]); root.saved = false; }
        show(root.p, ARCADE_ROOT_CHILD, active);
        show(root.p, ARCADE_ROOT_HEADER, active);
        show(root.p, ARCADE_ROOT_TITLE, active);
    }
    // Tab highlights: ours while our page is up, otherwise the stock ones (which the
    // game's own scripts keep pointing at the right stock tab).
    int nav = !active ? 0 : c.mode == Mode::Settings ? 2 : 1;
    if (c.views[1].p && c.navState != nav) {
        show(c.views[1].p, ARCADE_NAV_SELECTED, nav == 1);
        show(c.views[1].p, SETTINGS_NAV_SELECTED, nav == 2);
        for (auto i : ARCADE_NAV_STOCK_SELECTED) show(c.views[1].p, i, nav == 0);
        if (root.p && nav) text(root.p, ARCADE_ROOT_TITLE, nav == 2 ? "SETTINGS" : "ARCADE");
        c.navState = nav;
    }
    if (active && c.views[2].p) {
        U now = GetTickCount64();
        bool frames = shared && shared->latestFrame != LONG(arcade::NO_FRAME);
        if (hostAlive() && frames) setStatus(c, c.views[2].p, false, "");
        else if (hostAlive()) setStatus(c, c.views[2].p, true, "ARCADE HOST RUNNING - WAITING FOR FIRST FRAME");
        else setStatus(c, c.views[2].p, true, "STARTING ARCADE HOST... (SEE %LOCALAPPDATA%\\ECHOARCADE)");
        lastTabletTick = now;
    }
}

struct Button { U name; unsigned handle; unsigned short reasons; };
void gate(Context& c, void* cs) {
    auto resource = at<void*>(cs, 0xd0);
    if (!resource) return;
    auto rows = at<P>(resource, 0);
    U count = at<U>(resource, 0x30);
    unsigned n = at<unsigned short>(cs, 0x10a);
    if (count > 8192 || n > count) { failRuntime("unexpected native button pool layout"); return; }
    std::vector<Button> buttons;
    auto instances = at<P>(cs, 0x110), inverse = at<P>(cs, 0xf0), handles = at<P>(cs, 0xe0), masks = at<P>(cs, 0xf8);
    for (unsigned i = 0; i < n; i++) {
        unsigned row = at<unsigned short>(instances, i * 400);
        if (row >= count) { failRuntime("invalid button row"); return; }
        U name = at<U>(rows, row * 296);
        if (!isOurTab(name) && cellOf(name) < 0 && !isContent(name)) continue;
        if (!isContent(name) && at<U>(rows, row * 296 + 8) != TABLET_ACTOR) continue;
        unsigned slot = at<unsigned short>(inverse, i * 2);
        if (slot >= count) { failRuntime("invalid button handle slot"); return; }
        unsigned h = slot | (unsigned(at<unsigned short>(handles, slot * 4 + 2)) << 16);
        buttons.push_back({name, h, at<unsigned short>(masks, i * 2)});
    }
    std::set<unsigned> live;
    for (auto& b : buttons) live.insert(b.handle);
    for (auto it = c.masked.begin(); it != c.masked.end();) it = live.count(*it) ? std::next(it) : c.masked.erase(it);
    bool ready = c.views[0].p && c.views[1].p && c.views[2].p;
    for (auto& b : buttons) {
        bool hide;
        if (fault) hide = !isOurTab(b.name) && !isContent(b.name);  // leave stock buttons alone after a fault
        else if (isOurTab(b.name)) hide = !ready;
        else if (isContent(b.name)) hide = c.page != Page::Stock;
        else hide = c.page != Page::Arcade;  // touch cells
        if (fault && isOurTab(b.name)) hide = true;
        if (hide && !c.masked.count(b.handle)) {
            if (b.reasons & 0x8000) { failRuntime("button disable bit 0x8000 already owned"); return; }
            disableButton(cs, b.handle, 0x8000);
            c.masked.insert(b.handle);
        } else if (!hide && c.masked.count(b.handle)) {
            enableButton(cs, b.handle, 0x8000);
            c.masked.erase(b.handle);
        }
    }
}

// Touch hitboxes follow the tablet size: CR15ButtonInteractCR rows hold each poke
// box's centre (+0x80 x, +0x84 y) and half size (+0x9c, +0xa0) in metres from the
// canvas's top-left corner, plus a bounds centre (+0xb8, +0xbc). The button system
// reads the rows every update, so scaling them in memory moves the touch areas.
std::unordered_map<P, std::array<float, 6>> hitboxOriginal;  // stock values per row (never re-read)
std::map<void*, float> hitboxScaleOf;                         // scale last applied per button resource
void scaleHitboxes(void* cs) {
    auto resource = at<void*>(cs, 0xd0);
    float k = tablet::appliedScale();
    auto known = hitboxScaleOf.find(resource);
    if (!resource || (known != hitboxScaleOf.end() && known->second == k) || (known == hitboxScaleOf.end() && k == 1.f)) return;
    constexpr unsigned OFF[6] = {0x80, 0x84, 0x9c, 0xa0, 0xb8, 0xbc};
    auto rows = at<P>(resource, 0);
    U count = at<U>(resource, 0x30);
    unsigned n = 0;
    for (U i = 0; i < count && i < 8192; i++) {
        P r = rows + i * 296;
        if (at<U>(r, 8) != TABLET_ACTOR) continue;
        auto it = hitboxOriginal.find(r);
        if (it == hitboxOriginal.end()) {
            std::array<float, 6> o;
            for (int j = 0; j < 6; j++) o[j] = at<float>(r, OFF[j]);
            it = hitboxOriginal.emplace(r, o).first;
        }
        for (int j = 0; j < 6; j++) at<float>(r, OFF[j]) = it->second[j] * k;
        n++;
    }
    if (n) logf("tablet: %u touch areas scaled x%.2f", n, k);
    hitboxScaleOf[resource] = k;
}

void refreshSelection() {
    bool any = false;
    int mode = 0;
    for (auto& [gs, c] : contexts)
        if (c.page == Page::Arcade && c.views[2].p != nullptr) { any = true; mode = int(c.mode); }
    arcadeSelected = any && !fault;
    pageMode = mode;
}

// ---- hooks (engine threads) ----
void safeRemember(void* p) {
    __try { remember(p); } __except (EXCEPTION_EXECUTE_HANDLER) { failRuntime("access fault while inspecting a canvas"); }
}

U loaded(void* p, U a, U b, U c) {
    U r = loadedOriginal(p, a, b, c);
    if (r == 0 && !fault) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        safeRemember(p);
    }
    return r;
}

void unloadLocked(void* p) {
    pending.erase(p);
    auto it = canvases.find(p);
    if (it == canvases.end()) return;
    auto gs = it->second.first;
    unsigned kind = it->second.second;
    auto& ctx = contexts[gs];
    if (ctx.views[kind].p == p) ctx.views[kind] = View{};
    if (kind == 0) { ctx.page = Page::Stock; releaseAllTouches(gs); tablet::tabletPresent(false); controllers::levelChanged("the tablet unloaded"); }
    if (kind == 1) ctx.navState = -1;
    if (kind == 2) ctx.statusShown = -1, ctx.statusText.clear();
    canvases.erase(it);
    refreshSelection();
}

void unload(void* p, U a, U b, U c) {
    { std::lock_guard<std::recursive_mutex> lock(mutex); unloadLocked(p); }
    unloadOriginal(p, a, b, c);
}

void buttonTick(void* cs) {
    std::vector<void*> retry(pending.begin(), pending.end());
    for (auto p : retry) remember(p);
    auto& ctx = contexts[at<void*>(cs, 0x80)];
    gate(ctx, cs);
    if (ctx.views[0].p) scaleHitboxes(cs);  // only the button system that owns the tablet
    render(ctx);
}

void safeButtonTick(void* cs) {
    __try { buttonTick(cs); } __except (EXCEPTION_EXECUTE_HANDLER) { failRuntime("access fault in tablet update"); }
}

void button(void* cs) {
    { std::lock_guard<std::recursive_mutex> lock(mutex); if (!fault) safeButtonTick(cs); }
    buttonOriginal(cs);
    { std::lock_guard<std::recursive_mutex> lock(mutex); if (!fault) safeButtonTick(cs); }
    if (!fault) posters::afterButtonUpdate(cs);  // fingertips for the docked lobby poster
}

void onEvent(void* gs, U event, U actor, U component) {
    int cell = cellOf(component);
    if (!isOurTab(component) && !isStockTab(component) && cell < 0) return;
    auto it = contexts.find(gs);
    if (it == contexts.end() || !it->second.views[0].p) return;
    auto& c = it->second;
    std::array<U, 3> key = {reinterpret_cast<U>(gs), actor, component};
    if (event == RELEASE) {
        if (held.erase(key) && cell >= 0 && shared) arcade::pushTouch(shared, unsigned(cell), arcade::TouchUp);
        return;
    }
    if (!held.insert(key).second) return;
    if (isStockTab(component)) {
        if (c.page == Page::Arcade) { releaseAllTouches(gs); held.insert(key); logf("left ARCADE page"); }
        c.page = Page::Stock;
    } else if (isOurTab(component)) {
        Mode mode = component == SETTINGS_TAB ? Mode::Settings : Mode::Arcade;
        if (c.page != Page::Arcade || c.mode != mode) logf("%s page selected", mode == Mode::Settings ? "SETTINGS" : "ARCADE");
        if (c.page == Page::Arcade && c.mode != mode) { releaseAllTouches(gs); held.insert(key); }
        c.page = Page::Arcade;
        c.mode = mode;
        startHost();
    } else if (c.page == Page::Arcade && shared) {
        arcade::pushTouch(shared, unsigned(cell), arcade::TouchDown);
    }
    refreshSelection();
}

void safeEvent(void* gs, U event, U actor, U component) {
    __try { onEvent(gs, event, actor, component); } __except (EXCEPTION_EXECUTE_HANDLER) { failRuntime("access fault in event dispatch"); }
}

void dispatch(void* gs, U event, U actor, U component, int arg) {
    if (!fault && (event == PRESS || event == RELEASE)) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        safeEvent(gs, event, actor, component);
    }
    dispatchOriginal(gs, event, actor, component, arg);
}

// ---- setup ----
bool hook(unsigned rva, void* detour, void** original) {
    MH_STATUS s = MH_CreateHook(exe + rva, detour, original);
    if (s == MH_OK) s = MH_EnableHook(exe + rva);
    if (s != MH_OK) { logf("hook at %x failed: %s", rva, MH_StatusToString(s)); return false; }
    return true;
}

bool verifyExecutable() {
    exe = reinterpret_cast<P>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(exe + at<unsigned>(exe, 0x3c));
    if (nt->OptionalHeader.SizeOfImage != EXE_IMAGE_SIZE || nt->FileHeader.TimeDateStamp != EXE_TIMESTAMP) {
        logf("unsupported echovr.exe (timestamp %u, size %u)", nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        return false;
    }
    for (auto& s : SIGNATURES)
        if (memcmp(exe + s.rva, s.bytes, 16) != 0) { logf("code signature mismatch at %x (another mod patched it?)", s.rva); return false; }
    return true;
}

void heartbeatLoop() {
    for (;;) {
        Sleep(100);
        U now = GetTickCount64();
        bool visible = arcadeSelected && now - lastTabletTick.load() < 500;
        if (shared) {
            shared->gameHeartbeat = LONG64(now);
            shared->pageVisible = visible;
            shared->pageMode = pageMode;
            if (shared->tabletScale > 0) tablet::request(shared->tabletScale / 1000.f);
            posters::heartbeat(now);
            controllers::tick(now);
            voice::heartbeat(now);
        }
        stream::setVisible(visible);
    }
}

void initialize() {
    logf("EchoArcade runtime starting (pid %lu)", GetCurrentProcessId());
    if (!verifyExecutable()) { fault = true; return; }
    for (unsigned i = 0; i < ARCADE_GRID_COLS * ARCADE_GRID_ROWS; i++) cellIndex[ARCADE_CELLS[i]] = i;
    HANDLE mapping = nullptr;
    shared = arcade::open(&mapping);
    if (!shared) logf("shared memory unavailable (error %lu); touches and frames disabled", GetLastError());
    else { shared->gamePid = LONG(GetCurrentProcessId()); shared->cols = ARCADE_GRID_COLS; shared->rows = ARCADE_GRID_ROWS; }
    job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        // Breakaway allowed: the tablet's NORMAL TABLET helper has to outlive Echo (it uninstalls after).
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) { logf("MinHook init failed"); fault = true; return; }
    show = fn<Show>(exe, 0x71c820);
    text = fn<Text>(exe, 0x727f10);
    alpha = fn<Alpha>(exe, 0x726f00);
    enableButton = fn<Mask>(exe, 0x92bd10);
    disableButton = fn<Mask>(exe, 0x92b9e0);
    bool ok = hook(0x71fc90, reinterpret_cast<void*>(loaded), reinterpret_cast<void**>(&loadedOriginal)) &&
              hook(0x7287b0, reinterpret_cast<void*>(unload), reinterpret_cast<void**>(&unloadOriginal)) &&
              hook(0x510060, reinterpret_cast<void*>(dispatch), reinterpret_cast<void**>(&dispatchOriginal)) &&
              hook(0x92f3f0, reinterpret_cast<void*>(button), reinterpret_cast<void**>(&buttonOriginal));
    if (!ok) { fault = true; return; }
    logf("tablet hooks installed; %u touch cells", unsigned(cellIndex.size()));
    if (shared) posters::install(exe, shared);  // DOCK, poster touch and light gun (disables only itself on a mismatch)
    if (shared) controllers::install(exe, shared);  // CONTROLLERS: hooks go in the first time it's switched on
    voice::attach(shared);                          // VOICE: Echo's voice chat mode from the tablet
    std::thread(heartbeatLoop).detach();
    if (shared) stream::install(shared);
}

// Starts once, whichever comes first: EchoLoader's PluginInit (only called when the
// plugin has "args" in echoloader.json) or our own DllMain.
void start(HMODULE self) {
    static std::once_flag once;
    std::call_once(once, [self] {
        if (GetEnvironmentVariableW(L"ECHO_INNER", nullptr, 0)) return;  // a second Echo VR the arcade runs on the tablet: not for us
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(self, path, MAX_PATH);
        pluginDir = path;
        pluginDir.resize(pluginDir.find_last_of(L"\\/"));
        logf("EchoArcade loaded from %ls", path);
        if (wchar_t* block = GetEnvironmentStringsW()) {
            const wchar_t* end = block;
            while (*end) end += wcslen(end) + 1;
            startupEnvironment.assign(block, size_t(end - block) + 1);  // keep the final terminator
            FreeEnvironmentStringsW(block);
        }
        tweaks::start(pluginDir);  // independent of the tablet: must beat the game's first Oculus FOV query
        voice::hookEarly(reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)));  // must beat the game's settings load
        // Echo above the arcade's apps in the scheduler (a Minecraft launcher on a small CPU made
        // Echo stutter); echo_tweaks.ini [cpu] echo_above_normal=0 leaves the priority alone.
        if (GetPrivateProfileIntW(L"cpu", L"echo_above_normal", 1, (pluginDir + L"\\EchoArcade\\echo_tweaks.ini").c_str()) != 0) {
            bool ok = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) != FALSE;
            logf("cpu: Echo VR %s above-normal priority", ok ? "runs at" : "could not get");
        }
        tablet::start(pluginDir);
        // Hooks and the probe D3D12 device are created off the loader thread.
        HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { initialize(); return 0; }, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    });
}

}  // namespace

extern "C" __declspec(dllexport) void PluginInit(const char*) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&PluginInit), &self);
    start(self);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        HMODULE pinned = nullptr;  // never unload while hooks point into us
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
        start(self);  // the thread it creates runs after the loader lock is released
    }
    return TRUE;
}
