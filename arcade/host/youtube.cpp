// YouTube for the YOUTUBE tile: yt-dlp finds the videos (search, playlists, mixes) and mpv,
// through its own yt-dlp hook, plays them. Nothing here talks to YouTube directly.
#include "media.h"
#include "thumbs.h"
#include <set>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <cstdio>
#include <thread>
#include <algorithm>

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
            // The video's 320 x 180 picture as a JPEG (the ones yt-dlp lists come back as WebP,
            // which GDI+ cannot read; every YouTube video has this one, custom thumbnails included).
            std::wstring thumb = id.empty() ? L"" : L"https://i.ytimg.com/vi/" + id + L"/mqdefault.jpg";
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
            out.push_back({name.empty() ? id : name, info, L"https://www.youtube.com/watch?v=" + id, thumb});
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
    int code = runYtdlp(ytdlp, L"--ignore-config --no-warnings --flat-playlist -J " + ytRuntimeArgs() + args, so, se, 90000);
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

bool ytSearch(const std::wstring& ytdlp, const std::wstring& query, std::vector<YtVideo>& out, std::wstring& error, int count) {
    std::wstring title;
    return runList(ytdlp, quoted(L"ytsearch" + std::to_wstring(std::clamp(count, 1, 50)) + L":" + query), title, out, error);
}

bool ytFeed(const std::wstring& ytdlp, const std::wstring& cookies, std::vector<YtVideo>& out, std::wstring& error) {
    std::wstring title;
    return runList(ytdlp, L"--cookies " + quoted(cookies) + L" --playlist-end 40 :ytrec", title, out, error);
}

