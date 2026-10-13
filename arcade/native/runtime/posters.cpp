// Lobby-poster screen: DOCK, poster touch and the light gun (see posters.h).
//
// Engine facts (echovr.exe 1683152886), all checked by signature at startup:
//   CR15NetDynamicPosterCS (component key sym("R15NetDynamicPoster"))
//     +0x80 gamespace, +0xfc u16 poster count, +0x100 posters (0x58 each), +0x108 keys (16 each,
//     first qword = the poster actor's handle; its low 16 bits index the gamespace's actor
//     name tables, gamespace +0x370 -> +0xb8, else +0x80, as the engine's own messages do).
//     Poster: +0x04 u32 art slice, +0x08 texture it shows (-1 = the level's fallback art),
//     +0x10 texture it replaces on the model, +0x18 handle for +0x08, +0x48 server poster key.
//     The system has no per-frame update. 0x140d00050 start (phase callback, once per system
//     when its level starts) subscribes 0x140d0f900 to the gamespace event
//     evt_client_settings_changed; that handler resolves +0x48 to +0x08 and re-applies +0x08
//     when it changed (fallback art while it is -1). So DOCK runs from the touch-button
//     Update instead, on the poster systems learned at start.
//     0x140d20410 OverrideTexture(cs, key, poster, slice): loads +0x08 into +0x18 and puts it
//     on the poster model. 0x140cd9430 FallbackArt(cs, key, poster, slice): the level's own
//     art back on the model. 0x140ca3740 destructor.
//   Touch buttons (CR15ButtonInteractCS): after its update, +0x1f0 / +0x200 hold the two
//     fingertips in world space as (x, y, z, radius); the engine writes radius 0.0085 (8.5 mm).
//   CR15NetBulletCS 0x140ce2020 Fire(cs, bullet, info, speed, muzzle, dir, fromNetwork, flag):
//     bullet = +0xf8 + 0x298 * u16 at (+0xc8 + 4 * handle); its +0xf8 position and +0x104
//     direction are set by Fire.
//   Gamespace +0xd0 rotation (x, y, z, w), +0xe0 position, +0xec scale: its place in the world.
#include "posters.h"
#include "d3d12_stream.h"
#include "log.h"
#include "../generated/arcade_tab.h"
#include "../generated/posters.h"
#include "../shared/poster_turn.h"
#include "../vendor/minhook/include/MinHook.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace posters {
namespace {

using U = uint64_t;
using P = unsigned char*;
template <class T> T& at(void* p, size_t n) { return *reinterpret_cast<T*>(static_cast<P>(p) + n); }
constexpr U NONE = ~U(0);

constexpr unsigned RVA_START = 0xd00050, RVA_DESTROY = 0xca3740, RVA_OVERRIDE = 0xd20410, RVA_FALLBACK = 0xcd9430,
                   RVA_FIRE = 0xce2020;
struct Signature { unsigned rva; unsigned char bytes[16]; };
constexpr Signature SIGNATURES[] = {
    {RVA_START, {0x40,0x53,0x48,0x83,0xec,0x40,0x48,0x8b,0xd9,0xe8,0x32,0xe0,0x3c,0xff,0x85,0xc0}},
    {RVA_DESTROY, {0x48,0x89,0x4c,0x24,0x08,0x53,0x56,0x57,0x48,0x83,0xec,0x20,0x8b,0xf2,0x48,0x8b}},
    {RVA_OVERRIDE, {0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x48,0x83,0xec,0x40,0x48,0x8b,0x81,0xb8}},
    {RVA_FALLBACK, {0x48,0x89,0x5c,0x24,0x18,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x40,0x48,0x8b,0x81}},
    {RVA_FIRE, {0x48,0x89,0x5c,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57}},
};
constexpr unsigned POSTER_STRIDE = 0x58, BULLET_STRIDE = 0x298;

using Start = U (*)(void*);
using Destroy = void* (*)(void*, unsigned);
using Override = int (*)(void*, void*, void*, U);
using Fallback = void (*)(void*, void*, void*, U);
using Fire = U (*)(void*, unsigned, void*, float, void*, void*, int, int);
Start startOriginal;
Destroy destroyOriginal;
Override overrideTexture;
Fallback fallbackArt;
Fire fireOriginal;

arcade::Shared* shared = nullptr;
std::atomic<bool> fault{false};
std::mutex mutex;

// ---- geometry ----
struct Vec { float x = 0, y = 0, z = 0; };
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(Vec a) { return std::sqrt(dot(a, a)); }
bool finite(Vec a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z) && std::fabs(a.x) + std::fabs(a.y) + std::fabs(a.z) < 1e5f; }

// A placement: world = position + rotate(rotation, scale * local). Posters scale uniformly.
struct Xform {
    float q[4] = {0, 0, 0, 1};
    Vec t;
    float s = 1;
    Vec rotate(Vec v, bool inverse = false) const {
        Vec u{inverse ? -q[0] : q[0], inverse ? -q[1] : q[1], inverse ? -q[2] : q[2]};
        Vec c = cross(u, v) + v * q[3];
        return v + cross(u, c) * 2.f;
    }
    Vec apply(Vec v) const { return t + rotate(v * s); }
    Vec unapply(Vec w) const { return rotate(w - t, true) * (1.f / s); }
};

