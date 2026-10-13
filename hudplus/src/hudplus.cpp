// EchoHudPlus.dll -- EchoLoader plugin: extra readouts next to Echo VR's helmet HUD.
//
//  * Boost gauges (off by default), one per boost charge: a red arc while the charge
//    comes back, green when it's ready (with a pulse).
//  * Wrist thruster gauges at the bottom, one per hand, in your HUD colour: the ring
//    fills with how much of that hand's push goes into one direction with the other
//    hand (full = the best speed). Your mute icon sits in the left ring and the
//    connection bars in the right one (Echo's own ones are hidden by hud_lines). While your
//    hands push lined up, the edges of the view glow in the HUD colour, like shielding:
//    steady, brighter the better they line up, none up to 3.5 m/s, then building to its
//    peak at 4.8.
//  * HUD colour: your team's (Echo's blue or orange), white when not on a team.
//  * Speed tracker: above the speed, in Echo's own speed font and your HUD colour, the
//    top speed of your last run at or above 10 m/s, held until the next run replaces it.
//  * Stun flash: when you get stunned, a green flash if your shield was up on your side
//    (or dropped within shield_grace_ms), red if it wasn't.
//  * Pass counter at the top: "+1" each time the disc goes to a teammate, the count itself
//    from the 6th pass, gone when the other team gets it; a goal shows the chain 50% bigger,
//    flashing. From Echo's own API (127.0.0.1:6721, Settings > Game > API access).
//
// Game data comes from three hooks in echovr.exe (see game_memory.h), each enabled only
// if the code at that address is exactly what it was written for. The overlay is one
// texture shown through head-locked Oculus quad layers that Echo's frames carry to the
// compositor, the same way the readout's own canvases sit in front of your eyes. It is
// painted on a thread of its own; Echo's render thread spends microseconds on it.
// If the runtime ever rejects a frame with them, that frame is resubmitted without
// them and the overlay turns off for the session (see the log).
//
// Settings: plugins\EchoHudPlus\settings.ini (re-read when it changes: the arcade tablet's
// SETTINGS page writes it)   Log: %LOCALAPPDATA%\EchoHudPlus\hudplus.log
#include "hudplus_core.h"
#include "game_memory.h"
#include "passes.h"
#include "../discbutton/minhook/include/MinHook.h"
#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <share.h>
#include <string>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace {

using namespace hudplus;
using ovrResult = int;
using Session = void*;
using SwapChain = void*;

struct SwapChainDesc {  // ovrTextureSwapChainDesc
    int type, format, arraySize, width, height, mipLevels, sampleCount;
    char staticImage;
    unsigned miscFlags, bindFlags;
};
static_assert(sizeof(SwapChainDesc) == 40, "ovrTextureSwapChainDesc layout");
constexpr int kTexture2D = 0, kFormatRGBA8sRGB = 5;  // ovrTexture_2D, OVR_FORMAT_R8G8B8A8_UNORM_SRGB

using CreateSwapChainFn = ovrResult(__cdecl*)(Session, IUnknown*, const SwapChainDesc*, SwapChain*);
using GetBufferFn = ovrResult(__cdecl*)(Session, SwapChain, int, IID, void**);
using ChainIntFn = ovrResult(__cdecl*)(Session, SwapChain, int*);
using CommitFn = ovrResult(__cdecl*)(Session, SwapChain);
using SubmitFn = ovrResult(__cdecl*)(Session, long long, const void*, const void* const*, unsigned);

CreateSwapChainFn realCreate = nullptr;
GetBufferFn getBuffer = nullptr;
ChainIntFn getLength = nullptr, getCurrentIndex = nullptr;
CommitFn commit = nullptr;
SubmitFn realSubmitFrame = nullptr, realEndFrame = nullptr;

Settings settings;
DigitFont digitFont;  // Echo's speed digits (plugins\EchoHudPlus\digits.bin), if installed
bool enabled = true;
std::mutex logLock;
FILE* logFile = nullptr;

void logf(const char* format, ...) {
    std::lock_guard<std::mutex> lock(logLock);
    if (!logFile) {
        wchar_t base[MAX_PATH];
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return;
        std::wstring dir = std::wstring(base) + L"\\EchoHudPlus";
        CreateDirectoryW(dir.c_str(), nullptr);
        logFile = _wfsopen((dir + L"\\hudplus.log").c_str(), L"a", _SH_DENYNO);
        if (!logFile) return;
    }
    fprintf(logFile, "%llu ", GetTickCount64());
    va_list args;
    va_start(args, format);
    vfprintf(logFile, format, args);
    va_end(args);
    fputc('\n', logFile);
    fflush(logFile);
}

double now() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

// ---- game state, written by the game hooks, read by the render thread ----------------
std::mutex stateLock;
struct State {
    double localAt = -1;  // when the boost hook last read the local player
    game::Local local;
    SpeedTracker tracker;
    bool shieldUp = false;
    double shieldUpAt = -1;
    bool voicePressed = false;
    double voiceAt = -1;  // when the voice hook last found your entry
    Flash flash;
    RollingMax peak;  // top speed over the last few seconds
    PassCounter passes;
    int apiTeam = -1;  // your team, as the API sees it (for teams=mine)
} state;

uintptr_t batteryVtable = 0;
int logBudget = 120;  // game-side diagnostic lines per session

