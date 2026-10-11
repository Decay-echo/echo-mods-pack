// Send the arcade apps' sound to the VR headset instead of the Windows default device.
//
// Echo VR plays through the headset's own endpoint (Link / Air Link: "Oculus Virtual
// Audio Device"). The apps we launch would use the Windows default. Windows lets any
// process get its own default endpoint (Settings > Sound > Volume mixer, per app);
// this uses the same per-process policy, via the AudioPolicyConfig factory that the
// volume mixer and tools like EarTrumpet use.
#include "host.h"
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
