// Tablet sharing between players (see share.h).
#include "share.h"
#include "thumbs.h"
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <objbase.h>
#include <objidl.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <natupnp.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <algorithm>
#include <chrono>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mmdevapi.lib")

namespace {

const wchar_t* const ALPHABET = L"ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // 32: no I, O, 0, 1
const char MAGIC[4] = {'E', 'A', 'T', 'S'};
constexpr uint16_t VERSION = 2;
constexpr int SAMPLE_RATE = 48000, CHANNELS = 2, CHUNK_FRAMES = 960;  // 20 ms a chunk
constexpr uint64_t FRAME_INTERVAL = 66, UP_INTERVAL = 125;            // ms: 15 pictures a second down, 8 up, at most
constexpr int JPEG_QUALITY = 62, UP_QUALITY = 50;
enum Msg : uint8_t { MsgPicture = 1, MsgAudio = 2, MsgPing = 3, MsgState = 4 };

uint64_t now() { return GetTickCount64(); }

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::wstring dotted(uint32_t ip) {
    wchar_t b[32];
    swprintf_s(b, L"%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

// ---- GDI+: JPEG in and out
void gdiplus() {
    static ULONG_PTR token = [] {
        ULONG_PTR t = 0;
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&t, &input, nullptr);
        return t;
    }();
    (void)token;
}

CLSID jpegEncoder() {
    static CLSID clsid = [] {
        CLSID c{};
        UINT n = 0, size = 0;
        Gdiplus::GetImageEncodersSize(&n, &size);
        std::vector<uint8_t> buffer(size);
        auto* info = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
        if (size && Gdiplus::GetImageEncoders(n, size, info) == Gdiplus::Ok)
            for (UINT i = 0; i < n; i++)
                if (wcscmp(info[i].MimeType, L"image/jpeg") == 0) { c = info[i].Clsid; break; }
        return c;
    }();
    return clsid;
}

bool encodeJpeg(const uint32_t* bgra, int w, int h, int quality, std::vector<uint8_t>& out) {
    gdiplus();
    Gdiplus::Bitmap bitmap(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(const_cast<uint32_t*>(bgra)));
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    Gdiplus::EncoderParameters params;
    params.Count = 1;
    params.Parameter[0].Guid = Gdiplus::EncoderQuality;
    params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    ULONG q = ULONG(quality);
    params.Parameter[0].Value = &q;
    CLSID clsid = jpegEncoder();
    bool ok = bitmap.Save(stream, &clsid, &params) == Gdiplus::Ok;
    if (ok) {
        STATSTG st{};
        stream->Stat(&st, STATFLAG_NONAME);
        out.resize(size_t(st.cbSize.QuadPart));
        LARGE_INTEGER zero{};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG got = 0;
        ok = SUCCEEDED(stream->Read(out.data(), ULONG(out.size()), &got)) && got == out.size();
    }
    stream->Release();
    return ok;
}

std::shared_ptr<SharePicture> decodeJpeg(const uint8_t* data, size_t size) {
    gdiplus();
    auto pic = std::make_shared<SharePicture>();
    IStream* stream = SHCreateMemStream(data, UINT(size));
    if (!stream) return pic;
    {
        Gdiplus::Bitmap bitmap(stream);
        if (bitmap.GetLastStatus() == Gdiplus::Ok && bitmap.GetWidth() && bitmap.GetHeight()) {
            int w = int(bitmap.GetWidth()), h = int(bitmap.GetHeight());
            Gdiplus::BitmapData d{};
            Gdiplus::Rect all(0, 0, w, h);
            if (bitmap.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &d) == Gdiplus::Ok) {
                pic->w = w;
                pic->h = h;
                pic->bgra.resize(size_t(w) * h * 4);
                for (int y = 0; y < h; y++)
                    memcpy(pic->bgra.data() + size_t(y) * w * 4, static_cast<uint8_t*>(d.Scan0) + size_t(y) * d.Stride, size_t(w) * 4);
                bitmap.UnlockBits(&d);
            }
        }
    }
    stream->Release();
    return pic;
}

// ---- sockets
struct Winsock {
    Winsock() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
};
void winsock() { static Winsock w; }

bool sendAll(SOCKET s, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size) {
        int n = send(s, p, int(std::min<size_t>(size, 1u << 20)), 0);
        if (n <= 0) return false;
        p += n;
        size -= size_t(n);
    }
    return true;
}
bool recvAll(SOCKET s, void* data, size_t size) {
    char* p = static_cast<char*>(data);
    while (size) {
        int n = recv(s, p, int(std::min<size_t>(size, 1u << 20)), 0);
        if (n <= 0) return false;
        p += n;
        size -= size_t(n);
    }
    return true;
}
bool sendMsg(SOCKET s, uint8_t type, const void* data, uint32_t size) {
    uint8_t head[5] = {type};
    memcpy(head + 1, &size, 4);
    return sendAll(s, head, 5) && (size == 0 || sendAll(s, data, size));
}
void keepAlive(SOCKET s) {  // a dead peer noticed within half a minute
    tcp_keepalive k{1, 10000, 3000};
    DWORD got = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &k, sizeof k, nullptr, 0, &got, nullptr, nullptr);
}
void timeouts(SOCKET s, DWORD sendMs, DWORD recvMs) {
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendMs), sizeof sendMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recvMs), sizeof recvMs);
}
uint32_t lanAddress() {
    winsock();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    uint32_t ip = 0;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        sockaddr_in me{};
        int len = sizeof me;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) ip = ntohl(me.sin_addr.s_addr);
    }
    closesocket(s);
    return ip;
}

