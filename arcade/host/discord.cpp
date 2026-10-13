// Discord through the desktop client's local pipe (see discord.h).
#include "discord.h"
#include "thumbs.h"
#include <objbase.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <algorithm>
#include <chrono>
#include <ctime>

using namespace winrt::Windows::Data::Json;

namespace {

constexpr uint32_t OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4;
const wchar_t* const SCOPES_FULL = L"[\"identify\",\"guilds\",\"rpc\",\"rpc.voice.read\",\"rpc.voice.write\",\"rpc.notifications.read\",\"messages.read\"]";
const wchar_t* const SCOPES_LESS = L"[\"identify\",\"guilds\",\"rpc\",\"rpc.voice.read\",\"rpc.voice.write\"]";

uint64_t now() { return GetTickCount64(); }

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring widen(const char* s, size_t len) {
    if (!len) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, int(len), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, int(len), w.data(), n);
    return w;
}
std::wstring jsonString(const std::wstring& s) {  // quoted and escaped
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { out += L'\\'; out += c; }
        else if (c == L'\n') out += L"\\n";
        else if (c == L'\r') out += L"\\r";
        else if (c < 32) { wchar_t b[8]; swprintf_s(b, L"\\u%04x", unsigned(c)); out += b; }
        else out += c;
    }
    return out + L"\"";
}
std::string urlEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { char b[4]; sprintf_s(b, "%%%02X", c); out += b; }
    }
    return out;
}
std::wstring str(const JsonObject& o, const wchar_t* key) {
    try { return o.HasKey(key) && o.Lookup(key).ValueType() == JsonValueType::String ? std::wstring(o.GetNamedString(key)) : L""; } catch (...) { return L""; }
}
double num(const JsonObject& o, const wchar_t* key, double fallback = 0) {
    try { return o.HasKey(key) && o.Lookup(key).ValueType() == JsonValueType::Number ? o.GetNamedNumber(key) : fallback; } catch (...) { return fallback; }
}
bool flag(const JsonObject& o, const wchar_t* key) {
    try { return o.HasKey(key) && o.Lookup(key).ValueType() == JsonValueType::Boolean && o.GetNamedBoolean(key); } catch (...) { return false; }
}
JsonObject obj(const JsonObject& o, const wchar_t* key) {
    try { return o.HasKey(key) && o.Lookup(key).ValueType() == JsonValueType::Object ? o.GetNamedObject(key) : nullptr; } catch (...) { return nullptr; }
}
JsonArray arr(const JsonObject& o, const wchar_t* key) {
    try { return o.HasKey(key) && o.Lookup(key).ValueType() == JsonValueType::Array ? o.GetNamedArray(key) : nullptr; } catch (...) { return nullptr; }
}
DiscordUser userOf(const JsonObject& u) {
    DiscordUser out;
    if (!u) return out;
    out.id = str(u, L"id");
    out.name = str(u, L"global_name");
    if (out.name.empty()) out.name = str(u, L"username");
    out.avatar = str(u, L"avatar");
    return out;
}
std::wstring trim32(const std::wstring& s) { return s.size() > 31 ? s.substr(0, 31) : s; }

}  // namespace

Discord::Discord() {}
Discord::~Discord() { stop(); }

void Discord::configure(const std::wstring& clientId, const std::wstring& clientSecret, const std::wstring& loginPath) {
    clientId_ = clientId;
    clientSecret_ = clientSecret;
    loginPath_ = loginPath;
}

DiscordState Discord::state() const {
    std::lock_guard<std::mutex> lock(stateLock_);
    return state_;
}
void Discord::setStatus(const std::wstring& s) {
    std::lock_guard<std::mutex> lock(stateLock_);
    if (state_.status != s) { state_.status = s; state_.serial++; }
}
void Discord::changed() {
    std::lock_guard<std::mutex> lock(stateLock_);
    state_.serial++;
}