void observeLocal(void* system, void* entry) {
    game::Local l;
    bool ok = game::readLocal(system, entry, batteryVtable, l);
    double t = now();
    // Only the local player prefabs have a PlayerNav component, so one entry is expected.
    static const void* lastEntry = nullptr;
    static double lastEntryAt = -1;
    static bool warned = false;
    if (lastEntry && entry != lastEntry && t - lastEntryAt < 0.05 && !warned) {
        warned = true;
        logf("note: more than one PlayerNav entry updating at once (%p, %p); showing the latest", lastEntry, entry);
    }
    lastEntry = entry;
    lastEntryAt = t;
    float before;
    bool first;
    {
        std::lock_guard<std::mutex> g(stateLock);
        first = state.localAt < 0;
        before = state.local.energy;
        if (ok) {
            state.local = l;
            state.localAt = t;
            if (l.speedOk) {
                state.tracker.update(l.speed, t, settings.speedThreshold, settings.burstGap);
                state.peak.add(l.speed, t);
            }
        }
    }
    static uint32_t lastOn[2] = {0, 0};
    static int thrusterLogBudget = 12;  // enough to tell which hand is which
    if (ok && l.thrustersOk && (l.handOn[0] != lastOn[0] || l.handOn[1] != lastOn[1])) {
        if (thrusterLogBudget > 0) {
            thrusterLogBudget--;
            logf("thrusters: hand0 %s, hand1 %s", l.handOn[0] ? "on" : "off", l.handOn[1] ? "on" : "off");
        }
        lastOn[0] = l.handOn[0];
        lastOn[1] = l.handOn[1];
    }
    static int lastTeam = -9, lastMuted = -1, netLogBudget = 20;
    if (ok && (l.team != lastTeam || int(l.muted) != lastMuted) && netLogBudget > 0) {
        netLogBudget--;
        logf("network game %s: team %d, voice %s, mode %d, ping %d ms", l.netOk ? "found" : "not found", l.team, l.muted ? "muted" : "on",
             l.voiceMode, l.ping);
    }
    if (ok) {
        lastTeam = l.team;
        lastMuted = l.muted;
    }
    static int lastFailStage = -1;
    if (!ok) {
        if (l.stage != lastFailStage && logBudget > 0) {
            logBudget--;
            logf("local player read stopped at stage %d", l.stage);
        }
        lastFailStage = l.stage;
        return;
    }
    if (logBudget > 0 && (first || (l.battery && l.energy < before - 1))) {
        logBudget--;
        logf("%s: actor %016llx battery=%d energy %.1f/%.1f cost %.1f rate %.1f delay %.2f (left %.2f) speed %.2f "
             "thrusters=%d single %.2f/%.2f two %.2f/%.2f stage %d",
             first ? "local player found" : "boost", (unsigned long long)l.actor, l.battery, l.energy, l.maxPower, l.cost, l.rate, l.delay,
             l.delayLeft, l.speed, l.thrustersOk, l.handSingle[0], l.handSingle[1], l.handTwo[0], l.handTwo[1], l.stage);
    }
}

void observeShield(void* punchSystem) {
    const void* ctrl;
    uint32_t id;
    {
        std::lock_guard<std::mutex> g(stateLock);
        if (state.localAt < 0) return;
        ctrl = state.local.ctrlSystem;
        id = state.local.ctrlId;
    }
    uint32_t flags = 0;
    static int found = -1;
    bool ok = game::readShield(punchSystem, ctrl, id, flags);
    if (int(ok) != found && logBudget > 0) {
        logBudget--;
        logf(ok ? "local shield state found (flags %08x)" : "local shield state not found", flags);
    }
    found = ok;
    if (!ok) return;
    bool up = (flags & game::kShieldUp) != 0;
    bool was;
    {
        std::lock_guard<std::mutex> g(stateLock);
        was = state.shieldUp;
        state.shieldUp = up;
        if (up) state.shieldUpAt = now();
    }
    if (up != was && logBudget > 0) {
        logBudget--;
        logf("shield %s (flags %08x)", up ? "up" : "down", flags);
    }
}

void observeVoice(void* voipSystem) {
    bool pressed = false;
    bool ok = game::readVoicePressed(voipSystem, pressed);
    bool was;
    {
        std::lock_guard<std::mutex> g(stateLock);
        was = state.voicePressed;
        if (ok) {
            state.voicePressed = pressed;
            state.voiceAt = now();
        }
    }
    static int found = -1, budget = 12;
    if ((int(ok) != found || (ok && pressed != was)) && budget > 0) {
        budget--;
        logf(ok ? "voice: talk button %s" : "voice: your entry not found%s", ok ? (pressed ? "held" : "released") : "");
    }
    found = ok;
}

void observeStun(void* punchableSystem, uint32_t id, float seconds) {
    uint64_t actor = 0;
    bool already = false;
    bool ok = game::readStunTarget(punchableSystem, id, actor, already);
    double t = now();
    bool mine, green = false, up;
    double upAgo;
    uint64_t local;
    {
        std::lock_guard<std::mutex> g(stateLock);
        local = state.local.actor;
        mine = ok && state.localAt >= 0 && actor == local && !already && seconds > 0.01f && seconds < 30;
        up = state.shieldUp;
        upAgo = state.shieldUpAt >= 0 ? t - state.shieldUpAt : -1;
        if (mine) {
            green = shieldCounts(state.shieldUp, state.shieldUpAt, t, settings.shieldGrace);
            state.flash = Flash{t, seconds, green};
        }
    }
    if (logBudget > 0 && (mine || ok)) {
        logBudget--;
        logf("stun %.2fs on %016llx (you %016llx)%s%s shield %s, last up %.2fs ago", seconds, (unsigned long long)actor,
             (unsigned long long)local, already ? " already stunned" : "", mine ? (green ? " -> GREEN" : " -> RED") : "",
             up ? "up" : "down", upAgo);
    }
}

// ---- pass counter: Echo's API, polled on its own thread -------------------------------
std::atomic<bool> passesWanted{false};  // set by the render thread: HUD and pass counter on
#ifdef HUDPLUS_TEST
std::atomic<int> apiPort{6721};
#else
constexpr int apiPort = 6721;
#endif

// GET http://127.0.0.1:6721/session. False if the API isn't answering (off, or not in a match).
// The connection stays open between asks (dropped and remade after a failure).
bool fetchSession(HINTERNET http, HINTERNET& connection, std::string& body) {
    if (!connection) connection = WinHttpConnect(http, L"127.0.0.1", INTERNET_PORT(int(apiPort)), 0);
    if (!connection) return false;
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/session", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    bool ok = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, size = sizeof status;
    ok = ok && WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                   WINHTTP_NO_HEADER_INDEX) && status == 200;
    body.clear();
    for (DWORD got = 0; ok; ) {
        char buf[8192];
        if (!WinHttpReadData(request, buf, sizeof buf, &got)) { ok = false; break; }
        if (got == 0) break;
        body.append(buf, got);
        if (body.size() > (1 << 20)) { ok = false; break; }
    }
    if (request) WinHttpCloseHandle(request);
    if (!ok) { WinHttpCloseHandle(connection); connection = nullptr; }
    return ok;
}

DWORD WINAPI pollPasses(LPVOID) {
    HINTERNET http = WinHttpOpen(L"EchoHudPlus", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!http) { logf("pass counter: WinHTTP unavailable (%lu)", GetLastError()); return 0; }
    WinHttpSetTimeouts(http, 250, 250, 250, 500);
    bool answering = true;  // for the log: say when the API stops / starts answering
    int parseErrors = 0;
    HINTERNET connection = nullptr;
    while (true) {
        if (!passesWanted) {
            if (connection) { WinHttpCloseHandle(connection); connection = nullptr; }
            Sleep(250);
            continue;
        }
        std::string body;
        Possession p;
        bool got = fetchSession(http, connection, body);
        if (got) {
            Json doc;
            if (parseJson(body, doc)) p = readSession(doc);
            else if (parseErrors++ < 3) logf("pass counter: unreadable /session answer (%zu bytes)", body.size());
        }
        if (got != answering) {
            answering = got;
            logf(got ? "pass counter: Echo's API answers" : "pass counter: no answer from Echo's API (not in a match, or API access off)");
        }
        {
            std::lock_guard<std::mutex> g(stateLock);
            state.passes.update(p, now());
            state.apiTeam = p.myTeam;
        }
        Sleep(got ? 100 : 1000);  // 10 a second in a match (a catch lasts longer); once a second otherwise
    }
}

