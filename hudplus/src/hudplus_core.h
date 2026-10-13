// Shared by EchoHudPlus.dll and its tests: settings, the boost / thruster / speed /
// stun logic, the overlay image (gauges, speed digits, stun vignette), the Oculus quad
// layer bytes and the D3D12 texture uploads.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace hudplus {

struct Settings {
    // Where Echo's readout sits, head-relative (see hud_lines/make_hud_lines_patch.py).
    float hudDistance = 0.319f;   // metres in front of the eyes
    float hudHeight = -0.10f;     // metres above eye level: where the cluster hangs (bottom of the view)
    float pxPerMetre = 512.f;     // readout canvas pixels per metre
    // Positions below are canvas px from the speed's centre: x out to each side, y down.
    // boost gauges (bottom of the view; off by default, the thruster gauges took their place)
    bool boost = false;
    float gaugePx = 18.f;         // gauge size, px
    float boostX = 52.f;
    float boostY = 128.f;
    bool swapSides = false;       // default: left gauge = the charge that comes back first
    // wrist thruster gauges (bottom of the view), with the mute icon in the left one
    // and the connection bars in the right one
    bool thrusters = true;
    float thrusterPx = 38.f;
    float thrusterX = 60.f;       // either side of the speeds
    float thrusterY = 43.f;       // centred on the two speeds
    bool swapHands = false;       // default: left gauge = Echo's hand 0
    bool icons = true;            // draw the mute / connection icons in the rings
    bool rings = true;            // draw the thruster gauge rings around them (off: just the icons)
    float pingBars[3] = {50, 100, 150};  // ms: below these, 4 / 3 / 2 bars (else 1)
    bool edgeGlow = true;         // while your hands push lined up, the edges of the view glow in the HUD colour (like shielding)
    float edgeGlowStrength = 0.f;   // opacity just above edgeGlowFrom (0 = fades in from nothing); none at or below it
    float edgeGlowPeak = 0.55f;     // opacity at the edges, lined up, from edgeGlowTo up (steady, low enough to see through)
    float edgeGlowFrom = 3.5f, edgeGlowTo = 4.8f;  // m/s: builds over this window, stays at the peak above it
    // Thruster score (cos of half the angle between the hands: 0.9 is about 50 degrees, 0.985 about 20):
    // the glow starts at edgeGlowAlignFrom and is full from edgeGlowAlignFull.
    float edgeGlowAlignFrom = 0.9f, edgeGlowAlignFull = 0.985f;
    // colours (sRGB): Echo's team colours, and white when you're not on a team
    uint8_t blue[3] = {34, 160, 197}, orange[3] = {255, 132, 1}, neutral[3] = {235, 235, 235};
    // speeds: your current speed on top and the last-run speed under it, one box between
    // the rings (Echo's own speed text is hidden by hud_lines)
    bool speed = true;
    float speedThreshold = 10.f;  // m/s
    float trackerPx = 34.f;       // box height, px (twice as wide)
    float fontPx = 13.5f;         // last-speed digit size, px
    float currentFontPx = 16.f;   // current-speed digit size, px (slightly bigger)
    bool peakBadge = true;        // your top speed over the last peakSeconds, in a pill beside the current speed
    float peakSeconds = 7.f;
    // speed rings, side by side, numbered: each fills from the previous ring's speed to
    // its own and lights up (fills in) once you reach it
    bool glow = true;
    static constexpr int kRings = 5;
    float glowAt[kRings] = {1.f, 2.f, 3.f, 4.f, 5.f};
    float glowPx = 14.f;
    float glowGap = 3.f;
    float glowY = 13.f;           // just above the two speeds
    float trackerDown = 43.f;     // px below the speed centre (negative: above it)
    float burstGap = 0.3f;        // s under the threshold before a run counts as over
    // stun flash
    bool stun = true;
    float shieldGrace = 0.2f;     // s: a shield dropped this recently still counts as up
    float flashStrength = 0.85f;  // peak opacity of the flash, 0..1
    float maxFps = 60.f;          // the draw thread paints at most this often (the speed changes every frame)
    // pass counter at the top of the view (from Echo's API): +1 for each pass inside a team,
    // the count itself from passCountFrom passes, gone when the other team gets the disc;
    // a goal shows the chain 50% bigger, flashing
    bool passes = true;
    bool passesMine = false;      // only your team's chains (default: either team's, in its colour)
    int passCountFrom = 6;
    float passPx = 26.f;          // box height, canvas px (three times as wide)
    float passY = 0.075f;         // metres above eye level, at the HUD's distance
    float passMaxSeconds = 5.f;   // a pass must reach the teammate within this (loose time)
    // Discord (from the arcade host's block): the voice call's people above the speeds, the
    // newest message's sender with +N at the top
    bool discord = true;
    float discordPx = 13.5f;       // the voice row's height, readout px (avatars a little smaller; half of the first cut, by request)
    float discordUpPx = 6.f;      // gap between the row and the speeds box, px
    float badgeScale = 1.5f;      // the message badge: the pass counter's spot, this much bigger
    float badgeSeconds = 3.f;     // how long it stays after a message (the pass counter waits)
    int passMaxBounces = 1;       // and bounce at most this often on the way (1: a bounce pass counts)
};

inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// Echo's network-game team index: 0 blue, 1 orange, 2 spectators; anything else is none.
enum Team { kTeamBlue = 0, kTeamOrange = 1, kTeamNone = -1 };

// Can others hear you right now? Not when muted; otherwise by Echo's voice mode: open
// mic always, push to talk while the button is held, push to mute while it isn't.
inline bool talking(bool muted, int mode, bool pressed) {
    if (muted) return false;
    return mode == 1 || (mode == 2 && pressed) || (mode == 3 && !pressed);
}

// Connection bars (1..4) from ping in ms; 0 when there's no ping to show.
inline int pingBars(int pingMs, const float limits[3]) {
    if (pingMs <= 0 || pingMs >= 999) return 0;
    return pingMs < limits[0] ? 4 : pingMs < limits[1] ? 3 : pingMs < limits[2] ? 2 : 1;
}

// ---- boost charges -------------------------------------------------------------
// Echo's arena boost spends power_cost (49.9) from a battery of max_power (100), so two
// boosts fit. After a boost the battery waits recharge_delay (0.25 s), then refills at
// recharge_rate (33.3/s). Charge k (0-based) is ready once energy >= (k+1) * cost.
struct Battery {
    float energy = 0, maxPower = 0, cost = 0;
};

inline int chargeCount(const Battery& b) {
    if (!(b.cost > 0.01f) || !(b.maxPower >= b.cost)) return 0;
    int n = int(std::floor(b.maxPower / b.cost + 1e-3f));
    return n > 4 ? 4 : n;
}

inline float chargeFill(const Battery& b, int k) {
    if (!(b.cost > 0.01f)) return 1;
    return clamp01((b.energy - k * b.cost) / b.cost + 1e-4f);
}

// ---- wrist thrusters -----------------------------------------------------------------
// Echo's "iron man" hand thrusters (mp_arena_movement: single_hand_max_vel 1,
// two_hands_max_vel 2, accel 1.5). Each thrusting hand pushes toward a target velocity
// along its own direction: max_vel x direction, using two_hands_max_vel when both hands
// thrust. The pushes add up and the player accelerates toward that target speed. So
// the most speed in one direction (4 m/s in the arena) comes from both hands thrusting
// along the same line; one hand alone tops out at 1 m/s.
struct Thrusters {
    bool on[2] = {false, false};
    float dir[2][3] = {};
    float single[2] = {1, 1}, two[2] = {2, 2};  // per hand: max_vel x the hand's factor
};