// ---- the audio of one process tree (Windows 11 / Windows 10 20348+)
class Activator : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    IAudioClient* client = nullptr;
    HRESULT result = E_FAIL;
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *out = static_cast<IAgileObject*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ULONG(InterlockedIncrement(&refs_)); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = ULONG(InterlockedDecrement(&refs_));
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        IUnknown* unknown = nullptr;
        HRESULT hr = E_FAIL;
        op->GetActivateResult(&hr, &unknown);
        result = hr;
        if (SUCCEEDED(hr) && unknown) {
            unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client));
            unknown->Release();
        }
        SetEvent(done);
        return S_OK;
    }
private:
    ~Activator() { CloseHandle(done); }
    LONG refs_ = 1;
};

WAVEFORMATEX pcmFormat() {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = CHANNELS;
    f.nSamplesPerSec = SAMPLE_RATE;
    f.wBitsPerSample = 16;
    f.nBlockAlign = WORD(CHANNELS * 2);
    f.nAvgBytesPerSec = SAMPLE_RATE * f.nBlockAlign;
    return f;
}

// ---- IMA ADPCM tables
const int INDEX_STEP[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int STEPS[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                       31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                       130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                       544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                       2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                       9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

uint8_t adpcmNibble(int sample, AdpcmState& st) {
    int step = STEPS[st.index], diff = sample - st.predictor, nibble = 0;
    if (diff < 0) { nibble = 8; diff = -diff; }
    int delta = step >> 3;
    if (diff >= step) { nibble |= 4; diff -= step; delta += step; }
    if (diff >= step / 2) { nibble |= 2; diff -= step / 2; delta += step / 2; }
    if (diff >= step / 4) { nibble |= 1; delta += step / 4; }
    st.predictor = std::clamp(st.predictor + ((nibble & 8) ? -delta : delta), -32768, 32767);
    st.index = std::clamp(st.index + INDEX_STEP[nibble], 0, 88);
    return uint8_t(nibble);
}
int16_t adpcmSample(uint8_t nibble, AdpcmState& st) {
    int step = STEPS[st.index], delta = step >> 3;
    if (nibble & 4) delta += step;
    if (nibble & 2) delta += step / 2;
    if (nibble & 1) delta += step / 4;
    st.predictor = std::clamp(st.predictor + ((nibble & 8) ? -delta : delta), -32768, 32767);
    st.index = std::clamp(st.index + INDEX_STEP[nibble], 0, 88);
    return int16_t(st.predictor);
}

// An audio chunk on the wire: u16 frames, u8 channels, u8 pad, then a channel at a time:
// i16 predictor, u8 index, u8 pad, frames/2 bytes of nibbles.
std::vector<uint8_t> packAudio(const int16_t* interleaved, int frames, AdpcmState state[CHANNELS]) {
    std::vector<uint8_t> out(4 + CHANNELS * (4 + size_t(frames) / 2));
    out[0] = uint8_t(frames & 255);
    out[1] = uint8_t(frames >> 8);
    out[2] = CHANNELS;
    size_t at = 4;
    std::vector<int16_t> mono(static_cast<size_t>(frames));
    for (int c = 0; c < CHANNELS; c++) {
        for (int i = 0; i < frames; i++) mono[size_t(i)] = interleaved[size_t(i) * CHANNELS + c];
        int16_t predictor = int16_t(state[c].predictor);
        memcpy(out.data() + at, &predictor, 2);
        out[at + 2] = uint8_t(state[c].index);
        at += 4;
        adpcmEncode(mono.data(), frames, state[c], out.data() + at);
        at += size_t(frames) / 2;
    }
    return out;
}
bool unpackAudio(const uint8_t* data, size_t size, std::vector<int16_t>& interleaved) {
    if (size < 4) return false;
    int frames = data[0] | (data[1] << 8), channels = data[2];
    if (channels != CHANNELS || frames <= 0 || frames % 2 || size < 4 + size_t(channels) * (4 + size_t(frames) / 2)) return false;
    interleaved.assign(size_t(frames) * CHANNELS, 0);
    size_t at = 4;
    std::vector<int16_t> mono(static_cast<size_t>(frames));
    for (int c = 0; c < CHANNELS; c++) {
        AdpcmState st;
        int16_t predictor;
        memcpy(&predictor, data + at, 2);
        st.predictor = predictor;
        st.index = std::clamp<int>(data[at + 2], 0, 88);
        at += 4;
        adpcmDecode(data + at, frames, st, mono.data());
        at += size_t(frames) / 2;
        for (int i = 0; i < frames; i++) interleaved[size_t(i) * CHANNELS + c] = mono[size_t(i)];
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------- codes, ADPCM (public)

std::wstring shareCodeText(uint32_t ip, uint16_t port, uint16_t secret) {
    uint64_t v = (uint64_t(ip) << 28) | (uint64_t(port) << 12) | (secret & 0xfff);
    std::wstring s(12, L'A');
    for (int i = 11; i >= 0; i--) { s[size_t(i)] = ALPHABET[v & 31]; v >>= 5; }
    return s.substr(0, 4) + L"-" + s.substr(4, 4) + L"-" + s.substr(8, 4);
}

bool shareCodeParse(const std::wstring& text, uint32_t& ip, uint16_t& port, uint16_t& secret) {
    uint64_t v = 0;
    int digits = 0;
    for (wchar_t ch : text) {
        wchar_t c = wchar_t(towupper(ch));
        if (c == L'-' || c == L' ') continue;
        if (c == L'I') c = L'J';  // the likely misreads
        if (c == L'O' || c == L'0') c = L'Q';
        if (c == L'1') c = L'L';
        const wchar_t* at = wcschr(ALPHABET, c);
        if (!at || !c) return false;
        v = (v << 5) | uint64_t(at - ALPHABET);
        if (++digits > 12) return false;
    }
    if (digits != 12) return false;
    ip = uint32_t(v >> 28);
    port = uint16_t((v >> 12) & 0xffff);
    secret = uint16_t(v & 0xfff);
    return ip != 0 && port != 0;
}

void adpcmEncode(const int16_t* samples, int count, AdpcmState& state, uint8_t* out) {
    for (int i = 0; i + 1 < count; i += 2) {
        uint8_t lo = adpcmNibble(samples[i], state), hi = adpcmNibble(samples[i + 1], state);
        out[i / 2] = uint8_t(lo | (hi << 4));
    }
}
void adpcmDecode(const uint8_t* in, int count, AdpcmState& state, int16_t* samples) {
    for (int i = 0; i + 1 < count; i += 2) {
        samples[i] = adpcmSample(in[i / 2] & 15, state);
        samples[i + 1] = adpcmSample(in[i / 2] >> 4, state);
    }
}

// ---------------------------------------------------------------- the server

struct ShareServer::Client {
    SOCKET socket = INVALID_SOCKET;
    std::string name;
    std::mutex lock;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> audio;
    std::shared_ptr<const SharePicture> picture;  // their tablet (split screen)
    uint8_t state = 0;                            // ShareState
    bool gone = false, ready = false;
};

std::vector<Participant> ShareServer::participants() const {
    std::vector<Participant> out;
    std::lock_guard<std::mutex> lock(clientsLock_);
    for (auto& c : clients_) {
        std::lock_guard<std::mutex> cl(c->lock);
        if (!c->ready || c->gone) continue;
        out.push_back({wide(c->name), c->picture, ShareState(c->state)});
    }
    return out;
}

// A viewer's side of the socket: pings, its tablet (split screen) and whether it is looking.
void ShareServer::read(std::shared_ptr<Client> c) {
    SOCKET s = c->socket;
    std::vector<uint8_t> payload;
    while (!stopping_) {
        uint8_t head[5];
        if (!recvAll(s, head, sizeof head)) break;
        uint32_t size = 0;
        memcpy(&size, head + 1, 4);
        if (size > (16u << 20)) break;
        payload.resize(size);
        if (size && !recvAll(s, payload.data(), size)) break;
        if (head[0] == MsgPicture && split_) {
            auto pic = decodeJpeg(payload.data(), payload.size());
            if (pic->w) {
                std::lock_guard<std::mutex> lock(c->lock);
                c->picture = pic;
            }
        } else if (head[0] == MsgState && size >= 1) {
            std::lock_guard<std::mutex> lock(c->lock);
            c->state = payload[0];
            c->cv.notify_all();
        }
    }
    std::lock_guard<std::mutex> lock(c->lock);
    c->gone = true;
    if (c->socket != INVALID_SOCKET) closesocket(c->socket);
    c->socket = INVALID_SOCKET;
    c->cv.notify_all();
}

ShareServer::ShareServer() { winsock(); }
ShareServer::~ShareServer() { stop(); }

void ShareServer::setStatus(const std::wstring& s) {
    std::lock_guard<std::mutex> lock(statusLock_);
    status_ = s;
}
std::wstring ShareServer::status() const {
    std::lock_guard<std::mutex> lock(statusLock_);
    return status_;
}
std::wstring ShareServer::code() const { return publicIp_ ? shareCodeText(publicIp_, port_, secret_) : L""; }
std::wstring ShareServer::lanCode() const { return lanIp_ ? shareCodeText(lanIp_, port_, secret_) : L""; }

void ShareServer::start(uint16_t port, const std::wstring& name, bool split) {
    if (running_) return;
    stopping_ = false;
    split_ = split;
    port_ = port;
    name_ = name;
    secret_ = uint16_t((uint32_t(GetTickCount64()) * 2654435761u >> 8) & 0xfff);
    if (!secret_) secret_ = 1;
    publicIp_ = 0;
    lanIp_ = 0;
    mapped_ = false;
    listen_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (listen_ == INVALID_SOCKET || bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(listen_, 4) != 0) {
        int err = WSAGetLastError();
        hostLog("share: could not listen on port %u (%d)", port, err);
        setStatus(L"Could not open port " + std::to_wstring(port) + L" (" + std::to_wstring(err) + L")");
        if (listen_ != INVALID_SOCKET) closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return;
    }
    running_ = true;
    setStatus(L"Finding your address...");
    hostLog("share: %s on port %u as %ls", split ? "split screen" : "sharing the tablet", port, name.c_str());
    acceptThread_ = std::thread([this] { acceptLoop(); });
    encodeThread_ = std::thread([this] { encodeLoop(); });
    addressThread_ = std::thread([this] { findAddress(); });
    captureThread_ = std::thread([this] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD failed = 0;
        while (!stopping_) {
            DWORD pid = audioPid_;
            if (!pid || pid == failed || viewers_ == 0) { Sleep(200); continue; }
            captureLoop(pid);  // until the app, the viewers or the sharing change
            if (capturing_ == 0) failed = pid;  // it never started: not again for this app
        }
        CoUninitialize();
    });
}

void ShareServer::stop() {
    if (!running_) return;
    stopping_ = true;
    running_ = false;
    if (listen_ != INVALID_SOCKET) closesocket(listen_);
    listen_ = INVALID_SOCKET;
    rawCv_.notify_all();
    {
        std::lock_guard<std::mutex> lock(clientsLock_);
        for (auto& c : clients_) {
            std::lock_guard<std::mutex> cl(c->lock);
            c->gone = true;
            if (c->socket != INVALID_SOCKET) closesocket(c->socket);
            c->socket = INVALID_SOCKET;
            c->cv.notify_all();
        }
    }
    for (auto* t : {&acceptThread_, &encodeThread_, &addressThread_, &captureThread_})
        if (t->joinable()) t->join();
    for (auto& t : clientThreads_)
        if (t.joinable()) t.join();
    clientThreads_.clear();
    clients_.clear();
    viewers_ = 0;
    if (mapped_) {  // the router's mapping taken away again, off the UI thread (UPnP is slow)
        uint16_t port = port_;
        std::thread([port] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            IUPnPNAT* nat = nullptr;
            IStaticPortMappingCollection* mappings = nullptr;
            if (SUCCEEDED(CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), reinterpret_cast<void**>(&nat))) &&
                SUCCEEDED(nat->get_StaticPortMappingCollection(&mappings)) && mappings) {
                BSTR tcp = SysAllocString(L"TCP");
                mappings->Remove(port, tcp);
                SysFreeString(tcp);
                mappings->Release();
            }
            if (nat) nat->Release();
            CoUninitialize();
        }).detach();
    }
    setStatus(L"");
    hostLog("share: stopped");
}

