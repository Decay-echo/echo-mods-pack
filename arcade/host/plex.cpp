// Plex: link with a plex.tv/link code, find your server, browse, direct play in mpv.
// The token is stored per user in %LOCALAPPDATA%\EchoArcade\plex.json and never logged.
#include "media.h"
#include <combaseapi.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <fstream>
#include <sstream>

using namespace winrt;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Web::Http;
using namespace std::chrono_literals;

namespace {

std::wstring storePath() {
    std::wstring dir = localAppData() + L"\\EchoArcade";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\plex.json";
}

std::string newClientId() {
    GUID g;
    CoCreateGuid(&g);
    char s[64];
    snprintf(s, sizeof(s), "echo-arcade-%08lx%04x%04x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1]);
    return s;
}

std::wstring w(const std::string& s) { return std::wstring(to_hstring(s)); }
std::string n(const hstring& s) { return to_string(s); }
std::string str(const JsonObject& o, const wchar_t* key) {
    auto v = o.TryLookup(key);
    if (!v) return {};
    if (v.ValueType() == JsonValueType::String) return n(v.GetString());
    if (v.ValueType() == JsonValueType::Number) return std::to_string(int64_t(v.GetNumber()));
    return {};
}
double num(const JsonObject& o, const wchar_t* key) {
    auto v = o.TryLookup(key);
    return v && v.ValueType() == JsonValueType::Number ? v.GetNumber() : 0;
}
JsonArray arr(const JsonObject& o, const wchar_t* key) {
    auto v = o.TryLookup(key);
    return v && v.ValueType() == JsonValueType::Array ? v.GetArray() : JsonArray();
}
std::string encode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { char b[4]; snprintf(b, sizeof(b), "%%%02X", c); out += b; }
    }
    return out;
}

}  // namespace

Plex::Plex() {
    std::ifstream f(storePath());
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        auto o = JsonObject::Parse(to_hstring(ss.str()));
        clientId_ = str(o, L"clientId");
        token_ = str(o, L"token");
    } catch (...) {}
    if (clientId_.empty()) { clientId_ = newClientId(); save(); }
}

void Plex::save() {
    JsonObject o;
    o.SetNamedValue(L"clientId", JsonValue::CreateStringValue(to_hstring(clientId_)));
    o.SetNamedValue(L"token", JsonValue::CreateStringValue(to_hstring(token_)));
    std::ofstream(storePath(), std::ios::binary) << n(o.Stringify());
}

void Plex::unlink() { token_.clear(); server_.clear(); serverToken_.clear(); save(); }

std::string Plex::get(const std::wstring& url, const char* method, int timeoutSeconds) {
    try {
        static HttpClient client;
        HttpRequestMessage req(strcmp(method, "POST") == 0 ? HttpMethod::Post() : HttpMethod::Get(), Uri(url));
        auto h = req.Headers();
        h.TryAppendWithoutValidation(L"Accept", L"application/json");
        h.TryAppendWithoutValidation(L"X-Plex-Product", L"Echo Arcade");
        h.TryAppendWithoutValidation(L"X-Plex-Version", L"1.0");
        h.TryAppendWithoutValidation(L"X-Plex-Platform", L"Windows");
        h.TryAppendWithoutValidation(L"X-Plex-Device-Name", L"Echo VR Tablet");
        h.TryAppendWithoutValidation(L"X-Plex-Client-Identifier", to_hstring(clientId_));
        bool plexTv = url.rfind(L"https://plex.tv/", 0) == 0;
        const std::string& token = plexTv ? token_ : serverToken_;
        if (!token.empty()) h.TryAppendWithoutValidation(L"X-Plex-Token", to_hstring(token));
        auto op = client.SendRequestAsync(req);
        if (op.wait_for(std::chrono::seconds(timeoutSeconds)) != AsyncStatus::Completed) {
            op.Cancel();
            hostLog("plex: no answer in %d s (%ls)", timeoutSeconds, Uri(url).Host().c_str());
            return {};
        }
        auto resp = op.GetResults();
        if (!resp.IsSuccessStatusCode()) {
            hostLog("plex: HTTP %d from %ls", int(resp.StatusCode()), Uri(url).Host().c_str());
            return {};
        }
        return n(resp.Content().ReadAsStringAsync().get());
    } catch (hresult_error const& e) {
        hostLog("plex: request failed 0x%08x (%ls)", unsigned(e.code()), Uri(url).Host().c_str());
        return {};
    }
}

bool Plex::startLink(std::wstring& code, std::string& pinId) {
    auto body = get(L"https://plex.tv/api/v2/pins?strong=false", "POST");
    try {
        auto o = JsonObject::Parse(to_hstring(body));
        pinId = str(o, L"id");
        code = w(str(o, L"code"));
        return !pinId.empty() && !code.empty();
    } catch (...) { return false; }
}

bool Plex::pollLink(const std::string& pinId) {
    auto body = get(w("https://plex.tv/api/v2/pins/" + pinId));
    try {
        auto token = str(JsonObject::Parse(to_hstring(body)), L"authToken");
        if (token.empty()) return false;
        token_ = token;
        save();
        hostLog("plex: linked");
        return true;
    } catch (...) { return false; }
}