// Per hand: the share of that hand's best push that goes into the combined direction
// (1 = both hands on and lined up, 0.5 = alone in the arena, 0 = off).
inline void thrusterScores(const Thrusters& t, float out[2]) {
    out[0] = out[1] = 0;
    bool both = t.on[0] && t.on[1];
    float sum[3] = {0, 0, 0}, contrib[2][3] = {};
    for (int h = 0; h < 2; h++) {
        if (!t.on[h]) continue;
        float len = std::sqrt(t.dir[h][0] * t.dir[h][0] + t.dir[h][1] * t.dir[h][1] + t.dir[h][2] * t.dir[h][2]);
        if (!(len > 1e-4f)) continue;
        float m = both ? t.two[h] : t.single[h];
        for (int i = 0; i < 3; i++) sum[i] += contrib[h][i] = t.dir[h][i] / len * m;
    }
    float slen = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
    if (!(slen > 1e-4f)) return;
    for (int h = 0; h < 2; h++) {
        if (!t.on[h] || !(t.two[h] > 0)) continue;
        float along = (contrib[h][0] * sum[0] + contrib[h][1] * sum[1] + contrib[h][2] * sum[2]) / slen;
        out[h] = clamp01(along / t.two[h]);
    }
}

// How well the two hands line up for the screen glow, 0..1: 0 unless both push, then from
// edgeGlowAlignFrom to edgeGlowAlignFull of the thruster score (smoothly in between).
inline float alignment(const Settings& s, const Thrusters& th, const float score[2]) {
    if (!th.on[0] || !th.on[1] || !(s.edgeGlowAlignFull > s.edgeGlowAlignFrom)) return 0;
    float t = clamp01((std::min(score[0], score[1]) - s.edgeGlowAlignFrom) / (s.edgeGlowAlignFull - s.edgeGlowAlignFrom));
    return t * t * (3 - 2 * t);
}

// ---- speed tracker -----------------------------------------------------------------
// Holds the top speed of the last run at or above the threshold. A new run (after the
// speed stayed under the threshold for burstGap seconds) replaces it.
struct SpeedTracker {
    float shown = NAN;
    bool live = false;
    double belowSince = -1;

    void update(float speed, double now, float threshold, float gap) {
        if (speed >= threshold) {
            if (!live) {
                live = true;
                shown = speed;
            } else if (speed > shown) {
                shown = speed;
            }
            belowSince = -1;
        } else if (live) {
            if (belowSince < 0) belowSince = now;
            else if (now - belowSince >= gap) live = false;
        }
    }
};

// ---- rolling top speed ----------------------------------------------------------------
// Highest speed over the last few seconds, in 0.1 s buckets: holds your top speed for
// `seconds`, then falls to the next highest still inside the window.
struct RollingMax {
    static constexpr int kBuckets = 160;  // up to 16 s
    static constexpr long long kEmpty = -(1ll << 62);
    float v[kBuckets] = {};
    long long id[kBuckets];
    RollingMax() { std::fill(id, id + kBuckets, kEmpty); }
    void add(float speed, double t) {
        long long b = (long long)std::floor(t * 10);
        int i = int(((b % kBuckets) + kBuckets) % kBuckets);
        if (id[i] != b) {
            id[i] = b;
            v[i] = speed;
        } else if (speed > v[i]) {
            v[i] = speed;
        }
    }
    float max(double t, float seconds) const {  // -1 when nothing in the window
        long long now = (long long)std::floor(t * 10), oldest = now - (long long)std::lround(seconds * 10) + 1;
        float m = -1;
        for (int i = 0; i < kBuckets; i++)
            if (id[i] >= oldest && id[i] <= now && v[i] > m) m = v[i];
        return m;
    }
};

// ---- stun flash ---------------------------------------------------------------------
inline bool shieldCounts(bool upNow, double lastUp, double now, float grace) {
    return upNow || (lastUp >= 0 && now - lastUp <= grace);
}

struct Flash {
    double start = -1;
    float seconds = 0;  // stun length
    bool green = false;
};

// Opacity of the flash t seconds in: a quick rise, a bright hit, then a pulse for the
// rest of the stun, then a fade.
inline float flashAlpha(const Flash& f, double now, float strength) {
    if (f.start < 0) return 0;
    const double rise = 0.06, hit = 0.25, fade = 0.45;
    double t = now - f.start, hold = f.seconds > hit ? f.seconds : hit;
    auto pulse = [&](double at) { return strength * (0.55 + 0.2 * std::cos(6.2831853 * 2.0 * (at - hit))); };
    if (t < 0) return 0;
    if (t < rise) return float(strength * t / rise);
    if (t < hit) return float(strength + (pulse(hit) - strength) * (t - rise) / (hit - rise));
    if (t < hold) return float(pulse(t));
    if (t < hold + fade) return float(pulse(hold) * (1 - (t - hold) / fade));
    return 0;
}

// Screen-edge glow level: follows its target, in quickly and out a little slower, so hand
// jitter doesn't make it flicker.
constexpr float kEdgeGlowIn = 0.12f, kEdgeGlowOut = 0.3f;  // s, from off to full and back
inline float edgeGlowStep(float level, float target, float dt) {
    if (!(dt > 0)) return level;
    return target > level ? std::min(target, level + dt / kEdgeGlowIn) : std::max(target, level - dt / kEdgeGlowOut);
}

// How far into the speed window you are: 0 at edgeGlowFrom (and below), 1 at edgeGlowTo.
inline float speedPotential(const Settings& s, float speed) {
    return s.edgeGlowTo > s.edgeGlowFrom ? clamp01((speed - s.edgeGlowFrom) / (s.edgeGlowTo - s.edgeGlowFrom)) : 0.f;
}

// Edge opacity while lined up, from your speed: none at or below edgeGlowFrom, then steady
// (no pulsing), rising from edgeGlowStrength to edgeGlowPeak at edgeGlowTo and staying there.
inline float edgeGlowAlpha(const Settings& s, float speed) {
    if (!(speed > s.edgeGlowFrom)) return 0;
    return s.edgeGlowStrength + (s.edgeGlowPeak - s.edgeGlowStrength) * speedPotential(s, speed);
}

// ---- overlay image ------------------------------------------------------------------
// One 1024x384 RGBA texture (sRGB, premultiplied alpha) holds every element; each quad
// layer shows its own viewport of it.
constexpr int kTexW = 1024, kTexH = 384;
struct Rect { int x, y, w, h; };
constexpr Rect kGaugeRect[2] = {{0, 0, 128, 128}, {128, 0, 128, 128}};
constexpr Rect kTextRect = {456, 0, 556, 232};  // the speeds box, at high resolution (~2.4:1)
constexpr Rect kFlashRect = {4, 132, 120, 120};
constexpr Rect kThrustRect[2] = {{136, 128, 128, 128}, {264, 128, 128, 128}};
constexpr Rect kGlowRect[5] = {{260, 2, 60, 60}, {326, 2, 60, 60}, {392, 2, 60, 60}, {260, 66, 60, 60}, {326, 66, 60, 60}};
constexpr Rect kPassRect = {0, 258, 378, 126};  // the pass counter (3:1)
constexpr Rect kVoiceRect = {384, 258, 512, 64};  // Discord: 8 avatars of 64 (a 56 px disc and its ring)
constexpr Rect kBadgeRect = {900, 258, 124, 64};  // Discord: the newest message's sender and +N

struct Color { float r, g, b; };  // linear 0..1
constexpr Color kReady = {0.1f, 1.f, 0.25f}, kCharging = {1.f, 0.08f, 0.05f}, kGood = {1.f, 0.8f, 0.08f}, kTrack = {1.f, 1.f, 1.f};
constexpr Color kText = {1.f, 1.f, 1.f}, kTextLive = {1.f, 0.78f, 0.35f};
constexpr Color kGreen = {0.1f, 1.f, 0.25f}, kRed = {1.f, 0.06f, 0.04f};

inline float fromSrgb(uint8_t v) {
    float s = v / 255.f;
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}
inline Color linearColor(const uint8_t rgb[3]) { return {fromSrgb(rgb[0]), fromSrgb(rgb[1]), fromSrgb(rgb[2])}; }
inline Color teamColor(const Settings& s, int team) {
    return linearColor(team == kTeamBlue ? s.blue : team == kTeamOrange ? s.orange : s.neutral);
}

inline uint8_t toSrgb(float linear) {
    linear = clamp01(linear);
    float s = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1 / 2.4f) - 0.055f;
    return uint8_t(std::lround(s * 255));
}

