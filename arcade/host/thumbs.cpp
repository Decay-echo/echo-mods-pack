// Video thumbnails (see thumbs.h).
#include "thumbs.h"
#include <winhttp.h>
#include <shlwapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {

std::vector<uint8_t> request(const std::wstring& url, const wchar_t* method, const std::wstring& headers, const std::string& body, DWORD* statusOut) {
    std::vector<uint8_t> out;
    if (statusOut) *statusOut = 0;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256], path[2048], extra[2048];
    parts.lpszHostName = host; parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path; parts.dwUrlPathLength = 2048;
    parts.lpszExtraInfo = extra; parts.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return out;
    std::wstring target = std::wstring(path) + extra;
    HINTERNET session = WinHttpOpen(L"EchoArcade", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return out;
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 15000);
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET req = connection ? WinHttpOpenRequest(connection, method, target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                    parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (req && WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : DWORD(-1),
                                  body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), DWORD(body.size()), DWORD(body.size()), 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (statusOut) *statusOut = status;
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(req, &available) && available) {
            size_t at = out.size();
            if (at + available > 8u << 20) { out.clear(); break; }
            out.resize(at + available);
            DWORD got = 0;
            if (!WinHttpReadData(req, out.data() + at, available, &got)) { out.clear(); break; }
            out.resize(at + got);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return out;
}

// GET over HTTPS; at most 2 MB; empty on any failure.
std::vector<uint8_t> httpGet(const std::wstring& url) {
    std::vector<uint8_t> out;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256], path[2048], extra[2048];
    parts.lpszHostName = host; parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path; parts.dwUrlPathLength = 2048;
    parts.lpszExtraInfo = extra; parts.dwExtraInfoLength = 2048;  // "?sqp=...": the picture's size variant
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) { hostLog("thumb: bad url (%lu)", GetLastError()); return out; }
    std::wstring target = std::wstring(path) + extra;
    HINTERNET session = WinHttpOpen(L"EchoArcade", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return out;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 8000);
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) hostLog("thumb: HTTP %lu for %ls", status, url.c_str());
        if (status == 200) {
            DWORD available = 0;
            while (WinHttpQueryDataAvailable(request, &available) && available) {
                size_t at = out.size();
                if (at + available > 2u << 20) { out.clear(); break; }
                out.resize(at + available);
                DWORD got = 0;
                if (!WinHttpReadData(request, out.data() + at, available, &got)) { out.clear(); break; }
                out.resize(at + got);
            }
        }
    }
    else hostLog("thumb: request failed (%lu) for %ls", GetLastError(), url.c_str());
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return out;
}

// Decodes a JPEG/PNG/WebP (what GDI+ can) and scales it to W x H, aspect-filled.
std::shared_ptr<Picture> decode(const std::vector<uint8_t>& bytes, int W, int H) {
    auto pic = std::make_shared<Picture>();
    if (bytes.empty()) return pic;
    IStream* stream = SHCreateMemStream(bytes.data(), UINT(bytes.size()));
    if (!stream) return pic;
    {
        Gdiplus::Bitmap source(stream);
        if (source.GetLastStatus() != Gdiplus::Ok)
            hostLog("thumb: GDI+ could not read it (status %d; first bytes %02x %02x %02x %02x)", int(source.GetLastStatus()), bytes[0], bytes.size() > 1 ? bytes[1] : 0, bytes.size() > 2 ? bytes[2] : 0, bytes.size() > 3 ? bytes[3] : 0);
        if (source.GetLastStatus() == Gdiplus::Ok && source.GetWidth() && source.GetHeight()) {
            Gdiplus::Bitmap scaled(W, H, PixelFormat32bppARGB);  // ("small" is a Windows macro)
            Gdiplus::Graphics g(&scaled);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            // fill the box: crop the longer side
            float sw = float(source.GetWidth()), sh = float(source.GetHeight());
            float scale = std::max(W / sw, H / sh);
            float dw = sw * scale, dh = sh * scale;
            g.DrawImage(&source, Gdiplus::RectF((W - dw) / 2, (H - dh) / 2, dw, dh));
            Gdiplus::BitmapData data{};
            Gdiplus::Rect all(0, 0, W, H);
            if (scaled.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) == Gdiplus::Ok) {
                pic->w = W; pic->h = H;
                pic->bgra.resize(size_t(W) * H * 4);
                for (int y = 0; y < H; y++)
                    memcpy(pic->bgra.data() + size_t(y) * W * 4, static_cast<uint8_t*>(data.Scan0) + size_t(y) * data.Stride, size_t(W) * 4);
                for (size_t i = 3; i < pic->bgra.size(); i += 4) pic->bgra[i] = 255;
                scaled.UnlockBits(&data);
            }
        }
    }
    stream->Release();
    return pic;
}

}  // namespace

std::vector<uint8_t> httpGetBytes(const std::wstring& url) { return httpGet(url); }
std::vector<uint8_t> httpRequest(const std::wstring& url, const wchar_t* method, const std::wstring& headers, const std::string& body, DWORD* status) {
    return request(url, method, headers, body, status);
}

Thumbs::Thumbs() {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&gdiplus_, &input, nullptr);
}

Thumbs::~Thumbs() {
    for (auto& w : workers_) w.wait();
    if (gdiplus_) Gdiplus::GdiplusShutdown(gdiplus_);
}

std::shared_ptr<const Picture> Thumbs::get(const std::wstring& url) {
    if (url.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(lock_);
    auto it = have_.find(url);
    if (it != have_.end()) return it->second;
    if (loading_.count(url) || loading_.size() >= 4) return nullptr;  // a few at a time
    loading_.insert(url);
    workers_.erase(std::remove_if(workers_.begin(), workers_.end(), [](auto& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }), workers_.end());
    workers_.push_back(std::async(std::launch::async, [this, url] { fetch(url); }));
    return nullptr;
}

void Thumbs::fetch(const std::wstring& url) {
    auto bytes = httpGet(url);
    auto pic = decode(bytes, W, H);
    if (!pic->w) hostLog("thumb: no picture (%zu bytes) from %ls", bytes.size(), url.c_str());
    std::lock_guard<std::mutex> lock(lock_);
    have_[url] = pic;
    loading_.erase(url);
    if (have_.size() > 400) have_.clear();  // a long browse: start over rather than grow
}
