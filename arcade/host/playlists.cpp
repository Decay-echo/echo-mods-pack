// Builds RetroArch playlists from the ROM folders each time RetroArch is opened,
// so dropped-in games appear under Playlists already paired with a core.
#include "apps.h"
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace {

struct System {
    const wchar_t* folder;     // apps\roms\<folder>
    const char* playlist;      // RetroArch database name (also enables thumbnails)
    const wchar_t* core;       // cores\<core>_libretro.dll
    const char* coreName;
    std::set<std::wstring> extensions;
};

const std::set<std::wstring> DISC = {L".m3u", L".cue", L".chd", L".ccd", L".iso", L".img", L".pbp", L".bin"};
std::set<std::wstring> ext(std::initializer_list<const wchar_t*> list, bool disc = false) {
    std::set<std::wstring> s(list.begin(), list.end());
    if (disc) s.insert(DISC.begin(), DISC.end());
    return s;
}

// Keep in sync with SYSTEMS in tools/setup_apps.py and docs/ROMS.md.
const System SYSTEMS[] = {
    {L"atari2600", "Atari - 2600", L"stella", "Atari - 2600 (Stella)", ext({L".a26", L".bin", L".zip"})},
    {L"atari7800", "Atari - 7800", L"prosystem", "Atari - 7800 (ProSystem)", ext({L".a78", L".bin", L".zip"})},
    {L"lynx", "Atari - Lynx", L"handy", "Atari - Lynx (Handy)", ext({L".lnx", L".zip"})},
    {L"nes", "Nintendo - Nintendo Entertainment System", L"nestopia", "Nintendo - NES / Famicom (Nestopia)", ext({L".nes", L".fds", L".unf", L".unif", L".zip", L".7z"})},
    {L"snes", "Nintendo - Super Nintendo Entertainment System", L"snes9x", "Nintendo - SNES / SFC (Snes9x - Current)", ext({L".sfc", L".smc", L".fig", L".swc", L".bs", L".zip", L".7z"})},
    {L"n64", "Nintendo - Nintendo 64", L"mupen64plus_next", "Nintendo - Nintendo 64 (Mupen64Plus-Next)", ext({L".n64", L".v64", L".z64", L".zip", L".7z"})},
    {L"gb", "Nintendo - Game Boy", L"gambatte", "Nintendo - Game Boy / Color (Gambatte)", ext({L".gb", L".gbc", L".dmg", L".zip", L".7z"})},
    {L"gba", "Nintendo - Game Boy Advance", L"mgba", "Nintendo - Game Boy Advance (mGBA)", ext({L".gba", L".zip", L".7z"})},
    {L"nds", "Nintendo - Nintendo DS", L"melondsds", "Nintendo - DS (melonDS DS)", ext({L".nds", L".zip", L".7z"})},
    {L"3ds", "Nintendo - Nintendo 3DS", L"citra", "Nintendo - 3DS (Citra)", ext({L".3ds", L".3dsx", L".cci", L".cxi", L".app", L".elf"})},
    {L"gamecube", "Nintendo - GameCube", L"dolphin", "Nintendo - GameCube / Wii (Dolphin)", ext({L".iso", L".gcm", L".gcz", L".rvz", L".ciso", L".m3u"})},
    {L"wii", "Nintendo - Wii", L"dolphin", "Nintendo - GameCube / Wii (Dolphin)", ext({L".iso", L".wbfs", L".rvz", L".gcz", L".wad", L".m3u"})},
    {L"virtualboy", "Nintendo - Virtual Boy", L"mednafen_vb", "Nintendo - Virtual Boy (Beetle VB)", ext({L".vb", L".vboy", L".zip"})},
    {L"mastersystem", "Sega - Master System - Mark III", L"genesis_plus_gx", "Sega - MS/GG/MD/CD (Genesis Plus GX)", ext({L".sms", L".zip", L".7z"})},
    {L"gamegear", "Sega - Game Gear", L"genesis_plus_gx", "Sega - MS/GG/MD/CD (Genesis Plus GX)", ext({L".gg", L".zip", L".7z"})},
    {L"genesis", "Sega - Mega Drive - Genesis", L"genesis_plus_gx", "Sega - MS/GG/MD/CD (Genesis Plus GX)", ext({L".md", L".gen", L".smd", L".bin", L".68k", L".zip", L".7z"})},
    {L"segacd", "Sega - Mega-CD - Sega CD", L"genesis_plus_gx", "Sega - MS/GG/MD/CD (Genesis Plus GX)", ext({}, true)},
    {L"32x", "Sega - 32X", L"picodrive", "Sega - MS/MD/CD/32X (PicoDrive)", ext({L".32x", L".zip", L".7z"})},
    {L"saturn", "Sega - Saturn", L"mednafen_saturn", "Sega - Saturn (Beetle Saturn)", ext({}, true)},
    {L"dreamcast", "Sega - Dreamcast", L"flycast", "Sega - Dreamcast/NAOMI (Flycast)", ext({L".gdi", L".cdi", L".chd", L".cue", L".m3u"})},
    {L"psx", "Sony - PlayStation", L"pcsx_rearmed", "Sony - PlayStation (PCSX ReARMed)", ext({}, true)},
    {L"ps2", "Sony - PlayStation 2", L"pcsx2", "Sony - PlayStation 2 (LRPS2)", ext({L".iso", L".chd", L".cso", L".bin", L".cue", L".m3u"})},
    {L"psp", "Sony - PlayStation Portable", L"ppsspp", "Sony - PlayStation Portable (PPSSPP)", ext({L".iso", L".cso", L".pbp", L".chd", L".elf"})},
    {L"pcengine", "NEC - PC Engine - TurboGrafx 16", L"mednafen_pce_fast", "NEC - PC Engine / CD (Beetle PCE FAST)", ext({L".pce", L".zip"})},
    {L"pcenginecd", "NEC - PC Engine CD - TurboGrafx-CD", L"mednafen_pce_fast", "NEC - PC Engine / CD (Beetle PCE FAST)", ext({}, true)},
    {L"ngp", "SNK - Neo Geo Pocket Color", L"mednafen_ngp", "SNK - Neo Geo Pocket / Color (Beetle NeoPop)", ext({L".ngp", L".ngc", L".zip"})},
    {L"wonderswan", "Bandai - WonderSwan Color", L"mednafen_wswan", "Bandai - WonderSwan / Color (Beetle Cygne)", ext({L".ws", L".wsc", L".zip"})},
    {L"msx", "Microsoft - MSX", L"bluemsx", "MSX/SVI/ColecoVision/SG-1000 (blueMSX)", ext({L".rom", L".mx1", L".mx2", L".dsk", L".cas", L".zip"})},
    {L"arcade", "FBNeo - Arcade Games", L"fbneo", "Arcade (FinalBurn Neo)", ext({L".zip", L".7z"})},
    {L"mame", "MAME 2003-Plus", L"mame2003_plus", "Arcade (MAME 2003-Plus)", ext({L".zip"})},
    {L"dos", "DOS", L"dosbox_pure", "DOS (DOSBox-Pure)", ext({L".zip", L".dosz", L".exe", L".com", L".bat", L".iso", L".cue"})},
    {L"doom", "DOOM", L"prboom", "DOOM (PrBoom)", ext({L".wad"})},
};

std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string json(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(towlower(c));
    return s;
}

// Files referenced by an .m3u (discs of one game) or a .cue (its .bin tracks) are
// launched through that list, so they get no playlist entry of their own.
void collectReferenced(const fs::path& list, std::set<fs::path>& referenced) {
    std::ifstream in(list, std::ios::binary);
    std::string line;
    bool cue = lower(list.extension().wstring()) == L".cue";
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        std::string name;
        if (cue) {
            auto a = line.find('"'), b = line.rfind('"');
            if (line.find("FILE") == std::string::npos || a == std::string::npos || b <= a) continue;
            name = line.substr(a + 1, b - a - 1);
        } else {
            if (line.empty() || line[0] == '#') continue;
            name = line;
        }
        int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), int(name.size()), nullptr, 0);
        std::wstring wide(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, name.c_str(), int(name.size()), wide.data(), n);
        std::error_code ec;
        referenced.insert(fs::weakly_canonical(list.parent_path() / wide, ec));
    }
}

}  // namespace