// ---- game hooks ------------------------------------------------------------------------
// Each detour passes the original's return value back, in case a caller reads it.
using BoostInputFn = uint64_t(__fastcall*)(void*, void*, float);
using UpdateFn = uint64_t(__fastcall*)(void*, void*, void*, void*);
using StunFn = uint64_t(__fastcall*)(void*, uint32_t, uint32_t, uint32_t, uint64_t, float);
BoostInputFn realBoostInput = nullptr;
UpdateFn realPunchUpdate = nullptr, realVoipUpdate = nullptr;
StunFn realStun = nullptr;

uint64_t __fastcall boostInputHook(void* system, void* entry, float dt) {
    uint64_t r = realBoostInput(system, entry, dt);
    observeLocal(system, entry);
    return r;
}
uint64_t __fastcall punchUpdateHook(void* system, void* a2, void* a3, void* a4) {
    uint64_t r = realPunchUpdate(system, a2, a3, a4);
    observeShield(system);
    return r;
}
uint64_t __fastcall voipUpdateHook(void* system, void* a2, void* a3, void* a4) {
    uint64_t r = realVoipUpdate(system, a2, a3, a4);
    observeVoice(system);
    return r;
}
uint64_t __fastcall stunHook(void* system, uint32_t id, uint32_t a3, uint32_t a4, uint64_t a5, float seconds) {
    observeStun(system, id, seconds);
    return realStun(system, id, a3, a4, a5, seconds);
}

bool hookAt(void* target, void* detour, void** original, const char* name) {
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s == MH_OK) s = MH_EnableHook(target);
    logf("%s hook %s", name, s == MH_OK ? "installed" : MH_StatusToString(s));
    return s == MH_OK;
}

void hookGame() {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    bool boost = game::codeMatches(base, game::kBoostInputRva, game::kBoostInputBytes, sizeof game::kBoostInputBytes) &&
                 game::batteryVtableMatches(base);
    bool punch = game::codeMatches(base, game::kPunchUpdateRva, game::kPunchUpdateBytes, sizeof game::kPunchUpdateBytes);
    bool stun = game::codeMatches(base, game::kStunRva, game::kStunBytes, sizeof game::kStunBytes);
    bool voip = game::codeMatches(base, game::kVoipUpdateRva, game::kVoipUpdateBytes, sizeof game::kVoipUpdateBytes);
    if (!boost) {
        logf("echovr.exe is not the build this was made for: no game data (overlay stays empty)");
        return;
    }
    batteryVtable = base + game::kBatteryVtableRva;
    if (!hookAt(reinterpret_cast<void*>(base + game::kBoostInputRva), reinterpret_cast<void*>(boostInputHook),
                reinterpret_cast<void**>(&realBoostInput), "boost input")) return;
    if (voip)
        hookAt(reinterpret_cast<void*>(base + game::kVoipUpdateRva), reinterpret_cast<void*>(voipUpdateHook),
               reinterpret_cast<void**>(&realVoipUpdate), "voice update");
    else
        logf("voice update code differs from expected: mic shows only muted / open mic");
    if (!settings.stun) return;
    if (punch && stun) {
        hookAt(reinterpret_cast<void*>(base + game::kPunchUpdateRva), reinterpret_cast<void*>(punchUpdateHook),
               reinterpret_cast<void**>(&realPunchUpdate), "shield update");
        hookAt(reinterpret_cast<void*>(base + game::kStunRva), reinterpret_cast<void*>(stunHook), reinterpret_cast<void**>(&realStun), "stun");
    } else {
        logf("shield/stun code differs from expected (%d/%d): stun flash off", punch, stun);
    }
}

// ---- overlay ---------------------------------------------------------------------------
// Echo's render thread decides only WHAT to show: a DrawJob (a few keys, numbers and strings,
// built in microseconds from the game state) that it hands to the draw thread. The draw
// thread paints the cells that changed, encodes the picture and copies it into a staging
// buffer, at most max_fps times a second. Showing a finished picture, back on the render
// thread, is one pre-recorded command list and a commit. Nothing slow runs inside Echo's
// frame; the log says how long our part took, once a minute.
ID3D12CommandQueue* gameQueue = nullptr;
enum class Mode { Waiting, Ready, Off } mode = Mode::Waiting;
size_t headerSize = 0;
bool triedOtherLayout = false;

struct DrawJob {
    uint64_t serial = 0;
    uint32_t generation = 0;  // settings generation: a new one repaints everything
    float maxFps = 60;
    int colorKey = 0;
    Color hud{1, 1, 1};
    int flashKey = -1;        // stun flash (0..), edge glow (1000..) or nothing (-1)
    bool flashGreen = false;
    float flashAlpha = 0, edge = 0;
    int gaugeKey[2] = {-2, -2};
    float gaugeFill[2] = {0, 0}, gaugePulse[2] = {0, 0};
    int thrustKey[2] = {-2, -2};
    float score[2] = {0, 0};
    bool thrustOn[2] = {false, false}, icons = true, rings = false, talk = false;
    int bars = 0;
    int glowKey[Settings::kRings] = {-2, -2, -2, -2, -2};
    float glowFill[Settings::kRings] = {}, glowAt[Settings::kRings] = {};
    char current[16] = "", last[16] = "", peak[16] = "";
    float currentSize = 0, lastSize = 0;
    int passKey = -1, passNumber = 0;
    bool passPlus = false;
    Color passColor{1, 1, 1};
    float passAlpha = 0, passScale = 1;
};

bool sameText(const DrawJob& a, const DrawJob& b) {  // sizes and colour matter only with text to show
    if (std::strcmp(a.current, b.current) || std::strcmp(a.last, b.last) || std::strcmp(a.peak, b.peak)) return false;
    return !a.current[0] || (a.currentSize == b.currentSize && a.lastSize == b.lastSize && a.colorKey == b.colorKey);
}
bool differs(const DrawJob& a, const DrawJob& b) {
    if (a.generation != b.generation || a.flashKey != b.flashKey || a.passKey != b.passKey || !sameText(a, b)) return true;
    for (int i = 0; i < 2; i++)
        if (a.gaugeKey[i] != b.gaugeKey[i] || a.thrustKey[i] != b.thrustKey[i]) return true;
    for (int k = 0; k < Settings::kRings; k++)
        if (a.glowKey[k] != b.glowKey[k]) return true;
    return false;
}

