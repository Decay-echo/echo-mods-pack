// "YouTube on this PC": finds the browser window showing YouTube (the tablet shows it through
// WindowCapture) and controls playback through Windows' media sessions, so the browser keeps its
// own sound and never needs focus.
#include "media.h"
#include <dwmapi.h>
#include <objbase.h>  // UIAutomation.h needs COM's `interface` (host.h's Windows headers are lean)
#include <UIAutomation.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <cwctype>

using namespace winrt::Windows::Media::Control;

namespace {

struct Candidate { HWND hwnd = nullptr; long long score = -1; };

BOOL CALLBACK findYouTube(HWND hwnd, LPARAM param) {
    auto best = reinterpret_cast<Candidate*>(param);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    wchar_t title[512];
    // InternalGetWindowText sends no message, so a busy browser cannot stall the tablet.
    if (!InternalGetWindowText(hwnd, title, 512) || !wcsstr(title, L"YouTube")) return TRUE;
    if (wcsstr(title, L"EchoArcade")) return TRUE;
    BOOL cloaked = FALSE;  // on another virtual desktop, or a hidden UWP shell window
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    RECT r{};
    GetWindowRect(hwnd, &r);
    long long area = (long long)(r.right - r.left) * (r.bottom - r.top);
    // Prefer a window that can be captured now: not minimized, not cloaked; then the biggest.
    long long score = area + (IsIconic(hwnd) ? 0 : (1LL << 40)) + (cloaked ? 0 : (1LL << 41));
    if (score > best->score) { best->hwnd = hwnd; best->score = score; }
    return TRUE;
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(towlower(c));
    return s;
}

GlobalSystemMediaTransportControlsSessionManager& manager() {
    static GlobalSystemMediaTransportControlsSessionManager m = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    return m;
}

// The session whose song is the one in the window's title; else a browser's; else Windows' current one.
GlobalSystemMediaTransportControlsSession pick(const std::wstring& windowTitle) {
    GlobalSystemMediaTransportControlsSession browser{nullptr};
    std::wstring want = lower(windowTitle);
    for (auto s : manager().GetSessions()) {
        try {
            auto props = s.TryGetMediaPropertiesAsync().get();
            std::wstring title = lower(std::wstring(props.Title()));
            if (!title.empty() && !want.empty() && (want.find(title) != std::wstring::npos || title.find(want) != std::wstring::npos))
                return s;
            std::wstring app = lower(std::wstring(s.SourceAppUserModelId()));
            if (!browser && (app.find(L"chrome") != std::wstring::npos || app.find(L"msedge") != std::wstring::npos ||
                             app.find(L"firefox") != std::wstring::npos || app.find(L"opera") != std::wstring::npos ||
                             app.find(L"brave") != std::wstring::npos || app.find(L"vivaldi") != std::wstring::npos))
                browser = s;
        } catch (...) {}
    }
    return browser ? browser : manager().GetCurrentSession();
}

void mediaKey(WORD vk) {  // fallback when Windows lists no media session: the keyboard's media keys
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

double seconds(winrt::Windows::Foundation::TimeSpan t) { return std::chrono::duration<double>(t).count(); }

}  // namespace

HWND findYouTubeWindow() {
    Candidate best;
    EnumWindows(findYouTube, reinterpret_cast<LPARAM>(&best));
    return best.hwnd;
}

std::wstring youTubeWindowTitle(HWND window) {
    wchar_t buf[512];
    if (!InternalGetWindowText(window, buf, 512)) return {};
    std::wstring t = buf;
    size_t cut = t.find(L" - YouTube");
    if (cut != std::wstring::npos) t.resize(cut);
    if (t.size() > 3 && t[0] == L'(') {  // "(3) Song": the notification count
        size_t close = t.find(L") ");
        if (close != std::wstring::npos && close < 6) t.erase(0, close + 2);
    }
    return t == L"YouTube" ? std::wstring() : t;
}

MediaInfo mediaInfo(const std::wstring& title) {
    MediaInfo info;
    try {
        auto s = pick(title);
        if (!s) return info;
        auto props = s.TryGetMediaPropertiesAsync().get();
        info.title = props.Title();
        info.artist = props.Artist();
        auto playback = s.GetPlaybackInfo();
        info.playing = playback.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
        info.canSeek = playback.Controls().IsPlaybackPositionEnabled();
        auto timeline = s.GetTimelineProperties();
        double end = seconds(timeline.EndTime() - timeline.StartTime());
        if (end > 0) {
            double pos = seconds(timeline.Position() - timeline.StartTime());
            // The position is as of the session's last update; while playing it has moved on.
            if (info.playing) pos += std::chrono::duration<double>(winrt::clock::now() - timeline.LastUpdatedTime()).count();
            info.duration = end;
            info.position = std::clamp(pos, 0.0, end);
        }
        info.valid = true;
    } catch (...) {}
    return info;
}

// YouTube's skip button is "Skip", "Skip Ad" or "Skip Ads" depending on its layout version; the
// page also has a "Skip navigation" link at the top, which is not an ad.
static bool isSkipAdName(std::wstring name) {
    name = lower(name);
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
    if (name.find(L"navigation") != std::wstring::npos) return false;
    return name == L"skip" || name.rfind(L"skip ad", 0) == 0;
}

namespace {

struct SkipButton { winrt::com_ptr<IUIAutomationElement> element; RECT rect{}; std::wstring name; };

// YouTube's Skip button, if an ad is offering it right now (visible and enabled).
bool findSkipButton(IUIAutomation* uia, IUIAutomationElement* root, SkipButton& out) {
    VARIANT button{};
    button.vt = VT_I4;
    button.lVal = UIA_ButtonControlTypeId;
    winrt::com_ptr<IUIAutomationCondition> isButton, namedSkip, both;
    winrt::check_hresult(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, button, isButton.put()));
    VARIANT skip{};
    skip.vt = VT_BSTR;
    skip.bstrVal = SysAllocString(L"skip");
    // Buttons whose name contains "skip": the browser does the search, so it stays quick.
    HRESULT hr = uia->CreatePropertyConditionEx(UIA_NamePropertyId, skip,
                                                PropertyConditionFlags(PropertyConditionFlags_IgnoreCase | PropertyConditionFlags_MatchSubstring),
                                                namedSkip.put());
    VariantClear(&skip);
    if (SUCCEEDED(hr)) winrt::check_hresult(uia->CreateAndCondition(isButton.get(), namedSkip.get(), both.put()));
    else both = isButton;  // Windows before 1809: every button, filtered by name below
    winrt::com_ptr<IUIAutomationElementArray> found;
    winrt::check_hresult(root->FindAll(TreeScope_Descendants, both.get(), found.put()));
    int count = 0;
    if (found) found->get_Length(&count);
    for (int i = 0; i < count; i++) {
        winrt::com_ptr<IUIAutomationElement> element;
        if (FAILED(found->GetElement(i, element.put())) || !element) continue;
        BSTR name = nullptr;
        if (FAILED(element->get_CurrentName(&name)) || !name) continue;
        std::wstring text(name, SysStringLen(name));
        SysFreeString(name);
        BOOL offscreen = TRUE, enabled = FALSE;
        RECT rect{};
        element->get_CurrentIsOffscreen(&offscreen);
        element->get_CurrentIsEnabled(&enabled);
        element->get_CurrentBoundingRectangle(&rect);
        if (!isSkipAdName(text) || offscreen || !enabled || rect.right <= rect.left || rect.bottom <= rect.top) continue;
        out = {element, rect, text};
        return true;
    }
    return false;
}

// A mouse click posted to the browser window at a screen point: Chrome turns it into a real click
// on the page (with coordinates and pointer events), and nothing moves the cursor or takes focus.
void postClick(HWND window, POINT at) {
    ScreenToClient(window, &at);
    LPARAM where = MAKELPARAM(at.x, at.y);
    PostMessageW(window, WM_MOUSEMOVE, 0, where);
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, where);
    PostMessageW(window, WM_LBUTTONUP, 0, where);
}

