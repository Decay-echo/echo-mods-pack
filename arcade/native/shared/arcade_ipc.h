// Shared memory between EchoArcade.dll (inside echovr.exe) and ArcadeHost.exe.
// Frames and dock requests flow host -> game; touches, shots, page visibility and the
// dock state flow game -> host.
// Everything is local to this PC: no sockets, no network.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace arcade {

constexpr wchar_t SHARED_NAME[] = L"Local\\EchoArcade.Shared.v3";
constexpr uint32_t MAGIC = 0x41524344;  // 'ARCD'
constexpr uint32_t VERSION = 3;
// Set (manual-reset event) while the Touch controllers belong to the arcade, so other plugins
// that read them (DiscButton's A button) step aside.
constexpr wchar_t CONTROLLER_EVENT[] = L"Local\\EchoArcade.ControllerMode";
constexpr uint32_t WIDTH = 1024, HEIGHT = 574, PITCH = WIDTH * 4;
constexpr uint32_t FRAME_BYTES = PITCH * HEIGHT;
constexpr uint32_t FRAME_BUFFERS = 3;
constexpr uint32_t TOUCH_RING = 256;
constexpr uint32_t NO_FRAME = 0xffffffffu;

// TouchUp/Down: a tablet touch cell. Point*: a finger on the docked lobby poster, at a
// screen pixel. Shot: a bullet from the player's gun hit the docked poster (light gun).
enum TouchKind : uint32_t { TouchUp = 0, TouchDown = 1, PointDown = 2, PointMove = 3, PointUp = 4, Shot = 5 };
constexpr uint32_t SHOT_POINTER = 2;  // Point* pointers are 0 and 1 (the two fingertips)

struct TouchEvent {
    uint32_t seq;   // written last; equals ring position + 1 when valid
    uint32_t cell;  // TouchUp/Down: row * cols + col. Point*/Shot: pointer id
    uint32_t kind;  // TouchKind
    uint32_t xy;    // Point*/Shot: screen pixel, x | y << 16
};

// Dock (host -> game): bump dockSerial after setting dockWant (1 = dock on the nearest
// lobby poster, 0 = undock). The game answers by copying dockSerial to dockDone.
enum DockState : LONG { Undocked = 0, Docked = 1, DockFailed = 2 };

// FREEZE: while the host sets controllerWant (and controllerLevel still equals levelSerial),
// the game hides the Touch controllers' buttons and sticks from Echo VR, holds the player still
// and copies the real controllers into pad for RetroArch. Off: the game is not doing it (yet,
// or it couldn't hook).
enum ControllerState : LONG { ControllersOff = 0, ControllersOn = 1, ControllersFrozen = 2, ControllersReleased = 3 };  // Released: hands up
constexpr uint32_t BUTTON_A = 0x1, BUTTON_B = 0x2, BUTTON_RTHUMB = 0x4, BUTTON_X = 0x100, BUTTON_Y = 0x200,
                   BUTTON_LTHUMB = 0x400, BUTTON_ENTER = 0x100000;  // ovrButton_* (Enter = the left menu button)
struct PadState {
    uint32_t buttons;            // ovrButton_* bits
    uint32_t touches;            // ovrTouch_* bits
    float trigger[2], grip[2];   // index and hand triggers, 0..1, [0] left [1] right
    float stick[2][2];           // thumbsticks x, y, -1..1, y up
};

#pragma warning(push)
#pragma warning(disable : 4324)  // padding before the page-aligned frames is intended
struct Shared {
    uint32_t magic, version, width, height, pitch, cols, rows, reserved0;

    // ---- host -> game ----
    volatile LONG hostPid;
    volatile LONG latestFrame;      // index of newest complete buffer, NO_FRAME if none
    volatile LONG frameSerial;      // bumps each published frame
    volatile LONG tabletScale;      // requested tablet size x1000 from SETTINGS (0 = unchanged)
    volatile LONG64 hostHeartbeat;  // GetTickCount64 of the host's last loop
    char status[96];                // shown on the tablet when the host is missing
    volatile LONG dockWant;         // 1 dock on the nearest poster, 0 undock
    volatile LONG dockSerial;       // bumped by the host for each request

    // ---- game -> host ----
    volatile LONG gamePid;
    volatile LONG pageVisible;      // ARCADE page is the active tablet page
    volatile LONG readingFrame;     // buffer the game is copying right now, NO_FRAME if none
    volatile LONG pageMode;         // 0 = ARCADE tab, 1 = SETTINGS tab (same page, host draws it)
    volatile LONG64 gameHeartbeat;
    volatile LONG touchWrite;       // total events ever written
    volatile LONG dockDone;         // last dockSerial the game handled
    volatile LONG dockState;        // DockState
    volatile LONG shots;            // light-gun shots that hit the docked poster
    char dockText[96];              // what happened, for the tablet (ASCII)
    TouchEvent touches[TOUCH_RING];