// The game's name for a tile: the file name without its tags, e.g.
// "Super Smash Bros. Melee (USA) (En,Ja) (Rev 2)" -> "SUPER SMASH BROS. MELEE".
static std::wstring tileTitle(const fs::path& rom) {
    std::wstring s = rom.stem().wstring(), out;
    int depth = 0;
    for (wchar_t ch : s) {
        if (ch == L'(' || ch == L'[') depth++;
        else if (ch == L')' || ch == L']') depth = std::max(0, depth - 1);
        else if (depth == 0) out += wchar_t(towupper(ch));
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    for (size_t i = 0; i + 1 < out.size();)  // doubled spaces where tags were
        if (out[i] == L' ' && out[i + 1] == L' ') out.erase(i, 1); else i++;
    return out.empty() ? lower(rom.stem().wstring()) : out;
}

std::vector<OwnGame> ownGames(const Config& c, size_t most) {
    std::error_code ec;
    fs::path roms = c.roms, retro = fs::path(c.retroarch).parent_path();
    struct Found { OwnGame game; fs::file_time_type when; std::wstring stem; };
    std::vector<Found> found;
    for (auto& sys : SYSTEMS) {
        if (!wcscmp(sys.folder, L"doom")) continue;  // the bundled shareware has its own tile
        fs::path dir = roms / sys.folder;
        if (!fs::is_directory(dir, ec)) continue;
        std::set<fs::path> referenced;
        std::vector<fs::path> games;
        for (auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
            if (!e.is_regular_file(ec)) continue;
            auto ext = lower(e.path().extension().wstring());
            if (ext == L".m3u" || ext == L".cue") collectReferenced(e.path(), referenced);
            if (sys.extensions.count(ext)) games.push_back(e.path());
        }
        for (auto& g : games) {
            std::error_code ec2;
            if (referenced.count(fs::weakly_canonical(g, ec2))) continue;
            std::wstring system = sys.folder;
            for (auto& ch : system) ch = wchar_t(towupper(ch));
            found.push_back({{tileTitle(g), system, (retro / L"cores" / (std::wstring(sys.core) + L"_libretro.dll")).wstring(), g.wstring()},
                             fs::last_write_time(g, ec2), lower(g.stem().wstring())});
        }
    }
    std::vector<OwnGame> out;
    // roms\tiles.txt: the games you want on the tiles, one file name (or part of one) per line.
    std::ifstream wanted(roms / L"tiles.txt", std::ios::binary);
    std::string line;
    while (out.size() < most && std::getline(wanted, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        int n = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), int(line.size()), nullptr, 0);
        std::wstring wide(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.c_str(), int(line.size()), wide.data(), n);
        wide = lower(wide);
        for (auto& f : found)
            if (f.stem.find(wide) != std::wstring::npos && std::none_of(out.begin(), out.end(), [&](const OwnGame& o) { return o.rom == f.game.rom; })) {
                out.push_back(f.game);
                break;
            }
    }
    if (out.empty()) {  // else the newest files: the games you added last
        std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.when > b.when; });
        for (auto& f : found)
            if (out.size() < most) out.push_back(f.game);
    }
    return out;
}