struct Overlay {  // the render thread's side
    SwapChain chain = nullptr;
    std::vector<ID3D12Resource*> buffers;
    Uploader uploader;
    DrawJob lastJob;  // what the draw thread was last asked for
    uint64_t serial = 0;
    uint32_t generation = 0;
    double readyAt[2] = {-1, -1};
    float lastFill[2] = {1, 1};
    float edgeLevel = 0;  // edge glow, 0..1: how lined up the hands are (smoothed)
    double edgeAt = -1;
    QuadLayer quads[12];
    double frameUs = 0, frameMaxUs = 0, statsAt = -1;  // our part of each frame, for the log
    unsigned frames = 0;
    int errorBudget = 10;
} ov;

// ---- the draw thread ----
struct Painter {
    HANDLE thread = nullptr, wake = nullptr;
    std::mutex lock;
    DrawJob pending;  // the newest job only: what the render thread wanted last
    bool hasPending = false;
    std::atomic<uint64_t> drawn{0};
    std::atomic<unsigned> pictures{0};
    std::atomic<long long> busyUs{0};
    Canvas canvas;
    std::vector<uint8_t> bytes;
    DrawJob last;
    bool first = true;
    double lastDrawAt = -1;
} painter;

void post(const DrawJob& job) {
    {
        std::lock_guard<std::mutex> g(painter.lock);
        painter.pending = job;
        painter.hasPending = true;
    }
    SetEvent(painter.wake);
}

// Paints what changed since the last job into the canvas. True if anything did.
bool paint(const DrawJob& job) {
    Canvas& cv = painter.canvas;
    const DrawJob& last = painter.last;
    bool all = painter.first || job.generation != last.generation, any = false;
    if (all || job.flashKey != last.flashKey) {
        if (job.flashKey >= 1000) drawEdgeGlow(cv, kFlashRect, job.hud, job.edge, job.edge);
        else drawFlash(cv, kFlashRect, job.flashGreen ? kGreen : kRed, job.flashKey < 0 ? 0 : job.flashAlpha);
        any = true;
    }
    for (int side = 0; side < 2; side++) {
        if (all || job.gaugeKey[side] != last.gaugeKey[side]) {
            if (job.gaugeKey[side] == -2) cv.clear(kGaugeRect[side]);
            else drawGauge(cv, kGaugeRect[side], job.gaugeFill[side], job.gaugePulse[side]);
            any = true;
        }
        if (all || job.thrustKey[side] != last.thrustKey[side]) {
            if (job.thrustKey[side] == -2) cv.clear(kThrustRect[side]);
            else drawThrusterCell(cv, kThrustRect[side], side, job.score[side], job.thrustOn[side], job.hud, job.icons, job.talk, job.bars, job.rings);
            any = true;
        }
    }
    for (int k = 0; k < Settings::kRings; k++)
        if (all || job.glowKey[k] != last.glowKey[k]) {
            if (job.glowKey[k] == -2) cv.clear(kGlowRect[k]);
            else drawRingCell(cv, kGlowRect[k], job.glowFill[k], job.hud, job.glowAt[k], &digitFont);
            any = true;
        }
    if (all || !sameText(job, last)) {
        drawSpeeds(cv, kTextRect, job.current, job.last, job.hud, &digitFont, job.currentSize, job.lastSize, job.peak);
        any = true;
    }
    if (all || job.passKey != last.passKey) {
        drawPassCounter(cv, kPassRect, job.passNumber, job.passPlus, job.passColor, job.passAlpha, &digitFont);
        any = true;
    }
    painter.last = job;
    painter.first = false;
    return any;
}

DWORD WINAPI drawThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(painter.wake, 250);
        DrawJob job;
        bool have = false;
        {
            std::lock_guard<std::mutex> g(painter.lock);
            if (painter.hasPending) { job = painter.pending; painter.hasPending = false; have = true; }
        }
        if (!have) continue;
        double wait = painter.lastDrawAt >= 0 ? painter.lastDrawAt + 1.0 / std::max(job.maxFps, 1.f) - now() : 0;
        if (wait > 0.0005) {  // no more than max_fps pictures a second: wait, then take the newest job
            Sleep(DWORD(wait * 1000 + 0.5));
            std::lock_guard<std::mutex> g(painter.lock);
            if (painter.hasPending) { job = painter.pending; painter.hasPending = false; }
        }
        double t0 = now();
        painter.lastDrawAt = t0;
        if (paint(job)) {
            painter.canvas.encode(painter.bytes);
            int slot = ov.uploader.acquire(1000);
            if (slot >= 0) {
                ov.uploader.fill(slot, painter.bytes.data());
                ov.uploader.publish(slot);
                painter.pictures++;
            } else if (ov.errorBudget > 0) {
                ov.errorBudget--;
                logf("draw thread: the GPU is still reading every staging buffer; picture skipped");
            }
        }
        painter.busyUs += (long long)((now() - t0) * 1e6);
        painter.drawn = job.serial;
    }
}

// ---- the render thread's side ----
ovrResult __cdecl hookedCreate(Session session, IUnknown* d3d, const SwapChainDesc* desc, SwapChain* out) {
    ID3D12CommandQueue* queue = nullptr;
    if (!gameQueue && d3d && SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&queue)))) {
        gameQueue = queue;  // kept for the process lifetime
        logf("captured the game's D3D12 queue (%dx%d swap chain)", desc ? desc->width : 0, desc ? desc->height : 0);
    }
    return realCreate(session, d3d, desc, out);
}

void prepare(Session session, const void* const* layers, unsigned count) {
    mode = Mode::Off;  // until everything below succeeds
    if (!gameQueue) { logf("no D3D12 queue captured (hook installed too late?); overlay off"); return; }
    SwapChainDesc desc{kTexture2D, kFormatRGBA8sRGB, 1, kTexW, kTexH, 1, 1, 0, 0, 0};
    ovrResult r = realCreate(session, gameQueue, &desc, &ov.chain);
    if (r < 0) { logf("ovr_CreateTextureSwapChainDX failed (%d); overlay off", r); return; }
    int length = 0;
    if (getLength(session, ov.chain, &length) < 0 || length < 1 || length > 8) { logf("swap chain length %d; overlay off", length); return; }
    for (int i = 0; i < length; i++) {
        ID3D12Resource* texture = nullptr;
        r = getBuffer(session, ov.chain, i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&texture));
        if (r < 0 || !texture) { logf("ovr_GetTextureSwapChainBufferDX(%d) failed (%d); overlay off", i, r); return; }
        ov.buffers.push_back(texture);  // kept for the process lifetime
    }
    if (const char* error = ov.uploader.init(gameQueue, ov.buffers)) { logf("uploader: %s; overlay off", error); return; }
    std::vector<uint8_t> clear(size_t(kTexW) * kTexH * 4, 0);  // the first picture: nothing
    int index = 0;
    if (getCurrentIndex(session, ov.chain, &index) < 0 || index < 0 || index >= length) { logf("swap chain index %d; overlay off", index); return; }
    if (const char* error = ov.uploader.write(index, clear.data())) { logf("first overlay upload failed (%s); overlay off", error); return; }
    if (commit(session, ov.chain) < 0) { logf("first commit failed; overlay off"); return; }
    if (!painter.thread) {
        painter.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        painter.thread = painter.wake ? CreateThread(nullptr, 0, drawThread, nullptr, 0, nullptr) : nullptr;
        if (!painter.thread) { logf("draw thread failed (%lu); overlay off", GetLastError()); return; }
        SetThreadPriority(painter.thread, THREAD_PRIORITY_BELOW_NORMAL);  // never ahead of Echo's own threads
    }
    headerSize = detectHeaderSize(layers, count);
    mode = Mode::Ready;
    logf("overlay ready: %d buffers, layer header %zu bytes, game submits %u layers; painting on its own thread, at most %.0f pictures/s",
         length, headerSize, count, settings.maxFps);
}