struct Canvas {
    int w = kTexW, h = kTexH;
    std::vector<float> px = std::vector<float>(size_t(kTexW) * kTexH * 4, 0.f);  // linear premultiplied
    mutable int dx0 = 0, dy0 = 0, dx1 = kTexW, dy1 = kTexH;  // changed since the last encode (all, at first)
    void touch(int x0, int y0, int x1, int y1) {
        dx0 = std::min(dx0, x0), dy0 = std::min(dy0, y0), dx1 = std::max(dx1, x1), dy1 = std::max(dy1, y1);
    }
    void clear(const Rect& r) {
        touch(r.x, r.y, r.x + r.w, r.y + r.h);
        for (int y = r.y; y < r.y + r.h; y++) std::fill(&px[(size_t(y) * w + r.x) * 4], &px[(size_t(y) * w + r.x + r.w) * 4], 0.f);
    }
    void over(int x, int y, Color c, float a) {  // premultiplied "over"
        if (a <= 0) return;
        if (x < dx0 || x >= dx1 || y < dy0 || y >= dy1) touch(x, y, x + 1, y + 1);
        float* p = &px[(size_t(y) * w + x) * 4];
        float k = 1 - a;
        p[0] = c.r * a + p[0] * k;
        p[1] = c.g * a + p[1] * k;
        p[2] = c.b * a + p[2] * k;
        p[3] = a + p[3] * k;
    }
    // RGBA8 bytes (sRGB-encoded premultiplied colour, linear alpha) for upload. `out` keeps
    // the last encode: only the part drawn since then is encoded again.
    void encode(std::vector<uint8_t>& out) const {
        static const std::vector<uint8_t> lut = [] {  // linear in 1/4095 steps -> sRGB byte
            std::vector<uint8_t> t(4096);
            for (int i = 0; i < 4096; i++) t[i] = toSrgb(i / 4095.f);
            return t;
        }();
        if (out.size() != size_t(w) * h * 4) {
            out.assign(size_t(w) * h * 4, 0);
            dx0 = dy0 = 0, dx1 = w, dy1 = h;
        }
        for (int y = std::max(dy0, 0); y < std::min(dy1, h); y++)
            for (int x = std::max(dx0, 0); x < std::min(dx1, w); x++) {
                size_t i = size_t(y) * w + x;
                const float* p = &px[i * 4];
                uint8_t a = uint8_t(clamp01(p[3]) * 255 + .5f);
                for (int ch = 0; ch < 3; ch++) out[i * 4 + ch] = a ? lut[size_t(clamp01(p[ch]) * 4095 + .5f)] : 0;  // no light where it's clear
                out[i * 4 + 3] = a;
            }
        dx0 = w, dy0 = h, dx1 = 0, dy1 = 0;
    }
};

inline float smooth(float e0, float e1, float x) {
    float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}

// A ring gauge in a 128x128 cell: a dim track with a red arc growing clockwise from
// 12 o'clock while the charge comes back; when ready, a full green ring with a filled
// centre, plus an outer glow for `pulse` (1 just ready .. 0).
inline void drawGauge(Canvas& cv, const Rect& r, float fill, float pulse) {
    cv.clear(r);
    const float cx = r.w / 2.f, cy = r.h / 2.f, ringR = 38, half = 6;
    const bool ready = fill >= 1;
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++) {
            float dx = x + .5f - cx, dy = y + .5f - cy, d = std::sqrt(dx * dx + dy * dy);
            float ring = clamp01(half - std::fabs(d - ringR) + .5f);
            int X = r.x + x, Y = r.y + y;
            if (ready) {
                if (pulse > 0) cv.over(X, Y, kReady, 0.55f * pulse * clamp01(1 - std::fabs(d - (ringR + 13)) / 7));
                cv.over(X, Y, kReady, 0.95f * ring);
                cv.over(X, Y, kReady, 0.85f * clamp01(22 - d + .5f));
            } else {
                cv.over(X, Y, kTrack, 0.22f * ring);
                if (ring > 0 && fill > 0) {
                    float a = std::atan2(dx, -dy) / 6.2831853f;  // 0 at 12 o'clock, clockwise
                    if (a < 0) a += 1;
                    float along = (fill - a) * 6.2831853f * d;  // px of arc left past this pixel
                    cv.over(X, Y, kCharging, 0.95f * ring * clamp01(along + .5f));
                }
            }
        }
}

// Thruster gauge, in the HUD colour: the arc fills clockwise from 12 o'clock with the
// hand's score; at the best push the whole circle fills in. A faint ring when the hand
// isn't thrusting. (thrusterColor is the older green / yellow / red scheme.)
constexpr float kThrustBest = 0.95f, kThrustGood = 0.75f;
inline Color thrusterColor(float score) { return score >= kThrustBest ? kReady : score >= kThrustGood ? kGood : kCharging; }

// A filled disc in three darker shades of the HUD colour rather than flat colour: a deep
// outer band, a medium middle band and a lighter centre (all darker than the bright
// ring around it), with a faint diamond weave and a dark line at the very edge.
// `unit` is the cell's px per 128 (so the bands and pattern keep their size in any cell).
constexpr float kShadeOuter = 0.3f, kShadeMiddle = 0.5f, kShadeInner = 0.75f;  // of the HUD colour (linear)
inline Color fillInk(Color c) { return {c.r + (1 - c.r) * 0.6f, c.g + (1 - c.g) * 0.6f, c.b + (1 - c.b) * 0.6f}; }  // light tint on fills

inline void litDisc(Canvas& cv, int X, int Y, float dx, float dy, float radius, float unit, Color c, float alpha) {
    float d = std::sqrt(dx * dx + dy * dy);
    float cov = clamp01(radius - d + .5f);
    if (cov <= 0) return;
    // band blend: 1 px soft steps between the three shades
    float toMiddle = clamp01(radius * 0.72f - d + .5f), toInner = clamp01(radius * 0.42f - d + .5f);
    float shade = kShadeOuter + (kShadeMiddle - kShadeOuter) * toMiddle + (kShadeInner - kShadeMiddle) * toInner;
    const float period = 9 * unit;
    float u = (dx + dy) / period, v = (dx - dy) / period;
    shade *= 0.88f + 0.12f * (0.5f + 0.5f * std::cos(6.2831853f * u) * std::cos(6.2831853f * v));
    shade *= 1 - 0.4f * clamp01(1 - (radius - d) / (2 * unit));  // dark edge line
    cv.over(X, Y, {c.r * shade, c.g * shade, c.b * shade}, alpha * cov);
}

inline void drawMeter(Canvas& cv, const Rect& r, float score, bool active, Color c) {
    cv.clear(r);
    const float unit = r.w / 128.f, cx = r.w / 2.f, cy = r.h / 2.f, ringR = 52 * unit, half = 5 * unit;
    const bool full = active && score >= kThrustBest;
    if (full) score = 1;
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++) {
            float dx = x + .5f - cx, dy = y + .5f - cy, d = std::sqrt(dx * dx + dy * dy);
            float ring = clamp01(half - std::fabs(d - ringR) + .5f);
            int X = r.x + x, Y = r.y + y;
            if (full) {
                cv.over(X, Y, c, 0.35f * clamp01(1 - (d - ringR - half) / (6 * unit)) * (d > ringR ? 1.f : 0.f));  // glow outside
                litDisc(cv, X, Y, dx, dy, ringR - half - 1.5f * unit, unit, c, 0.92f);                              // textured fill
            }
            if (ring <= 0) continue;
            cv.over(X, Y, c, (active ? 0.25f : 0.15f) * ring);
            if (active && score > 0) {
                float a = std::atan2(dx, -dy) / 6.2831853f;
                if (a < 0) a += 1;
                cv.over(X, Y, c, 0.95f * ring * clamp01((score - a) * 6.2831853f * d + .5f));
            }
        }
}

// Icons drawn inside a 128x128 ring cell, centred, about 44 px across.
inline float strokeCov(float px, float py, float ax, float ay, float bx, float by, float width) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float len2 = vx * vx + vy * vy;
    float t = len2 > 0 ? clamp01((wx * vx + wy * vy) / len2) : 0;
    float dx = wx - t * vx, dy = wy - t * vy;
    return clamp01(width / 2 - std::sqrt(dx * dx + dy * dy) + .5f);
}

