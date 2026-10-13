// Voice chat mode from the tablet (see voice.h).
//
// Engine facts (echovr.exe 1683152886): 0x140c2ebc0 ReadGameSettings(block, json) fills the
// "game" section's block from settings_mp_v2.json (its one caller is the settings loader);
// the block holds voipmode at +4 (voipmodeffect +8, announcer +0xc, music +0x10,
// voiploudnesslevel +0x44). The loader numbers the modes 1 open mic (VOIPAlwaysOn), 2 push to
// talk, 3 push to mute, 0 when none applies (muted); the save path writes "voipmode" from +4.
#include "voice.h"
#include "log.h"
#include "../vendor/minhook/include/MinHook.h"
#include <atomic>
#include <cstring>

namespace voice {
namespace {

constexpr unsigned RVA_READ_GAME_SETTINGS = 0xc2ebc0;
constexpr unsigned char SIGNATURE[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xda, 0x4c, 0x8d, 0x0d};
constexpr unsigned MODE_OFFSET = 4;

using ReadFn = int(__fastcall*)(void*, void*);
ReadFn original = nullptr;
std::atomic<void*> block{nullptr};
arcade::Shared* shared = nullptr;
LONG handled = 0;
bool hooked = false;

int& modeOf(void* b) { return *reinterpret_cast<int*>(static_cast<unsigned char*>(b) + MODE_OFFSET); }

int __fastcall readGameSettings(void* settings, void* json) {
    int r = original(settings, json);
    block = settings;
    logf("voice: the game read its settings into %p (voipmode %d: %s)", settings, modeOf(settings),
         modeOf(settings) == 1 ? "open mic" : modeOf(settings) == 2 ? "push to talk" : modeOf(settings) == 3 ? "push to mute" : "muted");
    return r;
}

}  // namespace

void hookEarly(unsigned char* exe) {
    if (!exe || memcmp(exe + RVA_READ_GAME_SETTINGS, SIGNATURE, 16) != 0) {
        logf("voice: code signature mismatch; the voice chat buttons do nothing this session");
        return;
    }
    MH_STATUS init = MH_Initialize();
    MH_STATUS s = (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED)
                      ? MH_CreateHook(exe + RVA_READ_GAME_SETTINGS, reinterpret_cast<void*>(readGameSettings), reinterpret_cast<void**>(&original))
                      : init;
    if (s == MH_OK) s = MH_EnableHook(exe + RVA_READ_GAME_SETTINGS);
    hooked = s == MH_OK;
    if (!hooked) logf("voice: hook failed: %s", MH_StatusToString(s));
}

void attach(arcade::Shared* s) {
    shared = s;
    if (!shared) return;
    handled = shared->voiceSerial;
    shared->voiceDone = handled;
    shared->voiceState = -1;
}

void heartbeat(unsigned long long) {
    if (!shared) return;
    void* b = block.load();
    LONG serial = shared->voiceSerial;
    if (serial != handled) {
        handled = serial;
        int want = int(shared->voiceWant) - 1;
        if (!b) logf("voice: the tablet asked for mode %d, but the game's settings block is not known (hook %s)", want, hooked ? "in" : "missing");
        else if (want >= 0 && want <= 3) {
            modeOf(b) = want;
            logf("voice: voipmode %d from the tablet", want);
        }
        shared->voiceDone = serial;
    }
    shared->voiceState = b ? modeOf(b) : -1;
}

}  // namespace voice