// Safety net for "Echo VR stays the front app": the click is posted, so the browser should never
// come forward, but if it did, the window that was in front (Echo VR) gets it back at once. Same
// method as the light gun's (lightgun.cpp): Windows lets the process that sent the last input
// change the foreground window, so send a 0-pixel mouse move first.
void keepFront(HWND before, HWND browser) {
    HWND now = GetForegroundWindow();
    if (!before || now == before || !IsWindow(before)) return;
    DWORD browserPid = 0, nowPid = 0;
    GetWindowThreadProcessId(browser, &browserPid);
    GetWindowThreadProcessId(now, &nowPid);
    if (now != browser && nowPid != browserPid) return;  // something else took over: not ours to undo
    INPUT nudge{};
    nudge.type = INPUT_MOUSE;
    nudge.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &nudge, sizeof(nudge));
    bool ok = SetForegroundWindow(before) != FALSE;
    if (!ok) {
        DWORD current = GetWindowThreadProcessId(now, nullptr), self = GetCurrentThreadId();
        if (current && current != self) {
            AttachThreadInput(self, current, TRUE);
            ok = SetForegroundWindow(before) != FALSE;
            AttachThreadInput(self, current, FALSE);
        }
    }
    hostLog("youtube: the browser came to the front after a skip; gave it back to %p (%s)", static_cast<void*>(before),
            ok ? "ok" : "failed");
}