void ShareServer::findAddress() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    lanIp_ = lanAddress();
    auto body = httpGetBytes(L"https://api.ipify.org");
    std::string text(body.begin(), body.end());
    in_addr addr{};
    if (!text.empty() && text.size() < 16 && inet_pton(AF_INET, text.c_str(), &addr) == 1) publicIp_ = ntohl(addr.s_addr);
    hostLog("share: LAN address %ls, public address %ls", dotted(lanIp_).c_str(), publicIp_ ? dotted(publicIp_).c_str() : L"unknown");
    if (!stopping_ && lanIp_) {  // the router asked to pass the port to this PC
        IUPnPNAT* nat = nullptr;
        IStaticPortMappingCollection* mappings = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), reinterpret_cast<void**>(&nat));
        if (SUCCEEDED(hr)) hr = nat->get_StaticPortMappingCollection(&mappings);
        if (SUCCEEDED(hr) && mappings) {
            BSTR tcp = SysAllocString(L"TCP"), ip = SysAllocString(dotted(lanIp_).c_str()), what = SysAllocString(L"Echo Arcade tablet share");
            IStaticPortMapping* mapping = nullptr;
            hr = mappings->Add(port_, tcp, port_, ip, VARIANT_TRUE, what, &mapping);
            mapped_ = SUCCEEDED(hr) && mapping;
            if (mapping) mapping->Release();
            SysFreeString(tcp);
            SysFreeString(ip);
            SysFreeString(what);
            mappings->Release();
            hostLog("share: UPnP port mapping %s (0x%08lx)", mapped_ ? "made" : "refused", hr);
        } else {
            hostLog("share: no UPnP on this network (0x%08lx): forward TCP port %u to this PC by hand, or use the LAN code", hr, port_);
        }
        if (nat) nat->Release();
    }
    if (!publicIp_ && !lanIp_) setStatus(L"No network address found");
    else if (!publicIp_) setStatus(L"No internet address (LAN code only)");
    else setStatus(mapped_ ? L"" : L"No UPnP: forward TCP port " + std::to_wstring(port_) + L" to this PC");
    hostLog("share: your code %ls, LAN code %ls", code().empty() ? L"(none)" : code().c_str(), lanCode().empty() ? L"(none)" : lanCode().c_str());
    CoUninitialize();
}