int writePlaylists(const Config& c) {
    std::error_code ec;
    fs::path roms = c.roms, retro = fs::path(c.retroarch).parent_path();
    fs::path out = retro / L"playlists";
    fs::create_directories(out, ec);
    int total = 0;
    for (auto& sys : SYSTEMS) {
        fs::path dir = roms / sys.folder;
        fs::path core = retro / L"cores" / (std::wstring(sys.core) + L"_libretro.dll");
        std::vector<fs::path> games;
        std::set<fs::path> referenced;
        if (fs::is_directory(dir, ec)) {
            for (auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
                if (!e.is_regular_file(ec)) continue;
                auto ext = lower(e.path().extension().wstring());
                if (ext == L".m3u" || ext == L".cue") collectReferenced(e.path(), referenced);
                if (!sys.extensions.count(ext)) continue;
                if (lower(e.path().filename().wstring()) == L"prboom.wad") continue;  // engine data, not a game
                games.push_back(e.path());
            }
        }
        std::sort(games.begin(), games.end());
        std::string corePath = json(utf8(core.wstring())), coreName = json(sys.coreName);
        std::string body = "{\n  \"version\": \"1.5\",\n  \"default_core_path\": \"" + corePath + "\",\n  \"default_core_name\": \"" +
                           coreName + "\",\n  \"label_display_mode\": 0,\n  \"right_thumbnail_mode\": 0,\n  \"left_thumbnail_mode\": 0,\n"
                           "  \"sort_mode\": 0,\n  \"items\": [";
        int count = 0;
        for (auto& g : games) {
            std::error_code ec2;
            if (referenced.count(fs::weakly_canonical(g, ec2))) continue;
            body += std::string(count ? "," : "") + "\n    {\n      \"path\": \"" + json(utf8(g.wstring())) + "\",\n      \"label\": \"" +
                    json(utf8(g.stem().wstring())) + "\",\n      \"core_path\": \"" + corePath + "\",\n      \"core_name\": \"" + coreName +
                    "\",\n      \"crc32\": \"DETECT\",\n      \"db_name\": \"" + json(sys.playlist) + ".lpl\"\n    }";
            count++;
        }
        body += "\n  ]\n}\n";
        fs::path file = out / (std::string(sys.playlist) + ".lpl");
        if (!count) { fs::remove(file, ec); continue; }  // no empty playlists in the menu
        std::ofstream(file, std::ios::binary) << body;
        total += count;
        hostLog("playlist %s: %d game(s)", sys.playlist, count);
    }
    return total;
}
