// Send the arcade apps' sound to the VR headset instead of the Windows default device.
//
// Echo VR plays through the headset's own endpoint (Link / Air Link: "Oculus Virtual
// Audio Device"). The apps we launch would use the Windows default. Windows lets any
// process get its own default endpoint (Settings > Sound > Volume mixer, per app);
// this uses the same per-process policy, via the AudioPolicyConfig factory that the
// volume mixer and tools like EarTrumpet use.
#include "host.h"
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <roapi.h>
#include <winstring.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace {

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
    return s;
}

// Windows.Media.Internal.AudioPolicyConfig (Windows 10 21H2 and later, Windows 11).
// Only the three methods at the end are used; the others are placeholders.
MIDL_INTERFACE("ab3d4648-e242-459f-b02f-541c70306324")
IAudioPolicyConfigFactory : public IInspectable {
public:
    virtual HRESULT STDMETHODCALLTYPE p0() = 0; virtual HRESULT STDMETHODCALLTYPE p1() = 0;
    virtual HRESULT STDMETHODCALLTYPE p2() = 0; virtual HRESULT STDMETHODCALLTYPE p3() = 0;
    virtual HRESULT STDMETHODCALLTYPE p4() = 0; virtual HRESULT STDMETHODCALLTYPE p5() = 0;
    virtual HRESULT STDMETHODCALLTYPE p6() = 0; virtual HRESULT STDMETHODCALLTYPE p7() = 0;
    virtual HRESULT STDMETHODCALLTYPE p8() = 0; virtual HRESULT STDMETHODCALLTYPE p9() = 0;
    virtual HRESULT STDMETHODCALLTYPE p10() = 0; virtual HRESULT STDMETHODCALLTYPE p11() = 0;
    virtual HRESULT STDMETHODCALLTYPE p12() = 0; virtual HRESULT STDMETHODCALLTYPE p13() = 0;
    virtual HRESULT STDMETHODCALLTYPE p14() = 0; virtual HRESULT STDMETHODCALLTYPE p15() = 0;
    virtual HRESULT STDMETHODCALLTYPE p16() = 0; virtual HRESULT STDMETHODCALLTYPE p17() = 0;
    virtual HRESULT STDMETHODCALLTYPE p18() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(UINT pid, EDataFlow flow, ERole role, HSTRING device) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(UINT pid, EDataFlow flow, ERole role, HSTRING* device) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

}  // namespace

thread_local long lastAudioError = 0;

AudioTarget findAudioDevice(const std::wstring& patterns) {
    AudioTarget out;
    if (patterns.empty() || lower(patterns) == L"default") return out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)))
        return out;
    UINT count = 0;
    devices->GetCount(&count);
    std::vector<std::wstring> wanted;
    for (size_t start = 0; start <= patterns.size();) {
        size_t bar = patterns.find(L'|', start);
        std::wstring p = lower(patterns.substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start));
        if (!p.empty()) wanted.push_back(p);
        if (bar == std::wstring::npos) break;
        start = bar + 1;
    }
    for (auto& want : wanted) {  // pattern order = preference order
        for (UINT i = 0; i < count; i++) {
            ComPtr<IMMDevice> device;
            ComPtr<IPropertyStore> props;
            LPWSTR id = nullptr;
            PROPVARIANT name;
            PropVariantInit(&name);
            if (FAILED(devices->Item(i, &device)) || FAILED(device->GetId(&id))) continue;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) &&
                SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR &&
                lower(name.pwszVal).find(want) != std::wstring::npos) {
                out.id = id;
                out.name = name.pwszVal;
            }
            PropVariantClear(&name);
            CoTaskMemFree(id);
            if (!out.id.empty()) return out;
        }
    }
    return out;
}

// The process's own volume on the device (Windows' per-app volume), 0..1. False until the
// process has an audio session there, which it gets once it plays something.
bool setProcessVolume(DWORD pid, const AudioTarget& target, float level) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return false;
    if (target.id.empty() ? FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) : FAILED(enumerator->GetDevice(target.id.c_str(), &device)))
        return false;
    ComPtr<IAudioSessionManager2> manager;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager))) return false;
    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(manager->GetSessionEnumerator(&sessions))) return false;
    int count = 0;
    sessions->GetCount(&count);
    bool done = false;
    for (int i = 0; i < count; i++) {
        ComPtr<IAudioSessionControl> control;
        ComPtr<IAudioSessionControl2> control2;
        DWORD sessionPid = 0;
        if (FAILED(sessions->GetSession(i, &control)) || FAILED(control.As(&control2)) || FAILED(control2->GetProcessId(&sessionPid)) || sessionPid != pid)
            continue;
        ComPtr<ISimpleAudioVolume> volume;
        if (SUCCEEDED(control.As(&volume)) && SUCCEEDED(volume->SetMasterVolume(std::clamp(level, 0.f, 1.f), nullptr))) done = true;
    }
    return done;
}

bool routeProcessAudio(DWORD pid, const AudioTarget& target) {
    if (target.id.empty()) return false;
    static const wchar_t cls[] = L"Windows.Media.Internal.AudioPolicyConfig";
    HSTRING className = nullptr;
    if (FAILED(WindowsCreateString(cls, UINT32(wcslen(cls)), &className))) return false;
    ComPtr<IAudioPolicyConfigFactory> factory;
    HRESULT hr = RoGetActivationFactory(className, __uuidof(IAudioPolicyConfigFactory), &factory);
    WindowsDeleteString(className);
    if (FAILED(hr)) { lastAudioError = hr; return false; }
    std::wstring path = L"\\\\?\\SWD#MMDEVAPI#" + target.id + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
    HSTRING device = nullptr;
    if (FAILED(WindowsCreateString(path.c_str(), UINT32(path.size()), &device))) return false;
    HRESULT a = factory->SetPersistedDefaultAudioEndpoint(pid, eRender, eMultimedia, device);
    HRESULT b = factory->SetPersistedDefaultAudioEndpoint(pid, eRender, eConsole, device);
    WindowsDeleteString(device);
    lastAudioError = FAILED(a) ? a : b;
    return SUCCEEDED(a) && SUCCEEDED(b);
}