Xform gamespaceXform(void* gs) {
    Xform x;
    memcpy(x.q, static_cast<P>(gs) + 0xd0, 16);
    memcpy(&x.t, static_cast<P>(gs) + 0xe0, 12);
    x.s = at<float>(gs, 0xec);
    float n = x.q[0] * x.q[0] + x.q[1] * x.q[1] + x.q[2] * x.q[2] + x.q[3] * x.q[3];
    if (!(std::fabs(n - 1) < .01f) || !finite(x.t) || !(x.s > .01f && x.s < 100)) {
        static int logged = 0;
        if (logged++ < 3) logf("posters: gamespace %p has no usable placement; using the level's own space", gs);
        return Xform{};
    }
    return x;
}

struct Tri { Vec a, b, c, n; float uv[3][2]; };
struct Mesh { std::vector<Tri> tris; Vec center; };
std::vector<Mesh> meshes;
std::unordered_map<U, unsigned> table;  // poster actor -> POSTERS index

// Closest point on a triangle (Ericson, Real-Time Collision Detection 5.1.5), as barycentric weights.
void closest(const Tri& t, Vec p, float& wa, float& wb, float& wc) {
    Vec ab = t.b - t.a, ac = t.c - t.a, ap = p - t.a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) { wa = 1; wb = wc = 0; return; }
    Vec bp = p - t.b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) { wb = 1; wa = wc = 0; return; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { float v = d1 / (d1 - d3); wa = 1 - v; wb = v; wc = 0; return; }
    Vec cp = p - t.c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) { wc = 1; wa = wb = 0; return; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { float w = d2 / (d2 - d6); wa = 1 - w; wc = w; wb = 0; return; }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { float w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); wb = 1 - w; wc = w; wa = 0; return; }
    float denom = 1.f / (va + vb + vc);
    wb = vb * denom; wc = vc * denom; wa = 1 - wb - wc;
}

// How mesh `mesh` carries the picture (poster_turn.h); the usual way when it has no face.
PosterTurn turnOf(unsigned mesh) {
    if (mesh == POSTER_NO_MESH) return POSTER_UPRIGHT;
    const PosterMesh& m = POSTER_MESHES[mesh];
    return {m.ur, m.ud, m.vr, m.vd};
}

// Mesh UV -> host screen pixel (the shader doubles v; see arcade_layout.py), through the
// mesh's picture turn.
bool toPixel(float u, float v, unsigned mesh, int& x, int& y) {
    float fx = u * POSTER_TEX_W - POSTER_X0, fy = 2 * v * POSTER_TEX_H - POSTER_Y0;
    if (!(fx >= 0 && fy >= 0 && fx < POSTER_FRAME_W && fy < POSTER_FRAME_H)) return false;
    float ux, uy;
    if (!turnToUi(turnOf(mesh), int(POSTER_FRAME_W), int(POSTER_FRAME_H), fx, fy, ux, uy)) return false;
    x = int(ux); y = int(uy);
    return true;
}
void interpolate(const Tri& t, float wa, float wb, float wc, float& u, float& v) {
    u = t.uv[0][0] * wa + t.uv[1][0] * wb + t.uv[2][0] * wc;
    v = t.uv[0][1] * wa + t.uv[1][1] * wb + t.uv[2][1] * wc;
}

// ---- dock state ----
// Several posters can show the arcade at once. A dock is one poster slot whose texture was
// swapped; every poster of the same system that shares its model shows the arcade too (the
// texture is swapped on the model they share; each poster only picks its slice of it), so a
// dock's faces are all of them. A new dock is added, never swapped for the old one.
struct Dock {
    void* cs = nullptr;
    void* gs = nullptr;
    unsigned index = 0;
    U actor = 0, handle = 0, savedName = NONE, savedKey = NONE;
    unsigned mesh = 0;
    U texture = 0;       // the arcade texture it shows: the usual one, or the turned one
    Xform level, space;  // poster in its level, level in the world
    std::vector<Xform> faces;
};
std::vector<Dock> docks;
std::set<U> remembered;         // posters docked this session: docked again whenever their lobby loads
std::set<U> refused;            // posters that refused the arcade texture this session (not retried)
LONG handled = 0;               // last dockSerial answered
U lastTick = 0;                 // the touch-button Update last drove DOCK
std::set<void*> systems;        // live poster systems (from start to destructor)
std::set<void*> described;      // logged once each