void Discord::start() {
    if (running_) return;
    stopping_ = false;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void Discord::stop() {
    if (!running_) return;
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
    for (auto& t : avatarThreads_) if (t.joinable()) t.join();
    avatarThreads_.clear();
    running_ = false;
}

// ---- the pipe

bool Discord::connect() {
    for (int i = 0; i < 10; i++) {
        wchar_t name[64];
        swprintf_s(name, L"\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        pipe_ = h;
        return true;
    }
    return false;
}

bool Discord::writeFrame(uint32_t op, const std::string& json) {
    std::lock_guard<std::mutex> lock(writeLock_);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    std::string frame(8, '\0');
    uint32_t len = uint32_t(json.size());
    memcpy(frame.data(), &op, 4);
    memcpy(frame.data() + 4, &len, 4);
    frame += json;
    DWORD written = 0;
    return WriteFile(pipe_, frame.data(), DWORD(frame.size()), &written, nullptr) && written == frame.size();
}

// One frame, or false after `timeoutMs` with nothing complete (the bytes so far are kept).
bool Discord::readFrame(uint32_t& op, std::wstring& json, int timeoutMs) {
    uint64_t until = now() + uint64_t(timeoutMs);
    while (!stopping_) {
        if (readBuffer_.size() >= 8) {
            uint32_t len = 0;
            memcpy(&op, readBuffer_.data(), 4);
            memcpy(&len, readBuffer_.data() + 4, 4);
            if (len > (16u << 20)) { readBuffer_.clear(); return false; }
            if (readBuffer_.size() >= 8 + len) {
                json = widen(readBuffer_.data() + 8, len);
                readBuffer_.erase(0, 8 + len);
                return true;
            }
        }
        DWORD available = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) return false;  // the pipe broke
        if (!available) {
            if (now() >= until) return false;
            Sleep(15);
            continue;
        }
        std::string chunk(available, '\0');
        DWORD got = 0;
        if (!ReadFile(pipe_, chunk.data(), available, &got, nullptr)) return false;
        readBuffer_.append(chunk.data(), got);
    }
    return false;
}

bool Discord::handshake() {
    std::string hello = "{\"v\":1,\"client_id\":" + narrow(jsonString(clientId_)) + "}";
    if (!writeFrame(OP_HANDSHAKE, hello)) return false;
    uint32_t op = 0;
    std::wstring json;
    if (!readFrame(op, json, 5000)) return false;
    JsonObject o;
    if (!JsonObject::TryParse(json, o)) return false;
    if (op == OP_CLOSE) {
        hostLog("discord: handshake refused: %ls", json.c_str());
        setStatus(L"Discord refused the app id (" + str(o, L"message") + L")");
        return false;
    }
    auto data = obj(o, L"data");
    DiscordUser me = data ? userOf(obj(data, L"user")) : DiscordUser{};
    {
        std::lock_guard<std::mutex> lock(stateLock_);
        state_.pipe = true;
        state_.me = me;
        state_.serial++;
    }
    hostLog("discord: pipe open; Discord's user is %ls", me.name.c_str());
    return true;
}

void Discord::send(const std::wstring& cmd, const std::wstring& argsJson, Reply reply) {
    std::wstring nonce = std::to_wstring(++nonce_) + L"-" + std::to_wstring(now());
    if (reply) pending_[nonce] = reply;
    std::wstring json = L"{\"cmd\":" + jsonString(cmd) + L",\"args\":" + (argsJson.empty() ? L"{}" : argsJson) + L",\"nonce\":" + jsonString(nonce) + L"}";
    if (!writeFrame(OP_FRAME, narrow(json))) { pending_.erase(nonce); if (reply) reply(nullptr, L"the pipe is gone"); }
}
void Discord::subscribe(const std::wstring& evt, const std::wstring& argsJson) {
    std::wstring nonce = std::to_wstring(++nonce_);
    std::wstring json = L"{\"cmd\":\"SUBSCRIBE\",\"evt\":" + jsonString(evt) + L",\"args\":" + (argsJson.empty() ? L"{}" : argsJson) + L",\"nonce\":" + jsonString(nonce) + L"}";
    writeFrame(OP_FRAME, narrow(json));
}
void Discord::unsubscribe(const std::wstring& evt, const std::wstring& argsJson) {
    std::wstring nonce = std::to_wstring(++nonce_);
    std::wstring json = L"{\"cmd\":\"UNSUBSCRIBE\",\"evt\":" + jsonString(evt) + L",\"args\":" + (argsJson.empty() ? L"{}" : argsJson) + L",\"nonce\":" + jsonString(nonce) + L"}";
    writeFrame(OP_FRAME, narrow(json));
}

void Discord::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    uint64_t lastTry = 0;
    while (!stopping_) {
        if (pipe_ == INVALID_HANDLE_VALUE) {
            if (now() - lastTry < 3000) { Sleep(100); continue; }
            lastTry = now();
            if (!configured()) { setStatus(L"Not set up yet"); Sleep(500); continue; }
            readBuffer_.clear();
            if (!connect()) { setStatus(L"Discord is not running on this PC"); continue; }
            if (!handshake()) { CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; continue; }
            std::wstring access, refresh;
            uint64_t expires = 0;
            if (loadLogin(access, refresh, expires)) {
                if (expires < uint64_t(time(nullptr)) + 300 && !refresh.empty()) {
                    int expiresIn = 0;
                    std::wstring error;
                    std::string body = "client_id=" + urlEncode(narrow(clientId_)) + "&client_secret=" + urlEncode(narrow(clientSecret_)) +
                                       "&grant_type=refresh_token&refresh_token=" + urlEncode(narrow(refresh));
                    std::wstring newRefresh;
                    if (tokenRequest(body, access, newRefresh, expiresIn, error)) saveLogin(access, newRefresh.empty() ? refresh : newRefresh, expiresIn);
                    else { hostLog("discord: token refresh failed: %ls", error.c_str()); access.clear(); }
                }
                if (!access.empty()) authenticate(access);
                else setStatus(L"Tap CONNECT to sign in again");
            } else {
                setStatus(L"Tap CONNECT (Discord asks on your desktop once)");
            }
        }
        // actions from the tablet
        for (;;) {
            std::function<void()> action;
            {
                std::lock_guard<std::mutex> lock(actionsLock_);
                if (actions_.empty()) break;
                action = std::move(actions_.front());
                actions_.pop_front();
            }
            action();
        }
        uint32_t op = 0;
        std::wstring json;
        if (readFrame(op, json, 50)) {
            onFrame(op, json);
        } else if (pipe_ != INVALID_HANDLE_VALUE) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) {  // Discord closed
                hostLog("discord: the pipe closed");
                CloseHandle(pipe_);
                pipe_ = INVALID_HANDLE_VALUE;
                pending_.clear();
                std::lock_guard<std::mutex> lock(stateLock_);
                state_.pipe = false;
                state_.authed = false;
                state_.members.clear();
                state_.channelId.clear();
                state_.status = L"Discord closed; waiting for it";
                state_.serial++;
            }
        }
    }
    if (pipe_ != INVALID_HANDLE_VALUE) { writeFrame(OP_CLOSE, "{}"); CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; }
    CoUninitialize();
}