void ShareServer::acceptLoop() {
    while (!stopping_) {
        sockaddr_in from{};
        int len = sizeof from;
        SOCKET s = accept(listen_, reinterpret_cast<sockaddr*>(&from), &len);
        if (s == INVALID_SOCKET) { if (stopping_) break; Sleep(100); continue; }
        auto c = std::make_shared<Client>();
        c->socket = s;
        {
            std::lock_guard<std::mutex> lock(clientsLock_);
            clients_.push_back(c);
            clientThreads_.erase(std::remove_if(clientThreads_.begin(), clientThreads_.end(), [](std::thread& t) { return !t.joinable(); }), clientThreads_.end());
            clientThreads_.emplace_back([this, c] { serve(c); });
        }
    }
}

void ShareServer::serve(std::shared_ptr<Client> c) {
    SOCKET s = c->socket;
    keepAlive(s);
    timeouts(s, 8000, 30000);
    // hello: magic, version, secret, name
    uint8_t head[9];
    bool ok = recvAll(s, head, sizeof head) && memcmp(head, MAGIC, 4) == 0;
    uint16_t version = 0, secret = 0;
    std::string name;
    if (ok) {
        memcpy(&version, head + 4, 2);
        memcpy(&secret, head + 6, 2);
        name.resize(head[8]);
        ok = (name.empty() || recvAll(s, name.data(), name.size())) && version == VERSION;
    }
    bool accepted = ok && secret == secret_;
    if (ok) {
        std::string mine = utf8(name_);
        if (mine.size() > 255) mine.resize(255);
        std::vector<uint8_t> reply(9 + mine.size());
        memcpy(reply.data(), MAGIC, 4);
        memcpy(reply.data() + 4, &VERSION, 2);
        reply[6] = accepted ? 1 : 0;
        reply[7] = split_ ? 1 : 0;  // flags: bit 0 = send me your tablet
        reply[8] = uint8_t(mine.size());
        memcpy(reply.data() + 9, mine.data(), mine.size());
        ok = sendAll(s, reply.data(), reply.size());
    }
    if (!ok || !accepted) {
        hostLog("share: a connection was refused (%s)", !ok ? "bad hello" : "wrong code");
    } else {
        c->name = name;
        {
            std::lock_guard<std::mutex> lock(clientsLock_);
            c->ready = true;
            clientThreads_.emplace_back([this, c] { read(c); });
        }
        viewers_++;
        hostLog("share: %s is watching (%d now)", name.c_str(), int(viewers_));
        uint64_t sent = 0, lastSend = now();
        while (!stopping_) {
            std::vector<std::vector<uint8_t>> audio;
            bool looking = true;
            {
                std::unique_lock<std::mutex> lock(c->lock);
                c->cv.wait_for(lock, std::chrono::milliseconds(300), [&] { return c->gone || stopping_ || !c->audio.empty() || jpegSerial_ > sent; });
                if (c->gone) break;
                looking = c->state == uint8_t(ShareState::Looking);
                while (!c->audio.empty()) { audio.push_back(std::move(c->audio.front())); c->audio.pop_front(); }
            }
            if (!looking) { audio.clear(); sent = jpegSerial_; }  // away on their own tablet: nothing to show them
            bool good = true, sentNow = !audio.empty();
            for (auto& chunk : audio) good = good && sendMsg(s, MsgAudio, chunk.data(), uint32_t(chunk.size()));
            if (good && jpegSerial_ > sent) {
                std::shared_ptr<const std::vector<uint8_t>> picture;
                {
                    std::lock_guard<std::mutex> lock(frameLock_);
                    picture = jpeg_;
                    sent = jpegSerial_;
                }
                if (picture) { good = sendMsg(s, MsgPicture, picture->data(), uint32_t(picture->size())); sentNow = true; }
            }
            if (sentNow) lastSend = now();
            else if (good && now() - lastSend > 2000) { good = sendMsg(s, MsgPing, nullptr, 0); lastSend = now(); }  // a quiet screen: the viewer still knows we are here
            if (!good) break;
        }
        viewers_--;
        hostLog("share: %s left (%d now)", name.c_str(), int(viewers_));
    }
    {
        std::lock_guard<std::mutex> lock(c->lock);
        if (c->socket != INVALID_SOCKET) closesocket(c->socket);
        c->socket = INVALID_SOCKET;
        c->gone = true;
    }
    std::lock_guard<std::mutex> lock(clientsLock_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), c), clients_.end());
}

