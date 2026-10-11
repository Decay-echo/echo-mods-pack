// Reading Echo VR's own state for EchoHudPlus: the local player's boost battery and
// speed, their shield (block) flag, and who a stun lands on. Offsets are for
// echovr.exe build 0x6452dff6 (sha256 b6d08277...); hudplus.cpp checks the code bytes
// at each hook before using any of this. Every read goes through readBytes(), which
// turns a bad pointer into "no data" instead of a crash.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace hudplus::game {

// --- hook sites (RVAs) and the bytes expected there --------------------------------
// CR15PlayerNavCS::UpdateBeforeRaycasts calls this per PlayerNav entry, every frame; only the local
// player prefabs have one: void(playerNavSystem*, playerNavEntry*, float dt).
constexpr uintptr_t kBoostInputRva = 0x74F0B0;
constexpr uint8_t kBoostInputBytes[] = {0x4c, 0x8b, 0xdc, 0x55, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0xa8, 0x00, 0x00, 0x00,
                                        0x0f, 0xb7, 0x02, 0x48, 0x8b, 0xea, 0x8b, 0x7a, 0x24};
// Per-frame punch/shield update over every punch entry: void(punchSystem*).
constexpr uintptr_t kPunchUpdateRva = 0xD679E0;
constexpr uint8_t kPunchUpdateBytes[] = {0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41,
                                         0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0x08, 0xfa, 0xff, 0xff};
// Punchable "stun": void(punchableSystem*, uint32 id, uint32, uint32, uint64, float seconds).
constexpr uintptr_t kStunRva = 0xD534F0;
constexpr uint8_t kStunBytes[] = {0x44, 0x89, 0x44, 0x24, 0x18, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54,
                                  0x41, 0x55, 0x41, 0x56, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x50};
// Voice-chat broadcaster update over every voice entry: int(voipSystem*).
constexpr uintptr_t kVoipUpdateRva = 0xD7BD90;
constexpr uint8_t kVoipUpdateBytes[] = {0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
                                        0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x70, 0x01, 0x00, 0x00};
// CR15BatteryPackCS vtable, and two of its entries used as a fingerprint.
constexpr uintptr_t kBatteryVtableRva = 0x1C61158;
constexpr uintptr_t kBatteryVtable0Rva = 0x8B8E80, kBatteryVtable57Rva = 0x3319D0;

// --- generic component system (the CS classes whose entry array is at +0xF8) -------
// A handle id is u32: low 16 bits index the id map, high 16 bits are a generation.
// The id map holds {u16 slot, u16 generation} per id; slot indexes the entries and the
// 16-byte actor references (the first 8 bytes identify the actor).
constexpr size_t kIdMap = 0xC8, kIdCapacity = 0xF0, kSlotCount = 0xF2, kEntries = 0xF8, kActors = 0x100;

// PlayerNav entry (0x890 bytes, the hook's second argument); called "controller" below.
constexpr size_t kCtrlSelfId = 0x24, kCtrlNavSystem = 0x28, kCtrlNavId = 0x30;
// Movement entries (0x280 bytes) that the PlayerNav entry points to ("nav" below).
constexpr size_t kNavStride = 0x280, kNavBody = 0xD8, kNavBoostCost = 0xE0 + 0x3C, kNavBatterySystem = 0x250, kNavBatteryId = 0x258;
constexpr size_t kBodyVelocity = 0x70;
// Hand thrusters ("iron man") in the movement entry: thrust direction per hand, and the
// thruster state at +0x128 (per-hand on flags, per-hand factors, the balance settings).
constexpr size_t kNavHandDir0 = 0x1B4, kNavHandDir1 = 0x1C0, kNavIronMan = 0x128;
constexpr size_t kImOn0 = 0x2C, kImOn1 = 0x30, kImSingle0 = 0x34, kImSingle1 = 0x38, kImTwo0 = 0x3C, kImTwo1 = 0x40,
                 kImSingleMax = 0x4C, kImTwoMax = 0x50;