// The game state the picture is built from, copied out under the lock (no allocations).
struct Snapshot {
    game::Local local;
    double localAt = -1;
    float trackerShown = NAN;
    bool voicePressed = false;
    double voiceAt = -1;
    Flash flash;
    float peak = -1;
    PassView pass;
    int apiTeam = -1;
};

Snapshot snapshot(double t) {
    Snapshot s;
    std::lock_guard<std::mutex> g(stateLock);
    s.local = state.local;
    s.localAt = state.localAt;
    s.trackerShown = state.tracker.shown;
    s.voicePressed = state.voicePressed;
    s.voiceAt = state.voiceAt;
    s.flash = state.flash;
    if (settings.peakBadge) s.peak = state.peak.max(t, settings.peakSeconds);
    if (settings.passes) s.pass = passView(state.passes, t, settings.passCountFrom);
    s.apiTeam = state.apiTeam;
    return s;
}

// What the picture should show now: keys that say which cells changed, and their contents.
DrawJob buildJob(const Snapshot& s, double t) {
    DrawJob job;
    job.generation = ov.generation;
    job.maxFps = settings.maxFps;
    bool fresh = s.localAt >= 0 && t - s.localAt < 0.5;
    int team = s.local.netOk ? s.local.team : kTeamNone;
    job.hud = teamColor(settings, team);
    job.colorKey = team == kTeamBlue ? 1 : team == kTeamOrange ? 2 : 0;

    // wrist thrusters, scored up front: a good push also lights up the edges of the view
    const bool showThrusters = settings.thrusters && fresh;
    Thrusters th;
    float score[2] = {0, 0};
    if (showThrusters) {
        for (int h = 0; h < 2 && s.local.thrustersOk; h++) {
            th.on[h] = s.local.handOn[h] != 0;
            std::memcpy(th.dir[h], s.local.handDir[h], sizeof th.dir[h]);
            th.single[h] = s.local.handSingle[h];
            th.two[h] = s.local.handTwo[h];
        }
        thrusterScores(th, score);
    }
    float edgeDt = ov.edgeAt >= 0 ? float(t - ov.edgeAt) : 0;
    ov.edgeAt = t;
    float glowAlpha = fresh && s.local.speedOk ? edgeGlowAlpha(settings, s.local.speed) : 0;
    // One steady glow around the whole edge: as bright as the hands are lined up, times the speed.
    float aligned = showThrusters && settings.edgeGlow ? alignment(settings, th, score) : 0;
    ov.edgeLevel = edgeGlowStep(ov.edgeLevel, aligned, edgeDt);
    int edgeStep = int(ov.edgeLevel * glowAlpha * 48 + .5f);

    // stun flash, else the edge glow (drawn first so everything else sits on top of it)
    float alpha = settings.stun ? flashAlpha(s.flash, t, settings.flashStrength) : 0;
    job.flashKey = alpha > 0 ? int(alpha * 96) * 2 + (s.flash.green ? 1 : 0) : -1;
    job.flashGreen = s.flash.green;
    job.flashAlpha = alpha > 0 ? float(int(alpha * 96)) / 96 : 0;
    if (job.flashKey < 0 && edgeStep > 0) {
        job.flashKey = 1000 + edgeStep * 4 + job.colorKey;
        job.edge = edgeStep / 48.f;
    }

    // boost gauges
    Battery b{s.local.energy, s.local.maxPower, s.local.cost};
    int charges = settings.boost && fresh && s.local.battery ? (std::min)(chargeCount(b), 2) : 0;
    for (int side = 0; side < 2; side++) {
        int k = settings.swapSides ? 1 - side : side;
        if (charges == 1) k = side == 1 ? 0 : 9;  // a single charge goes on the right
        if (k >= charges) { job.gaugeKey[side] = -2; continue; }
        float fill = chargeFill(b, k);
        if (fill >= 1 && ov.lastFill[side] < 1) ov.readyAt[side] = t;
        ov.lastFill[side] = fill;
        float pulse = ov.readyAt[side] >= 0 ? clamp01(1 - float(t - ov.readyAt[side]) / 0.4f) : 0;
        job.gaugeKey[side] = fill >= 1 ? 1000 + int(pulse * 24) : int(fill * 240);
        job.gaugeFill[side] = fill;
        job.gaugePulse[side] = pulse;
    }

    // wrist thruster gauges in the HUD colour, the mute icon in the left ring and the
    // connection bars in the right one (rings=0: just the icons)
    if (showThrusters && (settings.rings || settings.icons)) {
        job.bars = pingBars(s.local.ping, settings.pingBars);
        bool pressedKnown = s.voiceAt >= 0 && t - s.voiceAt < 0.5;
        job.talk = talking(s.local.muted, s.local.voiceMode, pressedKnown && s.voicePressed);
        job.icons = settings.icons;
        job.rings = settings.rings;
        for (int side = 0; side < 2; side++) {
            int h = settings.swapHands ? 1 - side : side;
            int icon = !settings.icons ? 0 : side == 0 ? 1 + int(job.talk) : 10 + job.bars;
            int gauge = settings.rings && th.on[h] ? int(score[h] * 120) + 1 : 0;
            job.thrustKey[side] = (gauge * 16 + icon) * 4 + job.colorKey;
            job.score[side] = score[h];
            job.thrustOn[side] = th.on[h];
        }
    }

    // speed rings: each fills from the previous ring's speed to its own, lit up at it
    for (int k = 0; k < Settings::kRings; k++) {
        if (!(settings.glow && fresh && s.local.speedOk)) { job.glowKey[k] = -2; continue; }
        float fill = glowFill(settings, k, s.local.speed);
        job.glowKey[k] = (fill >= 1 ? 200 : int(fill * 120)) * 4 + job.colorKey;
        job.glowFill[k] = fill;
        job.glowAt[k] = settings.glowAt[k];
    }

    // speeds: current on top, the last run's top speed under it
    if (settings.speed && fresh && s.local.speedOk) {
        formatSpeed(s.local.speed, job.current, sizeof job.current);
        formatSpeed(s.trackerShown, job.last, sizeof job.last);
    }
    if (settings.peakBadge && job.current[0]) formatSpeed(s.peak >= 0 ? s.peak : s.local.speed, job.peak, sizeof job.peak);
    float scale = kTextRect.h / settings.trackerPx;  // texture px per readout px
    job.currentSize = settings.currentFontPx * scale;
    job.lastSize = settings.fontPx * scale;

    // pass counter at the top, in the colour of the team with the chain
    passesWanted = settings.passes;
    PassView pv = s.pass;
    if (settings.passesMine && pv.team != s.apiTeam) pv = PassView{};
    if (pv.kind != PassView::None) {
        int teamKey = pv.team == kTeamBlue ? 1 : pv.team == kTeamOrange ? 2 : 0;
        job.passKey = ((pv.number * 2 + (pv.kind == PassView::Plus)) * 32 + int(pv.alpha * 24 + .5f)) * 4 + teamKey;
        job.passNumber = pv.number;
        job.passPlus = pv.kind == PassView::Plus;
        job.passColor = teamColor(settings, pv.team);
        job.passAlpha = pv.alpha;
        job.passScale = pv.scale;
    }
    return job;
}