void ShareServer::offer(const uint32_t* bgra, int w, int h) {
    if (!running_ || viewers_ == 0) return;
    uint64_t t = now();
    if (t - lastOffer_ < FRAME_INTERVAL) return;
    lastOffer_ = t;
    {
        std::lock_guard<std::mutex> lock(rawLock_);
        raw_.assign(bgra, bgra + size_t(w) * h);
        rawW_ = w;
        rawH_ = h;
        rawSerial_++;
    }
    rawCv_.notify_one();
}

void ShareServer::encodeLoop() {
    uint64_t encoded = 0, lastHash = 0;
    std::vector<uint32_t> copy;
    int w = 0, h = 0;
    while (!stopping_) {
        {
            std::unique_lock<std::mutex> lock(rawLock_);
            rawCv_.wait_for(lock, std::chrono::milliseconds(200), [&] { return stopping_ || rawSerial_ != encoded; });
            if (stopping_) break;
            if (rawSerial_ == encoded) continue;
            encoded = rawSerial_;
            copy = raw_;
            w = rawW_;
            h = rawH_;
        }
        uint64_t hash = 1469598103934665603ull;  // a sample of the pixels: an unchanged screen sends nothing
        for (size_t i = 0; i < copy.size(); i += 7) { hash ^= copy[i]; hash *= 1099511628211ull; }
        if (hash == lastHash) continue;
        lastHash = hash;
        auto out = std::make_shared<std::vector<uint8_t>>();
        if (!encodeJpeg(copy.data(), w, h, JPEG_QUALITY, *out)) continue;
        {
            std::lock_guard<std::mutex> lock(frameLock_);
            jpeg_ = out;
            jpegSerial_++;
        }
        std::lock_guard<std::mutex> lock(clientsLock_);
        for (auto& c : clients_) c->cv.notify_all();
    }
}