// A poster-system key's actor name, as the engine names it (OverrideTexture's messages):
// gamespace +0x370 -> the +0xb8 table at the handle's low 16 bits, else (-1 there) +0x80.
U nameOf(void* gs, U handle) {
    P names = at<P>(gs, 0x370);
    if (!names) return NONE;
    size_t index = size_t(handle & 0xffff);
    for (size_t list : {size_t(0xb8), size_t(0x80)}) {
        P a = at<P>(names, list);
        U name = a ? at<U>(a, index * 8) : NONE;
        if (name != NONE) return name;
    }
    return NONE;
}

// Which POSTERS entry a poster-system key is, or -1. The social lobby's central display carries
// a server name in the +0xb8 table (c8c33e48396521e1..e4) as well as its actor name at +0x80;
// whichever of the two the table knows counts.
int posterOf(void* gs, U handle) {
    P names = at<P>(gs, 0x370);
    if (!names) return -1;
    size_t index = size_t(handle & 0xffff);
    for (size_t list : {size_t(0xb8), size_t(0x80)}) {
        P a = at<P>(names, list);
        U name = a ? at<U>(a, index * 8) : NONE;
        if (name == NONE) continue;
        auto it = table.find(name);
        if (it != table.end()) return int(it->second);
    }
    return -1;
}

bool hasFace(unsigned mesh) { return mesh != POSTER_NO_MESH; }

Xform placement(const PosterInfo& info) {
    return Xform{{info.rot[0], info.rot[1], info.rot[2], info.rot[3]}, {info.pos[0], info.pos[1], info.pos[2]}, info.scale[0]};
}

// A poster's middle in the world: its face's middle, or its origin when it has no face map.
Vec centerOf(const PosterInfo& info, const Xform& space) {
    return space.apply(placement(info).apply(hasFace(info.mesh) ? meshes[info.mesh].center : Vec{}));
}

// Already showing the arcade: a dock in that system has the same model.
bool covered(void* cs, unsigned mesh) {
    for (auto& d : docks)
        if (d.cs == cs && d.mesh == mesh) return true;
    return false;
}

bool dockedIn(void* gs) {
    for (auto& d : docks)
        if (d.gs == gs) return true;
    return false;
}

void answer(LONG state, const char* fmt, ...) {
    char text[sizeof(shared->dockText)];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    memcpy(shared->dockText, text, sizeof(text));
    shared->dockState = state;
    MemoryBarrier();
    shared->dockDone = handled;
    logf("posters: %s", text);
}

LONG dockedState() { return docks.empty() ? arcade::Undocked : arcade::Docked; }

// Tells the stream which poster textures to feed: the usual one, the turned one, or neither.
void syncStream() {
    bool usual = false, turnedAny = false;
    PosterTurn turn = POSTER_UPRIGHT;
    for (auto& d : docks) {
        if (turned(turnOf(d.mesh))) { turnedAny = true; turn = turnOf(d.mesh); }
        else usual = true;
    }
    stream::setPosterActive(usual);
    stream::setTurnedActive(turnedAny, turn);
}

void releaseFingers();
bool playerPosition(void* gs, U now, Vec& out);

// A poster's own art back on its model: the server poster it had, else the level's art.
void restoreArt(void* cs, unsigned index, U name, U serverKey) {
    P item = at<P>(cs, 0x100) + size_t(index) * POSTER_STRIDE;
    U key[2] = {at<U>(at<P>(cs, 0x108), size_t(index) * 16), at<U>(at<P>(cs, 0x108), size_t(index) * 16 + 8)};
    at<U>(item, 0x48) = serverKey;
    at<U>(item, 0x08) = name;
    if (name != NONE) {
        int r = overrideTexture(cs, key, item, at<unsigned>(item, 4));
        if (r == 0) return;
        at<U>(item, 0x08) = NONE;
        logf("posters: restoring the server poster failed (%d); showing the level's art", r);
    }
    fallbackArt(cs, key, item, at<unsigned>(item, 4));  // nothing re-applies it per frame
}

bool slotStill(const Dock& d) {
    return d.index < at<unsigned short>(d.cs, 0xfc) && at<U>(at<P>(d.cs, 0x108), size_t(d.index) * 16) == d.handle;
}

// Every poster back to its own picture, and the session's list forgotten (DOCK turned off).
void undockAll(const char* why) {
    if (docks.empty()) { answer(arcade::Undocked, "%s", why); return; }
    stream::setPosterActive(false);  // no copies into the textures from here on
    stream::setTurnedActive(false, POSTER_UPRIGHT);
    releaseFingers();
    for (auto it = docks.rbegin(); it != docks.rend(); ++it)
        if (slotStill(*it)) restoreArt(it->cs, it->index, it->savedName, it->savedKey);
    docks.clear();
    remembered.clear();
    answer(arcade::Undocked, "%s", why);
}

