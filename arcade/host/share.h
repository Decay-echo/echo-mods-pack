#pragma once
#include "host.h"
#include <winsock2.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Tablet sharing between players (share.cpp). SHARE MY TABLET streams this tablet's screen
// (JPEG pictures, up to 15 a second, only when it changes) and the sound of the app on it
// (the app's own audio, 48 kHz stereo IMA ADPCM) over TCP to whoever connects with the share
// code; WATCH A FRIEND shows a friend's tablet the same way. Nothing is controlled remotely:
// a viewer only looks.
//
// SPLIT SCREEN is a share where the viewers send their own tablets back (up to 8 pictures a
// second): the host lays them out (2 halves, 3 three ways, 4 quadrants) and that layout is
// what everyone sees. A viewer also tells whether they are looking at the shared screen or
// away on their own tablet (no pictures go to a viewer who is away).
//
// The share code is this PC's public IPv4 address, the port and a 12-bit secret as 12 letters
// and digits ("XXXX-XXXX-XXXX"). The server asks the router for a port mapping (UPnP) and the
// internet for the public address; a second code with the LAN address serves friends on the
// same network or VPN.

struct SharePicture {
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;
};

enum class ShareState : uint8_t { Looking = 0, Away = 1 };  // a viewer: at the shared screen, or on their own tablet

struct Participant {  // a connected viewer, as the split screen sees it
    std::wstring name;
    std::shared_ptr<const SharePicture> picture;  // their tablet (split screen only; null until one came)
    ShareState state = ShareState::Looking;
};

std::wstring shareCodeText(uint32_t ip, uint16_t port, uint16_t secret);  // ip in host order
bool shareCodeParse(const std::wstring& text, uint32_t& ip, uint16_t& port, uint16_t& secret);

// IMA ADPCM, 4 bits a sample, one state per channel (tests use these too).
struct AdpcmState { int predictor = 0, index = 0; };
void adpcmEncode(const int16_t* samples, int count, AdpcmState& state, uint8_t* out);  // count even; out: count/2 bytes
void adpcmDecode(const uint8_t* in, int count, AdpcmState& state, int16_t* samples);

class ShareServer {
public:
    ShareServer();
    ~ShareServer();
    void start(uint16_t port, const std::wstring& name, bool split);
    void stop();
    bool running() const { return running_; }
    bool split() const { return split_; }
    std::wstring code() const;     // the public code ("" until the address is known)
    std::wstring lanCode() const;  // the LAN code
    std::wstring status() const;   // one line for the tile
    int viewers() const { return viewers_; }
    std::vector<Participant> participants() const;  // the connected viewers, in the order they came
    // The latest frame to send (the main thread, after each publish): copied and encoded when
    // someone watches, at most 15 times a second, and only when it changed.
    void offer(const uint32_t* bgra, int w, int h);
    // The app whose sound goes with the picture (0 = none): its process tree, captured apart
    // from everything else on the PC (Windows 11 / Windows 10 20348+).
    void audioSource(DWORD pid);

private:
    struct Client;
    void acceptLoop();
    void serve(std::shared_ptr<Client> c);
    void read(std::shared_ptr<Client> c);
    void encodeLoop();
    void captureLoop(DWORD pid);
    void findAddress();
    void setStatus(const std::wstring& s);
    void pushAudio(const std::vector<uint8_t>& chunk);

    std::atomic<bool> running_{false}, stopping_{false}, split_{false};
    std::atomic<int> viewers_{0};
    SOCKET listen_ = INVALID_SOCKET;
    uint16_t port_ = 0, secret_ = 0;
    uint32_t publicIp_ = 0, lanIp_ = 0;
    bool mapped_ = false;
    std::wstring name_;
    mutable std::mutex statusLock_;
    std::wstring status_;

    std::thread acceptThread_, encodeThread_, addressThread_, captureThread_;
    std::vector<std::thread> clientThreads_;
    mutable std::mutex clientsLock_;
    std::vector<std::shared_ptr<Client>> clients_;

    std::mutex rawLock_;
    std::condition_variable rawCv_;
    std::vector<uint32_t> raw_;
    int rawW_ = 0, rawH_ = 0;
    uint64_t rawSerial_ = 0, lastOffer_ = 0;

    std::mutex frameLock_;
    std::condition_variable frameCv_;
    std::shared_ptr<const std::vector<uint8_t>> jpeg_;
    std::atomic<uint64_t> jpegSerial_{0};

    std::atomic<DWORD> audioPid_{0};
    DWORD capturing_ = 0;
    std::atomic<bool> captureStop_{false};
};

class ShareClient {
public:
    ShareClient();
    ~ShareClient();
    // Connects on its own thread; the friend's sound plays on `audioDeviceId` ("" = default).
    void connect(const std::wstring& code, const std::wstring& myName, const std::wstring& audioDeviceId);
    void disconnect();
    bool active() const { return active_; }       // connecting or connected
    bool connected() const { return connected_; }
    bool failed() const { return failed_; }
    bool split() const { return split_; }         // the host runs a split screen: my tablet goes up
    std::wstring status() const;
    std::wstring friendName() const;
    std::wstring code() const { return code_; }
    std::shared_ptr<const SharePicture> latest();  // the newest picture (null until one arrived)
    uint64_t received() const { return received_; }
    // My own tablet for the host's split screen (the main thread, after each publish; at most
    // 8 a second, only when it changed), and whether I am looking at the shared screen.
    void offerUp(const uint8_t* bgra, int w, int h, int pitch);
    void setState(ShareState state);

private:
    void run(uint32_t ip, uint16_t port, uint16_t secret, std::wstring myName);
    void renderLoop();
    void uplinkLoop();
    void setStatus(const std::wstring& s);

    std::atomic<bool> active_{false}, connected_{false}, failed_{false}, stopping_{false}, split_{false};
    std::atomic<uint64_t> received_{0};
    std::thread thread_, renderThread_, upThread_;
    SOCKET socket_ = INVALID_SOCKET;
    std::mutex sendLock_;  // pings, pictures and states go out from the uplink thread only, but it stays safe
    std::wstring code_, audioDevice_;
    mutable std::mutex lock_;
    std::wstring status_, friendName_;
    std::shared_ptr<const SharePicture> picture_;

    std::mutex pcmLock_;
    std::deque<int16_t> pcm_;  // decoded, interleaved stereo 48 kHz, waiting for the render loop

    std::mutex upLock_;
    std::condition_variable upCv_;
    std::vector<uint32_t> up_;
    int upW_ = 0, upH_ = 0;
    uint64_t upSerial_ = 0, lastUp_ = 0;
    std::atomic<uint8_t> state_{0};
};