void Discord::onFrame(uint32_t op, const std::wstring& json) {
    if (op == OP_PING) { writeFrame(OP_PONG, narrow(json)); return; }
    if (op == OP_CLOSE) {
        hostLog("discord: close frame: %ls", json.c_str());
        return;
    }
    if (op != OP_FRAME) return;
    JsonObject o;
    if (!JsonObject::TryParse(json, o)) return;
    std::wstring cmd = str(o, L"cmd"), evt = str(o, L"evt"), nonce = str(o, L"nonce");
    auto data = obj(o, L"data");
    if (cmd == L"DISPATCH") { onEvent(evt, data); return; }
    auto it = pending_.find(nonce);
    if (it == pending_.end()) return;
    Reply reply = it->second;
    pending_.erase(it);
    if (evt == L"ERROR") {
        std::wstring message = data ? str(data, L"message") : L"error";
        hostLog("discord: %ls failed: %ls (%d)", cmd.c_str(), message.c_str(), int(data ? num(data, L"code") : 0));
        reply(nullptr, message);
    } else {
        reply(data, L"");
    }
}

// ---- sign-in

bool Discord::loadLogin(std::wstring& access, std::wstring& refresh, uint64_t& expires) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, loginPath_.c_str(), L"rb") != 0 || !f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    JsonObject o;
    if (!JsonObject::TryParse(widen(text.data(), text.size()), o)) return false;
    access = str(o, L"access_token");
    refresh = str(o, L"refresh_token");
    expires = uint64_t(num(o, L"expires"));
    return !access.empty();
}

void Discord::saveLogin(const std::wstring& access, const std::wstring& refresh, int expiresIn) {
    std::wstring json = L"{\"access_token\":" + jsonString(access) + L",\"refresh_token\":" + jsonString(refresh) + L",\"expires\":" +
                        std::to_wstring(uint64_t(time(nullptr)) + uint64_t(std::max(0, expiresIn))) + L"}";
    std::string bytes = narrow(json);
    FILE* f = nullptr;
    if (_wfopen_s(&f, loginPath_.c_str(), L"wb") == 0 && f) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); }
}