// The ad was skipped once its Skip button is gone (the next ad of a pair brings a new, still
// counting-down one, which is not offered yet).
bool skipGone(IUIAutomation* uia, IUIAutomationElement* root) {
    Sleep(1500);
    SkipButton still;
    return !findSkipButton(uia, root, still);
}

}  // namespace

bool skipYouTubeAd(HWND window) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool skipped = false;
    try {
        winrt::com_ptr<IUIAutomation> uia;
        winrt::check_hresult(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put())));
        winrt::com_ptr<IUIAutomationElement> root;
        winrt::check_hresult(uia->ElementFromHandle(window, root.put()));
        SkipButton skip;
        if (findSkipButton(uia.get(), root.get(), skip)) {
            // 1. A real click on the button. YouTube's Skip button ignores accessibility "presses"
            //    (the method the player's other buttons accept), so this comes first.
            POINT centre{(skip.rect.left + skip.rect.right) / 2, (skip.rect.top + skip.rect.bottom) / 2};
            HWND front = GetForegroundWindow();  // normally Echo VR: it must stay in front
            postClick(window, centre);
            skipped = skipGone(uia.get(), root.get());
            keepFront(front, window);
            hostLog("youtube: clicked \"%ls\" at %ld,%ld: %s", skip.name.c_str(), centre.x, centre.y,
                    skipped ? "ad skipped" : "the ad is still there");
            // 2. If the ad stayed, the accessibility press, in case this layout wants that instead.
            if (!skipped) {
                winrt::com_ptr<IUIAutomationInvokePattern> invoke;
                if (SUCCEEDED(skip.element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.put()))) && invoke &&
                    SUCCEEDED(invoke->Invoke())) {
                    skipped = skipGone(uia.get(), root.get());
                    keepFront(front, window);
                    hostLog("youtube: pressed \"%ls\" through accessibility: %s", skip.name.c_str(),
                            skipped ? "ad skipped" : "the ad is still there");
                }
            }
        }
    } catch (...) {}
    if (SUCCEEDED(co)) CoUninitialize();
    return skipped;
}