// Microphone: with sound waves either side while others can hear you, slashed (with a
// clear gap around the slash) while they can't.
inline void drawMic(Canvas& cv, const Rect& r, bool talking, Color c) {
    const bool muted = !talking;
    const float cx = r.w / 2.f, cy = r.h / 2.f;
    for (int y = int(cy) - 34; y < int(cy) + 34; y++)
        for (int x = int(cx) - 40; x < int(cx) + 40; x++) {
            float px = x + .5f, py = y + .5f;
            float cov = strokeCov(px, py, cx, cy - 16, cx, cy - 2, 14);  // capsule body
            float dx = px - cx, dy = py - (cy - 4), d = std::sqrt(dx * dx + dy * dy);
            if (dy > 0) cov = std::max(cov, clamp01(1.5f - std::fabs(d - 13) + .5f));    // cradle
            cov = std::max(cov, strokeCov(px, py, cx, cy + 9, cx, cy + 16, 3));          // stem
            cov = std::max(cov, strokeCov(px, py, cx - 8, cy + 17, cx + 8, cy + 17, 3)); // base
            if (muted) {
                float gap = strokeCov(px, py, cx - 18, cy - 20, cx + 18, cy + 20, 9);
                cov *= 1 - gap;
                cov = std::max(cov, strokeCov(px, py, cx - 18, cy - 20, cx + 18, cy + 20, 4));
            } else {
                float wx = std::fabs(px - cx), wy = py - (cy - 9), wd = std::sqrt(wx * wx + wy * wy);
                if (wx > 16 && std::fabs(wy) < wx * 0.75f)  // two arcs each side, beside the mic head
                    cov = std::max(cov, std::max(clamp01(1.5f - std::fabs(wd - 22) + .5f), clamp01(1.5f - std::fabs(wd - 30) + .5f)));
            }
            if (cov > 0) cv.over(r.x + x, r.y + y, c, 0.95f * cov);
        }
}

// Four rising bars; the lit ones (from the left) show connection quality, and a cross
// over unlit bars means no ping to show.
inline void drawBars(Canvas& cv, const Rect& r, int lit, Color c) {
    const float cx = r.w / 2.f, cy = r.h / 2.f, bw = 6, gap = 4, bottom = cy + 15;
    const float left = cx - (4 * bw + 3 * gap) / 2;
    for (int i = 0; i < 4; i++) {
        float x0 = left + i * (bw + gap), x1 = x0 + bw, top = bottom - 9 - i * 7;
        float a = i < lit ? 0.95f : 0.25f;
        for (int y = int(top) - 1; y <= int(bottom) + 1; y++)
            for (int x = int(x0) - 1; x <= int(x1) + 1; x++) {
                float px = x + .5f, py = y + .5f;
                float cov = clamp01(std::min({px - x0, x1 - px, py - top, bottom - py}) + .5f);
                if (cov > 0) cv.over(r.x + x, r.y + y, c, a * cov);
            }
    }
    if (lit == 0) {
        for (int y = int(cy) - 14; y < int(cy) + 14; y++)
            for (int x = int(cx) - 14; x < int(cx) + 14; x++) {
                float px = x + .5f, py = y + .5f;
                float cov = std::max(strokeCov(px, py, cx - 9, cy - 9, cx + 9, cy + 9, 3), strokeCov(px, py, cx - 9, cy + 9, cx + 9, cy - 9, 3));
                if (cov > 0) cv.over(r.x + x, r.y + y, c, 0.95f * cov);
            }
    }
}

// One thruster cell: the ring, plus the mute icon (left) or connection bars (right).
// When the ring is full the icon is drawn in a light tint on the filled circle.
inline void drawThrusterCell(Canvas& cv, const Rect& r, int side, float score, bool active, Color hud, bool icons, bool talking, int bars,
                             bool ring = true) {
    if (ring) drawMeter(cv, r, score, active, hud);
    else cv.clear(r);
    if (!icons) return;
    Color ink = ring && active && score >= kThrustBest ? fillInk(hud) : hud;
    if (side == 0) drawMic(cv, r, talking, ink);
    else drawBars(cv, r, bars, ink);
}

// Ring label text, e.g. "3" or "3.5".
inline void ringLabel(float v, char* out, size_t n) {
    if (std::fabs(v - std::round(v)) < 0.01f) snprintf(out, n, "%d", int(std::round(v)));
    else snprintf(out, n, "%.1f", v);
}

// Forward declarations: the digit font and its mask are defined below.
struct DigitFont;
inline std::vector<float> msdfTextMask(const DigitFont& f, const char* s, int w, int h, float size, float cy);
inline std::vector<float> textMask(const char* s, int w, int h);
inline bool fontOk(const DigitFont* f);
inline float fontCap(const DigitFont& f, float size);

// Label (digits) centred in a cell, `capPx` tall, in colour c.
inline void drawLabel(Canvas& cv, const Rect& r, const char* s, float capPx, Color c, float alpha, const DigitFont* font) {
    std::vector<float> m;
    if (fontOk(font)) {
        float size = capPx / fontCap(*font, 1.f);
        m = msdfTextMask(*font, s, r.w, r.h, size, r.h / 2.f);
    } else {
        int stripH = int(capPx / 0.62f + .5f);
        std::vector<float> strip = textMask(s, r.w, stripH);
        m.assign(size_t(r.w) * r.h, 0.f);
        int y0 = int(r.h / 2.f - stripH / 2.f + .5f);
        for (int y = 0; y < stripH; y++)
            if (y0 + y >= 0 && y0 + y < r.h)
                std::copy(strip.begin() + size_t(y) * r.w, strip.begin() + size_t(y + 1) * r.w, m.begin() + size_t(y0 + y) * r.w);
    }
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++)
            if (float cov = m[size_t(y) * r.w + x]; cov > 0) cv.over(r.x + x, r.y + y, c, alpha * cov);
}

// A speed ring with its speed written inside (light once the ring is filled in).
inline void drawRingCell(Canvas& cv, const Rect& r, float score, Color c, float speed, const DigitFont* font) {
    drawMeter(cv, r, score, true, c);
    char label[8];
    ringLabel(speed, label, sizeof label);
    float cap = r.h * (std::strlen(label) > 1 ? 0.24f : 0.34f);
    drawLabel(cv, r, label, cap, score >= kThrustBest ? fillInk(c) : c, 0.95f, font);
}

// Echo's speed digits: multi-channel signed distance field glyphs taken from the game's
// own font by make_digits.py (data/echo_digits.bin).
struct DigitFont {
    struct Glyph {
        int w = 0, h = 0, xo = 0, yo = 0;
        float adv = 0;
        std::vector<uint8_t> rgb;
    };
    bool ok = false;
    float pxRange = 4, em = 32;
    unsigned id = 0;  // which font a cached glyph mask belongs to
    Glyph g[128];
};

inline bool fontOk(const DigitFont* f) { return f && f->ok; }
inline float fontCap(const DigitFont& f, float size) { return size * 25 / f.em; }  // digits are 25 units tall

inline bool parseDigitFont(const uint8_t* d, size_t n, DigitFont& f) {
    f = DigitFont{};
    auto take = [&](size_t& at, void* out, size_t k) {
        if (at + k > n) return false;
        std::memcpy(out, d + at, k);
        at += k;
        return true;
    };
    size_t at = 0;
    char magic[4];
    uint32_t count;
    if (!take(at, magic, 4) || std::memcmp(magic, "EDG1", 4) || !take(at, &count, 4) || !take(at, &f.pxRange, 4) || !take(at, &f.em, 4))
        return false;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t ch;
        uint16_t w, h;
        int16_t xo, yo;
        float adv;
        if (!take(at, &ch, 4) || !take(at, &w, 2) || !take(at, &h, 2) || !take(at, &xo, 2) || !take(at, &yo, 2) || !take(at, &adv, 4))
            return false;
        if (ch >= 128 || w < 2 || h < 2 || w > 64 || h > 64) return false;
        DigitFont::Glyph& g = f.g[ch];
        g.w = w;
        g.h = h;
        g.xo = xo;
        g.yo = yo;
        g.adv = adv;
        g.rgb.resize(size_t(w) * h * 3);
        if (!take(at, g.rgb.data(), g.rgb.size())) return false;
    }
    f.ok = count > 0 && f.em > 0 && f.pxRange > 0;
    static std::atomic<unsigned> fonts{0};
    f.id = ++fonts;
    return f.ok;
}

