// Echo VR tweaks that live in the Oculus runtime rather than the game.
//
// FOV: scale the eye field of view the game asks the Oculus runtime for
// (ovr_GetHmdDesc -> DefaultEyeFov), so Echo renders a wider or taller view.
// In the headset the compositor maps it back; the desktop mirror and recordings
// get the wider view. Same approach as heisthecat31/EchoVR-Haptics' FovMultiplier.
// Settings: plugins\EchoArcade\echo_tweaks.ini  [fov] x=1.0 y=1.0 (1.0 .. 2.0). Below 1.0 the game
// would render less than the lenses show, cutting off the edges of the view.
#include "ovr_tweaks.h"
#include "log.h"
#include "../vendor/minhook/include/MinHook.h"
#include <algorithm>
#include <string>
#include <thread>

namespace {

struct FovPort { float up, down, left, right; };  // tangents of the half-angles
// ovrHmdDesc (OVR_CAPI.h, 64-bit layout). Returned by value from ovr_GetHmdDesc.
struct HmdDesc {
    int type;
    char pad0[4];
    char productName[64];
    char manufacturer[64];
    short vendorId, productId;
    char serialNumber[24];
    short firmwareMajor, firmwareMinor;
    unsigned availableHmdCaps, defaultHmdCaps, availableTrackingCaps, defaultTrackingCaps;
    FovPort defaultEyeFov[2];
    FovPort maxEyeFov[2];
    int resolution[2];
    float displayRefreshRate;
    char pad1[4];
};
static_assert(offsetof(HmdDesc, defaultEyeFov) == 0xB8, "ovrHmdDesc layout");

using GetHmdDesc = HmdDesc(__cdecl*)(void* session);
GetHmdDesc realGetHmdDesc = nullptr;
float fovX = 1.f, fovY = 1.f;

HmdDesc __cdecl hookedGetHmdDesc(void* session) {
    HmdDesc d = realGetHmdDesc(session);
    for (auto& eye : d.defaultEyeFov) {
        eye.up *= fovY; eye.down *= fovY;
        eye.left *= fovX; eye.right *= fovX;
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        logf("fov: eye tangents now up %.3f down %.3f left %.3f right %.3f (x%.2f, y%.2f)",
             d.defaultEyeFov[0].up, d.defaultEyeFov[0].down, d.defaultEyeFov[0].left, d.defaultEyeFov[0].right, fovX, fovY);
    }
    return d;
}

float readFloat(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    wchar_t buf[32];
    GetPrivateProfileStringW(section, key, L"1.0", buf, 32, ini.c_str());
    return std::clamp(float(_wtof(buf)), 1.0f, 2.0f);
}

}  // namespace

namespace tweaks {

void start(const std::wstring& pluginDir) {
    std::wstring ini = pluginDir + L"\\EchoArcade\\echo_tweaks.ini";
    fovX = readFloat(ini, L"fov", L"x");
    fovY = readFloat(ini, L"fov", L"y");
    if (fovX == 1.f && fovY == 1.f) { logf("fov: default (no change)"); return; }
    std::thread([] {
        // The game loads the Oculus runtime shortly after start and asks for the
        // eye FOV right after; hook as soon as the DLL appears.
        HMODULE ovr = nullptr;
        for (int i = 0; i < 60000 && !ovr; i++) {
            ovr = GetModuleHandleW(L"LibOVRRT64_1.dll");
            if (!ovr) Sleep(1);
        }
        void* target = ovr ? reinterpret_cast<void*>(GetProcAddress(ovr, "ovr_GetHmdDesc")) : nullptr;
        if (!target) { logf("fov: Oculus runtime (LibOVRRT64_1.dll) not found; FOV unchanged"); return; }
        MH_STATUS init = MH_Initialize();
        MH_STATUS s = (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED)
                          ? MH_CreateHook(target, reinterpret_cast<void*>(hookedGetHmdDesc), reinterpret_cast<void**>(&realGetHmdDesc))
                          : init;
        if (s == MH_OK) s = MH_EnableHook(target);
        logf("fov: x%.2f y%.2f hook %s", fovX, fovY, s == MH_OK ? "installed" : MH_StatusToString(s));
    }).detach();
}

}  // namespace tweaks