// Battery entries, 16 bytes: u16 resource, energy, pending recharge, delay left.
constexpr size_t kBatStride = 0x10, kBatEnergy = 4, kBatPending = 8, kBatDelayLeft = 0xC;
constexpr size_t kBatResources = 0xB8, kResStride = 0x38, kResMaxPower = 0x20, kResRate = 0x24, kResDelay = 0x28;
// The network game, reached from any component system: system +0x80 -> scene, scene +8
// -> game, game +0x8518 -> network game (what Echo's own ping expression does).
constexpr size_t kSysScene = 0x80, kSceneGame = 0x8, kGameNet = 0x8518;
// Network game: local player count, player count, peer -> player slot table, player
// records (0x250 bytes; u16 at +0x178 = peer in bits 0-4, team in bits 10-12), the
// peer table (ping), and the game flags (bit 38 = your voice chat is muted).
constexpr size_t kNgLocals = 0xDE, kNgLocalPlayers = 0xDC, kNgPlayers = 0xE2, kNgPeerSlots = 0x138, kNgRecStride = 0x250,
                 kNgRecPeerTeam = 0x178, kNgPeerNet = 0x40, kNetPeers = 0x18 + 0x360, kNetPeerCount = 0x18 + 0x368,
                 kPeerStride = 0xD8, kPeerPing = 0x8A, kNgFlags = 0x2DA0;
constexpr uint64_t kFlagVoipMuted = 1ull << 38;
// The network game points back at the game (+8), which holds the voice-chat mode:
// 0 off, 1 open mic, 2 push to talk, 3 push to mute.
constexpr size_t kNgGame = 0x8, kGameVoiceMode = 0x8C14;
// Voice broadcaster system: network game at +0xB8; entries (0x78 bytes) with the
// player's peer handle at +8 and flags at +6, bit 0x400 = talk button held.
constexpr size_t kVoipNet = 0xB8, kVoipCount = 0xFC, kVoipEntries = 0x100, kVoipStride = 0x78, kVoipFlags = 6, kVoipPeer = 8;
constexpr uint16_t kVoipPressed = 0x400;

// Punch system (its entry array sits 8 bytes later than the generic layout).
constexpr size_t kPunchCount = 0xFC, kPunchEntries = 0x100, kPunchStride = 0x628, kPunchCtrlSystem = 0x10, kPunchCtrlId = 0x18,
                 kPunchFlags = 0x608;
constexpr uint32_t kShieldUp = 1;
// Punchable entries, 0x48 bytes; flags bits 0-1 mean stunned.
constexpr size_t kPunchableStride = 0x48, kPunchableFlags = 4;

inline bool plausible(const void* p) {
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000 && v < 0x7FFFFFFF0000ull;
}

