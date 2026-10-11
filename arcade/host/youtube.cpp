// YouTube for the YOUTUBE tile: yt-dlp finds the videos (search, playlists, mixes) and mpv,
// through its own yt-dlp hook, plays them. Nothing here talks to YouTube directly.
#include "media.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <cstdio>
#include <thread>

using namespace winrt::Windows::Data::Json;

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

void drain(HANDLE pipe, std::string& into) {
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(pipe, buf, sizeof(buf), &got, nullptr) && got) into.append(buf, got);
}

std::wstring lastLine(const std::string& text) {
    std::wstring w = widen(text), line;
    size_t end = w.find_last_not_of(L"\r\n ");
    if (end == std::wstring::npos) return {};
    size_t start = w.find_last_of(L"\r\n", end);
    line = w.substr(start == std::wstring::npos ? 0 : start + 1, end - (start == std::wstring::npos ? 0 : start + 1) + 1);
    if (line.rfind(L"ERROR: ", 0) == 0) line.erase(0, 7);
    return line;
}

// Runs yt-dlp with no console window and returns its exit code (-1: it could not start).
// A job object holds it and the process it unpacks into, so a timeout ends both.
int runYtdlp(const std::wstring& exe, const std::wstring& args, std::string& out, std::string& err, DWORD timeoutMs) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 0)) return -1;
    if (!CreatePipe(&errR, &errW, &sa, 0)) { CloseHandle(outR); CloseHandle(outW); return -1; }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
    }
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(outW);
    CloseHandle(errW);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(outR);
        CloseHandle(errR);
        if (job) CloseHandle(job);
        return -1;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::thread watchdog([&] {
        if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT) {
            hostLog("youtube: yt-dlp took over %lu s; stopping it", timeoutMs / 1000);
            if (job) TerminateJobObject(job, 1); else TerminateProcess(pi.hProcess, 1);
        }
    });
    std::thread errReader([&] { drain(errR, err); });
    drain(outR, out);
    errReader.join();
    watchdog.join();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(outR);
    CloseHandle(errR);
    if (job) CloseHandle(job);
    return int(code);
}

std::wstring text(const JsonObject& o, const wchar_t* name) {
    auto v = o.TryLookup(name);
    return v && v.ValueType() == JsonValueType::String ? std::wstring(v.GetString()) : std::wstring();
}

double number(const JsonObject& o, const wchar_t* name) {
    auto v = o.TryLookup(name);
    return v && v.ValueType() == JsonValueType::Number ? v.GetNumber() : -1;
}

// Arguments go to CreateProcess, not a shell: quotes and backslashes are the only hazards.
std::wstring quoted(std::wstring s) {
    std::wstring clean;
    for (wchar_t c : s) if (c != L'"' && c != L'\\' && c >= 32) clean += c;
    return L"\"" + clean + L"\"";
}

// yt-dlp -J prints one JSON object (ASCII, \u escapes). Flat lists hold id, title, channel, duration.
bool parseList(const std::string& json, std::wstring& title, std::vector<YtVideo>& out, std::wstring& error) {
    try {
        auto root = JsonObject::Parse(winrt::to_hstring(json));
        title = text(root, L"title");
        auto entries = root.TryLookup(L"entries");
        if (!entries || entries.ValueType() != JsonValueType::Array) { error = L"yt-dlp returned no list"; return false; }
        for (auto v : entries.GetArray()) {
            if (v.ValueType() != JsonValueType::Object) continue;
            auto e = v.GetObject();
            std::wstring id = text(e, L"id"), name = text(e, L"title");
            if (id.empty() || name == L"[Private video]" || name == L"[Deleted video]") continue;
            std::wstring channel = text(e, L"channel");
            if (channel.empty()) channel = text(e, L"uploader");
            double seconds = number(e, L"duration");
            std::wstring info = channel;
            if (seconds > 0) {
                int t = int(seconds), h = t / 3600, m = t / 60 % 60, s = t % 60;
                wchar_t b[32];
                if (h) swprintf_s(b, L"%d:%02d:%02d", h, m, s); else swprintf_s(b, L"%d:%02d", m, s);
                info += (info.empty() ? L"" : L"   -   ") + std::wstring(b);
            } else if (number(e, L"duration") < 0 && text(e, L"live_status") == L"is_live") {
                info += L"   -   LIVE";
            }
            out.push_back({name.empty() ? id : name, info, L"https://www.youtube.com/watch?v=" + id});
        }
        return true;
    } catch (...) {
        error = L"could not read yt-dlp's answer";
        return false;
    }
}

bool runList(const std::wstring& ytdlp, const std::wstring& args, std::wstring& title, std::vector<YtVideo>& out,
             std::wstring& error) {
    std::string so, se;
    int code = runYtdlp(ytdlp, L"--ignore-config --no-warnings --flat-playlist -J " + args, so, se, 60000);
    if (code == -1) { error = L"yt-dlp did not start (" + ytdlp + L")"; return false; }
    if (code != 0 || so.empty()) {
        error = lastLine(se);
        if (error.empty()) error = L"yt-dlp failed (exit " + std::to_wstring(code) + L")";
        hostLog("youtube: yt-dlp exit %d: %ls", code, error.c_str());
        return false;
    }
    return parseList(so, title, out, error);
}

}  // namespace

std::vector<YtLink> loadYouTubeLinks(const std::wstring& file) {
    std::vector<YtLink> links;
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"rb") || !f) return links;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.rfind("\xEF\xBB\xBF", 0) == 0) data.erase(0, 3);
    std::wstring all = widen(data), line;
    auto trim = [](std::wstring s) {
        size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
        return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
    };
    for (size_t i = 0; i <= all.size(); i++) {
        if (i < all.size() && all[i] != L'\n') { line += all[i]; continue; }
        std::wstring l = trim(line);
        line.clear();
        if (l.empty() || l[0] == L'#') continue;
        size_t bar = l.find_last_of(L'|');
        std::wstring name = bar == std::wstring::npos ? l : trim(l.substr(0, bar));
        std::wstring url = bar == std::wstring::npos ? l : trim(l.substr(bar + 1));
        if (url.find(L"youtu") == std::wstring::npos) continue;
        bool playlist = url.find(L"list=") != std::wstring::npos || url.find(L"/playlist") != std::wstring::npos;
        links.push_back({name.empty() ? url : name, url, playlist});
    }
    return links;
}

bool ytSearch(const std::wstring& ytdlp, const std::wstring& query, std::vector<YtVideo>& out, std::wstring& error) {
    std::wstring title;
    return runList(ytdlp, quoted(L"ytsearch20:" + query), title, out, error);
}

bool ytPlaylist(const std::wstring& ytdlp, const std::wstring& url, std::wstring& title, std::vector<YtVideo>& out,
                std::wstring& error) {
    return runList(ytdlp, L"--yes-playlist --playlist-end 200 " + quoted(url), title, out, error);
}

std::wstring ytUpdate(const std::wstring& ytdlp) {
    std::string so, se;
    int code = runYtdlp(ytdlp, L"-U", so, se, 120000);
    if (code == -1) return L"yt-dlp did not start";
    std::wstring said = lastLine(so + "\n" + se);
    hostLog("youtube: yt-dlp -U exit %d: %ls", code, said.c_str());
    return said.empty() ? L"yt-dlp update finished" : said;
}