// A finished picture, if the draw thread has one: one command list and a commit.
void showReady(Session session) {
    int slot = ov.uploader.takeReady();
    if (slot < 0) return;
    int index = -1;
    if (getCurrentIndex(session, ov.chain, &index) < 0 || index < 0 || index >= int(ov.buffers.size())) {
        ov.uploader.discard(slot);
        return;
    }
    if (const char* error = ov.uploader.submit(slot, index)) {
        if (ov.errorBudget > 0) { ov.errorBudget--; logf("overlay upload: %s", error); }
        return;
    }
    if (commit(session, ov.chain) < 0 && ov.errorBudget > 0) { ov.errorBudget--; logf("overlay commit failed"); }
}

// The quads to show for a job, bottom layer first.
unsigned layout(const DrawJob& job, const QuadLayer** out) {
    unsigned n = 0;
    auto add = [&](const Rect& r, Placement p) {
        buildQuad(ov.quads[n], headerSize, ov.chain, r, p.x, p.y, p.z, p.w, p.h);
        out[n] = &ov.quads[n];
        n++;
    };
    if (job.flashKey >= 0) add(kFlashRect, flashPlacement());
    for (int side = 0; side < 2; side++)
        if (job.gaugeKey[side] != -2) add(kGaugeRect[side], gaugePlacement(settings, side));
    for (int side = 0; side < 2; side++)
        if (job.thrustKey[side] != -2) add(kThrustRect[side], thrusterPlacement(settings, side));
    for (int k = 0; k < Settings::kRings; k++)
        if (job.glowKey[k] != -2) add(kGlowRect[k], glowPlacement(settings, k));
    if (job.current[0]) add(kTextRect, trackerPlacement(settings));
    if (job.passKey >= 0) add(kPassRect, passPlacement(settings, job.passScale));
    return n;
}

// Every frame: what to show (to the draw thread if it changed), a finished picture if there
// is one, and the quads.
unsigned refresh(Session session, const QuadLayer** out) {
    double t = now();
    DrawJob job = buildJob(snapshot(t), t);
    if (differs(job, ov.lastJob)) {
        job.serial = ++ov.serial;
        ov.lastJob = job;
        post(job);
    }
    showReady(session);
    return layout(ov.lastJob, out);
}

// Our part of each frame, summed up in the log once a minute.
void account(double t0) {
    double t = now(), us = (t - t0) * 1e6;
    ov.frameUs += us;
    ov.frames++;
    if (us > ov.frameMaxUs) ov.frameMaxUs = us;
    if (ov.statsAt < 0) ov.statsAt = t;
    if (t - ov.statsAt < 60) return;
    unsigned pictures = painter.pictures.exchange(0);
    long long busy = painter.busyUs.exchange(0);
    logf("last %.0f s: %u frames, HUD %.0f us avg / %.0f us max on Echo's render thread; %u pictures painted (%.0f us each, draw thread)",
         t - ov.statsAt, ov.frames, ov.frameUs / std::max(1u, ov.frames), ov.frameMaxUs, pictures, pictures ? double(busy) / pictures : 0.0);
    ov.frameUs = ov.frameMaxUs = 0;
    ov.frames = 0;
    ov.statsAt = t;
}

// ---- settings.ini, watched while Echo runs ----
void readSettings(const std::wstring& ini);  // below, with the keys
std::wstring settingsPath;
FILETIME settingsStamp{};
ULONGLONG settingsCheckedAt = 0;

FILETIME stampOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) ? a.ftLastWriteTime : FILETIME{};
}

// Once a second: if settings.ini changed (the tablet's SETTINGS page, or by hand), read it
// again and redraw everything. Runs even while the HUD is switched off, so it can come back.
void watchSettings() {
    ULONGLONG now = GetTickCount64();
    if (settingsPath.empty() || now - settingsCheckedAt < 1000) return;
    settingsCheckedAt = now;
    FILETIME stamp = stampOf(settingsPath);
    if (CompareFileTime(&stamp, &settingsStamp) == 0) return;
    settingsStamp = stamp;
    readSettings(settingsPath);
    ov.generation++;  // the next job differs from anything before: everything is repainted
    logf("settings.ini changed: reloaded (HUD %s)", enabled ? "on" : "off");
}

thread_local bool inSubmit = false;  // ovr_SubmitFrame may call ovr_EndFrame inside the runtime

struct SubmitGuard {
    SubmitGuard() { inSubmit = true; }
    ~SubmitGuard() { inSubmit = false; }
};

ovrResult submit(SubmitFn real, Session session, long long frame, const void* scale, const void* const* layers, unsigned count) {
    if (!inSubmit) watchSettings();
    if (inSubmit || !enabled || mode == Mode::Off || !layers || count == 0 || count > 5) {
        if (!inSubmit) passesWanted = false;
        return real(session, frame, scale, layers, count);
    }
    SubmitGuard guard;
    if (mode == Mode::Waiting) prepare(session, layers, count);
    if (mode != Mode::Ready) return real(session, frame, scale, layers, count);
    double t0 = now();
    const QuadLayer* extra[12];
    unsigned n = refresh(session, extra);
    account(t0);
    if (n == 0) return real(session, frame, scale, layers, count);
    const void* list[5 + 12];
    for (unsigned i = 0; i < count; i++) list[i] = layers[i];
    for (unsigned i = 0; i < n; i++) list[count + i] = extra[i]->bytes;
    ovrResult r = real(session, frame, scale, list, count + n);
    if (r >= 0) return r;
    if (!triedOtherLayout) {  // the header-layout guess may be wrong: try the other one once
        triedOtherLayout = true;
        headerSize = headerSize == 8 ? 136 : 8;
        n = refresh(session, extra);  // rebuilds the quads with the other header
        for (unsigned i = 0; i < n; i++) list[count + i] = extra[i]->bytes;
        ovrResult again = real(session, frame, scale, list, count + n);
        if (again >= 0) { logf("runtime accepted the %zu-byte layer header", headerSize); return again; }
    }
    mode = Mode::Off;
    logf("runtime rejected the overlay layers (%d); overlay off for this session", r);
    return real(session, frame, scale, layers, count);
}

