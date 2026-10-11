// Captures one app window with Windows.Graphics.Capture into CPU memory.
// Works for covered or background windows; the cursor is not captured.
#include "host.h"
#include <d3d11.h>
#include <dxgi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

struct WindowCapture::Impl {
    com_ptr<ID3D11Device> d3d;
    com_ptr<ID3D11DeviceContext> context;
    IDirect3DDevice device{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    Direct3D11CaptureFramePool::FrameArrived_revoker revoker;
    com_ptr<ID3D11Texture2D> staging;
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    std::mutex m;
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    uint64_t serial = 0;

    bool createDevice() {
        if (d3d) return true;
        D3D_FEATURE_LEVEL level;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, d3d.put(), &level, context.put()))) return false;
        auto dxgi = d3d.as<IDXGIDevice>();
        com_ptr<::IInspectable> inspectable;
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()))) return false;
        device = inspectable.as<IDirect3DDevice>();
        return true;
    }

    void onFrame() {
        auto frame = pool.TryGetNextFrame();
        if (!frame) return;
        auto content = frame.ContentSize();
        if (content.Width != poolSize.Width || content.Height != poolSize.Height) {
            poolSize = content;
            pool.Recreate(device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize);
        }
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> texture;
        if (FAILED(access->GetInterface(IID_PPV_ARGS(texture.put())))) return;
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);
        int cw = std::min<int>(content.Width, desc.Width), ch = std::min<int>(content.Height, desc.Height);
        if (cw <= 0 || ch <= 0) return;
        D3D11_TEXTURE2D_DESC sd{};
        if (staging) staging->GetDesc(&sd);
        if (!staging || int(sd.Width) != cw || int(sd.Height) != ch) {
            staging = nullptr;
            D3D11_TEXTURE2D_DESC nd{};
            nd.Width = cw; nd.Height = ch; nd.MipLevels = 1; nd.ArraySize = 1; nd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            nd.SampleDesc.Count = 1; nd.Usage = D3D11_USAGE_STAGING; nd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(d3d->CreateTexture2D(&nd, nullptr, staging.put()))) return;
        }
        D3D11_BOX box{0, 0, 0, UINT(cw), UINT(ch), 1};
        context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, texture.get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
        {
            std::lock_guard<std::mutex> lock(m);
            pixels.resize(size_t(cw) * ch * 4);
            for (int y = 0; y < ch; y++)
                memcpy(pixels.data() + size_t(y) * cw * 4, static_cast<uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(cw) * 4);
            w = cw; h = ch; serial++;
        }
        context->Unmap(staging.get(), 0);
    }
};

WindowCapture::WindowCapture() : impl_(std::make_unique<Impl>()) {}
WindowCapture::~WindowCapture() { stop(); }

bool WindowCapture::start(HWND window) {
    stop();
    auto& d = *impl_;
    try {
        if (!GraphicsCaptureSession::IsSupported() || !d.createDevice()) { hostLog("capture: not supported"); return false; }
        auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        check_hresult(interop->CreateForWindow(window, guid_of<GraphicsCaptureItem>(), put_abi(d.item)));
        d.poolSize = d.item.Size();
        d.pool = Direct3D11CaptureFramePool::CreateFreeThreaded(d.device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, d.poolSize);
        d.revoker = d.pool.FrameArrived(auto_revoke, [this](auto&&, auto&&) {
            try { impl_->onFrame(); } catch (...) {}
        });
        d.session = d.pool.CreateCaptureSession(d.item);
        try { d.session.IsCursorCaptureEnabled(false); } catch (...) {}
        try { d.session.IsBorderRequired(false); } catch (...) {}
        d.session.StartCapture();
        hostLog("capture: started %dx%d", d.poolSize.Width, d.poolSize.Height);
        return true;
    } catch (hresult_error const& e) {
        hostLog("capture: failed 0x%08x", unsigned(e.code()));
        stop();
        return false;
    }
}

void WindowCapture::stop() {
    auto& d = *impl_;
    d.revoker.revoke();
    try {
        if (d.session) d.session.Close();
        if (d.pool) d.pool.Close();
    } catch (...) {}
    d.session = nullptr; d.pool = nullptr; d.item = nullptr;
    std::lock_guard<std::mutex> lock(d.m);
    d.pixels.clear(); d.w = d.h = 0;
}

bool WindowCapture::active() const { return impl_->session != nullptr; }

bool WindowCapture::latest(std::vector<uint8_t>& out, int& w, int& h, uint64_t& serial) {
    auto& d = *impl_;
    std::lock_guard<std::mutex> lock(d.m);
    if (!d.w || d.serial == serial) return false;
    out = d.pixels; w = d.w; h = d.h; serial = d.serial;
    return true;
}