bool Discord::tokenRequest(const std::string& body, std::wstring& access, std::wstring& refresh, int& expiresIn, std::wstring& error) {
    DWORD status = 0;
    auto answer = httpRequest(L"https://discord.com/api/oauth2/token", L"POST", L"Content-Type: application/x-www-form-urlencoded\r\n", body, &status);
    JsonObject o;
    if (answer.empty() || !JsonObject::TryParse(widen(reinterpret_cast<const char*>(answer.data()), answer.size()), o)) {
        error = L"Discord did not answer (status " + std::to_wstring(status) + L")";
        return false;
    }
    if (!str(o, L"error").empty()) {
        error = str(o, L"error") + L": " + str(o, L"error_description");
        return false;
    }
    access = str(o, L"access_token");
    refresh = str(o, L"refresh_token");
    expiresIn = int(num(o, L"expires_in", 604800));
    return !access.empty();
}

void Discord::authorize() {
    std::lock_guard<std::mutex> lock(actionsLock_);
    actions_.push_back([this] {
        if (pipe_ == INVALID_HANDLE_VALUE) { setStatus(L"Discord is not running on this PC"); return; }
        {
            std::lock_guard<std::mutex> l(stateLock_);
            state_.busy = true;
            state_.status = L"Discord is asking on your desktop: click Authorize";
            state_.serial++;
        }
        hostLog("discord: asking for authorization");
        auto exchange = [this](const JsonObject& data, const std::wstring& error) {
            std::wstring code = data ? str(data, L"code") : L"";
            if (code.empty()) {
                std::lock_guard<std::mutex> l(stateLock_);
                state_.busy = false;
                state_.status = L"Not authorized: " + (error.empty() ? L"no code came back" : error);
                state_.serial++;
                return;
            }
            std::string body = "client_id=" + urlEncode(narrow(clientId_)) + "&client_secret=" + urlEncode(narrow(clientSecret_)) +
                               "&grant_type=authorization_code&code=" + urlEncode(narrow(code)) + "&redirect_uri=" + urlEncode("http://localhost");
            std::wstring access, refresh, err;
            int expiresIn = 0;
            if (!tokenRequest(body, access, refresh, expiresIn, err)) {
                hostLog("discord: token exchange failed: %ls", err.c_str());
                std::lock_guard<std::mutex> l(stateLock_);
                state_.busy = false;
                state_.status = L"Sign-in failed: " + err + L" (is http://localhost among the app's redirects?)";
                state_.serial++;
                return;
            }
            saveLogin(access, refresh, expiresIn);
            authenticate(access);
        };
        std::wstring args = L"{\"client_id\":" + jsonString(clientId_) + L",\"scopes\":" + SCOPES_FULL + L"}";
        send(L"AUTHORIZE", args, [this, exchange](const JsonObject& data, const std::wstring& error) {
            if (data) { exchange(data, error); return; }
            // a scope Discord will not grant this app: try the smaller set (no message notifications)
            hostLog("discord: AUTHORIZE with the full scopes failed (%ls); trying without notifications", error.c_str());
            std::wstring less = L"{\"client_id\":" + jsonString(clientId_) + L",\"scopes\":" + SCOPES_LESS + L"}";
            send(L"AUTHORIZE", less, exchange);
        });
    });
}

void Discord::authenticate(const std::wstring& access) {
    access_ = access;
    send(L"AUTHENTICATE", L"{\"access_token\":" + jsonString(access) + L"}", [this](const JsonObject& data, const std::wstring& error) {
        if (!data) {
            hostLog("discord: AUTHENTICATE failed: %ls", error.c_str());
            if (++authFailures_ >= 2) DeleteFileW(loginPath_.c_str());  // a stale token: sign in again
            std::lock_guard<std::mutex> l(stateLock_);
            state_.busy = false;
            state_.authed = false;
            state_.status = L"Tap CONNECT (the saved sign-in did not work)";
            state_.serial++;
            return;
        }
        authFailures_ = 0;
        auto user = userOf(obj(data, L"user"));
        {
            std::lock_guard<std::mutex> l(stateLock_);
            state_.busy = false;
            state_.authed = true;
            if (!user.id.empty()) state_.me = user;
            state_.status = L"";
            state_.serial++;
        }
        std::wstring scopes;
        if (auto a = arr(data, L"scopes")) for (auto v : a) if (v.ValueType() == JsonValueType::String) scopes += std::wstring(v.GetString()) + L" ";
        hostLog("discord: signed in as %ls (scopes: %ls)", user.name.c_str(), scopes.c_str());
        afterAuth();
    });
}

