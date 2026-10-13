// Movie playback (mpv), YouTube (yt-dlp) and Plex browsing for ArcadeHost.
#pragma once
#include "host.h"
#include <atomic>
#include <functional>
#include <thread>

// ---- mpv JSON IPC (named pipe) ----
class MpvControl {
public:
    ~MpvControl();
    static std::wstring pipeName();
    void connect();                    // retries in the background until mpv's pipe exists
    void disconnect();
    bool connected() const { return connected_; }
    void command(const std::string& jsonArray);  // e.g. ["cycle","pause"]
    double position() const { return position_; }
    double duration() const { return duration_; }
    bool paused() const { return paused_; }
    double volume() const { return volume_; }
    std::wstring mediaTitle() const { std::lock_guard<std::mutex> lock(titleLock_); return title_; }
    int playlistPos() const { return playlistPos_; }      // 0-based, -1 = none
    int playlistCount() const { return playlistCount_; }
private:
    void readLoop();
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::thread reader_;
    std::mutex writeLock_;
    std::atomic<bool> running_{false}, connected_{false}, paused_{false};
    std::atomic<double> position_{0}, duration_{0}, volume_{100};
    std::atomic<int> playlistPos_{-1}, playlistCount_{0};
    mutable std::mutex titleLock_;
    std::wstring title_;
};

// ---- YouTube: yt-dlp finds videos (search, playlists, mixes); mpv plays them ----
struct YtVideo { std::wstring title, info, url, thumb; };   // info: channel and length; thumb: a picture's URL
struct YtLink { std::wstring name, url; bool playlist; };    // a line of youtube.txt
std::vector<YtLink> loadYouTubeLinks(const std::wstring& file);
// These run yt-dlp and block (seconds); ArcadeHost calls them on worker threads.
bool ytSearch(const std::wstring& ytdlp, const std::wstring& query, std::vector<YtVideo>& out, std::wstring& error, int count = 20);
// The signed-in home feed (recommendations), with the cookies file an exported cookies.txt gives.
bool ytFeed(const std::wstring& ytdlp, const std::wstring& cookies, std::vector<YtVideo>& out, std::wstring& error);
// Signing in without a browser: Google's "device code" (the TV way). ytDeviceCode asks for one
// (the user enters userCode at url on a phone); ytDevicePoll asks every `interval` seconds
// whether they did (0 waiting, 1 signed in, 2 slow down, -1 failed); ytRefreshToken trades the
// refresh token for a fresh access token; ytFeedSignedIn fetches the home feed with it
// (InnerTube, the TV client); the raw answer is written to dumpPath when nothing was found.
bool ytDeviceCode(std::wstring& deviceCode, std::wstring& userCode, std::wstring& url, int& interval, int& expires, std::wstring& error);
int ytDevicePoll(const std::wstring& deviceCode, std::wstring& refresh, std::wstring& access, int& expiresIn, std::wstring& error);
bool ytRefreshToken(const std::wstring& refresh, std::wstring& access, int& expiresIn, std::wstring& error);
bool ytFeedSignedIn(const std::wstring& access, std::vector<YtVideo>& out, std::wstring& error, const std::wstring& dumpPath);
// YouTube needs a JavaScript runtime in yt-dlp now: Deno's path (empty = none, searches may fail).
void ytSetDeno(const std::wstring& deno);
std::wstring ytRuntimeArgs();  // the yt-dlp options for it ("" without Deno)
bool ytPlaylist(const std::wstring& ytdlp, const std::wstring& url, std::wstring& title, std::vector<YtVideo>& out,
                std::wstring& error);
std::wstring ytUpdate(const std::wstring& ytdlp);  // yt-dlp -U; returns what it said

// ---- YouTube on this PC: the browser window showing YouTube, captured onto the tablet ----
// The browser keeps playing its own sound. Controls go through Windows' media sessions (what the
// volume flyout's media controls use), so the browser never needs focus.
HWND findYouTubeWindow();                       // a visible top-level window titled "... YouTube ..."
std::wstring youTubeWindowTitle(HWND window);   // the video's name, without " - YouTube - <browser>"
struct MediaInfo {
    bool valid = false, playing = false, canSeek = false;
    std::wstring title, artist;
    double position = -1, duration = -1;        // seconds; -1 = unknown
    std::vector<uint8_t> art;                   // the album art / thumbnail (a JPEG or PNG), when asked for
};
enum class MediaCommand { PlayPause, Next, Previous, Seek };
// Both block briefly; ArcadeHost calls them on worker threads. `title`: the window's video name,
// used to pick the browser's media session when several apps have one.
MediaInfo mediaInfo(const std::wstring& title);
bool mediaCommand(MediaCommand command, const std::wstring& title, double seconds = 0);  // Seek: to `seconds`
// The MEDIA page: whatever Windows counts as the current media session (Spotify, a browser, mpv...).
MediaInfo mediaNowPlaying(std::wstring& app, bool withArt = false);
bool mediaControl(MediaCommand command);
// Presses YouTube's own "Skip" button in the browser window once an ad offers it, through UI
// Automation (no mouse, no focus change). Returns true if it skipped one. Blocks briefly; call it on
// a worker thread. Unskippable ads are left alone: there is no button to press.
bool skipYouTubeAd(HWND window);

// Using the page like a tablet. Input is posted to the browser window, so the cursor never moves
// and Echo VR keeps focus. Points are screen pixels; the UIA ones block briefly (worker threads).
bool windowFrame(HWND window, RECT& screen);              // what the window capture shows
bool youTubePageArea(HWND window, RECT& screen);          // the web page, without tabs and address bar
void pageHover(HWND window, POINT at);
void pageTap(HWND window, POINT at);
void pageScroll(HWND window, POINT at, int right, int up);  // wheel units (120 = a notch)
bool browserBack(HWND window);
bool youTubeHome(HWND window);
bool youTubeSearch(HWND window, const std::wstring& query);

// ---- Plex (plex.tv PIN link + direct play from your server) ----
struct PlexItem {
    std::wstring title, subtitle;
    std::string key;         // browse path (/library/...) for folders
    std::string ratingKey;   // metadata id for playable items
    std::string partKey;     // /library/parts/... for playable items
    std::string type;        // movie, episode, show, season, library, ...
    double resumeSeconds = 0, durationSeconds = 0;
    bool playable() const { return !partKey.empty(); }
};

class Plex {
public:
    Plex();
    bool linked() const { return !token_.empty(); }
    bool serverReady() const { return !server_.empty(); }
    std::wstring serverName() const { return serverName_; }

    // All network calls block; ArcadeHost runs them on worker threads.
    bool startLink(std::wstring& code, std::string& pinId);  // shows a code for plex.tv/link
    bool pollLink(const std::string& pinId);                 // true once the user approved
    bool findServer(std::wstring& error);                    // picks a reachable server
    std::vector<PlexItem> libraries();                       // plus "Continue Watching"
    std::vector<PlexItem> children(const std::string& key);
    std::wstring streamUrl(const PlexItem& item) const;
    void timeline(const PlexItem& item, const char* state, double seconds);
    void unlink();

private:
    std::string get(const std::wstring& url, const char* method = "GET", int timeoutSeconds = 8);
    void save();
    std::string clientId_, token_;
    std::wstring server_, serverName_;
    std::string serverToken_;
};

// Local video files and folders under the configured movie folders.
struct FileEntry { std::wstring name, path; bool folder; };
std::vector<FileEntry> listVideos(const std::wstring& folder);
bool isVideoFile(const std::wstring& path);
