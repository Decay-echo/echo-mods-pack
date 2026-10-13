// Touch controllers for the arcade (CONTROLLERS on the RetroArch screen).
//
// While the host sets Shared::controllerWant (and is alive):
//  * ovr_GetInputState: the real Touch state is copied to Shared::pad, and Echo VR gets
//    neutral controllers (no buttons, triggers, grips or sticks), so it can't thrust, grab,
//    boost or turn. Capacitive touches stay, so the hands keep their finger poses, and hand
//    tracking is a different call: you can still poke the tablet.
//  * the PlayerNav update of the local player (once per frame) is followed by setting its
//    velocity to zero, so you stay where you are instead of drifting.
// Both hooks go in the first time the controllers are switched on. If the host goes away
// (heartbeat lost), Echo gets the controllers back on its own.
#include "controllers.h"
#include "log.h"
#include "../vendor/minhook/include/MinHook.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace {

using P = unsigned char*;

// ovrInputState as of LibOVR 1.55 (echovr.exe reports client 1.55.0 to the runtime), 120 bytes.
struct InputState {
    double time;
    uint32_t buttons, touches;
    float trigger[2], grip[2];
    float stick[2][2];
    uint32_t controllerType;
    float triggerNoDeadzone[2], gripNoDeadzone[2], stickNoDeadzone[2][2];
    float triggerRaw[2], gripRaw[2], stickRaw[2][2];
};
static_assert(sizeof(InputState) == 120 && offsetof(InputState, controllerType) == 48, "ovrInputState layout");

constexpr uint32_t LTOUCH = 1, RTOUCH = 2;                         // ovrControllerType bits (Touch = 3, Active = ~0)
constexpr uint32_t LEFT_BUTTONS = 0x00F00F00, RIGHT_BUTTONS = 0x0000000F;  // X Y LThumb LShoulder Enter Back..., A B RThumb RShoulder
constexpr uint32_t LEFT_TOUCHES = 0x0000FF00, RIGHT_TOUCHES = 0x000000FF;

// The PlayerNav boost-input update (also hooked by EchoHudPlus): void(system*, entry*, float dt),
// called every frame for each PlayerNav entry; only the local player has one.
constexpr uintptr_t NAV_UPDATE_RVA = 0x74F0B0;
constexpr unsigned char NAV_UPDATE_BYTES[] = {0x4c, 0x8b, 0xdc, 0x55, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0xa8, 0x00, 0x00, 0x00};
// PlayerNav entry -> movement entry -> body -> linear velocity (see EchoHudPlus' game_memory.h).
constexpr size_t CTRL_NAV_SYSTEM = 0x28, CTRL_NAV_ID = 0x30;
constexpr size_t ID_MAP = 0xC8, ID_CAPACITY = 0xF0, SLOT_COUNT = 0xF2, ENTRIES = 0xF8;
constexpr size_t NAV_STRIDE = 0x280, NAV_BODY = 0xD8, BODY_VELOCITY = 0x70;

using GetInputState = int(__cdecl*)(void* session, uint32_t type, InputState* state);
using NavUpdate = uint64_t(__fastcall*)(void* system, void* entry, float dt);
// ovrTrackingState (LibOVR 1.x, 312 bytes; a little more here for safety): the head pose at 0,
// the hand poses at 96 and 184, each pose's position 16 bytes in (after its orientation).
struct TrackingState { unsigned char bytes[320]; };
using GetTrackingState = TrackingState(__cdecl*)(void* session, double absTime, char latencyMarker);
constexpr size_t HEAD_POSITION = 16, HAND_POSITION[2] = {96 + 16, 96 + 88 + 16};
constexpr float HANDS_UP_ABOVE_HEAD = 0.10f;   // m: a controller this far above the headset counts as up
constexpr uint64_t HANDS_UP_MS = 5000;         // both hands up this long: the player wants out of FREEZE
constexpr float HANDS_TOGETHER = 0.28f;        // m: the right hand this close to the left is reaching for the wrist tablet

arcade::Shared* shared = nullptr;
P exe = nullptr;
HANDLE modeEvent = nullptr;
GetInputState realGetInputState = nullptr;
GetTrackingState getTrackingState = nullptr;
NavUpdate realNavUpdate = nullptr;
std::atomic<uint64_t> handsUpSince{0};
std::atomic<bool> released{false};
std::atomic<bool> handsTogether{false};  // the right grip then goes to Echo: grab the tablet, throw it away
std::atomic<bool> inputHooked{false}, navHooked{false}, triedHooks{false};
std::atomic<uint64_t> lastFreeze{0};
std::atomic<bool> eventSet{false};
std::mutex padLock;
arcade::PadState padNow{};  // merged left/right state (Echo may ask for one hand at a time)

// On while the host wants it, it is still the level FREEZE was switched on in, and the host is alive.
bool wanted() {
    return shared && shared->controllerWant && shared->controllerLevel == shared->levelSerial &&
           arcade::alive(shared->hostHeartbeat, GetTickCount64());
}