// ---- the page, like a tablet ----
namespace {

// Runs `f(uia, window element)` with UI Automation on this (worker) thread.
template <class F> bool withWindow(HWND window, F&& f) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    try {
        winrt::com_ptr<IUIAutomation> uia;
        winrt::check_hresult(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put())));
        winrt::com_ptr<IUIAutomationElement> root;
        winrt::check_hresult(uia->ElementFromHandle(window, root.put()));
        ok = f(uia.get(), root.get());
    } catch (...) {}
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

// The first element of a control type whose name passes `want` (lower-cased), in page order.
template <class W> winrt::com_ptr<IUIAutomationElement> findNamed(IUIAutomation* uia, IUIAutomationElement* root, long type, W&& want) {
    VARIANT v{};
    v.vt = VT_I4;
    v.lVal = type;
    winrt::com_ptr<IUIAutomationCondition> condition;
    winrt::check_hresult(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, condition.put()));
    winrt::com_ptr<IUIAutomationElementArray> found;
    winrt::check_hresult(root->FindAll(TreeScope_Descendants, condition.get(), found.put()));
    int count = 0;
    if (found) found->get_Length(&count);
    for (int i = 0; i < count; i++) {
        winrt::com_ptr<IUIAutomationElement> e;
        if (FAILED(found->GetElement(i, e.put())) || !e) continue;
        BSTR name = nullptr;
        if (FAILED(e->get_CurrentName(&name)) || !name) continue;
        std::wstring text(name, SysStringLen(name));
        SysFreeString(name);
        if (want(lower(text))) return e;
    }
    return nullptr;
}

// Presses an element: a click at its middle if it is on screen, else the accessibility press.
bool press(HWND window, IUIAutomationElement* e) {
    RECT r{};
    BOOL offscreen = TRUE;
    e->get_CurrentBoundingRectangle(&r);
    e->get_CurrentIsOffscreen(&offscreen);
    if (!offscreen && r.right > r.left && r.bottom > r.top) {
        postClick(window, {(r.left + r.right) / 2, (r.top + r.bottom) / 2});
        return true;
    }
    winrt::com_ptr<IUIAutomationInvokePattern> invoke;
    return SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.put()))) && invoke && SUCCEEDED(invoke->Invoke());
}

LPARAM clientPoint(HWND window, POINT at) {
    ScreenToClient(window, &at);
    return MAKELPARAM(at.x, at.y);
}

}  // namespace

bool windowFrame(HWND window, RECT& screen) {
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &screen, sizeof(screen))) && screen.right > screen.left;
}

bool youTubePageArea(HWND window, RECT& screen) {
    return withWindow(window, [&](IUIAutomation* uia, IUIAutomationElement* root) {
        VARIANT v{};
        v.vt = VT_I4;
        v.lVal = UIA_DocumentControlTypeId;
        winrt::com_ptr<IUIAutomationCondition> isDocument;
        winrt::check_hresult(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, isDocument.put()));
        winrt::com_ptr<IUIAutomationElement> page;  // the tab's page comes before any frame inside it
        if (FAILED(root->FindFirst(TreeScope_Descendants, isDocument.get(), page.put())) || !page) return false;
        RECT r{};
        page->get_CurrentBoundingRectangle(&r);
        if (r.right - r.left < 100 || r.bottom - r.top < 100) return false;
        screen = r;
        return true;
    });
}

void pageHover(HWND window, POINT at) { PostMessageW(window, WM_MOUSEMOVE, 0, clientPoint(window, at)); }

void pageTap(HWND window, POINT at) { postClick(window, at); }

void pageScroll(HWND window, POINT at, int right, int up) {
    LPARAM where = MAKELPARAM(at.x, at.y);  // wheel messages carry screen coordinates
    if (up) PostMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, short(std::clamp(up, -2400, 2400))), where);
    if (right) PostMessageW(window, WM_MOUSEHWHEEL, MAKEWPARAM(0, short(std::clamp(right, -2400, 2400))), where);
}

