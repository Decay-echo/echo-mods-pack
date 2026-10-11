// Light gun for RetroArch: a bullet on the docked poster becomes an aimed trigger pull at the
// same spot of RetroArch's picture.
//
// "touch" (default): RetroArch's dinput driver aims the light gun at the first touch on its
// window (WM_POINTER*, focused or not), and the invisible overlay from writeRetroConfig() turns
// that touch into the trigger too (input_overlay_lightgun_trigger_on_touch), for every core
// and port. So a shot is a short injected touch. WS_EX_NOACTIVATE stops the touch from
// activating RetroArch, and because touch goes to whichever window is on top at that point,
// the window is raised (not activated) while you shoot and parked again after.
// "focus": the fallback. Focus RetroArch, put the cursor on the spot and click (RetroArch only
// reads the mouse while it has focus); focus and the cursor go back after a pause.
#include "apps.h"
#include <algorithm>
#include <chrono>

namespace {

constexpr DWORD GAP_MS = 50;            // released this long before the next shot
constexpr DWORD FOCUS_SETTLE_MS = 80;   // RetroArch re-acquires the mouse a few frames after focus
constexpr uint64_t PARK_AFTER_MS = 3000, FOCUS_BACK_MS = 2000;

// Windows lets the process that sent the last input change the foreground window.
bool bringForward(HWND window) {
    INPUT nudge{};
    nudge.type = INPUT_MOUSE;
    nudge.mi.dwFlags = MOUSEEVENTF_MOVE;  // moves by 0: harmless, but it counts as our input
    SendInput(1, &nudge, sizeof(nudge));
    if (SetForegroundWindow(window)) return true;
    DWORD current = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), self = GetCurrentThreadId();
    if (!current || current == self) return false;
    AttachThreadInput(self, current, TRUE);
    bool ok = SetForegroundWindow(window) != FALSE;
    AttachThreadInput(self, current, FALSE);
    return ok;
}

void mouseButton(DWORD flag) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flag;
    SendInput(1, &in, sizeof(in));
}

}  // namespace

LightGun::LightGun(const Config& config)
    : focusMode_(config.lightGun == L"focus"), enabled_(config.lightGun == L"touch" || config.lightGun == L"focus"),
      offscreen_(config.windowMode == L"offscreen") {
    if (enabled_) worker_ = std::thread([this] { run(); });
    hostLog("light gun: %ls", enabled_ ? config.lightGun.c_str() : L"off");
}

LightGun::~LightGun() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void LightGun::shoot(HWND window, float u, float v, DWORD holdMs) {
    if (!enabled_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() < 4) queue_.push_back({window, u, v, holdMs});
    }
    wake_.notify_all();
}

void LightGun::appClosed() {
    if (!enabled_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        closed_ = true;
    }
    wake_.notify_all();
}

void LightGun::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        wake_.wait_for(lock, std::chrono::milliseconds(250), [this] { return stop_ || closed_ || !queue_.empty(); });
        if (stop_) break;
        if (!queue_.empty()) {
            Shot shot = queue_.front();
            queue_.pop_front();
            lock.unlock();
            fire(shot);
            lock.lock();
            continue;
        }
        bool closed = closed_;
        closed_ = false;
        lock.unlock();
        uint64_t idle = GetTickCount64() - lastShot_;
        if (closed || idle > FOCUS_BACK_MS) giveFocusBack();
        if (closed || idle > PARK_AFTER_MS) park();
        lock.lock();
    }
    lock.unlock();
    giveFocusBack();
    park();
}

// Keep the window on screen and on top (without activating it) while shooting.
void LightGun::raise(HWND window) {
    if (raised_ == window) return;
    if (!focusMode_) {
        LONG_PTR ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (!(ex & WS_EX_NOACTIVATE)) SetWindowLongPtrW(window, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
    }
    RECT r;
    GetWindowRect(window, &r);
    UINT move = offscreen_ ? 0 : SWP_NOMOVE;  // offscreen mode parks it far left: bring it back
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | move);
    raised_ = window;
    hostLog("light gun: RetroArch window raised at %ld,%ld", offscreen_ ? 0 : r.left, offscreen_ ? 0 : r.top);
}