void Discord::afterAuth() {
    subscribe(L"VOICE_CHANNEL_SELECT", L"");
    subscribe(L"VOICE_SETTINGS_UPDATE", L"");
    subscribe(L"NOTIFICATION_CREATE", L"");
    send(L"GET_VOICE_SETTINGS", L"", [this](const JsonObject& data, const std::wstring&) {
        if (!data) return;
        std::lock_guard<std::mutex> l(stateLock_);
        state_.selfMute = flag(data, L"mute");
        state_.selfDeaf = flag(data, L"deaf");
        state_.serial++;
    });
    send(L"GET_SELECTED_VOICE_CHANNEL", L"", [this](const JsonObject& data, const std::wstring&) {
        std::wstring id = data ? str(data, L"id") : L"";
        watchChannel(id);
        if (!data) return;
        std::vector<DiscordMember> members;
        if (auto states = arr(data, L"voice_states"))
            for (auto v : states) {
                if (v.ValueType() != JsonValueType::Object) continue;
                auto s = v.GetObject();
                DiscordMember m;
                m.user = userOf(obj(s, L"user"));
                std::wstring nick = str(s, L"nick");
                if (!nick.empty()) m.user.name = nick;
                if (auto vs = obj(s, L"voice_state")) { m.mute = flag(vs, L"mute") || flag(vs, L"self_mute"); m.deaf = flag(vs, L"deaf") || flag(vs, L"self_deaf"); }
                members.push_back(m);
            }
        std::lock_guard<std::mutex> l(stateLock_);
        state_.channelId = id;
        state_.channelName = str(data, L"name");
        for (auto& m : members) m.self = m.user.id == state_.me.id;
        state_.members = members;
        state_.serial++;
    });
}

// The voice events of one channel (the one I am in); the previous channel's are let go.
void Discord::watchChannel(const std::wstring& channelId) {
    if (watched_ == channelId) return;
    static const wchar_t* const EVENTS[] = {L"VOICE_STATE_CREATE", L"VOICE_STATE_UPDATE", L"VOICE_STATE_DELETE", L"SPEAKING_START", L"SPEAKING_STOP"};
    if (!watched_.empty())
        for (auto e : EVENTS) unsubscribe(e, L"{\"channel_id\":" + jsonString(watched_) + L"}");
    watched_ = channelId;
    if (!watched_.empty())
        for (auto e : EVENTS) subscribe(e, L"{\"channel_id\":" + jsonString(watched_) + L"}");
}

