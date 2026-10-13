// Pass counter: who has the disc, from Echo VR's own API (http://127.0.0.1:6721/session,
// on when Settings > Game > API access is on), and the chain of passes inside one team.
// Header-only, no Windows: hudplus.cpp polls the API, hudplus_test.cpp checks the rest.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace hudplus {

// ---- a small JSON reader (enough for the session document) ----------------------------
struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;                           // Array
    std::vector<std::pair<std::string, Json>> fields;  // Object
    const Json* get(const char* key) const {
        if (type != Object) return nullptr;
        for (const auto& f : fields)
            if (f.first == key) return &f.second;
        return nullptr;
    }
};

namespace json_detail {
inline void skip(const char*& p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
}
inline void putUtf8(std::string& out, unsigned c) {
    if (c < 0x80) out += char(c);
    else if (c < 0x800) { out += char(0xC0 | c >> 6); out += char(0x80 | (c & 0x3F)); }
    else { out += char(0xE0 | c >> 12); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
}
inline bool string(const char*& p, const char* end, std::string& out) {
    if (p >= end || *p != '"') return false;
    p++;
    out.clear();
    while (p < end && *p != '"') {
        if (*p != '\\') { out += *p++; continue; }
        if (++p >= end) return false;
        char e = *p++;
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (end - p < 4) return false;
                char hex[5] = {p[0], p[1], p[2], p[3], 0};
                putUtf8(out, unsigned(std::strtoul(hex, nullptr, 16)));
                p += 4;
                break;
            }
            default: out += e;  // \" \\ \/
        }
    }
    if (p >= end) return false;
    p++;
    return true;
}
inline bool value(const char*& p, const char* end, Json& out, int depth) {
    if (depth > 32) return false;
    skip(p, end);
    if (p >= end) return false;
    out = Json{};
    if (*p == '{') {
        out.type = Json::Object;
        p++;
        skip(p, end);
        if (p < end && *p == '}') { p++; return true; }
        while (true) {
            std::string key;
            skip(p, end);
            if (!string(p, end, key)) return false;
            skip(p, end);
            if (p >= end || *p++ != ':') return false;
            out.fields.emplace_back(std::move(key), Json{});
            if (!value(p, end, out.fields.back().second, depth + 1)) return false;
            skip(p, end);
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == '}') { p++; return true; }
            return false;
        }
    }
    if (*p == '[') {
        out.type = Json::Array;
        p++;
        skip(p, end);
        if (p < end && *p == ']') { p++; return true; }
        while (true) {
            out.items.emplace_back();
            if (!value(p, end, out.items.back(), depth + 1)) return false;
            skip(p, end);
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == ']') { p++; return true; }
            return false;
        }
    }
    if (*p == '"') { out.type = Json::String; return string(p, end, out.s); }
    if (end - p >= 4 && !std::strncmp(p, "true", 4)) { out.type = Json::Bool; out.b = true; p += 4; return true; }
    if (end - p >= 5 && !std::strncmp(p, "false", 5)) { out.type = Json::Bool; p += 5; return true; }
    if (end - p >= 4 && !std::strncmp(p, "null", 4)) { p += 4; return true; }
    char* stop = nullptr;
    std::string number(p, std::min<size_t>(size_t(end - p), 40));
    out.n = std::strtod(number.c_str(), &stop);
    if (stop == number.c_str()) return false;
    out.type = Json::Number;
    p += stop - number.c_str();
    return true;
}
}  // namespace json_detail

inline bool parseJson(const std::string& text, Json& out) {
    const char* p = text.data();
    return json_detail::value(p, p + text.size(), out, 0);
}

// ---- what one /session answer says ------------------------------------------------------
struct Possession {
    bool ok = false;           // a match the API describes
    bool playing = false;      // the disc is in play (not a goal, round start, pause ...)
    bool scoring = false;      // a goal was just scored (the points may follow a moment later)
    std::string session;       // sessionid: a new match starts the counting over
    int points[2] = {0, 0};    // blue, orange
    int team = -1;             // who has the disc: 0 blue, 1 orange, -1 nobody
    std::string holder;        // their name
    int myTeam = -1;           // the team of the player this game runs for (client_name)
    int bounces = -1;          // disc.bounce_count (-1: not in the answer)
};