void ShareServer::pushAudio(const std::vector<uint8_t>& chunk) {
    std::lock_guard<std::mutex> lock(clientsLock_);
    for (auto& c : clients_) {
        std::lock_guard<std::mutex> cl(c->lock);
        if (c->audio.size() > 50) c->audio.pop_front();  // a slow link: a second at most waits
        c->audio.push_back(chunk);
        c->cv.notify_all();
    }
}

void ShareServer::audioSource(DWORD pid) { audioPid_ = pid; }

void ShareServer::captureLoop(DWORD pid) {
    capturing_ = 0;
    Activator* activator = new Activator;
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof params;
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, activator, &op);
    if (SUCCEEDED(hr)) {
        WaitForSingleObject(activator->done, 5000);
        op->Release();
        hr = activator->result;
    }
    IAudioClient* client = activator->client;
    activator->client = nullptr;
    activator->Release();
    if (!client) {
        hostLog("share: no sound from pid %lu (0x%08lx): the app's own audio needs Windows 11", pid, hr);
        return;
    }
    WAVEFORMATEX fmt = pcmFormat();
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    IAudioCaptureClient* capture = nullptr;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 2000000, 0, &fmt, nullptr);
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(event);
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        hostLog("share: sound capture of pid %lu failed (0x%08lx)", pid, hr);
        if (capture) capture->Release();
        client->Release();
        CloseHandle(event);
        return;
    }
    capturing_ = pid;
    hostLog("share: sound of pid %lu goes with the picture", pid);
    std::vector<int16_t> pending;
    AdpcmState state[CHANNELS];
    while (!stopping_ && audioPid_ == pid && viewers_ > 0) {
        WaitForSingleObject(event, 200);
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            size_t at = pending.size();
            pending.resize(at + size_t(frames) * CHANNELS);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) memset(pending.data() + at, 0, size_t(frames) * CHANNELS * 2);
            else memcpy(pending.data() + at, data, size_t(frames) * CHANNELS * 2);
            capture->ReleaseBuffer(frames);
        }
        while (pending.size() >= size_t(CHUNK_FRAMES) * CHANNELS) {
            pushAudio(packAudio(pending.data(), CHUNK_FRAMES, state));
            pending.erase(pending.begin(), pending.begin() + size_t(CHUNK_FRAMES) * CHANNELS);
        }
    }
    client->Stop();
    capture->Release();
    client->Release();
    CloseHandle(event);
    hostLog("share: sound of pid %lu stopped", pid);
}

// ---------------------------------------------------------------- the client

ShareClient::ShareClient() { winsock(); }
ShareClient::~ShareClient() { disconnect(); }

void ShareClient::setStatus(const std::wstring& s) {
    std::lock_guard<std::mutex> lock(lock_);
    status_ = s;
}
std::wstring ShareClient::status() const {
    std::lock_guard<std::mutex> lock(lock_);
    return status_;
}
std::wstring ShareClient::friendName() const {
    std::lock_guard<std::mutex> lock(lock_);
    return friendName_;
}
std::shared_ptr<const SharePicture> ShareClient::latest() {
    std::lock_guard<std::mutex> lock(lock_);
    return picture_;
}