void Discord::onEvent(const std::wstring& evt, const JsonObject& data) {
    if (evt == L"READY") return;
    if (evt == L"VOICE_CHANNEL_SELECT") {
        std::wstring id = data ? str(data, L"channel_id") : L"";
        hostLog("discord: voice channel %ls", id.empty() ? L"left" : id.c_str());
        {
            std::lock_guard<std::mutex> l(stateLock_);
            state_.channelId = id;
            if (id.empty()) { state_.channelName.clear(); state_.members.clear(); }
            state_.serial++;
        }
        watchChannel(id);
        if (!id.empty()) {
            send(L"GET_SELECTED_VOICE_CHANNEL", L"", [this](const JsonObject& d, const std::wstring&) {
                if (!d) return;
                std::vector<DiscordMember> members;
                if (auto states = arr(d, L"voice_states"))
                    for (auto v : states) {
                        if (v.ValueType() != JsonValueType::Object) continue;
                        auto s = v.GetObject();
                        DiscordMember m;
                        m.user = userOf(obj(s, L"user"));
                        std::wstring nick = str(s, L"nick");
                        if (!nick.empty()) m.user.name = nick;
                        if (auto vs = obj(s, L"voice_state")) { m.mute = flag(vs, L"mute") || flag(vs, L"self_mute"); m.deaf = flag(vs, L"deaf") || flag(vs, L"self_deaf"); }
                        members.push_back(m);
                    }
                std::lock_guard<std::mutex> l(stateLock_);
                state_.channelName = str(d, L"name");
                for (auto& m : members) m.self = m.user.id == state_.me.id;
                state_.members = members;
                state_.serial++;
            });
        }
        return;
    }
    if (evt == L"VOICE_STATE_CREATE" || evt == L"VOICE_STATE_UPDATE" || evt == L"VOICE_STATE_DELETE") {
        if (!data) return;
        DiscordMember m;
        m.user = userOf(obj(data, L"user"));
        std::wstring nick = str(data, L"nick");
        if (!nick.empty()) m.user.name = nick;
        if (auto vs = obj(data, L"voice_state")) { m.mute = flag(vs, L"mute") || flag(vs, L"self_mute"); m.deaf = flag(vs, L"deaf") || flag(vs, L"self_deaf"); }
        std::lock_guard<std::mutex> l(stateLock_);
        auto it = std::find_if(state_.members.begin(), state_.members.end(), [&](const DiscordMember& x) { return x.user.id == m.user.id; });
        if (evt == L"VOICE_STATE_DELETE") { if (it != state_.members.end()) state_.members.erase(it); }
        else if (it == state_.members.end()) { m.self = m.user.id == state_.me.id; state_.members.push_back(m); }
        else { m.speaking = it->speaking; m.self = it->self; *it = m; }
        state_.serial++;
        return;
    }
    if (evt == L"SPEAKING_START" || evt == L"SPEAKING_STOP") {
        std::wstring id = data ? str(data, L"user_id") : L"";
        std::lock_guard<std::mutex> l(stateLock_);
        for (auto& m : state_.members) if (m.user.id == id) m.speaking = evt == L"SPEAKING_START";
        state_.serial++;
        return;
    }
    if (evt == L"VOICE_SETTINGS_UPDATE") {
        if (!data) return;
        std::lock_guard<std::mutex> l(stateLock_);
        state_.selfMute = flag(data, L"mute");
        state_.selfDeaf = flag(data, L"deaf");
        state_.serial++;
        return;
    }
    if (evt == L"NOTIFICATION_CREATE") {
        if (!data) return;
        DiscordNote n;
        n.channelId = str(data, L"channel_id");
        n.title = str(data, L"title");
        n.text = str(data, L"body");
        n.at = now();
        auto message = obj(data, L"message");
        if (message) {
            n.from = userOf(obj(message, L"author"));
            if (n.text.empty()) n.text = str(message, L"content");
            n.call = int(num(message, L"type")) == 3;  // a call started in a DM
        }
        if (n.from.name.empty()) n.from.name = n.title.empty() ? L"Discord" : n.title;
        hostLog("discord: %s from %ls: %ls", n.call ? "call" : "message", n.from.name.c_str(), n.text.substr(0, 60).c_str());
        std::lock_guard<std::mutex> l(stateLock_);
        if (n.call) state_.call = n;
        else {
            state_.notes.push_front(n);
            if (state_.notes.size() > 30) state_.notes.pop_back();
            state_.unread++;
        }
        state_.serial++;
        return;
    }
}

// ---- actions