void LightGun::park() {
    if (!raised_) return;
    // HWND_BOTTOM also drops the topmost state.
    if (IsWindow(raised_)) SetWindowPos(raised_, HWND_BOTTOM, offscreen_ ? -8000 : 0, 0, 0, 0,
                                        SWP_NOSIZE | SWP_NOACTIVATE | (offscreen_ ? 0 : SWP_NOMOVE));
    raised_ = nullptr;
    hostLog("light gun: RetroArch window parked again");
}

void LightGun::giveFocusBack() {
    if (!focused_) return;
    focused_ = false;
    bool ok = previous_ && IsWindow(previous_) && bringForward(previous_);
    SetCursorPos(cursor_.x, cursor_.y);
    hostLog("light gun: focus back to %p (%s)", static_cast<void*>(previous_), ok ? "ok" : "failed");
    previous_ = nullptr;
}

void LightGun::fire(const Shot& shot) {
    HWND window = shot.window;
    if (!IsWindow(window)) return;
    if (raised_ && raised_ != window) raised_ = nullptr;  // RetroArch rebuilt its window
    raise(window);
    RECT client;
    POINT at{0, 0};
    if (!GetClientRect(window, &client) || !ClientToScreen(window, &at) || client.right < 2 || client.bottom < 2) return;
    at.x += std::clamp(LONG(shot.u * client.right), 0L, client.right - 1);
    at.y += std::clamp(LONG(shot.v * client.bottom), 0L, client.bottom - 1);
    if (!MonitorFromPoint(at, MONITOR_DEFAULTTONULL)) {
        hostLog("light gun: %ld,%ld is not on any monitor; shot dropped", at.x, at.y);
        return;
    }
    HWND top = GetAncestor(WindowFromPoint(at), GA_ROOT);
    if (top != window && covered_++ < 5) {
        wchar_t title[128] = L"";
        GetWindowTextW(top, title, 128);
        hostLog("light gun: another window is on top of RetroArch at %ld,%ld (%p \"%ls\")", at.x, at.y, static_cast<void*>(top), title);
    }
    bool ok = focusMode_ ? click(window, at, shot.hold) : touch(at, shot.hold);
    lastShot_ = GetTickCount64();
    if (shots_++ < 20) hostLog("light gun: shot at %.3f,%.3f -> screen %ld,%ld (%s)", shot.u, shot.v, at.x, at.y, ok ? "ok" : "failed");
}

bool LightGun::touch(POINT at, DWORD hold) {
    if (!touchReady_) {
        touchReady_ = InitializeTouchInjection(1, TOUCH_FEEDBACK_NONE) != FALSE;
        if (!touchReady_) {
            hostLog("light gun: touch injection unavailable (%lu); try light_gun=focus in arcade.ini", GetLastError());
            return false;
        }
    }
    POINTER_TOUCH_INFO t{};
    t.pointerInfo.pointerType = PT_TOUCH;
    t.pointerInfo.ptPixelLocation = at;
    t.touchMask = TOUCH_MASK_CONTACTAREA;
    t.rcContact = {at.x - 2, at.y - 2, at.x + 2, at.y + 2};
    t.pointerInfo.pointerFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    if (!InjectTouchInput(1, &t)) {
        hostLog("light gun: InjectTouchInput failed (%lu)", GetLastError());
        return false;
    }
    // A contact that is not updated gets cancelled, so keep it alive while the trigger is held.
    for (DWORD held = 0; held < hold; held += 16) {
        Sleep(16);
        t.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        InjectTouchInput(1, &t);
    }
    t.pointerInfo.pointerFlags = POINTER_FLAG_UP;
    bool ok = InjectTouchInput(1, &t) != FALSE;
    Sleep(GAP_MS);
    return ok;
}

bool LightGun::click(HWND window, POINT at, DWORD hold) {
    HWND current = GetForegroundWindow();
    if (current != window) {
        if (!focused_) {
            previous_ = current;
            GetCursorPos(&cursor_);
        }
        SetCursorPos(at.x, at.y);
        focused_ = true;
        if (!bringForward(window)) {
            hostLog("light gun: could not give RetroArch focus");
            return false;
        }
        Sleep(FOCUS_SETTLE_MS);
    } else {
        SetCursorPos(at.x, at.y);
        Sleep(20);  // a frame to take the new aim
    }
    mouseButton(MOUSEEVENTF_LEFTDOWN);
    Sleep(hold);
    mouseButton(MOUSEEVENTF_LEFTUP);
    Sleep(GAP_MS);
    return true;
}