inline Possession readSession(const Json& doc) {
    Possession p;
    const Json* status = doc.get("game_status");
    const Json* teams = doc.get("teams");
    if (!status || status->type != Json::String || !teams || teams->type != Json::Array) return p;
    p.ok = true;
    p.playing = status->s == "playing" || status->s == "sudden_death";
    p.scoring = status->s == "score";
    if (const Json* id = doc.get("sessionid"); id && id->type == Json::String) p.session = id->s;
    if (const Json* v = doc.get("blue_points"); v && v->type == Json::Number) p.points[0] = int(v->n);
    if (const Json* v = doc.get("orange_points"); v && v->type == Json::Number) p.points[1] = int(v->n);
    if (const Json* disc = doc.get("disc"))
        if (const Json* v = disc->get("bounce_count"); v && v->type == Json::Number) p.bounces = int(v->n);
    const Json* me = doc.get("client_name");
    for (size_t t = 0; t < teams->items.size() && t < 3; t++) {
        const Json* players = teams->items[t].get("players");
        if (!players || players->type != Json::Array) continue;
        for (const Json& pl : players->items) {
            const Json* name = pl.get("name");
            const Json* has = pl.get("possession");
            if (me && name && name->type == Json::String && me->type == Json::String && name->s == me->s) p.myTeam = int(t);
            if (t < 2 && has && has->type == Json::Bool && has->b && p.team < 0) {
                p.team = int(t);
                p.holder = name && name->type == Json::String ? name->s : "?";
            }
        }
    }
    return p;
}

// ---- the chain ----------------------------------------------------------------------------
// A pass: the disc goes from one player straight to a teammate. If it bounced on the way
// (the API's bounce count went up while it was loose), the catch doesn't count; the chain
// goes on. The other team getting it breaks the chain. A goal by the team with the chain
// shows it big for a bit.
constexpr double kPassPopSeconds = 1.0;   // how long "+1" stays up
constexpr double kPassGoalSeconds = 2.0;  // the goal flash: three blinks
constexpr float kPassGoalScale = 1.5f;

struct PassCounter {
    std::string session, holder;
    int team = -1, passes = 0;
    int points[2] = {-1, -1};
    double popAt = -1;
    double goalAt = -1;
    int goalPasses = 0, goalTeam = -1;
    bool loose = false;                       // nobody has it since the last holder
    int looseFirst = -1, looseMax = -1;       // the bounce count when it got loose, and the most seen since
    int bounced = 0;                          // catches not counted because the disc bounced first

    void breakChain() {
        team = -1;
        passes = 0;
        holder.clear();
        popAt = -1;
        loose = false;
    }
    void update(const Possession& p, double t) {
        if (!p.ok) {  // menus, lobby, no API: start over next match
            breakChain();
            session.clear();
            points[0] = points[1] = -1;
            return;
        }
        if (p.session != session) {
            breakChain();
            session = p.session;
            points[0] = points[1] = -1;
        }
        int scored = -1;
        if (points[0] >= 0) {
            if (p.points[0] > points[0]) scored = 0;
            else if (p.points[1] > points[1]) scored = 1;
        }
        points[0] = p.points[0];
        points[1] = p.points[1];
        if (scored >= 0) {
            if (scored == team && passes > 0) {
                goalAt = t;
                goalPasses = passes;
                goalTeam = team;
            }
            breakChain();
            return;
        }
        if (p.scoring) return;                    // the goal's points come next: keep the chain for them
        if (!p.playing) { breakChain(); return; }  // round over, round start, pause
        if (p.team < 0) {  // in the air or loose: the chain waits, and the bounces are watched
            if (!loose) { loose = true; looseFirst = looseMax = p.bounces; }
            else looseMax = std::max(looseMax, p.bounces);
            return;
        }
        // Whether Echo resets the count when the disc is thrown or when it is grabbed, a rise
        // while it was loose is a bounce. Seen in nobody's hand (under 0.1 s): no bounce.
        bool bouncedFlight = loose && looseFirst >= 0 && looseMax > looseFirst;
        loose = false;
        if (p.team != team) {
            breakChain();
            team = p.team;
            holder = p.holder;
            return;
        }
        if (p.holder != holder) {
            holder = p.holder;
            if (bouncedFlight) { bounced++; return; }
            passes++;
            popAt = t;
        }
    }
};

struct PassView {
    enum Kind { None, Plus, Count, Goal } kind = None;
    int number = 0, team = -1;
    float scale = 1, alpha = 1;
};

// What to show at time t: the goal flash, else the count from `countFrom` passes, else a
// "+1" for a second after each pass, else nothing.
inline PassView passView(const PassCounter& c, double t, int countFrom) {
    PassView v;
    if (c.goalAt >= 0 && t >= c.goalAt && t - c.goalAt < kPassGoalSeconds) {
        double phase = std::fmod((t - c.goalAt) / (kPassGoalSeconds / 3), 1.0);  // three blinks
        v.kind = PassView::Goal;
        v.number = c.goalPasses;
        v.team = c.goalTeam;
        v.scale = kPassGoalScale;
        v.alpha = phase < 0.7 ? 1.f : 0.2f;
        return v;
    }
    if (c.team < 0 || c.passes < 1) return v;
    v.team = c.team;
    if (c.passes >= countFrom) {
        v.kind = PassView::Count;
        v.number = c.passes;
        return v;
    }
    if (c.popAt >= 0 && t >= c.popAt && t - c.popAt < kPassPopSeconds) {
        v.kind = PassView::Plus;
        v.number = 1;
        double left = kPassPopSeconds - (t - c.popAt);
        v.alpha = left < 0.35 ? float(left / 0.35) : 1.f;  // fades out at the end
    }
    return v;
}

}  // namespace hudplus
