// Protocol checks for native/shared/arcade_ipc.h (no game needed).
#include "../native/shared/arcade_ipc.h"
#include <atomic>
#include <cstdio>
#include <thread>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

int main() {
    static_assert(offsetof(arcade::Shared, frames) % 4096 == 0, "frames must be page aligned");
    // tools/fake_game.py mirrors these offsets.
    static_assert(offsetof(arcade::Shared, hostPid) == 32 && offsetof(arcade::Shared, latestFrame) == 36);
    static_assert(offsetof(arcade::Shared, frameSerial) == 40 && offsetof(arcade::Shared, hostHeartbeat) == 48);
    static_assert(offsetof(arcade::Shared, dockWant) == 152 && offsetof(arcade::Shared, dockSerial) == 156);
    static_assert(offsetof(arcade::Shared, gamePid) == 160 && offsetof(arcade::Shared, pageVisible) == 164);
    static_assert(offsetof(arcade::Shared, readingFrame) == 168 && offsetof(arcade::Shared, gameHeartbeat) == 176);
    static_assert(offsetof(arcade::Shared, tabletScale) == 44 && offsetof(arcade::Shared, pageMode) == 172);
    static_assert(offsetof(arcade::Shared, touchWrite) == 184 && offsetof(arcade::Shared, dockDone) == 188);
    static_assert(offsetof(arcade::Shared, dockState) == 192 && offsetof(arcade::Shared, shots) == 196);
    static_assert(offsetof(arcade::Shared, dockText) == 200 && offsetof(arcade::Shared, touches) == 296);
    static_assert(offsetof(arcade::Shared, frames) == 8192 && sizeof(arcade::Shared) == 8192 + 3 * 2351104);
    static_assert(offsetof(arcade::Shared, controllerWant) == 4392 && offsetof(arcade::Shared, controllerLevel) == 4396);
    static_assert(offsetof(arcade::Shared, controllerState) == 4400 && offsetof(arcade::Shared, levelSerial) == 4404);
    static_assert(offsetof(arcade::Shared, padSerial) == 4408 && offsetof(arcade::Shared, padTick) == 4416);
    static_assert(offsetof(arcade::Shared, pad) == 4424 && sizeof(arcade::PadState) == 40);
    HANDLE mapping = nullptr;
    auto s = arcade::open(&mapping);
    CHECK(s != nullptr);
    if (!s) return 1;
    CHECK(s->magic == arcade::MAGIC && s->width == arcade::WIDTH && s->height == arcade::HEIGHT);

    // Host never writes the newest frame or the one being read.
    for (int latest = 0; latest < 3; latest++)
        for (int reading = -1; reading < 3; reading++) {
            s->latestFrame = latest; s->readingFrame = reading;
            uint32_t w = arcade::writableFrame(s);
            CHECK(LONG(w) != latest && LONG(w) != reading);
        }

    // Concurrent writer/reader: every frame the reader copies is internally consistent.
    s->latestFrame = LONG(arcade::NO_FRAME); s->readingFrame = LONG(arcade::NO_FRAME);
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (uint32_t n = 1; !stop; n++) {
            uint32_t i = arcade::writableFrame(s);
            auto px = reinterpret_cast<uint32_t*>(s->frames[i]);
            for (uint32_t k = 0; k < arcade::FRAME_BYTES / 4; k += 97) px[k] = n;
            px[arcade::FRAME_BYTES / 4 - 1] = n;
            arcade::publishFrame(s, i);
            Sleep(1);  // real host: one frame per 16 ms
        }
    });
    int torn = 0, copies = 0;
    for (uint64_t end = GetTickCount64() + 1500; GetTickCount64() < end;) {
        LONG index = s->latestFrame;
        if (index == LONG(arcade::NO_FRAME)) continue;
        InterlockedExchange(&s->readingFrame, index);
        if (s->latestFrame != index) { InterlockedExchange(&s->readingFrame, LONG(arcade::NO_FRAME)); continue; }
        auto px = reinterpret_cast<uint32_t*>(s->frames[index]);
        uint32_t first = px[0];
        for (uint32_t k = 0; k < arcade::FRAME_BYTES / 4; k += 97) torn += px[k] != first;
        torn += px[arcade::FRAME_BYTES / 4 - 1] != first;
        copies++;
        InterlockedExchange(&s->readingFrame, LONG(arcade::NO_FRAME));
    }
    stop = true; writer.join();
    CHECK(copies > 100);
    CHECK(torn == 0);

    // Touch ring round trip.
    LONG start = s->touchWrite;
    arcade::pushTouch(s, 5, arcade::TouchDown);
    arcade::pushTouch(s, 5, arcade::TouchUp);
    auto& a = s->touches[uint32_t(start) % arcade::TOUCH_RING];
    auto& b = s->touches[uint32_t(start + 1) % arcade::TOUCH_RING];
    CHECK(a.seq == uint32_t(start) + 1 && a.cell == 5 && a.kind == arcade::TouchDown);
    CHECK(b.seq == uint32_t(start) + 2 && b.kind == arcade::TouchUp);
    // Poster pointer and shot events carry an exact pixel.
    arcade::pushTouch(s, 1, arcade::PointMove, 1023, 573);
    arcade::pushTouch(s, arcade::SHOT_POINTER, arcade::Shot, 17, 300);
    auto& c = s->touches[uint32_t(start + 2) % arcade::TOUCH_RING];
    auto& d = s->touches[uint32_t(start + 3) % arcade::TOUCH_RING];
    CHECK(c.cell == 1 && c.kind == arcade::PointMove && (c.xy & 0xffff) == 1023 && (c.xy >> 16) == 573);
    CHECK(d.cell == arcade::SHOT_POINTER && d.kind == arcade::Shot && (d.xy & 0xffff) == 17 && (d.xy >> 16) == 300);
    // FREEZE: the controllers round trip, and a reader never sees a half-written pad.
    arcade::PadState in{arcade::BUTTON_A | arcade::BUTTON_ENTER, 0x10, {.25f, 1}, {0, .75f}, {{-1, .5f}, {.125f, -.25f}}}, out{};
    arcade::writePad(s, in);
    LONG64 tick = 0;
    CHECK(arcade::readPad(s, out, &tick) && std::memcmp(&in, &out, sizeof in) == 0 && tick > 0 && (s->padSerial & 1) == 0);
    arcade::writePad(s, arcade::PadState{});  // consistent before the writer's first update
    std::atomic<bool> stopPad{false};
    std::thread padWriter([&] {
        for (uint32_t n = 1; !stopPad; n++) {
            arcade::PadState p{};
            p.buttons = n; p.touches = n; p.trigger[0] = p.trigger[1] = p.grip[0] = p.grip[1] = float(n);
            arcade::writePad(s, p);
            for (int k = 0; k < 200; k++) YieldProcessor();  // far busier than the game (one write per frame)
        }
    });
    int padReads = 0, padTorn = 0;
    for (uint64_t end = GetTickCount64() + 300; GetTickCount64() < end;) {
        arcade::PadState p{};
        if (!arcade::readPad(s, p)) continue;
        padReads++;
        padTorn += p.touches != p.buttons || p.grip[1] != float(p.buttons) || p.trigger[0] != float(p.buttons);
    }
    stopPad = true; padWriter.join();
    std::printf("pad: %d consistent reads, %d torn\n", padReads, padTorn);
    CHECK(padReads > 100 && padTorn == 0);
    std::printf(failures ? "ipc_test: %d failure(s)\n" : "ipc_test: all checks passed (%d consistent frame copies)\n", failures ? failures : copies);
    return failures ? 1 : 0;
}
