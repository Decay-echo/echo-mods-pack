// Checks tablet_scale.cpp finds the tablet canvas size (0.3 x 0.3 m at 150 px/m) on
// the heap, resizes it and reports the applied scale; and that a copy the engine makes
// later (from the size at that time) is resized too on the next change.
#include "../native/runtime/tablet_scale.h"
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

static bool close(float a, float b) { return std::fabs(a - b) < 1e-5f; }

static bool waitFor(const float* size, float want) {
    for (int i = 0; i < 100; i++) {
        if (close(size[0], want)) return true;
        Sleep(100);
    }
    return false;
}

int main() {
    const float stock[3] = {0.3f, 0.3f, 150.f};
    std::vector<float> heap(1 << 20, 7.f);
    float* size = heap.data() + 4099;
    memcpy(size, stock, sizeof(stock));

    tablet::start(L"C:\\nonexistent");  // no ini: size 1.0
    tablet::tabletPresent(true);
    tablet::request(2.f);
    ULONGLONG t0 = GetTickCount64();
    if (!waitFor(size, 0.6f)) { printf("tablet_scan_test: FAILED (canvas size not changed within 10 s)\n"); return 1; }
    Sleep(200);
    bool first = close(size[1], 0.6f) && size[2] == 150.f && tablet::appliedScale() == 2.f;

    // The engine rebuilds a page's canvas component from the 2x size, then the size changes again.
    float* later = heap.data() + 70001;
    const float copied[3] = {0.6f, 0.6f, 150.f};
    memcpy(later, copied, sizeof(copied));
    tablet::request(3.f);
    bool second = waitFor(size, 0.9f) && waitFor(later, 0.9f);
    Sleep(200);
    second = second && close(later[1], 0.9f) && later[2] == 150.f && tablet::appliedScale() == 3.f;

    bool ok = first && second;
    printf("tablet_scan_test: %s (%.2f x %.2f m, later copy %.2f, applied %.2f, %llu ms)\n", ok ? "all checks passed" : "FAILED",
           size[0], size[1], later[0], tablet::appliedScale(), GetTickCount64() - t0);
    return ok ? 0 : 1;
}