ovrResult __cdecl hookedSubmitFrame(Session s, long long f, const void* v, const void* const* l, unsigned n) {
    return submit(realSubmitFrame, s, f, v, l, n);
}
ovrResult __cdecl hookedEndFrame(Session s, long long f, const void* v, const void* const* l, unsigned n) {
    return submit(realEndFrame, s, f, v, l, n);
}

// ---- settings and start-up ---------------------------------------------------------------
void readSettings(const std::wstring& ini) {
    settings = Settings{};  // keys missing from the file go back to their defaults
    auto f = [&](const wchar_t* section, const wchar_t* key, float fallback, float lo, float hi) {
        wchar_t buf[32];
        GetPrivateProfileStringW(section, key, L"", buf, 32, ini.c_str());
        float v = buf[0] ? float(_wtof(buf)) : fallback;
        return v < lo ? lo : v > hi ? hi : v;
    };
    auto b = [&](const wchar_t* section, const wchar_t* key, bool fallback) {
        return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, ini.c_str()) != 0;
    };
    Settings& s = settings;
    enabled = b(L"general", L"enabled", true);
    s.hudDistance = f(L"layout", L"hud_distance_m", s.hudDistance, 0.1f, 3.f);
    s.hudHeight = f(L"layout", L"hud_height_m", s.hudHeight, -1.f, 1.f);
    s.pxPerMetre = f(L"layout", L"px_per_metre", s.pxPerMetre, 50.f, 5000.f);
    s.boost = b(L"boost", L"enabled", s.boost);
    s.gaugePx = f(L"boost", L"size_px", s.gaugePx, 4.f, 100.f);
    s.boostX = f(L"boost", L"x_px", s.boostX, 0.f, 400.f);
    s.boostY = f(L"boost", L"y_px", s.boostY, -400.f, 400.f);
    s.swapSides = b(L"boost", L"swap_sides", s.swapSides);
    s.thrusters = b(L"thrusters", L"enabled", s.thrusters);
    s.thrusterPx = f(L"thrusters", L"size_px", s.thrusterPx, 4.f, 100.f);
    s.thrusterX = f(L"thrusters", L"x_px", s.thrusterX, 0.f, 400.f);
    s.thrusterY = f(L"thrusters", L"y_px", s.thrusterY, -400.f, 400.f);
    s.swapHands = b(L"thrusters", L"swap_hands", s.swapHands);
    s.icons = b(L"thrusters", L"icons", s.icons);
    s.rings = b(L"thrusters", L"rings", s.rings);
    s.edgeGlow = b(L"thrusters", L"screen_glow", s.edgeGlow);
    s.edgeGlowStrength = f(L"thrusters", L"screen_glow_strength", s.edgeGlowStrength, 0.f, 1.f);
    s.edgeGlowPeak = f(L"thrusters", L"screen_glow_peak", s.edgeGlowPeak, s.edgeGlowStrength, 1.f);
    s.edgeGlowFrom = f(L"thrusters", L"screen_glow_from", s.edgeGlowFrom, 0.f, 30.f);
    s.edgeGlowTo = f(L"thrusters", L"screen_glow_to", s.edgeGlowTo, s.edgeGlowFrom + 0.1f, 40.f);
    s.edgeGlowAlignFrom = f(L"thrusters", L"screen_glow_align_from", s.edgeGlowAlignFrom, 0.5f, 0.99f);
    s.edgeGlowAlignFull = f(L"thrusters", L"screen_glow_align_full", s.edgeGlowAlignFull, s.edgeGlowAlignFrom + 0.005f, 1.f);
    wchar_t buf[64];
    GetPrivateProfileStringW(L"thrusters", L"ping_bars_ms", L"", buf, 64, ini.c_str());
    float pb[3];
    if (swscanf_s(buf, L"%f,%f,%f", &pb[0], &pb[1], &pb[2]) == 3 && pb[0] < pb[1] && pb[1] < pb[2])
        for (int i = 0; i < 3; i++) s.pingBars[i] = pb[i];
    auto color = [&](const wchar_t* key, uint8_t out[3]) {
        wchar_t cbuf[32];
        GetPrivateProfileStringW(L"colors", key, L"", cbuf, 32, ini.c_str());
        int r, g, bl;
        if (swscanf_s(cbuf, L"%d,%d,%d", &r, &g, &bl) == 3) {
            out[0] = uint8_t(r & 255);
            out[1] = uint8_t(g & 255);
            out[2] = uint8_t(bl & 255);
        }
    };
    color(L"blue", s.blue);
    color(L"orange", s.orange);
    color(L"neutral", s.neutral);
    s.speed = b(L"speed", L"enabled", s.speed);
    s.speedThreshold = f(L"speed", L"threshold", s.speedThreshold, 0.f, 100.f);
    s.trackerPx = f(L"speed", L"size_px", s.trackerPx, 4.f, 200.f);
    s.fontPx = f(L"speed", L"font_px", s.fontPx, 4.f, 200.f);
    s.currentFontPx = f(L"speed", L"current_font_px", s.currentFontPx, 4.f, 200.f);
    s.glow = b(L"speed", L"ring", s.glow);
    GetPrivateProfileStringW(L"speed", L"rings_at", L"", buf, 64, ini.c_str());
    float ga[Settings::kRings];
    if (swscanf_s(buf, L"%f,%f,%f,%f,%f", &ga[0], &ga[1], &ga[2], &ga[3], &ga[4]) == 5 && ga[0] > 0 && ga[0] < ga[1] && ga[1] < ga[2] &&
        ga[2] < ga[3] && ga[3] < ga[4])
        for (int i = 0; i < Settings::kRings; i++) s.glowAt[i] = ga[i];
    s.peakBadge = b(L"speed", L"top_speed", s.peakBadge);
    s.peakSeconds = f(L"speed", L"top_speed_seconds", s.peakSeconds, 1.f, 15.f);
    s.glowPx = f(L"speed", L"ring_size_px", s.glowPx, 4.f, 100.f);
    s.glowY = f(L"speed", L"ring_y_px", s.glowY, -400.f, 400.f);
    s.glowGap = f(L"speed", L"ring_gap_px", s.glowGap, 0.f, 100.f);
    s.trackerDown = f(L"speed", L"down_px", s.trackerDown, -300.f, 300.f);
    s.burstGap = f(L"speed", L"run_gap_ms", s.burstGap * 1000, 0.f, 5000.f) / 1000;
    s.stun = b(L"stun", L"enabled", s.stun);
    s.shieldGrace = f(L"stun", L"shield_grace_ms", s.shieldGrace * 1000, 0.f, 2000.f) / 1000;
    s.flashStrength = f(L"stun", L"strength", s.flashStrength, 0.05f, 1.f);
    s.maxFps = f(L"general", L"max_fps", s.maxFps, 5.f, 240.f);
    s.passes = b(L"passes", L"enabled", s.passes);
    GetPrivateProfileStringW(L"passes", L"teams", L"both", buf, 64, ini.c_str());
    s.passesMine = _wcsicmp(buf, L"mine") == 0;
    s.passCountFrom = int(f(L"passes", L"count_from", float(s.passCountFrom), 1.f, 99.f));
    s.passPx = f(L"passes", L"size_px", s.passPx, 4.f, 200.f);
    s.passY = f(L"passes", L"y_m", s.passY, -1.f, 1.f);
}