void ShareClient::connect(const std::wstring& code, const std::wstring& myName, const std::wstring& audioDeviceId) {
    disconnect();
    uint32_t ip = 0;
    uint16_t port = 0, secret = 0;
    if (!shareCodeParse(code, ip, port, secret)) {
        failed_ = true;
        setStatus(L"That is not a share code (12 letters and digits)");
        return;
    }
    code_ = shareCodeText(ip, port, secret);
    audioDevice_ = audioDeviceId;
    stopping_ = false;
    failed_ = false;
    connected_ = false;
    split_ = false;
    active_ = true;
    received_ = 0;
    state_ = 0;
    upSerial_ = 0;
    lastUp_ = 0;
    {
        std::lock_guard<std::mutex> lock(lock_);
        picture_ = nullptr;
        friendName_.clear();
    }
    thread_ = std::thread([this, ip, port, secret, myName] { run(ip, port, secret, myName); });
}

void ShareClient::disconnect() {
    stopping_ = true;
    upCv_.notify_all();
    if (socket_ != INVALID_SOCKET) closesocket(socket_);
    socket_ = INVALID_SOCKET;
    if (thread_.joinable()) thread_.join();
    if (renderThread_.joinable()) renderThread_.join();
    active_ = false;
    connected_ = false;
    split_ = false;
}

void ShareClient::offerUp(const uint8_t* bgra, int w, int h, int pitch) {
    if (!split_ || !connected_ || w <= 0 || h <= 0) return;
    uint64_t t = now();
    if (t - lastUp_ < UP_INTERVAL) return;
    lastUp_ = t;
    {
        std::lock_guard<std::mutex> lock(upLock_);
        up_.resize(size_t(w) * h);
        for (int y = 0; y < h; y++) memcpy(up_.data() + size_t(y) * w, bgra + size_t(y) * pitch, size_t(w) * 4);
        upW_ = w;
        upH_ = h;
        upSerial_++;
    }
    upCv_.notify_one();
}

void ShareClient::setState(ShareState state) {
    uint8_t s = uint8_t(state);
    if (state_.exchange(s) != s) upCv_.notify_one();
}

// Up the socket: my tablet for the host's split screen (when it asked), whether I am looking,
// and a ping every two seconds otherwise.
void ShareClient::uplinkLoop() {
    uint64_t sent = 0, lastHash = 0, lastPing = now();
    uint8_t sentState = 255;
    std::vector<uint32_t> copy;
    std::vector<uint8_t> jpeg;
    int w = 0, h = 0;
    while (!stopping_ && connected_) {
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(upLock_);
            upCv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return stopping_ || !connected_ || upSerial_ != sent || state_ != sentState; });
            if (stopping_ || !connected_) break;
            if (upSerial_ != sent) {
                sent = upSerial_;
                copy = up_;
                w = upW_;
                h = upH_;
                have = true;
            }
        }
        bool good = true;
        uint8_t st = state_;
        if (st != sentState) {
            sentState = st;
            std::lock_guard<std::mutex> lock(sendLock_);
            good = sendMsg(socket_, MsgState, &st, 1);
            lastPing = now();
        }
        if (good && have && split_) {
            uint64_t hash = 1469598103934665603ull;
            for (size_t i = 0; i < copy.size(); i += 7) { hash ^= copy[i]; hash *= 1099511628211ull; }
            if (hash != lastHash) {
                lastHash = hash;
                if (encodeJpeg(copy.data(), w, h, UP_QUALITY, jpeg)) {
                    std::lock_guard<std::mutex> lock(sendLock_);
                    good = sendMsg(socket_, MsgPicture, jpeg.data(), uint32_t(jpeg.size()));
                    lastPing = now();
                }
            }
        }
        if (good && now() - lastPing > 2000) {
            lastPing = now();
            std::lock_guard<std::mutex> lock(sendLock_);
            good = sendMsg(socket_, MsgPing, nullptr, 0);
        }
        if (!good) break;
    }
}