// One glyph rasterized at one size, kept for reuse: the speed changes every frame, and
// rasterizing its distance-field glyphs each time was the HUD's biggest cost. Glyphs go
// at whole-pixel positions (invisible at these sizes). The mask starts 1 px before the
// glyph's left and top.
struct GlyphMask {
    int w = 0, h = 0;
    std::vector<float> cov;
};

inline const GlyphMask& glyphMask(const DigitFont& f, unsigned char ch, float size) {
    static std::mutex lock;
    static std::map<unsigned long long, GlyphMask> cache;
    unsigned long long key = (unsigned long long(f.id) << 40) | (unsigned long long(ch) << 32) | (unsigned long long)(size * 64 + .5f);
    std::lock_guard<std::mutex> guard(lock);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    GlyphMask& m = cache[key];
    const DigitFont::Glyph& g = f.g[ch];
    const float k = size / f.em;
    if (g.w < 2 || g.h < 2 || !(k > 0)) return m;
    m.w = int(g.w * k) + 3;
    m.h = int(g.h * k) + 3;
    m.cov.assign(size_t(m.w) * m.h, 0.f);
    const float left = 1, top = 1;
    // Outside the glyph's box counts as far outside (0), so its edge texels don't get
    // stretched into little ticks beside the digit.
    auto texel = [&](int xx, int yy, int c) {
        return xx < 0 || yy < 0 || xx >= g.w || yy >= g.h ? 0.f : g.rgb[(size_t(yy) * g.w + xx) * 3 + c] / 255.f;
    };
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++) {
            float u = (x + .5f - left) / k - .5f, v = (y + .5f - top) / k - .5f;
            if (u < -1 || v < -1 || u > g.w || v > g.h) continue;
            int iu = int(std::floor(u)), iv = int(std::floor(v));
            // Interpolate each texel's median rather than the three channels: the corners
            // round off a little, but the font's channel clashes (notches and cuts that
            // show when it's drawn this large) go away.
            float fu = u - iu, fv = v - iv;
            auto med3 = [&](int xx, int yy) {
                float r0 = texel(xx, yy, 0), g0 = texel(xx, yy, 1), b0 = texel(xx, yy, 2);
                return std::max(std::min(r0, g0), std::min(std::max(r0, g0), b0));
            };
            float a = med3(iu, iv) + (med3(iu + 1, iv) - med3(iu, iv)) * fu;
            float b = med3(iu, iv + 1) + (med3(iu + 1, iv + 1) - med3(iu, iv + 1)) * fu;
            float med = a + (b - a) * fv;
            m.cov[size_t(y) * m.w + x] = clamp01((med - 0.5f) * f.pxRange * k + 0.5f);
        }
    return m;
}

// Coverage mask for digits and dots in Echo's font, `size` px (em) high, centred.
inline std::vector<float> msdfTextMask(const DigitFont& f, const char* s, int w, int h, float size, float cy = -1) {
    std::vector<float> m(size_t(w) * h, 0.f);
    const float k = size / f.em, cap = 25 * k;  // digits are 25 units tall
    float total = 0;
    for (const char* c = s; *c; c++)
        if ((unsigned char)*c < 128) total += f.g[(unsigned char)*c].adv * k;
    float pen = (w - total) / 2, base = (cy < 0 ? h / 2.f : cy) + cap / 2;
    for (const char* c = s; *c; c++) {
        if ((unsigned char)*c >= 128) continue;
        const DigitFont::Glyph& g = f.g[(unsigned char)*c];
        if (g.w < 2 || g.h < 2) { pen += g.adv * k; continue; }
        const GlyphMask& gm = glyphMask(f, (unsigned char)*c, size);
        int x0 = int(std::lround(pen + g.xo * k)) - 1, y0 = int(std::lround(base - g.yo * k)) - 1;
        for (int y = 0; y < gm.h; y++) {
            int yy = y0 + y;
            if (yy < 0 || yy >= h) continue;
            for (int x = 0; x < gm.w; x++) {
                int xx = x0 + x;
                if (xx < 0 || xx >= w) continue;
                float cov = gm.cov[size_t(y) * gm.w + x];
                if (cov > 0) {
                    float& o = m[size_t(yy) * w + xx];
                    o = std::max(o, cov);
                }
            }
        }
        pen += g.adv * k;
    }
    return m;
}

// Seven-segment digits (and '.') drawn with round-capped strokes, slightly slanted.
// Segment order: a top, b top-right, c bottom-right, d bottom, e bottom-left, f top-left, g middle.
inline uint8_t segmentsOf(char ch) {
    static const uint8_t k[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    return ch >= '0' && ch <= '9' ? k[ch - '0'] : 0;
}

inline float segDistance(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = clamp01((wx * vx + wy * vy) / (vx * vx + vy * vy));
    float dx = wx - t * vx, dy = wy - t * vy;
    return std::sqrt(dx * dx + dy * dy);
}

// Coverage mask for a string of digits and dots, centred in a w x h cell.
inline std::vector<float> textMask(const char* s, int w, int h) {
    std::vector<float> m(size_t(w) * h, 0.f);
    const float H = h * 0.62f, W = H * 0.52f, T = H * 0.12f, gap = H * 0.2f, dotW = T * 1.6f, slant = 0.12f;
    float total = 0;
    for (const char* c = s; *c; c++) total += (*c == '.' ? dotW : W) + gap;
    total -= gap;
    float x0 = (w - total) / 2, top = (h - H) / 2, mid = top + H / 2, bot = top + H;
    for (const char* c = s; *c; c++) {
        float cw = *c == '.' ? dotW : W;
        float segs[8][4];
        int n = 0;
        float l = x0 + T / 2, rr = x0 + cw - T / 2;
        if (*c == '.') {
            segs[n][0] = segs[n][2] = x0 + cw / 2;
            segs[n][1] = segs[n][3] = bot - T / 2;
            n++;
        } else {
            uint8_t on = segmentsOf(*c);
            const float in = T * 0.9f;  // keep segments from merging at the corners
            float pts[7][4] = {{l + in, top + T / 2, rr - in, top + T / 2}, {rr, top + in, rr, mid - in},
                               {rr, mid + in, rr, bot - in},                {l + in, bot - T / 2, rr - in, bot - T / 2},
                               {l, mid + in, l, bot - in},                  {l, top + in, l, mid - in},
                               {l + in, mid, rr - in, mid}};
            for (int i = 0; i < 7; i++)
                if (on >> i & 1) std::memcpy(segs[n++], pts[i], sizeof pts[i]);
        }
        int xa = std::max(0, int(x0 - T - H * slant)), xb = std::min(w, int(x0 + cw + T + H * slant) + 1);
        for (int y = std::max(0, int(top - T)); y < std::min(h, int(bot + T) + 1); y++)
            for (int x = xa; x < xb; x++) {
                float py = y + .5f, px = x + .5f + (py - mid) * slant;  // undo the slant
                float best = 1e9f;
                for (int i = 0; i < n; i++) best = std::min(best, segDistance(px, py, segs[i][0], segs[i][1], segs[i][2], segs[i][3]));
                float cov = clamp01(T / 2 - best + .5f);
                float& o = m[size_t(y) * w + x];
                o = std::max(o, cov);
            }
        x0 += cw + gap;
    }
    return m;
}

// Text in Echo's font when it's available (size in texture px), else the built-in digits.
inline void drawText(Canvas& cv, const Rect& r, const char* s, Color c, float alpha, const DigitFont* font = nullptr, float size = 0) {
    cv.clear(r);
    if (!s || !*s) return;
    bool echo = font && font->ok && size > 0;
    std::vector<float> m = echo ? msdfTextMask(*font, s, r.w, r.h, size) : textMask(s, r.w, r.h);
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++) {
            float cov = m[size_t(y) * r.w + x];
            if (cov <= 0) continue;
            if (!echo) cv.over(r.x + x, r.y + y, {0, 0, 0}, 0.35f * clamp01(cov * 3));  // soft edge for the fallback digits
            cv.over(r.x + x, r.y + y, c, alpha * cov);
        }
}