namespace {
std::wstring deno;
}
void ytSetDeno(const std::wstring& path) { deno = path; }
std::wstring ytRuntimeArgs() {
    if (deno.empty()) return L"";
    return L"--js-runtimes " + quoted(L"deno:" + deno) + L" --remote-components ejs:github ";
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

// ---------------------------------------------------------------- signing in (Google's device code)
namespace {
// YouTube's own TV client (the one TVs sign in with): the device code flow is open to it.
const char* const CLIENT_ID = "861556708454-d6dlm3lh05idd8npek18k6be8ba3oc68.apps.googleusercontent.com";
const char* const CLIENT_SECRET = "SboVhoG9s0rNafixCSGGKXAT";
const wchar_t* const TV_VERSION = L"7.20250312.16.00";

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring widen(const std::vector<uint8_t>& b) {
    if (b.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(b.data()), int(b.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(b.data()), int(b.size()), w.data(), n);
    return w;
}
std::string urlEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { char b[4]; sprintf_s(b, "%%%02X", c); out += b; }
    }
    return out;
}
bool parseJson(const std::vector<uint8_t>& body, JsonObject& out) {
    try { return !body.empty() && JsonObject::TryParse(widen(body), out); } catch (...) { return false; }
}
std::wstring str(const JsonObject& o, const wchar_t* key) {
    try { return o.HasKey(key) && o.Lookup(key).ValueType() == JsonValueType::String ? std::wstring(o.GetNamedString(key)) : L""; } catch (...) { return L""; }
}
double num(const JsonObject& o, const wchar_t* key) {
    try { return o.HasKey(key) && o.Lookup(key).ValueType() == JsonValueType::Number ? o.GetNamedNumber(key) : 0; } catch (...) { return 0; }
}
// A YouTube "text": {"simpleText": ...} or {"runs": [{"text": ...}, ...]} or a plain string.
std::wstring ytText(const IJsonValue& v) {
    try {
        if (!v) return L"";
        if (v.ValueType() == JsonValueType::String) return std::wstring(v.GetString());
        if (v.ValueType() != JsonValueType::Object) return L"";
        auto o = v.GetObject();
        if (!str(o, L"simpleText").empty()) return str(o, L"simpleText");
        if (o.HasKey(L"runs") && o.Lookup(L"runs").ValueType() == JsonValueType::Array) {
            std::wstring t;
            for (auto r : o.Lookup(L"runs").GetArray())
                if (r.ValueType() == JsonValueType::Object) t += str(r.GetObject(), L"text");
            return t;
        }
    } catch (...) {}
    return L"";
}
IJsonValue field(const JsonObject& o, const wchar_t* key) {
    try { return o.HasKey(key) ? o.Lookup(key) : nullptr; } catch (...) { return nullptr; }
}
// Every video in the answer, whatever renderer YouTube wrapped it in (web or TV shapes).
void collectVideos(const IJsonValue& v, std::vector<YtVideo>& out, std::set<std::wstring>& seen, int depth = 0) {
    if (!v || depth > 60) return;
    try {
        if (v.ValueType() == JsonValueType::Array) {
            for (auto e : v.GetArray()) collectVideos(e, out, seen, depth + 1);
            return;
        }
        if (v.ValueType() != JsonValueType::Object) return;
        auto o = v.GetObject();
        std::wstring id, title, info;
        if (!str(o, L"videoId").empty() && o.HasKey(L"title")) {  // videoRenderer, compactVideoRenderer, gridVideoRenderer...
            id = str(o, L"videoId");
            title = ytText(field(o, L"title"));
            std::wstring channel = ytText(field(o, L"ownerText"));
            if (channel.empty()) channel = ytText(field(o, L"shortBylineText"));
            if (channel.empty()) channel = ytText(field(o, L"longBylineText"));
            std::wstring length = ytText(field(o, L"lengthText"));
            info = channel + (channel.empty() || length.empty() ? L"" : L"  -  ") + length;
        } else if (!str(o, L"contentId").empty() && str(o, L"contentType") == L"TILE_CONTENT_TYPE_VIDEO") {  // tileRenderer (TV)
            id = str(o, L"contentId");
            auto meta = field(o, L"metadata");
            if (meta && meta.ValueType() == JsonValueType::Object) {
                auto tm = field(meta.GetObject(), L"tileMetadataRenderer");
                if (tm && tm.ValueType() == JsonValueType::Object) {
                    auto t = tm.GetObject();
                    title = ytText(field(t, L"title"));
                    auto lines = field(t, L"lines");
                    if (lines && lines.ValueType() == JsonValueType::Array) {
                        for (auto line : lines.GetArray()) {
                            if (line.ValueType() != JsonValueType::Object) continue;
                            auto lr = field(line.GetObject(), L"lineRenderer");
                            if (!lr || lr.ValueType() != JsonValueType::Object) continue;
                            auto items = field(lr.GetObject(), L"items");
                            if (!items || items.ValueType() != JsonValueType::Array) continue;
                            std::wstring text;
                            for (auto item : items.GetArray()) {
                                if (item.ValueType() != JsonValueType::Object) continue;
                                auto li = field(item.GetObject(), L"lineItemRenderer");
                                if (li && li.ValueType() == JsonValueType::Object) text += ytText(field(li.GetObject(), L"text"));
                            }
                            if (!text.empty()) info += (info.empty() ? L"" : L"  -  ") + text;
                        }
                    }
                }
            }
        }
        if (!id.empty() && !title.empty() && seen.insert(id).second)
            out.push_back({title, info, L"https://www.youtube.com/watch?v=" + id, L"https://i.ytimg.com/vi/" + id + L"/mqdefault.jpg"});
        for (auto pair : o) collectVideos(pair.Value(), out, seen, depth + 1);
    } catch (...) {}
}
}  // namespace

bool ytDeviceCode(std::wstring& deviceCode, std::wstring& userCode, std::wstring& url, int& interval, int& expires, std::wstring& error) {
    DWORD status = 0;
    std::string body = std::string("client_id=") + CLIENT_ID + "&scope=" + urlEncode("https://www.googleapis.com/auth/youtube");
    auto answer = httpRequest(L"https://oauth2.googleapis.com/device/code", L"POST", L"Content-Type: application/x-www-form-urlencoded\r\n", body, &status);
    JsonObject o;
    if (!parseJson(answer, o)) { error = L"Google did not answer (status " + std::to_wstring(status) + L")"; return false; }
    if (!str(o, L"error").empty()) { error = L"Google: " + str(o, L"error_description").substr(0, 120); return false; }
    deviceCode = str(o, L"device_code");
    userCode = str(o, L"user_code");
    url = str(o, L"verification_url");
    interval = int(num(o, L"interval"));
    expires = int(num(o, L"expires_in"));
    if (interval < 1) interval = 5;
    if (expires < 60) expires = 1800;
    return !deviceCode.empty() && !userCode.empty();
}