void loadDigits(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) { logf("no digits.bin: last speed uses the built-in digits"); return; }
    std::vector<uint8_t> data(1 << 16);
    size_t n = fread(data.data(), 1, data.size(), f);
    fclose(f);
    bool ok = parseDigitFont(data.data(), n, digitFont);
    logf(ok ? "Echo's speed digits loaded" : "digits.bin unreadable: last speed uses the built-in digits");
}

bool hook(HMODULE ovr, const char* name, void* detour, void** original) {
    void* target = reinterpret_cast<void*>(GetProcAddress(ovr, name));
    return target && hookAt(target, detour, original, name);
}

DWORD WINAPI install(LPVOID pluginPath) {
    std::wstring dir = static_cast<const wchar_t*>(pluginPath);
    delete[] static_cast<wchar_t*>(pluginPath);
    dir.resize(dir.find_last_of(L"\\/"));
    settingsPath = dir + L"\\EchoHudPlus\\settings.ini";
    settingsStamp = stampOf(settingsPath);
    readSettings(settingsPath);
    loadDigits(dir + L"\\EchoHudPlus\\digits.bin");
    logf("EchoHudPlus starting (pid %lu), enabled=%d%s", GetCurrentProcessId(), enabled,
         enabled ? "" : " (hooks go in anyway, so it can be switched on while Echo runs)");
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) { logf("MinHook init failed; overlay off"); return 0; }
    hookGame();
    if (HANDLE poller = CreateThread(nullptr, 0, pollPasses, nullptr, 0, nullptr)) CloseHandle(poller);
    // Must be in place before the game creates its eye textures, so poll tightly.
    HMODULE ovr = nullptr;
    for (int i = 0; i < 60000 && !ovr; i++) {
        ovr = GetModuleHandleW(L"LibOVRRT64_1.dll");
        if (!ovr) Sleep(1);
    }
    if (!ovr) { logf("Oculus runtime (LibOVRRT64_1.dll) not found; overlay off"); return 0; }
    getBuffer = reinterpret_cast<GetBufferFn>(GetProcAddress(ovr, "ovr_GetTextureSwapChainBufferDX"));
    getLength = reinterpret_cast<ChainIntFn>(GetProcAddress(ovr, "ovr_GetTextureSwapChainLength"));
    getCurrentIndex = reinterpret_cast<ChainIntFn>(GetProcAddress(ovr, "ovr_GetTextureSwapChainCurrentIndex"));
    commit = reinterpret_cast<CommitFn>(GetProcAddress(ovr, "ovr_CommitTextureSwapChain"));
    if (!getBuffer || !getLength || !getCurrentIndex || !commit) { logf("Oculus runtime exports missing; overlay off"); return 0; }
    bool created = hook(ovr, "ovr_CreateTextureSwapChainDX", reinterpret_cast<void*>(hookedCreate), reinterpret_cast<void**>(&realCreate));
    bool submitHook = hook(ovr, "ovr_SubmitFrame", reinterpret_cast<void*>(hookedSubmitFrame), reinterpret_cast<void**>(&realSubmitFrame));
    bool endHook = hook(ovr, "ovr_EndFrame", reinterpret_cast<void*>(hookedEndFrame), reinterpret_cast<void**>(&realEndFrame));
    if (!created || !(submitHook || endHook)) logf("required Oculus hooks missing; overlay off");
    return 0;
}

void start(HMODULE self) {
    static std::once_flag once;
    std::call_once(once, [self] {
        auto* path = new wchar_t[MAX_PATH];
        GetModuleFileNameW(self, path, MAX_PATH);
        HANDLE t = CreateThread(nullptr, 0, install, path, 0, nullptr);
        if (t) CloseHandle(t);
        else delete[] path;
    });
}

}  // namespace

#ifdef HUDPLUS_TEST
// Test build only: drive the game-side code paths without echovr.exe.
extern "C" __declspec(dllexport) void hudplus_test_set_battery_vtable(uintptr_t vtable) { batteryVtable = vtable; }
extern "C" __declspec(dllexport) void hudplus_test_local(void* system, void* entry) { observeLocal(system, entry); }
extern "C" __declspec(dllexport) void hudplus_test_shield(void* punchSystem) { observeShield(punchSystem); }
extern "C" __declspec(dllexport) void hudplus_test_voice(void* voipSystem) { observeVoice(voipSystem); }
extern "C" __declspec(dllexport) void hudplus_test_stun(void* punchableSystem, uint32_t id, float seconds) {
    observeStun(punchableSystem, id, seconds);
}
extern "C" __declspec(dllexport) void hudplus_test_api_port(int port) { apiPort = port; }
// Our part of the frames so far: average and worst microseconds on the render thread, and
// the average per picture on the draw thread.
extern "C" __declspec(dllexport) void hudplus_test_stats(double* avgUs, double* maxUs, double* paintUs) {
    *avgUs = ov.frames ? ov.frameUs / ov.frames : 0;
    *maxUs = ov.frameMaxUs;
    unsigned pictures = painter.pictures.load();
    *paintUs = pictures ? double(painter.busyUs.load()) / pictures : 0;
}
// Waits until the draw thread has painted the newest job (the next submit shows it).
extern "C" __declspec(dllexport) int hudplus_test_wait_drawn(int timeoutMs) {
    ULONGLONG until = GetTickCount64() + timeoutMs;
    while (painter.drawn < ov.serial) {
        if (GetTickCount64() > until) return 0;
        Sleep(1);
    }
    return 1;
}
#endif

// EchoLoader calls PluginInit only for entries with "args"; DllMain covers the rest.
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
        start(self);
    }
    return TRUE;
}