// Width of a string in the digit font (or the built-in digits) at `size`, in px.
inline float textWidth(const DigitFont* font, const char* s, float size) {
    float w = 0;
    for (const char* ch = s; ch && *ch; ch++)
        w += fontOk(font) ? font->g[(unsigned char)*ch & 127].adv * size / font->em : size * 0.62f * (*ch == '.' ? 0.36f : 0.72f);
    return w;
}

// A rounded pill in the darker shades of the HUD colour (deep edge, lighter middle).
inline void drawPill(Canvas& cv, const Rect& r, float cx, float cy, float halfW, float rad, Color c) {
    for (int y = int(cy - rad - 2); y <= int(cy + rad + 2); y++)
        for (int x = int(cx - halfW - rad - 2); x <= int(cx + halfW + rad + 2); x++) {
            if (x < 0 || y < 0 || x >= r.w || y >= r.h) continue;
            float px = x + .5f, py = y + .5f;
            float qx = std::max(std::fabs(px - cx) - halfW, 0.f), qy = py - cy;
            float d = std::sqrt(qx * qx + qy * qy), inside = rad - d;
            float cov = clamp01(inside + .5f);
            if (cov <= 0) continue;
            float shade = kShadeOuter + (kShadeMiddle - kShadeOuter) * clamp01(inside - rad * 0.3f + .5f);
            shade *= 1 - 0.4f * clamp01(1 - inside / 1.5f);
            cv.over(r.x + x, r.y + y, {c.r * shade, c.g * shade, c.b * shade}, 0.95f * cov);
        }
}

// Current speed on top and the last speed under it, centred together in one box. The
// sizes are em sizes in texture px; the last speed is drawn a little fainter. `peak`
// (your recent top speed) goes in a filled pill right of the current speed.
inline void drawSpeeds(Canvas& cv, const Rect& r, const char* current, const char* last, Color c, const DigitFont* font, float currentSize,
                       float lastSize, const char* peak = nullptr) {
    cv.clear(r);
    const bool echo = font && font->ok;
    const float capC = currentSize * (echo ? 25 / font->em : 0.62f), capL = lastSize * (echo ? 25 / font->em : 0.62f);
    const float gap = 0.3f * capL, top = (r.h - (capC + gap + capL)) / 2;
    // The top-speed pill sits right of the current speed; the pair is centred together.
    const bool pill = peak && *peak && current && *current;
    const float pillSize = currentSize * 0.56f, pillCap = capC * 0.56f, pillRad = pillCap * 0.85f;
    const float pillTextW = pill ? textWidth(font, peak, pillSize) : 0, pillHalf = std::max(0.f, pillTextW / 2 - 0.3f * pillRad);
    const float curW = textWidth(font, current, currentSize), gapPill = pillRad * 0.6f;
    const float pairW = curW + (pill ? gapPill + 2 * (pillHalf + pillRad) : 0);
    const float curShift = pill ? (curW - pairW) / 2 : 0;  // move the current speed left to make room
    struct Line { const char* s; float size, cap, cy, alpha, dx; } lines[2] = {
        {current, currentSize, capC, top + capC / 2, 0.95f, curShift}, {last, lastSize, capL, top + capC + gap + capL / 2, 0.8f, 0}};
    for (const Line& line : lines) {
        if (!line.s || !*line.s) continue;
        std::vector<float> m;
        if (echo) {
            m = msdfTextMask(*font, line.s, r.w, r.h, line.size, line.cy);
        } else {  // built-in digits: draw into a strip as tall as the line, then place it
            int stripH = int(line.cap / 0.62f + .5f);
            std::vector<float> strip = textMask(line.s, r.w, stripH);
            m.assign(size_t(r.w) * r.h, 0.f);
            int y0 = int(line.cy - stripH / 2.f + .5f);
            for (int y = 0; y < stripH; y++)
                if (y0 + y >= 0 && y0 + y < r.h)
                    std::copy(strip.begin() + size_t(y) * r.w, strip.begin() + size_t(y + 1) * r.w, m.begin() + size_t(y0 + y) * r.w);
        }
        int shift = int(std::lround(line.dx));
        for (int y = 0; y < r.h; y++)
            for (int x = 0; x < r.w; x++) {
                int sx = x - shift;
                if (sx < 0 || sx >= r.w) continue;
                float cov = m[size_t(y) * r.w + sx];
                if (cov <= 0) continue;
                if (!echo) cv.over(r.x + x, r.y + y, {0, 0, 0}, 0.35f * clamp01(cov * 3));
                cv.over(r.x + x, r.y + y, c, line.alpha * cov);
            }
    }
    if (pill) {
        float pcx = r.w / 2.f + pairW / 2 - pillRad - pillHalf, pcy = lines[0].cy;
        drawPill(cv, r, pcx, pcy, pillHalf, pillRad, c);
        int x0 = int(pcx - pillHalf - pillRad), w = int(2 * (pillHalf + pillRad)) + 1, y0 = int(pcy - pillRad), h = int(2 * pillRad) + 1;
        Rect cell{r.x + std::max(0, x0), r.y + std::max(0, y0), std::min(w, r.w - std::max(0, x0)), std::min(h, r.h - std::max(0, y0))};
        drawLabel(cv, cell, peak, pillCap, fillInk(c), 0.95f, font);
    }
}

// The pass counter: "+1" (number 1, plus) or a number, centred, in Echo's digits and the
// team colour; the plus sign is drawn (Echo's font file has only digits).
// ---- Discord: round avatars with a ring (green while talking), from 48 x 48 RGBA bytes
inline float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
inline void drawAvatarDisc(Canvas& cv, int cx, int cy, int radius, const uint8_t* rgba, int n, bool has, Color ring, int ringPx, float alpha, Color fallback) {
    static const std::vector<float> lut = [] {
        std::vector<float> t(256);
        for (int i = 0; i < 256; i++) t[i] = srgbToLinear(i / 255.f);
        return t;
    }();
    int reach = radius + ringPx + 1;
    for (int y = cy - reach; y <= cy + reach; y++) {
        if (y < 0 || y >= cv.h) continue;
        for (int x = cx - reach; x <= cx + reach; x++) {
            if (x < 0 || x >= cv.w) continue;
            float dx = x + .5f - cx, dy = y + .5f - cy, d = std::sqrt(dx * dx + dy * dy);
            float inner = clamp01(radius - d + .5f);
            if (inner > 0) {
                Color c = fallback;
                if (has) {
                    int sx = std::clamp(int((dx + radius) / (2.f * radius) * n), 0, n - 1), sy = std::clamp(int((dy + radius) / (2.f * radius) * n), 0, n - 1);
                    const uint8_t* p = rgba + (size_t(sy) * n + sx) * 4;
                    c = {lut[p[0]], lut[p[1]], lut[p[2]]};
                }
                cv.over(x, y, c, alpha * inner);
            }
            if (ringPx > 0) {
                float band = clamp01(radius + ringPx - d + .5f) - clamp01(radius - d + .5f);
                if (band > 0) cv.over(x, y, ring, alpha * band);
            }
        }
    }
}
struct DiscordMemberView { const uint8_t* rgba; bool has, speaking, self, quiet; };
inline void drawVoiceRow(Canvas& cv, const Rect& r, const DiscordMemberView* members, int count, Color hud) {
    cv.clear(r);
    int n = count > 8 ? 8 : count;
    if (n <= 0) return;
    const int cell = 64, radius = 26;
    int x0 = r.x + (r.w - n * cell) / 2;
    const Color green{0.08f, 0.72f, 0.3f}, idle{0.22f, 0.24f, 0.30f}, fallback{0.28f, 0.30f, 0.45f};
    for (int i = 0; i < n; i++) {
        const auto& m = members[i];
        int cx = x0 + i * cell + cell / 2, cy = r.y + r.h / 2;
        drawAvatarDisc(cv, cx, cy, radius, m.rgba, 48, m.has, m.speaking ? green : m.self ? hud : idle, m.speaking ? 4 : 2, m.quiet ? 0.5f : 1.f, fallback);
    }
}
inline void drawMessageBadge(Canvas& cv, const Rect& r, const uint8_t* rgba, bool has, int unread, float alpha, const DigitFont* font) {
    cv.clear(r);
    if (unread <= 0 || alpha <= 0) return;
    const int radius = 26;
    drawAvatarDisc(cv, r.x + radius + 4, r.y + r.h / 2, radius, rgba, 48, has, Color{0.35f, 0.4f, 0.95f}, 3, alpha, Color{0.28f, 0.30f, 0.45f});
    char text[12];
    snprintf(text, sizeof text, "+%d", unread > 99 ? 99 : unread);
    const float cap = r.h * 0.5f, size = fontOk(font) ? cap / fontCap(*font, 1.f) : cap / 0.62f;
    Rect box{r.x + 2 * radius + 12, r.y + int(r.h * 0.22f), r.w - 2 * radius - 14, int(r.h * 0.56f)};
    drawText(cv, box, text, Color{1, 1, 1}, alpha, font, size);
}