std::atomic<void*> navSystem{nullptr};  // the local player's PlayerNav system: a new one means a new level

// Both hands held above the head for HANDS_UP_MS: the host sees ControllersReleased and ends
// FREEZE. A way out that needs no button.
void watchHandsUp(void* session) {
    if (!getTrackingState) return;
    TrackingState t = getTrackingState(session, 0.0, 0);
    float head[3], hand[2][3];
    std::memcpy(head, t.bytes + HEAD_POSITION, sizeof head);
    std::memcpy(hand[0], t.bytes + HAND_POSITION[0], sizeof hand[0]);
    std::memcpy(hand[1], t.bytes + HAND_POSITION[1], sizeof hand[1]);
    bool up = hand[0][1] > head[1] + HANDS_UP_ABOVE_HEAD && hand[1][1] > head[1] + HANDS_UP_ABOVE_HEAD;
    float dx = hand[0][0] - hand[1][0], dy = hand[0][1] - hand[1][1], dz = hand[0][2] - hand[1][2];
    handsTogether = dx * dx + dy * dy + dz * dz < HANDS_TOGETHER * HANDS_TOGETHER;
    uint64_t now = GetTickCount64();
    if (!up) { handsUpSince = 0; return; }
    uint64_t since = handsUpSince.load();
    if (!since) { handsUpSince = now; return; }
    if (now - since >= HANDS_UP_MS && !released.exchange(true)) logf("controllers: both hands above the head for %llu s: FREEZE off", HANDS_UP_MS / 1000);
}

int __cdecl hookedGetInputState(void* session, uint32_t type, InputState* state) {
    int result = realGetInputState(session, type, state);
    if (result < 0 || !state || !(type & (LTOUCH | RTOUCH)) || !wanted()) return result;
    if (type & LTOUCH) watchHandsUp(session);
    {
        std::lock_guard<std::mutex> lock(padLock);
        if (type & LTOUCH) {
            padNow.buttons = (padNow.buttons & ~LEFT_BUTTONS) | (state->buttons & LEFT_BUTTONS);
            padNow.touches = (padNow.touches & ~LEFT_TOUCHES) | (state->touches & LEFT_TOUCHES);
            padNow.trigger[0] = state->trigger[0]; padNow.grip[0] = state->grip[0];
            padNow.stick[0][0] = state->stick[0][0]; padNow.stick[0][1] = state->stick[0][1];
        }
        if (type & RTOUCH) {
            padNow.buttons = (padNow.buttons & ~RIGHT_BUTTONS) | (state->buttons & RIGHT_BUTTONS);
            padNow.touches = (padNow.touches & ~RIGHT_TOUCHES) | (state->touches & RIGHT_TOUCHES);
            padNow.trigger[1] = state->trigger[1]; padNow.grip[1] = handsTogether ? 0.f : state->grip[1];
            padNow.stick[1][0] = state->stick[1][0]; padNow.stick[1][1] = state->stick[1][1];
        }
        arcade::writePad(shared, padNow);
    }
    // Neutral controllers for Echo: everything that moves you or presses something. Except the
    // right grip while the hands are together: that is a grab of the wrist tablet (to throw it
    // away), which Echo keeps; the arcade's pad doesn't see it.
    float grip = state->grip[1], gripNd = state->gripNoDeadzone[1], gripRaw = state->gripRaw[1];
    state->buttons = 0;
    std::memset(state->trigger, 0, offsetof(InputState, controllerType) - offsetof(InputState, trigger));
    std::memset(state->triggerNoDeadzone, 0, sizeof(InputState) - offsetof(InputState, triggerNoDeadzone));
    if ((type & RTOUCH) && handsTogether) { state->grip[1] = grip; state->gripNoDeadzone[1] = gripNd; state->gripRaw[1] = gripRaw; }
    return result;
}