void ShareClient::run(uint32_t ip, uint16_t port, uint16_t secret, std::wstring myName) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring where = dotted(ip) + L":" + std::to_wstring(port);
    setStatus(L"Connecting to " + where + L"...");
    hostLog("share: connecting to %ls", where.c_str());
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    socket_ = s;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
    ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(s, &writable);
    FD_SET(s, &failed);
    timeval wait{8, 0};
    int err = 0;
    int len = sizeof err;
    bool ok = select(0, nullptr, &writable, &failed, &wait) > 0 && FD_ISSET(s, &writable) &&
              getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) == 0 && err == 0;
    u_long off = 0;
    ioctlsocket(s, FIONBIO, &off);
    std::string name;
    if (ok) {
        keepAlive(s);
        timeouts(s, 8000, 15000);
        std::string mine = utf8(myName);
        if (mine.size() > 255) mine.resize(255);
        std::vector<uint8_t> hello(9 + mine.size());
        memcpy(hello.data(), MAGIC, 4);
        memcpy(hello.data() + 4, &VERSION, 2);
        memcpy(hello.data() + 6, &secret, 2);
        hello[8] = uint8_t(mine.size());
        memcpy(hello.data() + 9, mine.data(), mine.size());
        uint8_t reply[9];
        ok = sendAll(s, hello.data(), hello.size()) && recvAll(s, reply, sizeof reply) && memcmp(reply, MAGIC, 4) == 0;
        if (ok) {
            name.resize(reply[8]);
            split_ = (reply[7] & 1) != 0;
            ok = name.empty() || recvAll(s, name.data(), name.size());
            if (ok && !reply[6]) {
                setStatus(L"Wrong code: ask your friend for the code on their SHARE tile");
                hostLog("share: %ls refused the code", where.c_str());
                ok = false;
                failed_ = true;
            }
        }
        if (!ok && !failed_) { setStatus(L"No tablet share answered at " + where); failed_ = true; }
    } else {
        setStatus(stopping_ ? L"" : L"Could not reach " + where + L" (is the friend sharing? firewall? port forwarded?)");
        failed_ = !stopping_;
        hostLog("share: could not connect to %ls (%d)", where.c_str(), err ? err : WSAGetLastError());
    }
    if (ok) {
        {
            std::lock_guard<std::mutex> lock(lock_);
            friendName_ = wide(name);
        }
        connected_ = true;
        setStatus(L"Watching " + wide(name));
        hostLog("share: watching %s at %ls%s", name.c_str(), where.c_str(), split_ ? " (split screen: my tablet goes up)" : "");
        renderThread_ = std::thread([this] { renderLoop(); });
        upThread_ = std::thread([this] { uplinkLoop(); });
        std::vector<uint8_t> payload;
        std::vector<int16_t> pcm;
        while (!stopping_) {
            uint8_t head[5];
            if (!recvAll(s, head, sizeof head)) break;
            uint32_t size = 0;
            memcpy(&size, head + 1, 4);
            if (size > (16u << 20)) break;
            payload.resize(size);
            if (size && !recvAll(s, payload.data(), size)) break;
            received_ += 5 + size;
            if (head[0] == MsgPicture) {
                auto pic = decodeJpeg(payload.data(), payload.size());
                if (pic->w) {
                    std::lock_guard<std::mutex> lock(lock_);
                    picture_ = pic;
                }
            } else if (head[0] == MsgAudio) {
                if (unpackAudio(payload.data(), payload.size(), pcm)) {
                    std::lock_guard<std::mutex> lock(pcmLock_);
                    pcm_.insert(pcm_.end(), pcm.begin(), pcm.end());
                    size_t most = size_t(SAMPLE_RATE) * CHANNELS / 2;  // half a second behind at most
                    if (pcm_.size() > most) pcm_.erase(pcm_.begin(), pcm_.begin() + long(pcm_.size() - most / 3));
                }
            }
        }
        if (!stopping_) {
            setStatus(L"Lost the connection to " + wide(name));
            hostLog("share: lost %s", name.c_str());
            failed_ = true;
        }
        connected_ = false;
        upCv_.notify_all();
        if (upThread_.joinable()) upThread_.join();
    }
    if (socket_ != INVALID_SOCKET) closesocket(socket_);
    socket_ = INVALID_SOCKET;
    active_ = false;
    CoUninitialize();
}

void ShareClient::renderLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (SUCCEEDED(hr)) hr = audioDevice_.empty() ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device) : enumerator->GetDevice(audioDevice_.c_str(), &device);
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    WAVEFORMATEX fmt = pcmFormat();
    UINT32 bufferFrames = 0;
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 2000000, 0, &fmt, nullptr);
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(event);
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        hostLog("share: the friend's sound cannot play here (0x%08lx)", hr);
    } else {
        bool primed = false;  // a tenth of a second gathered before playing, so bursts do not stutter
        while (!stopping_ && connected_) {
            WaitForSingleObject(event, 200);
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding)) || padding >= bufferFrames) continue;
            UINT32 want = bufferFrames - padding;
            BYTE* out = nullptr;
            if (FAILED(render->GetBuffer(want, &out))) continue;
            size_t samples = size_t(want) * CHANNELS, got = 0;
            {
                std::lock_guard<std::mutex> lock(pcmLock_);
                if (!primed && pcm_.size() >= size_t(SAMPLE_RATE) * CHANNELS / 10) primed = true;
                if (primed) {
                    got = std::min(samples, pcm_.size());
                    std::copy(pcm_.begin(), pcm_.begin() + long(got), reinterpret_cast<int16_t*>(out));
                    pcm_.erase(pcm_.begin(), pcm_.begin() + long(got));
                    if (pcm_.empty()) primed = false;
                }
            }
            if (got < samples) memset(out + got * 2, 0, (samples - got) * 2);
            render->ReleaseBuffer(want, 0);
        }
        client->Stop();
    }
    if (render) render->Release();
    if (client) client->Release();
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    CloseHandle(event);
    CoUninitialize();
}