inline void drawPassCounter(Canvas& cv, const Rect& r, int number, bool plus, Color c, float alpha, const DigitFont* font) {
    cv.clear(r);
    if (number <= 0 || alpha <= 0) return;
    char digits[12];
    snprintf(digits, sizeof digits, "%d", number);
    const float cap = r.h * 0.6f, size = fontOk(font) ? cap / fontCap(*font, 1.f) : cap / 0.62f;
    const float digitsW = textWidth(font, digits, size), plusW = plus ? cap * 0.78f : 0, gap = plus ? cap * 0.14f : 0;
    const float left = (r.w - (plusW + gap + digitsW)) / 2, cy = r.h / 2.f;
    // the digits, shifted right of the plus
    Rect cell{r.x + int(left + plusW + gap), r.y, int(digitsW + 4), r.h};
    cell.w = std::min(cell.w, r.x + r.w - cell.x);
    drawLabel(cv, cell, digits, cap, c, alpha, font);
    if (!plus) return;
    const float arm = plusW / 2, half = cap * 0.075f, cx = left + arm;  // two round-ended bars
    for (int y = int(cy - arm - 2); y <= int(cy + arm + 2); y++)
        for (int x = int(cx - arm - 2); x <= int(cx + arm + 2); x++) {
            if (x < 0 || y < 0 || x >= r.w || y >= r.h) continue;
            float px = x + .5f, py = y + .5f;
            float d = std::min(segDistance(px, py, cx - arm + half, cy, cx + arm - half, cy),
                               segDistance(px, py, cx, cy - arm + half, cx, cy + arm - half));
            float cov = clamp01(half - d + .5f);
            if (cov > 0) cv.over(r.x + x, r.y + y, c, alpha * cov);
        }
}

// Full-view vignette: tinted all over, strongest towards the edges.
inline void drawFlash(Canvas& cv, const Rect& r, Color c, float alpha) {
    cv.clear(r);
    if (alpha <= 0) return;
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++) {
            float dx = (x + .5f) / r.w * 2 - 1, dy = (y + .5f) / r.h * 2 - 1;
            float d = std::sqrt(dx * dx + dy * dy);
            cv.over(r.x + x, r.y + y, c, alpha * (0.18f + 0.82f * smooth(0.3f, 1.f, d)));
        }
}

// Edge glow, like Echo's shield tint: clear in the middle of the view, the HUD colour
// towards the edges. Each side can have its own opacity, blended across the middle (the
// HUD uses one for both); `inner` is where it starts, from the middle to the edge.
inline void drawEdgeGlow(Canvas& cv, const Rect& r, Color c, float left, float right, float inner = 0.3f) {
    cv.clear(r);
    if (left <= 0 && right <= 0) return;
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++) {
            float dx = (x + .5f) / r.w * 2 - 1, dy = (y + .5f) / r.h * 2 - 1;
            float toLeft = smooth(0.25f, -0.25f, dx);
            float alpha = left * toLeft + right * (1 - toLeft);
            if (alpha > 0) cv.over(r.x + x, r.y + y, c, alpha * smooth(inner, inner + 0.5f, std::sqrt(dx * dx + dy * dy)));
        }
}

inline void formatSpeed(float v, char* out, size_t n) {
    if (!(v >= 0) || v > 999) { if (n) out[0] = 0; return; }
    snprintf(out, n, "%.1f", v);
}

// ---- Oculus quad layers ---------------------------------------------------------------
// ovrLayerQuad, built as bytes because the header layout depends on the SDK version
// the game was built with: Type, Flags, then (SDK 1.25+) char Reserved[128].
constexpr int kLayerQuad = 3;
constexpr unsigned kFlagHighQuality = 0x1, kFlagHeadLocked = 0x4;

struct alignas(8) QuadLayer {
    unsigned char bytes[200] = {};
};

// Head-locked quad centred at (x, y, -z) metres (x right, y up, z ahead), facing the eye.
inline void buildQuad(QuadLayer& layer, size_t headerSize, void* swapChain, const Rect& vp, float x, float y, float z,
                      float width, float height) {
    std::memset(layer.bytes, 0, sizeof layer.bytes);
    auto put = [&](size_t at, const void* v, size_t n) { std::memcpy(layer.bytes + at, v, n); };
    int type = kLayerQuad;
    unsigned flags = kFlagHighQuality | kFlagHeadLocked;
    put(0, &type, 4);
    put(4, &flags, 4);
    size_t at = headerSize;
    put(at, &swapChain, 8);                    // ColorTexture
    int viewport[4] = {vp.x, vp.y, vp.w, vp.h};
    put(at + 8, viewport, 16);                 // Viewport
    float pose[7] = {0, 0, 0, 1, x, y, -z};    // orientation xyzw, position xyz
    put(at + 24, pose, 28);                    // QuadPoseCenter
    float size[2] = {width, height};
    put(at + 52, size, 8);                     // QuadSize (metres)
}

// The first layer the game submits tells the header layout: in the old layout its
// ColorTexture pointer sits at +8, in the new one +8 starts the zeroed Reserved block.
inline size_t detectHeaderSize(const void* const* layers, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        if (!layers[i]) continue;
        uint64_t at8;
        std::memcpy(&at8, static_cast<const unsigned char*>(layers[i]) + 8, 8);
        return at8 ? 8 : 136;
    }
    return 136;
}

// Where each element goes, from the readout layout in Settings (canvas px, y down).
struct Placement {
    float x, y, z, w, h;
};
inline Placement placeCanvasPx(const Settings& s, float px, float pyDown, float wPx, float hPx) {
    return {px / s.pxPerMetre, s.hudHeight - pyDown / s.pxPerMetre, s.hudDistance, wPx / s.pxPerMetre, hPx / s.pxPerMetre};
}
inline Placement gaugePlacement(const Settings& s, int side) {  // side 0 left, 1 right
    return placeCanvasPx(s, (side ? 1.f : -1.f) * s.boostX, s.boostY, s.gaugePx, s.gaugePx);
}
inline Placement thrusterPlacement(const Settings& s, int side) {
    return placeCanvasPx(s, (side ? 1.f : -1.f) * s.thrusterX, s.thrusterY, s.thrusterPx, s.thrusterPx);
}
inline Placement trackerPlacement(const Settings& s) {
    return placeCanvasPx(s, 0, s.trackerDown, s.trackerPx * kTextRect.w / kTextRect.h, s.trackerPx);
}
inline Placement glowPlacement(const Settings& s, int k) {  // k 0..4, left to right
    return placeCanvasPx(s, (k - (Settings::kRings - 1) / 2.f) * (s.glowPx + s.glowGap), s.glowY, s.glowPx, s.glowPx);
}
// Speed level: how many rings are lit.
inline int speedLevel(const Settings& s, float speed) {
    int n = 0;
    for (int k = 0; k < Settings::kRings; k++) n += speed >= s.glowAt[k];
    return n;
}
// How full speed ring k is: from the previous ring's speed (0 for the first) to its own.
inline float glowFill(const Settings& s, int k, float speed) {
    float lo = k > 0 ? s.glowAt[k - 1] : 0, hi = s.glowAt[k];
    return hi > lo ? clamp01((speed - lo) / (hi - lo)) : (speed >= hi ? 1.f : 0.f);
}
inline Placement flashPlacement() { return {0, 0, 1.f, 3.4f, 3.4f}; }  // ~120 degrees at 1 m
// The pass counter, centred at the top of the view; `scale` 1.5 for a goal.
inline Placement passPlacement(const Settings& s, float scale) {
    float h = s.passPx / s.pxPerMetre * scale;
    return {0, s.passY, s.hudDistance, h * kPassRect.w / kPassRect.h, h};
}
// Discord: the voice row just above the speeds box; the message badge above the pass counter.
inline Placement voicePlacement(const Settings& s) {
    float down = s.trackerDown - s.trackerPx * 0.5f - s.discordUpPx - s.discordPx * 0.5f;
    return placeCanvasPx(s, 0, down, s.discordPx * kVoiceRect.w / kVoiceRect.h, s.discordPx);
}
inline Placement badgePlacement(const Settings& s) {  // the pass counter's spot, bigger; the counter waits meanwhile
    float h = s.passPx / s.pxPerMetre * s.badgeScale;
    return {0, s.passY, s.hudDistance, h * kBadgeRect.w / kBadgeRect.h, h};
}