bool Plex::findServer(std::wstring& error) {
    auto body = get(L"https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1");
    if (body.empty()) { error = L"Could not reach plex.tv"; return false; }
    // Candidates, fastest first. Many routers refuse to resolve *.plex.direct names that
    // point at a LAN address (DNS rebinding protection), so the server's plain LAN
    // address comes first; then its https names, then the remote address, then the relay.
    struct Candidate { std::wstring uri; std::string token, kind; std::wstring name; };
    std::vector<Candidate> candidates[4];
    try {
        for (auto v : JsonArray::Parse(to_hstring(body))) {
            auto r = v.GetObject();
            if (str(r, L"provides").find("server") == std::string::npos) continue;
            std::string token = str(r, L"accessToken");
            std::wstring name = w(str(r, L"name"));
            for (auto cv : arr(r, L"connections")) {
                auto c = cv.GetObject();
                bool local = c.GetNamedBoolean(L"local", false), relay = c.GetNamedBoolean(L"relay", false);
                std::wstring uri = w(str(c, L"uri"));
                if (local) {
                    std::string address = str(c, L"address"), port = str(c, L"port");
                    if (!address.empty() && !port.empty() && address.find(':') == std::string::npos)
                        candidates[0].push_back({w("http://" + address + ":" + port), token, "LAN", name});
                    candidates[1].push_back({uri, token, "local https", name});
                } else candidates[relay ? 3 : 2].push_back({uri, token, relay ? "relay" : "remote", name});
            }
        }
    } catch (hresult_error const& e) {
        hostLog("plex: could not read the server list (0x%08x)", unsigned(e.code()));
    }
    size_t tried = 0;
    for (auto& group : candidates)
        for (auto& c : group) {
            tried++;
            server_ = c.uri;
            serverToken_ = c.token;
            if (!get(server_ + L"/identity", "GET", 5).empty()) {
                serverName_ = c.name;
                hostLog("plex: using server '%ls' (%s)", serverName_.c_str(), c.kind.c_str());
                return true;
            }
            hostLog("plex: '%ls' not reachable via %s", c.name.c_str(), c.kind.c_str());
        }
    if (!tried) hostLog("plex: this account lists no servers");
    server_.clear();
    error = L"No Plex server reachable from this PC";
    return false;
}

std::vector<PlexItem> Plex::libraries() {
    std::vector<PlexItem> out;
    out.push_back({L"Continue Watching", L"On Deck", "/library/onDeck", "", "", "library"});
    try {
        auto mc = JsonObject::Parse(to_hstring(get(server_ + L"/library/sections"))).GetNamedObject(L"MediaContainer");
        for (auto v : arr(mc, L"Directory")) {
            auto d = v.GetObject();
            auto type = str(d, L"type");
            if (type != "movie" && type != "show") continue;  // video libraries only
            out.push_back({w(str(d, L"title")), type == "movie" ? L"Movies" : L"TV Shows",
                           "/library/sections/" + str(d, L"key") + "/all", "", "", "library"});
        }
    } catch (...) {}
    return out;
}

std::vector<PlexItem> Plex::children(const std::string& key) {
    std::vector<PlexItem> out;
    try {
        auto mc = JsonObject::Parse(to_hstring(get(server_ + w(key)))).GetNamedObject(L"MediaContainer");
        for (auto v : arr(mc, L"Metadata")) {
            auto m = v.GetObject();
            PlexItem it;
            it.type = str(m, L"type");
            it.ratingKey = str(m, L"ratingKey");
            it.title = w(str(m, L"title"));
            it.resumeSeconds = num(m, L"viewOffset") / 1000;
            it.durationSeconds = num(m, L"duration") / 1000;
            if (it.type == "episode") {
                char se[32];
                snprintf(se, sizeof(se), "S%02dE%02d  ", int(num(m, L"parentIndex")), int(num(m, L"index")));
                it.subtitle = w(str(m, L"grandparentTitle"));
                it.title = w(se) + it.title;
            } else if (it.type == "movie") {
                it.subtitle = w(str(m, L"year"));
            } else if (it.type == "show" || it.type == "season") {
                it.subtitle = it.type == "show" ? w(str(m, L"childCount")) + L" seasons" : w(str(m, L"leafCount")) + L" episodes";
                it.key = str(m, L"key");
            }
            for (auto media : arr(m, L"Media")) {
                for (auto part : arr(media.GetObject(), L"Part")) { it.partKey = str(part.GetObject(), L"key"); break; }
                break;
            }
            if (it.durationSeconds > 0 && it.playable()) {
                int mins = int(it.durationSeconds / 60);
                it.subtitle += (it.subtitle.empty() ? L"" : L"  -  ") + std::to_wstring(mins) + L" min";
                if (it.resumeSeconds > 30) it.subtitle += L"  -  resume " + std::to_wstring(int(it.resumeSeconds / 60)) + L" min in";
            }
            if (it.playable() || !it.key.empty()) out.push_back(it);
        }
    } catch (...) {}
    return out;
}

std::wstring Plex::streamUrl(const PlexItem& item) const {
    return server_ + w(item.partKey) + L"?X-Plex-Token=" + w(serverToken_);
}

void Plex::timeline(const PlexItem& item, const char* state, double seconds) {
    if (item.ratingKey.empty() || server_.empty()) return;
    char q[512];
    snprintf(q, sizeof(q), "/:/timeline?ratingKey=%s&key=%s&state=%s&time=%lld&duration=%lld", item.ratingKey.c_str(),
             encode("/library/metadata/" + item.ratingKey).c_str(), state, (long long)(seconds * 1000),
             (long long)(item.durationSeconds * 1000));
    get(server_ + w(q));
}
