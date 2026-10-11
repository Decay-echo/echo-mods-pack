#pragma once
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <share.h>
#include <windows.h>

// %LOCALAPPDATA%\EchoArcade\runtime.log -- first place to look after a headset test.
inline void logf(const char* format, ...) {
    static std::mutex m;
    static FILE* file = nullptr;
    std::lock_guard<std::mutex> lock(m);
    if (!file) {
        wchar_t base[MAX_PATH];
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return;
        std::wstring dir = std::wstring(base) + L"\\EchoArcade";
        CreateDirectoryW(dir.c_str(), nullptr);
        file = _wfsopen((dir + L"\\runtime.log").c_str(), L"a", _SH_DENYNO);
        if (!file) return;
    }
    fprintf(file, "%llu ", GetTickCount64());
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fflush(file);
}
