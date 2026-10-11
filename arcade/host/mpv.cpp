#include "media.h"
#include <winrt/Windows.Data.Json.h>
#include <filesystem>
#include <set>

using namespace winrt::Windows::Data::Json;
namespace fs = std::filesystem;

std::wstring MpvControl::pipeName() { return L"\\\\.\\pipe\\echoarcade-mpv-" + std::to_wstring(GetCurrentProcessId()); }

MpvControl::~MpvControl() { disconnect(); }

void MpvControl::connect() {
    disconnect();
    running_ = true;
    position_ = 0; duration_ = 0; paused_ = false;
    playlistPos_ = -1; playlistCount_ = 0;
    { std::lock_guard<std::mutex> lock(titleLock_); title_.clear(); }
    reader_ = std::thread([this] { readLoop(); });
}

void MpvControl::disconnect() {
    running_ = false;
    connected_ = false;
    if (pipe_ != INVALID_HANDLE_VALUE) CancelIoEx(pipe_, nullptr);
    if (reader_.joinable()) reader_.join();
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
}

void MpvControl::command(const std::string& args) {
    if (!connected_) return;
    std::string line = "{\"command\":" + args + "}\n";
    std::lock_guard<std::mutex> lock(writeLock_);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    if (!WriteFile(pipe_, line.data(), DWORD(line.size()), nullptr, &ov) && GetLastError() == ERROR_IO_PENDING)
        GetOverlappedResult(pipe_, &ov, &written, TRUE);
    CloseHandle(ov.hEvent);
}

void MpvControl::readLoop() {
    // mpv creates the pipe a moment after it starts.
    while (running_ && pipe_ == INVALID_HANDLE_VALUE) {
        pipe_ = CreateFileW(pipeName().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe_ == INVALID_HANDLE_VALUE) Sleep(200);
    }
    if (!running_) return;
    connected_ = true;
    hostLog("mpv: control pipe connected");
    command(R"(["observe_property",1,"time-pos"])");
    command(R"(["observe_property",2,"duration"])");
    command(R"(["observe_property",3,"pause"])");
    command(R"(["observe_property",4,"volume"])");
    command(R"(["observe_property",5,"media-title"])");
    command(R"(["observe_property",6,"playlist-pos"])");
    command(R"(["observe_property",7,"playlist-count"])");
    std::string buffer;
    char chunk[4096];
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (running_) {
        ResetEvent(ov.hEvent);
        DWORD got = 0;
        if (!ReadFile(pipe_, chunk, sizeof(chunk), nullptr, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) break;
        }
        if (!GetOverlappedResult(pipe_, &ov, &got, TRUE) || !got) break;
        buffer.append(chunk, got);
        size_t nl;
        while ((nl = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            try {
                auto obj = JsonObject::Parse(winrt::to_hstring(line));
                if (obj.GetNamedString(L"event", L"") != L"property-change") continue;
                auto name = obj.GetNamedString(L"name", L"");
                auto data = obj.TryLookup(L"data");
                if (!data) continue;
                if (data.ValueType() == JsonValueType::Number) {
                    double v = data.GetNumber();
                    if (name == L"time-pos") position_ = v;
                    else if (name == L"duration") duration_ = v;
                    else if (name == L"volume") volume_ = v;
                    else if (name == L"playlist-pos") playlistPos_ = int(v);
                    else if (name == L"playlist-count") playlistCount_ = int(v);
                } else if (data.ValueType() == JsonValueType::Boolean && name == L"pause") {
                    paused_ = data.GetBoolean();
                } else if (data.ValueType() == JsonValueType::String && name == L"media-title") {
                    std::lock_guard<std::mutex> lock(titleLock_);
                    title_ = std::wstring(data.GetString());
                }
            } catch (...) {}
        }
    }
    CloseHandle(ov.hEvent);
    connected_ = false;
}

// ---- local files ----
static const std::set<std::wstring> VIDEO = {L".mkv", L".mp4", L".m4v", L".avi", L".mov", L".webm", L".wmv", L".flv",
                                             L".ts", L".m2ts", L".mpg", L".mpeg", L".ogv", L".3gp", L".vob"};

bool isVideoFile(const std::wstring& path) {
    std::wstring ext = fs::path(path).extension().wstring();
    for (auto& ch : ext) ch = wchar_t(towlower(ch));
    return VIDEO.count(ext) > 0;
}

std::vector<FileEntry> listVideos(const std::wstring& folder) {
    std::vector<FileEntry> dirs, files;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec)) {
        std::wstring name = e.path().filename().wstring();
        if (!name.empty() && name[0] == L'.') continue;
        if (e.is_directory(ec)) dirs.push_back({name, e.path().wstring(), true});
        else if (isVideoFile(name)) files.push_back({e.path().stem().wstring(), e.path().wstring(), false});
    }
    auto byName = [](const FileEntry& a, const FileEntry& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    dirs.insert(dirs.end(), files.begin(), files.end());
    return dirs;
}