    // ---- FREEZE: Touch controllers (after the touch ring, so the offsets above stay put) ----
    volatile LONG controllerWant;   // host -> game: 1 = the controllers drive the arcade
    volatile LONG controllerLevel;  // host -> game: the levelSerial it was switched on in (a new level ends it)
    volatile LONG controllerState;  // game -> host: ControllerState
    volatile LONG levelSerial;      // game -> host: bumps when the level changes
    volatile LONG padSerial;        // game -> host: seqlock around pad, odd while it is written
    LONG reserved1;
    volatile LONG64 padTick;        // game -> host: GetTickCount64 of the last pad update
    PadState pad;                   // game -> host: the real controllers (writePad / readPad)

    alignas(4096) uint8_t frames[FRAME_BUFFERS][FRAME_BYTES];
};
#pragma warning(pop)

inline bool alive(LONG64 heartbeat, uint64_t now, uint64_t tolerance = 3000) {
    return heartbeat != 0 && now >= uint64_t(heartbeat) && now - uint64_t(heartbeat) < tolerance;
}

// Opens (or creates) the mapping. Either side may start first.
inline Shared* open(HANDLE* mapping) {
    *mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), SHARED_NAME);
    if (!*mapping) return nullptr;
    bool created = GetLastError() != ERROR_ALREADY_EXISTS;
    auto s = static_cast<Shared*>(MapViewOfFile(*mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!s) { CloseHandle(*mapping); *mapping = nullptr; return nullptr; }
    if (created || s->magic != MAGIC) {
        std::memset(s, 0, offsetof(Shared, frames));
        s->version = VERSION; s->width = WIDTH; s->height = HEIGHT; s->pitch = PITCH;
        s->latestFrame = LONG(NO_FRAME); s->readingFrame = LONG(NO_FRAME);
        MemoryBarrier();
        s->magic = MAGIC;
    }
    return s->version == VERSION ? s : nullptr;
}

// Game side: publish a touch (cell) or a poster pointer / shot (pointer id, pixel).
inline void pushTouch(Shared* s, uint32_t cell, TouchKind kind, uint32_t x = 0, uint32_t y = 0) {
    LONG n = InterlockedIncrement(&s->touchWrite) - 1;
    auto& e = s->touches[uint32_t(n) % TOUCH_RING];
    e.seq = 0; MemoryBarrier();
    e.cell = cell; e.kind = kind; e.xy = (x & 0xffff) | (y << 16); MemoryBarrier();
    e.seq = uint32_t(n) + 1;
}

// Host side: pick a buffer that is neither the newest nor being read.
inline uint32_t writableFrame(Shared* s) {
    LONG latest = s->latestFrame, reading = s->readingFrame;
    for (uint32_t i = 0; i < FRAME_BUFFERS; i++)
        if (LONG(i) != latest && LONG(i) != reading) return i;
    return 0;
}

inline void publishFrame(Shared* s, uint32_t index) {
    MemoryBarrier();
    InterlockedExchange(&s->latestFrame, LONG(index));
    InterlockedIncrement(&s->frameSerial);
}

// The pad is copied a 32-bit word at a time through volatile pointers: volatile accesses keep
// their order relative to the serial (a plain memcpy may be moved across it by the compiler).
static_assert(sizeof(PadState) % 4 == 0, "PadState copies in 32-bit words");
constexpr size_t PAD_WORDS = sizeof(PadState) / 4;

// Game side (one writer at a time): publish the controllers.
inline void writePad(Shared* s, const PadState& p) {
    InterlockedIncrement(&s->padSerial);  // odd: being written
    auto dst = reinterpret_cast<volatile uint32_t*>(&s->pad);
    auto src = reinterpret_cast<const uint32_t*>(&p);
    for (size_t i = 0; i < PAD_WORDS; i++) dst[i] = src[i];
    s->padTick = LONG64(GetTickCount64());
    InterlockedIncrement(&s->padSerial);  // even: complete
}

// Host side: a consistent copy of the controllers, or false if the game was mid-write.
inline bool readPad(Shared* s, PadState& out, LONG64* tick = nullptr) {
    auto src = reinterpret_cast<const volatile uint32_t*>(&s->pad);
    auto dst = reinterpret_cast<uint32_t*>(&out);
    for (int attempt = 0; attempt < 8; attempt++) {
        LONG before = s->padSerial;
        if (before & 1) { YieldProcessor(); continue; }
        for (size_t i = 0; i < PAD_WORDS; i++) dst[i] = src[i];
        LONG64 when = s->padTick;
        _ReadWriteBarrier();
        if (InterlockedCompareExchange(&s->padSerial, before, before) == before) {  // full barrier, then the check
            if (tick) *tick = when;
            return true;
        }
    }
    return false;
}

}  // namespace arcade