int ytDevicePoll(const std::wstring& deviceCode, std::wstring& refresh, std::wstring& access, int& expiresIn, std::wstring& error) {
    DWORD status = 0;
    std::string body = std::string("client_id=") + CLIENT_ID + "&client_secret=" + CLIENT_SECRET + "&device_code=" + urlEncode(narrow(deviceCode)) +
                       "&grant_type=" + urlEncode("urn:ietf:params:oauth:grant-type:device_code");
    auto answer = httpRequest(L"https://oauth2.googleapis.com/token", L"POST", L"Content-Type: application/x-www-form-urlencoded\r\n", body, &status);
    JsonObject o;
    if (!parseJson(answer, o)) { error = L"no answer (status " + std::to_wstring(status) + L")"; return status == 0 ? 0 : -1; }
    std::wstring err = str(o, L"error");
    if (err == L"authorization_pending") return 0;
    if (err == L"slow_down") return 2;
    if (!err.empty()) { error = err == L"access_denied" ? L"You said no on the phone" : err == L"expired_token" ? L"The code expired" : L"Google: " + err; return -1; }
    access = str(o, L"access_token");
    refresh = str(o, L"refresh_token");
    expiresIn = int(num(o, L"expires_in"));
    return access.empty() ? -1 : 1;
}

bool ytRefreshToken(const std::wstring& refresh, std::wstring& access, int& expiresIn, std::wstring& error) {
    DWORD status = 0;
    std::string body = std::string("client_id=") + CLIENT_ID + "&client_secret=" + CLIENT_SECRET + "&refresh_token=" + urlEncode(narrow(refresh)) + "&grant_type=refresh_token";
    auto answer = httpRequest(L"https://oauth2.googleapis.com/token", L"POST", L"Content-Type: application/x-www-form-urlencoded\r\n", body, &status);
    JsonObject o;
    if (!parseJson(answer, o)) { error = L"Google did not answer (status " + std::to_wstring(status) + L")"; return false; }
    if (!str(o, L"error").empty()) { error = L"Google: " + str(o, L"error") + L" (sign in again)"; return false; }
    access = str(o, L"access_token");
    expiresIn = int(num(o, L"expires_in"));
    return !access.empty();
}

bool ytFeedSignedIn(const std::wstring& access, std::vector<YtVideo>& out, std::wstring& error, const std::wstring& dumpPath) {
    DWORD status = 0;
    std::string body = "{\"context\":{\"client\":{\"clientName\":\"TVHTML5\",\"clientVersion\":\"" + narrow(TV_VERSION) +
                       "\",\"hl\":\"en\",\"gl\":\"US\"}},\"browseId\":\"FEwhat_to_watch\"}";
    std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + access + L"\r\nX-YouTube-Client-Name: 7\r\nX-YouTube-Client-Version: " +
                           std::wstring(TV_VERSION) + L"\r\nUser-Agent: Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version\r\n";
    auto answer = httpRequest(L"https://www.youtube.com/youtubei/v1/browse?prettyPrint=false", L"POST", headers, body, &status);
    JsonObject o;
    if (!parseJson(answer, o)) { error = L"YouTube did not answer (status " + std::to_wstring(status) + L")"; return false; }
    std::set<std::wstring> seen;
    collectVideos(o, out, seen);
    hostLog("youtube: signed-in feed: status %lu, %zu videos (%zu bytes)", status, out.size(), answer.size());
    if (out.empty()) {
        FILE* f = nullptr;
        if (!dumpPath.empty() && _wfopen_s(&f, dumpPath.c_str(), L"wb") == 0 && f) { fwrite(answer.data(), 1, answer.size(), f); fclose(f); }
        error = status == 200 ? L"YouTube answered but no videos were found (the answer was saved for a look)" : L"YouTube refused (status " + std::to_wstring(status) + L")";
        return false;
    }
    return true;
}