// Puts the arcade on poster slot `slot` of system `cs`, keeping the posters already docked.
// Returns 0, or the engine's error from OverrideTexture.
int dockSlot(void* cs, unsigned slot, Xform space) {
    void* gs = at<void*>(cs, 0x80);
    P keys = at<P>(cs, 0x108);
    const PosterInfo& info = POSTERS[posterOf(gs, at<U>(keys, size_t(slot) * 16))];
    P item = at<P>(cs, 0x100) + size_t(slot) * POSTER_STRIDE;
    Dock d;
    d.cs = cs; d.gs = gs; d.index = slot; d.actor = info.actor; d.handle = at<U>(keys, size_t(slot) * 16); d.mesh = info.mesh;
    d.texture = turned(turnOf(info.mesh)) ? ARCADE_POSTER_TURNED_TEXTURE : ARCADE_POSTER_TEXTURE;
    d.level = placement(info);
    d.space = space;
    d.faces.push_back(d.level);
    for (unsigned i = 0, count = at<unsigned short>(cs, 0xfc); i < count; i++) {
        int which = posterOf(gs, at<U>(keys, size_t(i) * 16));
        if (i == slot || which < 0 || POSTERS[which].mesh != info.mesh) continue;
        d.faces.push_back(placement(POSTERS[which]));
    }
    d.savedName = at<U>(item, 0x08);
    d.savedKey = at<U>(item, 0x48);
    at<U>(item, 0x48) = NONE;  // stop the server's poster from being resolved back in
    at<U>(item, 0x08) = d.texture;
    U key[2] = {at<U>(keys, size_t(slot) * 16), at<U>(keys, size_t(slot) * 16 + 8)};
    // Its own art slice, as the engine passes (slice -1 drew the docked poster black).
    int r = overrideTexture(cs, key, item, at<unsigned>(item, 4));
    if (r != 0) {
        restoreArt(cs, slot, NONE, d.savedKey);  // as the engine does when a poster fails to load
        refused.insert(info.actor);
        logf("posters: %016llx refused the arcade texture (%d); not trying it again this session", info.actor, r);
        return r;
    }
    docks.push_back(d);
    remembered.insert(info.actor);
    Vec center = centerOf(info, space);
    logf("posters: docked on %016llx (system %p slot %u, was %016llx/%016llx) at %.2f %.2f %.2f; %zu poster(s) of this model; %zu dock(s) now; texture %s%s",
         info.actor, cs, slot, d.savedName, d.savedKey, center.x, center.y, center.z, d.faces.size(), docks.size(),
         stream::posterTextureReady() ? "ready" : "NOT CREATED YET", d.texture == ARCADE_POSTER_TURNED_TEXTURE ? " (picture turned for this display)" : "");
    syncStream();
    return 0;
}

// Engine thread, inside the touch-button Update: DOCK requests (AUTO DOCK sends one every ~2 s).
// The nearest poster that is not showing the arcade yet gets it; the others keep it.
void handleRequest(U now) {
    LONG serial = shared->dockSerial;
    if (serial == handled || systems.empty()) return;  // no poster system: the heartbeat answers
    handled = serial;
    if (!shared->dockWant) {
        undockAll(docks.empty() ? "Not docked." : "Undocked: the posters show their own pictures again.");
        return;
    }
    void* bestCs = nullptr;
    unsigned best = 0;
    float bestDistance = 1e9f;
    Vec player;
    Xform bestSpace;
    bool sawHands = false;
    for (void* cs : systems) {
        void* gs = at<void*>(cs, 0x80);
        if (!playerPosition(gs, now, player)) continue;
        sawHands = true;
        Xform space = gamespaceXform(gs);
        unsigned count = at<unsigned short>(cs, 0xfc);
        P keys = at<P>(cs, 0x108);
        for (unsigned i = 0; i < count && keys; i++) {
            int which = posterOf(gs, at<U>(keys, size_t(i) * 16));
            // Posters without a face map are never docked: on the combat-lobby set piece the
            // arcade texture drew blank and a shot at it crashed the GPU.
            if (which < 0 || !hasFace(POSTERS[which].mesh)) continue;
            float d = length(centerOf(POSTERS[which], space) - player);
            if (d < bestDistance) { bestDistance = d; bestCs = cs; best = i; bestSpace = space; }
        }
    }
    if (!sawHands) {
        answer(dockedState(), docks.empty() ? "Could not find your hands - touch the tablet." : "Docked on %zu poster(s).", docks.size());
        return;
    }
    if (!bestCs) { answer(dockedState(), docks.empty() ? "No poster here can show the arcade." : "Docked on %zu poster(s).", docks.size()); return; }
    void* gs = at<void*>(bestCs, 0x80);
    const PosterInfo& info = POSTERS[posterOf(gs, at<U>(at<P>(bestCs, 0x108), size_t(best) * 16))];
    if (covered(bestCs, info.mesh) || refused.count(info.actor)) {
        answer(dockedState(), docks.empty() ? "The poster near you cannot show the arcade." : "Docked on %zu poster(s).", docks.size());
        return;
    }
    int r = dockSlot(bestCs, best, bestSpace);
    if (r != 0) { answer(dockedState(), "The poster refused the arcade screen (error %d, see runtime.log).", r); return; }
    answer(arcade::Docked, "Docked on the nearest poster too (%.1f m away; %zu now). Touch it, or shoot it in combat.", bestDistance, docks.size());
}

