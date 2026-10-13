#pragma once
#include "host.h"
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>

// Video thumbnails for the list pages (YouTube): fetched (WinHTTP) and decoded (GDI+) on worker
// threads, kept as small BGRA pictures. get() returns the picture when it is there and starts
// the fetch when it is not (null until then; a failed one stays an empty picture).
struct Picture {
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;
};

class Thumbs {
public:
    static constexpr int W = 104, H = 60;
    Thumbs();
    ~Thumbs();
    std::shared_ptr<const Picture> get(const std::wstring& url);
private:
    void fetch(const std::wstring& url);
    std::mutex lock_;
    std::map<std::wstring, std::shared_ptr<const Picture>> have_;
    std::set<std::wstring> loading_;
    std::vector<std::future<void>> workers_;
    ULONG_PTR gdiplus_ = 0;
};