// ---- D3D12 uploads --------------------------------------------------------------------
struct ComRelease {
    IUnknown* p;
    ~ComRelease() { if (p) p->Release(); }
};

// Copies RGBA8 pictures into the overlay's swap-chain textures (which rest in
// PIXEL_SHADER_RESOURCE) on the game's DIRECT queue, with a ring of staging buffers. The
// draw thread fills a slot (and waits there, never on the game's thread, if the GPU is still
// reading it); the game's render thread only executes a command list recorded once per
// (slot, texture) and signals the fence: a few microseconds inside Echo's frame.
class Uploader {
public:
    static constexpr int kSlots = 3, kMaxTextures = 8;
    ~Uploader() { reset(); }

    const char* init(ID3D12CommandQueue* queue, const std::vector<ID3D12Resource*>& textures) {
        reset();
        if (textures.empty() || textures.size() > kMaxTextures) return "1 to 8 textures";
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return "queue is not a DIRECT queue";
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device_)))) return "GetDevice failed";
        queue_ = queue;
        D3D12_RESOURCE_DESC desc = textures[0]->GetDesc();
        UINT64 rowBytes;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint_, &rows_, &rowBytes, &total_);
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return "fence failed";
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) return "event failed";
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total_;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        textureCount_ = int(textures.size());
        for (Slot& s : slots_) {
            if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&s.staging)))) return "upload buffer failed";
            if (FAILED(s.staging->Map(0, nullptr, reinterpret_cast<void**>(&s.mapped)))) return "Map failed";
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc)))) return "allocator failed";
            for (int i = 0; i < textureCount_; i++) {  // recorded once, executed every time this slot goes to texture i
                ID3D12GraphicsCommandList* list = nullptr;
                if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc, nullptr, IID_PPV_ARGS(&list)))) return "list failed";
                s.lists[i] = list;
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = textures[i];
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                list->ResourceBarrier(1, &b);
                D3D12_TEXTURE_COPY_LOCATION dst{textures[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION src{s.staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                src.PlacedFootprint = footprint_;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                list->ResourceBarrier(1, &b);
                if (FAILED(list->Close())) return "Close failed";
            }
        }
        return nullptr;
    }
    bool ready() const { return queue_ != nullptr; }
    UINT width() const { return footprint_.Footprint.Width; }
    UINT rows() const { return rows_; }

    // Draw thread: the next staging slot, once the GPU has finished its last copy out of it
    // (waits here). -1 if that wait timed out or nothing is set up.
    int acquire(DWORD timeoutMs = 1000) {
        if (!queue_) return -1;
        for (int tries = 0; tries < 100000; tries++) {
            int k = int(next_ % kSlots);
            int st = state_[k].load();
            if (st == kSubmitting) { Sleep(0); continue; }  // the game's thread is on it: microseconds
            if (st == kReady) {                             // a picture it never showed: ours again
                int expectReady = k;
                ready_.compare_exchange_strong(expectReady, -1);
            }
            int expect = st == kReady ? kReady : kFree;
            if (!state_[k].compare_exchange_strong(expect, kFilling)) continue;
            next_++;
            UINT64 done = done_[k].load();
            if (fence_->GetCompletedValue() < done) {
                fence_->SetEventOnCompletion(done, event_);
                if (WaitForSingleObject(event_, timeoutMs) != WAIT_OBJECT_0) { state_[k] = kFree; return -1; }
            }
            return k;
        }
        return -1;
    }
    // Draw thread: the whole picture (width*rows*4 bytes) into the slot.
    void fill(int slot, const uint8_t* rgba) {
        Slot& s = slots_[slot];
        UINT w = width();
        for (UINT y = 0; y < rows_; y++)
            std::memcpy(s.mapped + footprint_.Offset + UINT64(y) * footprint_.Footprint.RowPitch, rgba + size_t(y) * w * 4, size_t(w) * 4);
    }
    // Draw thread: the slot holds a finished picture.
    void publish(int slot) {
        state_[slot] = kReady;
        ready_ = slot;
    }
    // Game thread: the newest finished picture's slot, now ours to submit; -1 if none.
    int takeReady() {
        int k = ready_.exchange(-1);
        if (k < 0) return -1;
        int expect = kReady;
        return state_[k].compare_exchange_strong(expect, kSubmitting) ? k : -1;
    }
    // Game thread: copy the slot into texture `index` on the queue and let the slot go once
    // the GPU is done with it.
    const char* submit(int slot, int index) {
        if (index < 0 || index >= textureCount_) { discard(slot); return "bad texture index"; }
        ID3D12CommandList* lists[] = {slots_[slot].lists[index]};
        queue_->ExecuteCommandLists(1, lists);
        UINT64 v = ++fenceValue_;
        HRESULT hr = queue_->Signal(fence_, v);
        done_[slot] = v;
        state_[slot] = kFree;
        return FAILED(hr) ? "Signal failed" : nullptr;
    }
    void discard(int slot) { state_[slot] = kFree; }
    // Fill and submit in one go (the first, clear picture; tests).
    const char* write(int index, const uint8_t* rgba) {
        int k = acquire(1000);
        if (k < 0) return "GPU busy";
        fill(k, rgba);
        publish(k);
        int t = takeReady();
        if (t != k) return "slot taken";
        return submit(t, index);
    }
    // Blocks until every queued copy has finished (tests and shutdown).
    bool flush() {
        if (!fence_ || fence_->GetCompletedValue() >= fenceValue_) return true;
        fence_->SetEventOnCompletion(fenceValue_, event_);
        return WaitForSingleObject(event_, 5000) == WAIT_OBJECT_0;
    }

private:
    enum { kFree = 0, kFilling = 1, kReady = 2, kSubmitting = 3 };
    struct Slot {
        ID3D12Resource* staging = nullptr;
        uint8_t* mapped = nullptr;
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* lists[kMaxTextures] = {};
    };
    void reset() {
        flush();
        for (Slot& s : slots_) {
            for (auto* l : s.lists) if (l) l->Release();
            if (s.alloc) s.alloc->Release();
            if (s.staging) s.staging->Release();
            s = Slot{};
        }
        for (int i = 0; i < kSlots; i++) { state_[i] = kFree; done_[i] = 0; }
        ready_ = -1;
        if (fence_) fence_->Release();
        if (device_) device_->Release();
        if (event_) CloseHandle(event_);
        fence_ = nullptr;
        device_ = nullptr;
        event_ = nullptr;
        queue_ = nullptr;
        textureCount_ = 0;
    }
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE event_ = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    UINT rows_ = 0;
    UINT64 total_ = 0, fenceValue_ = 0, next_ = 0;
    int textureCount_ = 0;
    Slot slots_[kSlots];
    std::atomic<int> state_[kSlots] = {};
    std::atomic<UINT64> done_[kSlots] = {};
    std::atomic<int> ready_{-1};
};

}  // namespace hudplus