// The posters docked earlier this session, back on whenever their lobby is loaded again.
void restoreRemembered() {
    if (remembered.empty() || !shared->dockWant) return;
    for (void* cs : systems) {
        void* gs = at<void*>(cs, 0x80);
        unsigned count = at<unsigned short>(cs, 0xfc);
        P keys = at<P>(cs, 0x108);
        for (unsigned i = 0; i < count && keys; i++) {
            int which = posterOf(gs, at<U>(keys, size_t(i) * 16));
            if (which < 0 || !hasFace(POSTERS[which].mesh)) continue;
            const PosterInfo& info = POSTERS[which];
            if (!remembered.count(info.actor) || refused.count(info.actor) || covered(cs, info.mesh)) continue;
            if (dockSlot(cs, i, gamespaceXform(gs)) == 0)
                answer(arcade::Docked, "The arcade is back on %zu poster(s).", docks.size());
        }
    }
}

void describe(void* cs) {
    void* gs = at<void*>(cs, 0x80);
    unsigned count = at<unsigned short>(cs, 0xfc), known = 0;
    P keys = at<P>(cs, 0x108);
    for (unsigned i = 0; i < count && keys; i++) known += posterOf(gs, at<U>(keys, size_t(i) * 16)) >= 0 ? 1 : 0;
    Xform space = gamespaceXform(gs);
    logf("posters: poster system %p (gamespace %p at %.2f %.2f %.2f x%.2f): %u posters, %u can dock",
         cs, gs, space.t.x, space.t.y, space.t.z, space.s, count, known);
    // Each slot's names in both tables (to check the lookup against what the game shows).
    P names = at<P>(gs, 0x370);
    for (unsigned i = 0; i < count && keys && names; i++) {
        U handle = at<U>(keys, size_t(i) * 16);
        size_t index = size_t(handle & 0xffff);
        P b8 = at<P>(names, 0xb8), n80 = at<P>(names, 0x80);
        logf("posters:   slot %u handle %016llx: +0xb8 %016llx, +0x80 %016llx -> %s", i, handle,
             b8 ? at<U>(b8, index * 8) : NONE, n80 ? at<U>(n80, index * 8) : NONE, posterOf(gs, handle) >= 0 ? "known" : "unknown");
    }
}

// Keeps the arcade on every docked poster.
void maintain() {
    for (size_t k = 0; k < docks.size();) {
        Dock& d = docks[k];
        if (!slotStill(d)) {
            logf("posters: poster %016llx went away; %zu dock(s) left", d.actor, docks.size() - 1);
            docks.erase(docks.begin() + long(k));
            syncStream();
            if (docks.empty()) releaseFingers();
            answer(dockedState(), docks.empty() ? "The poster went away; undocked." : "Docked on %zu poster(s).", docks.size());
            continue;
        }
        P item = at<P>(d.cs, 0x100) + size_t(d.index) * POSTER_STRIDE;
        // The server assigned new art to this poster while it is docked: remember it for
        // undock and keep the arcade on.
        if (at<U>(item, 0x48) != NONE) { d.savedKey = at<U>(item, 0x48); at<U>(item, 0x48) = NONE; }
        if (at<U>(item, 0x08) != d.texture) {
            logf("posters: the art of %016llx changed under the dock; putting the arcade back", d.actor);
            d.savedName = at<U>(item, 0x08);
            at<U>(item, 0x08) = d.texture;
            U key[2] = {at<U>(at<P>(d.cs, 0x108), size_t(d.index) * 16), at<U>(at<P>(d.cs, 0x108), size_t(d.index) * 16 + 8)};
            int r = overrideTexture(d.cs, key, item, at<unsigned>(item, 4));
            if (r != 0) {
                at<U>(item, 0x08) = d.savedName;
                logf("posters: the server replaced %016llx (%d); that one is undocked", d.actor, r);
                docks.erase(docks.begin() + long(k));
                syncStream();
                if (docks.empty()) releaseFingers();
                answer(dockedState(), docks.empty() ? "The server replaced the poster; undocked." : "Docked on %zu poster(s).", docks.size());
                continue;
            }
        }
        k++;
    }
}

// Engine thread, after a touch-button Update (every frame while a lobby is up).
void tick(U now) {
    lastTick = now;
    for (void* cs : systems)
        if (at<unsigned short>(cs, 0xfc) && described.insert(cs).second) describe(cs);  // once it has its posters
    maintain();
    restoreRemembered();
    handleRequest(now);
}

// ---- fingertips (from the touch-button systems) ----
struct Hands { Vec tip[2]; bool valid[2] = {}; void* gs = nullptr; U tick = 0; };
std::map<void*, Hands> hands;   // by button system
void* touchSource = nullptr;    // the button system whose fingertips drive the docked poster
struct Finger { bool down = false; int x = 0, y = 0; };
Finger fingers[2];

