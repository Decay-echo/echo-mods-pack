// FREEZE: the Touch controllers as RetroArch's RetroPad (player 1), while you're held still.
//
//   left stick       D-pad (or the left analog stick when the tablet's STICK is on)
//   right stick      right analog stick
//   A B X Y          face buttons: by position like an Xbox pad (A = bottom = RetroPad B,
//                    B = right = RetroPad A, X = left = RetroPad Y, Y = top = RetroPad X),
//                    or by their labels with arcade.ini freeze_buttons=labels
//   grips            L / R          triggers   L2 / R2
//   left stick click SELECT         right stick click   RetroArch's menu (open / close)
//   menu button      START (tap)    hold it 1 s         FREEZE off (the controllers go back to Echo)
#pragma once
#include "apps.h"

constexpr uint64_t FREEZE_RELEASE_HOLD_MS = 1000;  // hold the menu button this long to let go
constexpr uint64_t FREEZE_START_TAP_MS = 120;      // a menu-button tap holds START this long

struct PadFrame {
    bool pad[PadCount] = {};
    float left[2] = {}, right[2] = {};  // libretro analog sticks, -1..1, y down
    bool menuToggle = false;            // RetroArch's menu: open / close (once per click)
    bool release = false;               // the menu button was held: FREEZE off (once per hold)
};

class TouchPad {
public:
    PadFrame step(const arcade::PadState& s, uint64_t now, bool stickMode, bool byLabels) {
        PadFrame f;
        uint32_t b = s.buttons;
        bool a = b & arcade::BUTTON_A, bb = b & arcade::BUTTON_B, x = b & arcade::BUTTON_X, y = b & arcade::BUTTON_Y;
        if (byLabels) { f.pad[PadA] = a; f.pad[PadB] = bb; f.pad[PadX] = x; f.pad[PadY] = y; }
        else { f.pad[PadB] = a; f.pad[PadA] = bb; f.pad[PadY] = x; f.pad[PadX] = y; }
        for (int h = 0; h < 2; h++) {
            grip_[h] = held(grip_[h], s.grip[h]);
            trigger_[h] = held(trigger_[h], s.trigger[h]);
        }
        f.pad[PadL] = grip_[0]; f.pad[PadR] = grip_[1];
        f.pad[PadL2] = trigger_[0]; f.pad[PadR2] = trigger_[1];
        f.pad[PadSelect] = (b & arcade::BUTTON_LTHUMB) != 0;
        bool rthumb = (b & arcade::BUTTON_RTHUMB) != 0;
        f.menuToggle = rthumb && !rthumb_;
        rthumb_ = rthumb;
        // Menu button: a tap is START (sent on release, so a long hold never pauses the game first);
        // held for FREEZE_RELEASE_HOLD_MS it lets go of the controllers instead.
        bool menu = (b & arcade::BUTTON_ENTER) != 0;
        if (menu && !menuDown_) { menuAt_ = now; menuUsed_ = false; }
        if (menu && !menuUsed_ && now - menuAt_ >= FREEZE_RELEASE_HOLD_MS) { menuUsed_ = true; f.release = true; }
        if (!menu && menuDown_ && !menuUsed_) startUntil_ = now + FREEZE_START_TAP_MS;
        menuDown_ = menu;
        f.pad[PadStart] = now < startUntil_;
        float lx = s.stick[0][0], ly = s.stick[0][1];
        if (stickMode) {
            for (auto& d : dpad_) d = false;
            f.left[0] = lx; f.left[1] = -ly;
        } else {
            dpad_[0] = held(dpad_[0], ly); dpad_[1] = held(dpad_[1], -ly);
            dpad_[2] = held(dpad_[2], -lx); dpad_[3] = held(dpad_[3], lx);
            f.pad[PadUp] = dpad_[0]; f.pad[PadDown] = dpad_[1]; f.pad[PadLeft] = dpad_[2]; f.pad[PadRight] = dpad_[3];
        }
        f.right[0] = s.stick[1][0]; f.right[1] = -s.stick[1][1];
        return f;
    }
    void reset() { *this = TouchPad{}; }

private:
    static bool held(bool was, float v) { return was ? v > .35f : v > .55f; }  // a little hysteresis
    bool dpad_[4] = {}, grip_[2] = {}, trigger_[2] = {}, rthumb_ = false, menuDown_ = false, menuUsed_ = false;
    uint64_t menuAt_ = 0, startUntil_ = 0;
};
