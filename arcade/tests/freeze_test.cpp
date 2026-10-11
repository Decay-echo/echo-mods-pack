// FREEZE: Touch controllers -> RetroPad (host/touchpad.h). No game or RetroArch needed.
#include "../host/touchpad.h"
#include <cstdio>

static int failures = 0, checks = 0;
#define CHECK(x) do { checks++; if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static arcade::PadState pad(uint32_t buttons = 0) {
    arcade::PadState s{};
    s.buttons = buttons;
    return s;
}

static int pressedCount(const PadFrame& f) {
    int n = 0;
    for (bool b : f.pad) n += b;
    return n;
}

int main() {
    TouchPad tp;
    uint64_t t = 1000;

    // Nothing pressed: nothing held, sticks centred.
    PadFrame f = tp.step(pad(), t, false, false);
    CHECK(pressedCount(f) == 0 && f.left[0] == 0 && f.right[1] == 0 && !f.menuToggle && !f.release);

    // Face buttons by position (like an Xbox pad), and by label.
    f = tp.step(pad(arcade::BUTTON_A), t, false, false);
    CHECK(f.pad[PadB] && !f.pad[PadA] && pressedCount(f) == 1);
    f = tp.step(pad(arcade::BUTTON_B | arcade::BUTTON_X | arcade::BUTTON_Y), t, false, false);
    CHECK(f.pad[PadA] && f.pad[PadY] && f.pad[PadX] && !f.pad[PadB]);
    f = tp.step(pad(arcade::BUTTON_A | arcade::BUTTON_Y), t, false, true);
    CHECK(f.pad[PadA] && f.pad[PadY] && pressedCount(f) == 2);

    // Many inputs at once: running right, jumping, shooting and both shoulders together.
    arcade::PadState many = pad(arcade::BUTTON_A | arcade::BUTTON_B);
    many.stick[0][0] = 1; many.stick[0][1] = 1;   // up-right
    many.trigger[1] = 1; many.grip[0] = 1; many.grip[1] = .9f;
    f = tp.step(many, t, false, false);
    CHECK(f.pad[PadA] && f.pad[PadB] && f.pad[PadRight] && f.pad[PadUp] && f.pad[PadR2] && f.pad[PadL] && f.pad[PadR]);
    CHECK(!f.pad[PadLeft] && !f.pad[PadDown] && !f.pad[PadL2] && pressedCount(f) == 7);
    CHECK(f.left[0] == 0 && f.left[1] == 0);  // D-pad mode: the left stick doesn't also move the analog stick

    // Stick hysteresis: on above .55, stays on down to .35.
    tp.reset();
    arcade::PadState s = pad();
    s.stick[0][0] = .5f;
    CHECK(!tp.step(s, t, false, false).pad[PadRight]);
    s.stick[0][0] = .6f;
    CHECK(tp.step(s, t, false, false).pad[PadRight]);
    s.stick[0][0] = .4f;
    CHECK(tp.step(s, t, false, false).pad[PadRight]);
    s.stick[0][0] = .3f;
    CHECK(!tp.step(s, t, false, false).pad[PadRight]);

    // STICK mode: the left stick is the analog stick (y flipped to libretro's down = +).
    s = pad();
    s.stick[0][0] = .25f; s.stick[0][1] = .75f; s.stick[1][0] = -1; s.stick[1][1] = -.5f;
    f = tp.step(s, t, true, false);
    CHECK(f.left[0] == .25f && f.left[1] == -.75f && !f.pad[PadUp] && !f.pad[PadRight]);
    CHECK(f.right[0] == -1 && f.right[1] == .5f);

    // Stick clicks: left = SELECT (held), right = RetroArch's menu once per click.
    tp.reset();
    f = tp.step(pad(arcade::BUTTON_LTHUMB | arcade::BUTTON_RTHUMB), t, false, false);
    CHECK(f.pad[PadSelect] && f.menuToggle);
    f = tp.step(pad(arcade::BUTTON_LTHUMB | arcade::BUTTON_RTHUMB), t + 10, false, false);
    CHECK(f.pad[PadSelect] && !f.menuToggle);

    // Menu button tap = START for a moment after the release; never during the press.
    tp.reset();
    f = tp.step(pad(arcade::BUTTON_ENTER), t, false, false);
    CHECK(!f.pad[PadStart] && !f.release);
    f = tp.step(pad(), t + 200, false, false);
    CHECK(f.pad[PadStart] && !f.release);
    f = tp.step(pad(), t + 200 + FREEZE_START_TAP_MS - 1, false, false);
    CHECK(f.pad[PadStart]);
    f = tp.step(pad(), t + 200 + FREEZE_START_TAP_MS, false, false);
    CHECK(!f.pad[PadStart]);

    // Menu button held 1 s: FREEZE off (once), and no START afterwards.
    tp.reset();
    tp.step(pad(arcade::BUTTON_ENTER), t, false, false);
    f = tp.step(pad(arcade::BUTTON_ENTER), t + FREEZE_RELEASE_HOLD_MS - 1, false, false);
    CHECK(!f.release);
    f = tp.step(pad(arcade::BUTTON_ENTER), t + FREEZE_RELEASE_HOLD_MS, false, false);
    CHECK(f.release && !f.pad[PadStart]);
    f = tp.step(pad(arcade::BUTTON_ENTER), t + FREEZE_RELEASE_HOLD_MS + 50, false, false);
    CHECK(!f.release);
    f = tp.step(pad(), t + FREEZE_RELEASE_HOLD_MS + 100, false, false);
    CHECK(!f.pad[PadStart] && !f.release);

    std::printf(failures ? "freeze_test: %d of %d checks failed\n" : "freeze_test: all %d checks passed\n", failures ? failures : checks, checks);
    return failures ? 1 : 0;
}