bool playerPosition(void* gs, U now, Vec& out) {
    const Hands* best = nullptr;
    for (auto& [cs, h] : hands) {
        if (now - h.tick > 1500 || !(h.valid[0] || h.valid[1])) continue;
        if (!best || (h.gs == gs && best->gs != gs)) best = &h;
    }
    if (!best) return false;
    out = best->valid[0] && best->valid[1] ? (best->tip[0] + best->tip[1]) * .5f : best->tip[best->valid[0] ? 0 : 1];
    return true;
}

void releaseFingers() {
    for (unsigned i = 0; i < 2; i++)
        if (fingers[i].down) { arcade::pushTouch(shared, i, arcade::PointUp, fingers[i].x, fingers[i].y); fingers[i].down = false; }
}

// ---- hooks ----
void failPosters(const char* why) {
    if (fault.exchange(true)) return;
    logf("posters: FAULT: %s -- dock, poster touch and light gun disabled for this session", why);
    stream::setPosterActive(false);
}

void safeTick(U now) {
    __try { tick(now); } __except (EXCEPTION_EXECUTE_HANDLER) { failPosters("access fault in the poster tick"); }
}

U start(void* cs) {
    U r = startOriginal(cs);
    std::lock_guard<std::mutex> lock(mutex);
    systems.insert(cs);
    return r;
}

void* destroy(void* cs, unsigned flags) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        systems.erase(cs);
        described.erase(cs);
        // Its docks go with it (the posters stay remembered: back on when the lobby loads again).
        size_t before = docks.size();
        docks.erase(std::remove_if(docks.begin(), docks.end(), [cs](const Dock& d) { return d.cs == cs; }), docks.end());
        if (docks.size() != before) {
            if (docks.empty()) { stream::setPosterActive(false); releaseFingers(); }  // before the level frees anything
            answer(dockedState(), docks.empty() ? "Left the lobby; the arcade comes back on its posters when you return." : "Docked on %zu poster(s).", docks.size());
        }
    }
    return destroyOriginal(cs, flags);
}

float lastFace = 1e9f, lastFront = 0;  // the last contact() test: metres from the face, and in front of it

// Where a fingertip touches a poster showing the arcade: pixel, or false when not touching.
bool contact(Vec tip, bool wasDown, int& x, int& y) {
    float best = 1e9f, bestSigned = 0, u = 0, v = 0;
    unsigned bestMesh = POSTER_NO_MESH;
    for (const Dock& dock : docks)
    for (const Xform& face : dock.faces) {
        if (!hasFace(dock.mesh)) break;
        Vec local = face.unapply(dock.space.unapply(tip));
        float scale = face.s * dock.space.s;
        for (auto& t : meshes[dock.mesh].tris) {
            float wa, wb, wc;
            closest(t, local, wa, wb, wc);
            Vec p = t.a * wa + t.b * wb + t.c * wc;
            float distance = length(local - p) * scale;
            if (distance < best) {
                best = distance;
                bestSigned = dot(local - p, t.n) * scale;
                bestMesh = dock.mesh;
                interpolate(t, wa, wb, wc, u, v);
            }
        }
    }
    lastFace = best;
    lastFront = bestSigned;
    // Touching: within 1.5 cm in front (4 cm once down) and up to 8 cm through the surface.
    float front = wasDown ? .04f : .015f;
    if (!(bestSigned < front && bestSigned > -.08f && best < .12f)) return false;
    return toPixel(u, v, bestMesh, x, y);
}

void afterButton(void* cs, U now) {
    Hands h;
    h.gs = at<void*>(cs, 0x80);
    h.tick = now;
    for (int i = 0; i < 2; i++) {
        const float* f = reinterpret_cast<const float*>(static_cast<P>(cs) + (i ? 0x200 : 0x1f0));
        h.tip[i] = {f[0], f[1], f[2]};
        h.valid[i] = f[3] > 0 && f[3] < .1f && finite(h.tip[i]) && (f[0] != 0 || f[1] != 0 || f[2] != 0);
    }
    if (!h.valid[0] && !h.valid[1]) return;
    if (hands.find(cs) == hands.end()) {
        static int logged = 0;
        if (logged++ < 8)
            logf("posters: hands from touch system %p (gamespace %p): %.2f %.2f %.2f / %.2f %.2f %.2f", cs, h.gs,
                 h.tip[0].x, h.tip[0].y, h.tip[0].z, h.tip[1].x, h.tip[1].y, h.tip[1].z);
    }
    hands[cs] = h;
    for (auto it = hands.begin(); it != hands.end();) it = now - it->second.tick > 5000 ? hands.erase(it) : std::next(it);
    if (docks.empty()) return;
    auto source = hands.find(touchSource);
    bool stale = !touchSource || source == hands.end() || now - source->second.tick > 500;
    if (stale || (dockedIn(h.gs) && touchSource != cs && !dockedIn(source->second.gs))) {
        if (touchSource != cs) logf("posters: fingertips from touch system %p (gamespace %p)", cs, h.gs);
        touchSource = cs;
    }
    if (touchSource != cs) return;
    for (unsigned i = 0; i < 2; i++) {
        Finger& f = fingers[i];
        int x = 0, y = 0;
        bool on = h.valid[i] && contact(h.tip[i], f.down, x, y);
        static U lastNear = 0;
        if (h.valid[i] && !on && lastFace < .5f && now - lastNear > 2000) {
            lastNear = now;
            logf("posters: finger %u near the poster but not touching: %.3f m from the face, %.3f m in front", i, lastFace, lastFront);
        }
        if (on && !f.down) {
            f = {true, x, y};
            arcade::pushTouch(shared, i, arcade::PointDown, x, y);
            static int logged = 0;
            if (logged++ < 5) logf("posters: finger %u touched the poster at pixel %d,%d", i, x, y);
        } else if (on && (std::abs(x - f.x) >= 2 || std::abs(y - f.y) >= 2)) {
            f.x = x; f.y = y;
            arcade::pushTouch(shared, i, arcade::PointMove, x, y);
        } else if (!on && f.down) {
            f.down = false;
            arcade::pushTouch(shared, i, arcade::PointUp, f.x, f.y);
        }
    }
}

