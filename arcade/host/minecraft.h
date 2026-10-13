// Minecraft Java Edition on the tablet. The MINECRAFT tile starts your Minecraft Launcher (pick
// a profile and PLAY, tapping the tablet), the game's window takes the tablet, and FREEZE turns
// the Touch controllers into Bedrock's controller layout as keyboard and mouse (Java has no
// controller support of its own). The game window has the desktop focus while FREEZE is on;
// Echo VR gets it back after, which also pauses the game (Minecraft pauses when it loses focus).
// Echo's own controls are untouched: outside FREEZE nothing here runs.
#pragma once
#include "apps.h"
#include "touchpad.h"
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace minecraft {

inline bool fileExists(const std::wstring& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Your launcher: arcade.ini [paths] minecraft=, else the usual places. The Microsoft Store
// launcher has no file to run: explorer starts it by its app id ("shell:AppsFolder\...").
inline std::wstring findLauncher(const std::wstring& configured) {
    if (fileExists(configured) || configured.rfind(L"shell:", 0) == 0) return configured;
    wchar_t buf[MAX_PATH];
    std::vector<std::wstring> candidates;
    if (ExpandEnvironmentStringsW(L"%ProgramFiles(x86)%\\Minecraft Launcher\\MinecraftLauncher.exe", buf, MAX_PATH)) candidates.push_back(buf);
    if (ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Programs\\Minecraft Launcher\\MinecraftLauncher.exe", buf, MAX_PATH)) candidates.push_back(buf);
    candidates.push_back(L"C:\\XboxGames\\Minecraft Launcher\\Content\\Minecraft.exe");
    for (auto& c : candidates)
        if (fileExists(c)) return c;
    if (ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Packages\\Microsoft.4297127D64EC6_8wekyb3d8bbwe", buf, MAX_PATH) && fileExists(buf))
        return L"shell:AppsFolder\\Microsoft.4297127D64EC6_8wekyb3d8bbwe!Minecraft";
    return L"";
}

// Before the game starts, two lines of Minecraft's options.txt: not full screen (it would take
// the whole monitor at its resolution) and paused when it loses focus (FREEZE off = paused).
inline void prepareOptions() {
    wchar_t path[MAX_PATH];
    if (!ExpandEnvironmentStringsW(L"%APPDATA%\\.minecraft\\options.txt", path, MAX_PATH)) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    const std::string sep = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines;
    for (size_t at = 0; at <= text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (end < text.size() || !line.empty()) lines.push_back(line);
        at = end + 1;
    }
    bool changed = false, sawPause = false;
    for (auto& line : lines) {
        std::string key = line.substr(0, line.find(':'));
        if (key == "fullscreen" && line != "fullscreen:false") { line = "fullscreen:false"; changed = true; }
        if (key == "pauseOnLostFocus") {
            sawPause = true;
            if (line != "pauseOnLostFocus:true") { line = "pauseOnLostFocus:true"; changed = true; }
        }
    }
    if (!sawPause) { lines.push_back("pauseOnLostFocus:true"); changed = true; }
    if (!changed) return;
    std::string out;
    for (auto& line : lines) out += line + sep;
    if (_wfopen_s(&f, path, L"wb") == 0 && f) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        hostLog("minecraft: options.txt: fullscreen off, pause on lost focus on");
    }
}

// The game's window (GLFW, "Minecraft ...") if it is up, else the launcher's.
struct Windows {
    HWND game = nullptr, launcher = nullptr;
};
inline Windows find(DWORD launcherPid) {
    struct Ctx { DWORD pid; Windows w; } ctx{launcherPid, {}};
    EnumWindows([](HWND hwnd, LPARAM p) -> BOOL {
        auto c = reinterpret_cast<Ctx*>(p);
        if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
        RECT r{};
        GetWindowRect(hwnd, &r);
        if (r.right - r.left < 100 || r.bottom - r.top < 100) return TRUE;
        wchar_t cls[64] = L"", title[128] = L"";
        GetClassNameW(hwnd, cls, 64);
        InternalGetWindowText(hwnd, title, 128);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) return TRUE;
        if (!c->w.game && !wcscmp(cls, L"GLFW30") && wcsstr(title, L"Minecraft")) c->w.game = hwnd;
        else if (!c->w.launcher && (pid == c->pid || wcsstr(title, L"Minecraft Launcher"))) c->w.launcher = hwnd;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.w;
}

// A process's biggest visible window (Echo VR's, to give the focus back to).
inline HWND mainWindowOf(DWORD pid) {
    struct Ctx { DWORD pid; HWND best; long long area; } ctx{pid, nullptr, 0};
    EnumWindows([](HWND hwnd, LPARAM p) -> BOOL {
        auto c = reinterpret_cast<Ctx*>(p);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != c->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
        RECT r{};
        GetWindowRect(hwnd, &r);
        long long area = (long long)(r.right - r.left) * (r.bottom - r.top);
        if (area > c->area) { c->best = hwnd; c->area = area; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// Desktop focus to a window. Windows lets the process that sent the last input change the
// foreground window, so a 0-pixel mouse move goes first (as lightgun.cpp and screen.cpp do).
inline bool focus(HWND window) {
    if (!window || !IsWindow(window)) return false;
    if (GetForegroundWindow() == window) return true;
    INPUT nudge{};
    nudge.type = INPUT_MOUSE;
    nudge.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &nudge, sizeof nudge);
    if (IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);
    bool ok = SetForegroundWindow(window) != FALSE;
    if (!ok) {
        DWORD current = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), self = GetCurrentThreadId();
        if (current && current != self) {
            AttachThreadInput(self, current, TRUE);
            ok = SetForegroundWindow(window) != FALSE;
            AttachThreadInput(self, current, FALSE);
        }
    }
    return ok;
}

// The Touch controllers as Bedrock's controller layout, sent as keyboard and mouse input to the
// game window while it has the focus (Minecraft's default keys):
//   left stick: walk (W A S D)            right stick: look (the mouse)
//   A: jump (Space)   B: sneak (Shift)   X: drop (Q)   Y: inventory (E)
//   right trigger: attack / destroy (left mouse)   left trigger: use / place (right mouse)
//   right grip: next hotbar slot (wheel)   left grip: previous slot
//   left stick click: sprint (Ctrl)   right stick click: change view (F5)
//   menu button, tapped: pause / back (Esc); held 1 s: FREEZE off
// In the launcher (no game yet) the right stick moves the mouse pointer and the right trigger
// clicks, so PLAY can be pressed with the controllers too.
class Input {
public:
    // Sends what changed. Nothing is sent unless `window` is the foreground window: keys must
    // never land in another program.
    void apply(const arcade::PadState& s, const PadFrame& f, float lookSpeed, HWND window) {
        if (!window || GetForegroundWindow() != window) return;
        float lx = s.stick[0][0], ly = s.stick[0][1];  // y up
        key('W', ly > 0.4f);
        key('S', ly < -0.4f);
        key('A', lx < -0.4f);
        key('D', lx > 0.4f);
        key(VK_SPACE, (s.buttons & arcade::BUTTON_A) != 0);
        key(VK_LSHIFT, (s.buttons & arcade::BUTTON_B) != 0);
        key('Q', (s.buttons & arcade::BUTTON_X) != 0);
        key('E', (s.buttons & arcade::BUTTON_Y) != 0);
        key(VK_LCONTROL, (s.buttons & arcade::BUTTON_LTHUMB) != 0);
        key(VK_F5, (s.buttons & arcade::BUTTON_RTHUMB) != 0);
        key(VK_ESCAPE, f.pad[PadStart]);
        mouse(0, s.trigger[1] > 0.5f);
        mouse(1, s.trigger[0] > 0.5f);
        bool nextSlot = s.grip[1] > 0.6f, prevSlot = s.grip[0] > 0.6f;
        if (nextSlot && !nextSlot_) wheel(-120);
        if (prevSlot && !prevSlot_) wheel(120);
        nextSlot_ = nextSlot;
        prevSlot_ = prevSlot;
        float rx = s.stick[1][0], ry = s.stick[1][1];
        if (std::fabs(rx) < 0.12f) rx = 0;
        if (std::fabs(ry) < 0.12f) ry = 0;
        lookX_ += rx * std::fabs(rx) * lookSpeed;  // squared: fine aim near the centre
        lookY_ -= ry * std::fabs(ry) * lookSpeed;
        int dx = int(lookX_), dy = int(lookY_);
        if (dx || dy) {
            lookX_ -= dx;
            lookY_ -= dy;
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = MOUSEEVENTF_MOVE;
            in.mi.dx = dx;
            in.mi.dy = dy;
            SendInput(1, &in, sizeof in);
        }
    }
    // Everything up (FREEZE off, the HOME menu, the game gone).
    void releaseAll() {
        for (auto& [vk, down] : held_)
            if (down) sendKey(vk, false);
        held_.clear();
        for (int b = 0; b < 2; b++)
            if (mouse_[b]) { sendMouse(b, false); mouse_[b] = false; }
        nextSlot_ = prevSlot_ = false;
        lookX_ = lookY_ = 0;
    }

private:
    void key(WORD vk, bool down) {
        auto it = held_.find(vk);
        bool was = it != held_.end() && it->second;
        if (was == down) return;
        held_[vk] = down;
        sendKey(vk, down);
    }
    static void sendKey(WORD vk, bool down) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof in);
    }
    void mouse(int button, bool down) {
        if (mouse_[button] == down) return;
        mouse_[button] = down;
        sendMouse(button, down);
    }
    static void sendMouse(int button, bool down) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = button == 0 ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP) : (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
        SendInput(1, &in, sizeof in);
    }
    static void wheel(int amount) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = DWORD(amount);
        SendInput(1, &in, sizeof in);
    }
    std::unordered_map<WORD, bool> held_;
    bool mouse_[2] = {false, false}, nextSlot_ = false, prevSlot_ = false;
    float lookX_ = 0, lookY_ = 0;
};

}  // namespace minecraft