bool browserBack(HWND window) {
    // The browser's own Back button; else the "back" app command a mouse's side button sends.
    bool ok = withWindow(window, [&](IUIAutomation* uia, IUIAutomationElement* root) {
        auto back = findNamed(uia, root, UIA_ButtonControlTypeId, [](const std::wstring& n) { return n == L"back"; });
        winrt::com_ptr<IUIAutomationInvokePattern> invoke;
        return back && SUCCEEDED(back->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.put()))) && invoke &&
               SUCCEEDED(invoke->Invoke());
    });
    if (!ok) ok = PostMessageW(window, WM_APPCOMMAND, WPARAM(window), MAKELPARAM(0, APPCOMMAND_BROWSER_BACKWARD | FAPPCOMMAND_MOUSE)) != FALSE;
    hostLog("youtube: back (%s)", ok ? "ok" : "failed");
    return ok;
}

bool youTubeHome(HWND window) {
    bool ok = withWindow(window, [&](IUIAutomation* uia, IUIAutomationElement* root) {
        auto home = findNamed(uia, root, UIA_HyperlinkControlTypeId, [](const std::wstring& n) { return n == L"youtube home"; });
        winrt::com_ptr<IUIAutomationInvokePattern> invoke;
        return home && SUCCEEDED(home->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.put()))) && invoke &&
               SUCCEEDED(invoke->Invoke());
    });
    hostLog("youtube: home (%s)", ok ? "ok" : "not found");
    return ok;
}

bool youTubeSearch(HWND window, const std::wstring& query) {
    bool ok = withWindow(window, [&](IUIAutomation* uia, IUIAutomationElement* root) {
        // YouTube's search box ("Search" / "Search or ask a question"), not the browser's address bar.
        auto isBox = [](const std::wstring& n) { return n.rfind(L"search", 0) == 0 && n.find(L"address") == std::wstring::npos; };
        auto box = findNamed(uia, root, UIA_ComboBoxControlTypeId, isBox);
        if (!box) box = findNamed(uia, root, UIA_EditControlTypeId, isBox);
        if (!box) return false;
        winrt::com_ptr<IUIAutomationValuePattern> value;
        if (FAILED(box->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(value.put()))) || !value) return false;
        BSTR text = SysAllocString(query.c_str());
        HRESULT hr = value->SetValue(text);
        SysFreeString(text);
        if (FAILED(hr)) return false;
        Sleep(150);  // let the page take in the new text before the button reads it
        auto button = findNamed(uia, root, UIA_ButtonControlTypeId, [](const std::wstring& n) { return n == L"search"; });
        return button && press(window, button.get());
    });
    hostLog("youtube: search \"%ls\" (%s)", query.c_str(), ok ? "ok" : "no search box");
    return ok;
}

bool mediaCommand(MediaCommand command, const std::wstring& title, double to) {
    try {
        auto s = pick(title);
        if (s) {
            switch (command) {
                case MediaCommand::PlayPause: return s.TryTogglePlayPauseAsync().get();
                case MediaCommand::Next: return s.TrySkipNextAsync().get();
                case MediaCommand::Previous: return s.TrySkipPreviousAsync().get();
                case MediaCommand::Seek: {
                    auto timeline = s.GetTimelineProperties();
                    auto ticks = (timeline.StartTime() + std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
                                                             std::chrono::duration<double>(std::max(0.0, to)))).count();
                    return s.TryChangePlaybackPositionAsync(ticks).get();
                }
            }
        }
    } catch (...) {}
    switch (command) {
        case MediaCommand::PlayPause: mediaKey(VK_MEDIA_PLAY_PAUSE); return true;
        case MediaCommand::Next: mediaKey(VK_MEDIA_NEXT_TRACK); return true;
        case MediaCommand::Previous: mediaKey(VK_MEDIA_PREV_TRACK); return true;
        default: return false;
    }
}