// Where a ray (world space) first meets a poster showing the arcade: pixel and distance in metres.
bool rayHit(Vec worldOrigin, Vec worldDir, int& x, int& y, float& metres) {
    float bestMetres = 1e9f, u = 0, v = 0;
    unsigned hitMesh = POSTER_NO_MESH;
    for (const Dock& dock : docks)
    for (const Xform& face : dock.faces) {
        if (!hasFace(dock.mesh)) break;
        Vec o = face.unapply(dock.space.unapply(worldOrigin));
        Vec d = face.rotate(dock.space.rotate(worldDir, true), true);
        float toMetres = length(d) * face.s * dock.space.s;
        for (auto& t : meshes[dock.mesh].tris) {  // Moller-Trumbore
            Vec e1 = t.b - t.a, e2 = t.c - t.a, pv = cross(d, e2);
            float det = dot(e1, pv);
            if (std::fabs(det) < 1e-9f) continue;
            float inv = 1 / det;
            Vec tv = o - t.a;
            float b = dot(tv, pv) * inv;
            if (b < 0 || b > 1) continue;
            Vec qv = cross(tv, e1);
            float c = dot(d, qv) * inv;
            if (c < 0 || b + c > 1) continue;
            float along = dot(e2, qv) * inv;
            if (along > 0 && along * toMetres < bestMetres) { bestMetres = along * toMetres; hitMesh = dock.mesh; interpolate(t, 1 - b - c, b, c, u, v); }
        }
    }
    metres = bestMetres;
    return bestMetres < 1e9f && toPixel(u, v, hitMesh, x, y);
}

// Light gun: a local shot's ray against the docked poster's face.
void afterFire(void* cs, unsigned handle) {
    if (docks.empty()) return;
    P bullet = at<P>(cs, 0xf8) + size_t(at<unsigned short>(at<P>(cs, 0xc8), size_t(handle & 0xffff) * 4)) * BULLET_STRIDE;
    Vec origin = at<Vec>(bullet, 0xf8), dir = at<Vec>(bullet, 0x104);
    if (!finite(origin) || !finite(dir) || length(dir) < .5f) return;
    Xform space = gamespaceXform(at<void*>(cs, 0x80));
    Vec worldOrigin = space.apply(origin), worldDir = space.rotate(dir);
    static int hits = 0, misses = 0;
    int x, y;
    float metres = 0;
    if (rayHit(worldOrigin, worldDir, x, y, metres)) {
        arcade::pushTouch(shared, arcade::SHOT_POINTER, arcade::Shot, x, y);
        InterlockedIncrement(&shared->shots);
        if (hits++ < 20) logf("light gun: hit pixel %d,%d (%.1f m from the muzzle)", x, y, metres);
    } else if (misses++ < 20) {
        // Which known poster the shot went nearest to: tells a bad poster lookup from a bad aim.
        const Dock& dock = docks.front();
        U nearActor = 0;
        float closest = 1e9f;
        Vec unit = worldDir * (1.f / length(worldDir));
        unsigned count = at<unsigned short>(dock.cs, 0xfc);
        P keys = at<P>(dock.cs, 0x108);
        for (unsigned i = 0; i < count && keys; i++) {
            int which = posterOf(dock.gs, at<U>(keys, size_t(i) * 16));
            if (which < 0) continue;
            const PosterInfo& info = POSTERS[which];
            Vec c = centerOf(info, dock.space) - worldOrigin;
            float along = std::max(0.f, dot(c, unit)), d = length(c - unit * along);
            if (d < closest) { closest = d; nearActor = info.actor; }
        }
        logf("light gun: shot from %.2f %.2f %.2f dir %.2f %.2f %.2f missed the %zu docked poster(s); nearest poster to the shot %016llx (%.1f m off)",
             worldOrigin.x, worldOrigin.y, worldOrigin.z, worldDir.x, worldDir.y, worldDir.z, docks.size(), nearActor, closest);
    }
}