__declspec(noinline) inline bool readBytes(const void* src, void* dst, size_t n) {
    if (!plausible(src)) return false;
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T> inline bool rd(const void* base, size_t offset, T& out) {
    return readBytes(static_cast<const uint8_t*>(base) + offset, &out, sizeof(T));
}

// Slot of a handle id in a generic component system, with the same checks as the
// game's own IsValid (id in range, generation matches, slot in use).
inline bool slotOf(const void* system, uint32_t id, uint16_t& slot) {
    const uint8_t* map;
    uint16_t capacity, count, gen;
    uint16_t index = uint16_t(id & 0xFFFF);
    if (!rd(system, kIdMap, map) || !rd(system, kIdCapacity, capacity) || !rd(system, kSlotCount, count)) return false;
    if (index >= capacity || !rd(map, size_t(index) * 4, slot) || !rd(map, size_t(index) * 4 + 2, gen)) return false;
    return gen == uint16_t(id >> 16) && slot < count;
}

inline bool actorOf(const void* system, uint16_t slot, uint64_t& actor) {
    const uint8_t* actors;
    return rd(system, kActors, actors) && rd(actors, size_t(slot) * 16, actor);
}

struct Local {
    const void* ctrlSystem = nullptr;
    uint32_t ctrlId = 0;
    uint64_t actor = 0;
    bool speedOk = false;
    float speed = 0;
    bool netOk = false;  // network game found: team / ping / muted below are real
    int team = -1;       // 0 blue, 1 orange, 2 spectators, -1 unknown
    int ping = 999;      // ms, 999 = none
    bool muted = false;
    int voiceMode = -1;  // 0 off, 1 open mic, 2 push to talk, 3 push to mute
    bool thrustersOk = false;
    uint32_t handOn[2] = {0, 0};
    float handDir[2][3] = {};
    float handSingle[2] = {0, 0}, handTwo[2] = {0, 0};  // max_vel x per-hand factor
    bool battery = false;
    float energy = 0, maxPower = 0, cost = 0, rate = 0, delay = 0, delayLeft = 0, pending = 0;
    int stage = 0;  // how far the read got, for the log
};

// Everything the overlay needs about the local player, from the boost hook's arguments.
// Your team, ping and voice-chat mute, the way Echo's ping expression and session API
// find them. Returns false if there's no network game (menus, offline).
inline bool readNet(const void* system, Local& out) {
    const uint8_t *scene, *game, *ng;
    if (!rd(system, kSysScene, scene) || !rd(scene, kSceneGame, game) || !rd(game, kGameNet, ng) || !plausible(ng)) return false;
    const uint8_t* flagsAt;
    uint64_t flags;
    if (!rd(ng, kNgFlags, flagsAt) || !rd(flagsAt, 0, flags)) return false;
    out.muted = (flags & kFlagVoipMuted) != 0;
    out.netOk = true;
    const uint8_t* g;
    uint32_t mode;
    if (rd(ng, kNgGame, g) && rd(g, kGameVoiceMode, mode) && mode <= 3) out.voiceMode = int(mode);
    uint16_t locals, localPlayers, players, own, slot, field;
    if (!rd(ng, kNgLocals, locals) || !rd(ng, kNgLocalPlayers, localPlayers) || !rd(ng, kNgPlayers, players) ||
        !rd(ng, kNgRecPeerTeam, own) || !locals || !localPlayers)
        return true;
    if (!rd(ng, kNgPeerSlots + size_t(own & 0x1F) * 4, slot) || slot == 0xFFFF || slot >= players || slot >= 64 ||
        !rd(ng, size_t(slot) * kNgRecStride + kNgRecPeerTeam, field))
        return true;
    int team = (field >> 10) & 7;
    out.team = team <= 2 ? team : -1;
    const uint8_t *net, *peers;
    uint64_t peerCount;
    uint16_t ping;
    size_t peer = field & 0x1F;
    if (rd(ng, kNgPeerNet, net) && rd(net, kNetPeers, peers) && rd(net, kNetPeerCount, peerCount) && peer < peerCount &&
        rd(peers, peer * kPeerStride + kPeerPing, ping))
        out.ping = ping;
    return true;
}

inline bool readLocal(const void* ctrlSystem, const void* ctrlEntry, uintptr_t batteryVtable, Local& out) {
    out = Local{};
    out.ctrlSystem = ctrlSystem;
    readNet(ctrlSystem, out);
    if (!rd(ctrlEntry, kCtrlSelfId, out.ctrlId)) return false;
    out.stage = 1;
    const uint8_t* nav;
    uint32_t navId;
    uint16_t navSlot;
    const uint8_t* navEntries;
    if (!rd(ctrlEntry, kCtrlNavSystem, nav) || !rd(ctrlEntry, kCtrlNavId, navId) || !slotOf(nav, navId, navSlot) ||
        !rd(nav, kEntries, navEntries)) return false;
    out.stage = 2;
    const uint8_t* entry = navEntries + size_t(navSlot) * kNavStride;
    if (!actorOf(nav, navSlot, out.actor)) return false;
    out.stage = 3;
    const uint8_t* body;
    float v[3];
    if (rd(entry, kNavBody, body) && readBytes(body + kBodyVelocity, v, sizeof v) && std::isfinite(v[0] + v[1] + v[2])) {
        out.speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        out.speedOk = true;
    }
    out.stage = 4;
    {
        const uint8_t* im = entry + kNavIronMan;
        float f[4], maxes[2];
        if (rd(im, kImOn0, out.handOn[0]) && rd(im, kImOn1, out.handOn[1]) && readBytes(im + kImSingle0, f, sizeof f) &&
            readBytes(im + kImSingleMax, maxes, sizeof maxes) && readBytes(entry + kNavHandDir0, out.handDir[0], 12) &&
            readBytes(entry + kNavHandDir1, out.handDir[1], 12)) {
            out.handSingle[0] = f[0] * maxes[0];
            out.handSingle[1] = f[1] * maxes[0];
            out.handTwo[0] = f[2] * maxes[1];
            out.handTwo[1] = f[3] * maxes[1];
            bool finite = true;
            for (int h = 0; h < 2; h++)
                for (int i = 0; i < 3; i++) finite &= std::isfinite(out.handDir[h][i]);
            out.thrustersOk = finite && std::isfinite(f[0] + f[1] + f[2] + f[3] + maxes[0] + maxes[1]) && out.handOn[0] <= 1 && out.handOn[1] <= 1;
        }
    }
    const uint8_t* bat;
    uint32_t batId;
    uintptr_t vtable;
    uint16_t batSlot, res;
    const uint8_t* batEntries;
    const uint8_t* resTable;
    const uint8_t* resources;
    if (!rd(entry, kNavBoostCost, out.cost) || !rd(entry, kNavBatterySystem, bat) || !bat) return true;  // no battery in this mode
    out.stage = 5;
    if (!rd(entry, kNavBatteryId, batId) || !rd(bat, 0, vtable) || vtable != batteryVtable || !slotOf(bat, batId, batSlot) ||
        !rd(bat, kEntries, batEntries)) return true;
    out.stage = 6;
    const uint8_t* be = batEntries + size_t(batSlot) * kBatStride;
    if (!rd(be, 0, res) || !rd(be, kBatEnergy, out.energy) || !rd(be, kBatPending, out.pending) || !rd(be, kBatDelayLeft, out.delayLeft) ||
        !rd(bat, kBatResources, resTable) || !rd(resTable, 0, resources)) return true;
    const uint8_t* r = resources + size_t(res) * kResStride;
    if (!rd(r, kResMaxPower, out.maxPower) || !rd(r, kResRate, out.rate) || !rd(r, kResDelay, out.delay)) return true;
    out.stage = 7;
    out.battery = std::isfinite(out.energy) && std::isfinite(out.maxPower) && std::isfinite(out.cost) && out.maxPower > 0;
    return true;
}

// Whether your talk button is held, from your entry in the voice broadcaster system
// (the one whose peer is the network game's local peer).
inline bool readVoicePressed(const void* voipSystem, bool& pressed) {
    const uint8_t *ng, *entries;
    uint16_t locals, own, count;
    if (!rd(voipSystem, kVoipNet, ng) || !rd(ng, kNgLocals, locals) || !locals || !rd(ng, kNgRecPeerTeam, own) ||
        !rd(voipSystem, kVoipCount, count) || !rd(voipSystem, kVoipEntries, entries))
        return false;
    for (uint16_t i = 0; i < count && i < 64; i++) {
        const uint8_t* e = entries + size_t(i) * kVoipStride;
        uint32_t peer;
        uint16_t flags;
        if (!rd(e, kVoipPeer, peer) || !rd(e, kVoipFlags, flags)) return false;
        if (uint16_t(peer) == (own & 0x1F)) {
            pressed = (flags & kVoipPressed) != 0;
            return true;
        }
    }
    return false;
}

// The local player's punch entry is the one whose controller handle is ours.
inline bool readShield(const void* punchSystem, const void* ctrlSystem, uint32_t ctrlId, uint32_t& flags) {
    uint16_t count;
    const uint8_t* entries;
    if (!ctrlSystem || !rd(punchSystem, kPunchCount, count) || !rd(punchSystem, kPunchEntries, entries)) return false;
    for (uint16_t i = 0; i < count && i < 256; i++) {
        const uint8_t* e = entries + size_t(i) * kPunchStride;
        const void* sys;
        uint32_t id;
        if (!rd(e, kPunchCtrlSystem, sys) || !rd(e, kPunchCtrlId, id)) return false;
        if (sys == ctrlSystem && uint16_t(id) == uint16_t(ctrlId)) return rd(e, kPunchFlags, flags);
    }
    return false;
}

// Who a stun lands on, and whether they were already stunned.
inline bool readStunTarget(const void* punchableSystem, uint32_t id, uint64_t& actor, bool& alreadyStunned) {
    uint16_t slot;
    const uint8_t* entries;
    uint32_t flags;
    if (!slotOf(punchableSystem, id, slot) || !actorOf(punchableSystem, slot, actor) || !rd(punchableSystem, kEntries, entries) ||
        !rd(entries + size_t(slot) * kPunchableStride, kPunchableFlags, flags)) return false;
    alreadyStunned = (flags & 3) != 0;
    return true;
}

inline bool codeMatches(uintptr_t base, uintptr_t rva, const uint8_t* bytes, size_t n) {
    uint8_t have[64];
    return n <= sizeof have && readBytes(reinterpret_cast<const void*>(base + rva), have, n) && std::memcmp(have, bytes, n) == 0;
}

inline bool batteryVtableMatches(uintptr_t base) {
    uintptr_t v0, v57;
    const uint8_t* vt = reinterpret_cast<const uint8_t*>(base + kBatteryVtableRva);
    return rd(vt, 0, v0) && rd(vt, 57 * 8, v57) && v0 == base + kBatteryVtable0Rva && v57 == base + kBatteryVtable57Rva;
}

}  // namespace hudplus::game