// The local player's velocity, from the PlayerNav entry (nullptr if anything looks wrong).
float* velocityOf(void* entry) {
    __try {
        P ctrl = static_cast<P>(entry);
        P nav = *reinterpret_cast<P*>(ctrl + CTRL_NAV_SYSTEM);
        uint32_t id = *reinterpret_cast<uint32_t*>(ctrl + CTRL_NAV_ID);
        if (!nav) return nullptr;
        P map = *reinterpret_cast<P*>(nav + ID_MAP);
        uint16_t capacity = *reinterpret_cast<uint16_t*>(nav + ID_CAPACITY), count = *reinterpret_cast<uint16_t*>(nav + SLOT_COUNT);
        uint16_t index = uint16_t(id & 0xFFFF);
        if (!map || index >= capacity) return nullptr;
        uint16_t slot = *reinterpret_cast<uint16_t*>(map + size_t(index) * 4), gen = *reinterpret_cast<uint16_t*>(map + size_t(index) * 4 + 2);
        if (gen != uint16_t(id >> 16) || slot >= count) return nullptr;
        P entries = *reinterpret_cast<P*>(nav + ENTRIES);
        if (!entries) return nullptr;
        P body = *reinterpret_cast<P*>(entries + size_t(slot) * NAV_STRIDE + NAV_BODY);
        if (!body) return nullptr;
        float* v = reinterpret_cast<float*>(body + BODY_VELOCITY);
        return std::isfinite(v[0] + v[1] + v[2]) ? v : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool zeroVelocity(float* v) {
    __try {
        v[0] = v[1] = v[2] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t __fastcall hookedNavUpdate(void* system, void* entry, float dt) {
    uint64_t result = realNavUpdate(system, entry, dt);
    void* previous = navSystem.exchange(system);
    if (previous && previous != system) controllers::levelChanged("the player was rebuilt");
    if (wanted() && inputHooked) {
        static bool logged = false;
        float* v = velocityOf(entry);
        if (v && zeroVelocity(v)) {
            lastFreeze = GetTickCount64();
            if (!logged) { logged = true; logf("controllers: holding the player still (velocity at %p)", static_cast<void*>(v)); }
        } else if (!logged) {
            logged = true;
            logf("controllers: could not find the player's velocity; inputs are hidden but you may drift");
        }
    }
    return result;
}

// Our bytes, or a jump another plugin put there (EchoHudPlus hooks the same update first).
bool navUpdateLooksRight() {
    P at = exe + NAV_UPDATE_RVA;
    if (std::memcmp(at, NAV_UPDATE_BYTES, sizeof NAV_UPDATE_BYTES) == 0) return true;
    if (at[0] != 0xE9) return false;
    P target = at + 5 + *reinterpret_cast<int32_t*>(at + 1);
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(target), &owner) ||
        owner == reinterpret_cast<HMODULE>(exe))
        return false;
    wchar_t name[MAX_PATH] = L"?";
    GetModuleFileNameW(owner, name, MAX_PATH);
    logf("controllers: the player update is already hooked by %ls; chaining after it", name);
    return true;
}

void installHooks() {
    HMODULE ovr = GetModuleHandleW(L"LibOVRRT64_1.dll");
    void* input = ovr ? reinterpret_cast<void*>(GetProcAddress(ovr, "ovr_GetInputState")) : nullptr;
    getTrackingState = ovr ? reinterpret_cast<GetTrackingState>(GetProcAddress(ovr, "ovr_GetTrackingState")) : nullptr;
    if (!getTrackingState) logf("controllers: ovr_GetTrackingState not found; the hands-up way out of FREEZE is off");
    if (!input) {
        logf("controllers: Oculus runtime (LibOVRRT64_1.dll) not loaded; controllers unavailable");
    } else {
        MH_STATUS s = MH_CreateHook(input, reinterpret_cast<void*>(hookedGetInputState), reinterpret_cast<void**>(&realGetInputState));
        if (s == MH_OK) s = MH_EnableHook(input);
        inputHooked = s == MH_OK;
        logf("controllers: ovr_GetInputState hook %s", s == MH_OK ? "installed" : MH_StatusToString(s));
    }
    if (!inputHooked) return;  // no point holding the player still if the controllers still move them
    if (!navUpdateLooksRight()) {
        logf("controllers: unexpected code at the player update (%x); you won't be held still", unsigned(NAV_UPDATE_RVA));
        return;
    }
    MH_STATUS s = MH_CreateHook(exe + NAV_UPDATE_RVA, reinterpret_cast<void*>(hookedNavUpdate), reinterpret_cast<void**>(&realNavUpdate));
    if (s == MH_OK) s = MH_EnableHook(exe + NAV_UPDATE_RVA);
    navHooked = s == MH_OK;
    logf("controllers: player update hook %s", s == MH_OK ? "installed" : MH_StatusToString(s));
}

}  // namespace

namespace controllers {

void levelChanged(const char* why) {
    if (!shared) return;
    LONG n = InterlockedIncrement(&shared->levelSerial);
    if (shared->controllerWant) logf("controllers: level changed (%s, #%ld): FREEZE ends, Echo gets the controllers back", why, n);
}

void install(unsigned char* exeBase, arcade::Shared* s) {
    exe = exeBase;
    shared = s;
    modeEvent = CreateEventW(nullptr, TRUE, FALSE, arcade::CONTROLLER_EVENT);
}

void tick(uint64_t now) {
    if (!shared) return;
    bool want = wanted();
    if (want && !triedHooks.exchange(true)) installHooks();
    bool on = want && inputHooked;
    bool frozen = on && navHooked && now - lastFreeze.load() < 500;
    if (!want) { released = false; handsUpSince = 0; handsTogether = false; }
    shared->controllerState = !on ? arcade::ControllersOff : released ? arcade::ControllersReleased : frozen ? arcade::ControllersFrozen : arcade::ControllersOn;
    if (modeEvent && on != eventSet) {
        if (on) SetEvent(modeEvent); else ResetEvent(modeEvent);
        eventSet = on;
        logf("controllers: %s", on ? "now driving the arcade (Echo sees neutral controllers)" : "given back to Echo VR");
        if (!on) {
            std::lock_guard<std::mutex> lock(padLock);
            padNow = {};
        }
    }
}

}  // namespace controllers