void safeAfterFire(void* cs, unsigned handle) {
    __try { afterFire(cs, handle); } __except (EXCEPTION_EXECUTE_HANDLER) { failPosters("access fault reading a bullet"); }
}

U fire(void* cs, unsigned handle, void* info, float speed, void* muzzle, void* dir, int fromNetwork, int flag) {
    U r = fireOriginal(cs, handle, info, speed, muzzle, dir, fromNetwork, flag);
    if (!fault && !fromNetwork && !docks.empty()) {
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);  // never block an engine thread
        if (!lock.owns_lock()) return r;
        safeAfterFire(cs, handle);
    }
    return r;
}

void safeAfterButton(void* cs, U now) {
    __try { afterButton(cs, now); } __except (EXCEPTION_EXECUTE_HANDLER) { failPosters("access fault reading fingertips"); }
}

void buildGeometry() {
    meshes.clear();
    table.clear();
    for (auto& m : POSTER_MESHES) {
        Mesh mesh;
        Vec sum;
        for (unsigned i = 0; i < m.count; i++) {
            const PosterTri& p = m.tris[i];
            Tri t;
            t.a = {p.p[0][0], p.p[0][1], p.p[0][2]};
            t.b = {p.p[1][0], p.p[1][1], p.p[1][2]};
            t.c = {p.p[2][0], p.p[2][1], p.p[2][2]};
            Vec n = cross(t.b - t.a, t.c - t.a);
            t.n = n * (1.f / std::max(length(n), 1e-9f));
            memcpy(t.uv, p.uv, sizeof(t.uv));
            sum = sum + (t.a + t.b + t.c) * (1.f / 3);
            mesh.tris.push_back(t);
        }
        mesh.center = sum * (1.f / float(std::max(1u, m.count)));
        meshes.push_back(std::move(mesh));
    }
    for (unsigned i = 0; i < sizeof(POSTERS) / sizeof(POSTERS[0]); i++) table[POSTERS[i].actor] = i;
}

bool hook(P exe, unsigned rva, void* detour, void** original) {
    MH_STATUS s = MH_CreateHook(exe + rva, detour, original);
    if (s == MH_OK) s = MH_EnableHook(exe + rva);
    if (s != MH_OK) { logf("posters: hook at %x failed: %s", rva, MH_StatusToString(s)); return false; }
    return true;
}

}  // namespace

bool install(unsigned char* exe, arcade::Shared* s) {
    shared = s;
    if (!shared) return false;
    for (auto& sig : SIGNATURES)
        if (memcmp(exe + sig.rva, sig.bytes, 16) != 0) { logf("posters: code signature mismatch at %x; dock disabled", sig.rva); return false; }
    buildGeometry();
    overrideTexture = reinterpret_cast<Override>(exe + RVA_OVERRIDE);
    fallbackArt = reinterpret_cast<Fallback>(exe + RVA_FALLBACK);
    handled = shared->dockSerial;  // ignore requests from before this session
    shared->dockDone = handled;
    shared->dockState = arcade::Undocked;
    bool ok = hook(exe, RVA_START, reinterpret_cast<void*>(start), reinterpret_cast<void**>(&startOriginal)) &&
              hook(exe, RVA_DESTROY, reinterpret_cast<void*>(destroy), reinterpret_cast<void**>(&destroyOriginal)) &&
              hook(exe, RVA_FIRE, reinterpret_cast<void*>(fire), reinterpret_cast<void**>(&fireOriginal));
    if (!ok) { fault = true; return false; }
    logf("posters: dock ready: %zu posters, %zu face meshes", table.size(), meshes.size());
    return true;
}

void afterButtonUpdate(void* cs) {
    if (fault || !shared) return;
    U now = GetTickCount64();
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);  // never block an engine thread
    if (!lock.owns_lock()) return;
    safeAfterButton(cs, now);
    if (!fault) safeTick(now);
}

void heartbeat(unsigned long long now) {
    if (!shared) return;
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    LONG serial = shared->dockSerial;
    if (serial == handled) return;
    if (fault) { handled = serial; answer(arcade::DockFailed, "Dock is off for this session (see runtime.log)."); return; }
    // No poster system, or no touch-button Update to drive it: answer instead of leaving the
    // request hanging.
    static U pendingSince = 0;
    static LONG pendingSerial = 0;
    if (pendingSerial != serial) { pendingSerial = serial; pendingSince = now; }
    if (now - pendingSince < 1500 || (!systems.empty() && lastTick && now - lastTick < 1000)) return;
    handled = serial;
    if (!shared->dockWant) answer(arcade::Undocked, "Not docked.");
    else if (systems.empty()) answer(arcade::DockFailed, "No lobby poster here. Dock works in the social and combat lobbies.");
    else if (!docks.empty()) answer(arcade::Docked, "Docked on %zu poster(s).", docks.size());
    else answer(arcade::DockFailed, "Could not find your hands - touch the tablet.");
}

}  // namespace posters