void Discord::setMute(bool on) {
    std::lock_guard<std::mutex> lock(actionsLock_);
    actions_.push_back([this, on] { send(L"SET_VOICE_SETTINGS", std::wstring(L"{\"mute\":") + (on ? L"true" : L"false") + L"}", [](const JsonObject&, const std::wstring&) {}); });
}
void Discord::setDeaf(bool on) {
    std::lock_guard<std::mutex> lock(actionsLock_);
    actions_.push_back([this, on] { send(L"SET_VOICE_SETTINGS", std::wstring(L"{\"deaf\":") + (on ? L"true" : L"false") + L"}", [](const JsonObject&, const std::wstring&) {}); });
}
void Discord::joinChannel(const std::wstring& id) {
    std::lock_guard<std::mutex> lock(actionsLock_);
    actions_.push_back([this, id] {
        send(L"SELECT_VOICE_CHANNEL", L"{\"channel_id\":" + jsonString(id) + L",\"force\":true}", [this](const JsonObject&, const std::wstring& error) {
            if (!error.empty()) setStatus(L"Could not join: " + error);
        });
    });
}
void Discord::leaveChannel() {
    std::lock_guard<std::mutex> lock(actionsLock_);
    actions_.push_back([this] { send(L"SELECT_VOICE_CHANNEL", L"{\"channel_id\":null}", [](const JsonObject&, const std::wstring&) {}); });
}
void Discord::listGuilds(std::function<void(std::vector<DiscordRef>, std::wstring)> done) {
    std::lock_guard<std::mutex> lock(actionsLock_);
    actions_.push_back([this, done] {
        send(L"GET_GUILDS", L"", [done](const JsonObject& data, const std::wstring& error) {
            std::vector<DiscordRef> out;
            if (data) if (auto g = arr(data, L"guilds")) for (auto v : g) if (v.ValueType() == JsonValueType::Object) out.push_back({str(v.GetObject(), L"id"), str(v.GetObject(), L"name")});
            done(out, error);
        });
    });
}
void Discord::listVoiceChannels(const std::wstring& guildId, std::function<void(std::vector<DiscordRef>, std::wstring)> done) {
    std::lock_guard<std::mutex> lock(actionsLock_);
    actions_.push_back([this, guildId, done] {
        send(L"GET_CHANNELS", L"{\"guild_id\":" + jsonString(guildId) + L"}", [done](const JsonObject& data, const std::wstring& error) {
            std::vector<DiscordRef> out;
            if (data) if (auto c = arr(data, L"channels")) for (auto v : c) if (v.ValueType() == JsonValueType::Object && int(num(v.GetObject(), L"type")) == 2) out.push_back({str(v.GetObject(), L"id"), str(v.GetObject(), L"name")});
            done(out, error);
        });
    });
}
void Discord::markRead() {
    std::lock_guard<std::mutex> l(stateLock_);
    if (state_.unread) { state_.unread = 0; state_.serial++; }
}
void Discord::dismissCall() {
    std::lock_guard<std::mutex> l(stateLock_);
    if (state_.call.at) { state_.call = DiscordNote{}; state_.serial++; }
}
void Discord::signOut() {
    DeleteFileW(loginPath_.c_str());
    std::lock_guard<std::mutex> lock(actionsLock_);
    actions_.push_back([this] {
        watchChannel(L"");
        std::lock_guard<std::mutex> l(stateLock_);
        state_.authed = false;
        state_.members.clear();
        state_.channelId.clear();
        state_.channelName.clear();
        state_.status = L"Signed out. Tap CONNECT to sign in";
        state_.serial++;
    });
}

// ---- avatars

std::shared_ptr<const SharePicture> Discord::avatar(const DiscordUser& user, int size) {
    if (user.id.empty()) return nullptr;
    size = std::clamp(size, 32, 256);
    std::wstring key = user.id + L"/" + user.avatar + L"/" + std::to_wstring(size);
    std::lock_guard<std::mutex> lock(avatarLock_);
    auto it = avatars_.find(key);
    if (it != avatars_.end()) return it->second;
    if (avatarLoading_.count(key) || avatarLoading_.size() >= 4) return nullptr;
    avatarLoading_.insert(key);
    std::wstring url;
    if (!user.avatar.empty()) url = L"https://cdn.discordapp.com/avatars/" + user.id + L"/" + user.avatar + L".png?size=" + std::to_wstring(size);
    else {
        unsigned long long id = _wcstoui64(user.id.c_str(), nullptr, 10);
        url = L"https://cdn.discordapp.com/embed/avatars/" + std::to_wstring((id >> 22) % 6) + L".png";
    }
    avatarThreads_.erase(std::remove_if(avatarThreads_.begin(), avatarThreads_.end(), [](std::thread& t) { return !t.joinable(); }), avatarThreads_.end());
    avatarThreads_.emplace_back([this, key, url] { avatarFetch(key, url); });
    return nullptr;
}

void Discord::avatarFetch(const std::wstring& key, const std::wstring& url) {
    auto bytes = httpGetBytes(url);
    std::shared_ptr<const SharePicture> pic;
    if (!bytes.empty()) {
        auto p = decodeImage(bytes.data(), bytes.size());
        if (p && p->w) pic = p;
    }
    if (!pic) hostLog("discord: no picture from %ls (%zu bytes)", url.c_str(), bytes.size());
    std::lock_guard<std::mutex> lock(avatarLock_);
    avatars_[key] = pic;
    avatarLoading_.erase(key);
    changed();
}
