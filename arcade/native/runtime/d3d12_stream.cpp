#include "d3d12_stream.h"
#include "log.h"
#include "../generated/arcade_tab.h"
#include "../generated/posters.h"
#include "../vendor/minhook/include/MinHook.h"
#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <thread>

#pragma comment(lib, "d3d12.lib")

namespace stream {
namespace {

// Vtable slots (declaration order in d3d12.h).
constexpr int DEV_CREATE_COMMITTED = 27, DEV_CREATE_PLACED = 29;
constexpr int DEV4_CREATE_COMMITTED1 = 53;
constexpr int DEV8_CREATE_COMMITTED2 = 69, DEV8_CREATE_PLACED1 = 70;
constexpr int QUEUE_EXECUTE = 10;
constexpr int LIST_RESOURCE_BARRIER = 26;

using CreateCommitted = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreatePlaced = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateCommitted1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
// DESC1 starts with the same fields as DESC, which is all we read.
using CreateCommitted2 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using CreatePlaced1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using Barrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);

CreateCommitted origCommitted;
CreatePlaced origPlaced;
CreateCommitted1 origCommitted1;
CreateCommitted2 origCommitted2;
CreatePlaced1 origPlaced1;
Execute origExecute;
Barrier origBarrier;

arcade::Shared* shared = nullptr;
std::atomic<bool> running{false}, broken{false};
std::thread worker;

// A texture we copy each new host frame into, at (x, y).
struct Target {
    const char* name;
    UINT width, height, x, y;
    std::atomic<bool> active{false};
    std::atomic<ID3D12Resource*> fast{nullptr};
    ID3D12Resource* resource = nullptr;  // AddRef'd while we hold it (guarded by m)
    D3D12_RESOURCE_DESC desc{};
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    UINT64 uploads = 0;
};
enum { TABLET, POSTER, TARGETS };
Target targets[TARGETS] = {
    {"tablet", ARCADE_TEX_W, ARCADE_TEX_H, 0, 0},
    {"poster", POSTER_TEX_W, POSTER_TEX_H, POSTER_X0, POSTER_Y0},
};
static_assert(ARCADE_TEX_W == arcade::WIDTH && ARCADE_TEX_H == arcade::HEIGHT, "tablet texture is the host frame size");
static_assert(POSTER_FRAME_W == arcade::WIDTH && POSTER_FRAME_H == arcade::HEIGHT, "poster frame is the host frame size");

std::mutex m;  // guards everything below and each Target's non-atomic fields
ID3D12Device* device = nullptr;
ID3D12CommandQueue* queue = nullptr;
ID3D12Fence* fence = nullptr;
UINT64 fenceValue = 0;
constexpr UINT ROW_PITCH = arcade::PITCH;  // 4096: already a multiple of D3D12's 256-byte pitch rule

enum class SlotState { Free, Filling, Ready };
struct Slot {
    ID3D12Resource* upload = nullptr;
    uint8_t* mapped = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    UINT64 fenceValue = 0;
    LONG serial = 0;
    SlotState state = SlotState::Free;
};
Slot slots[2];
bool gpuReady = false;
thread_local bool ownCall = false;

bool anyActive() {
    for (auto& t : targets) if (t.active && t.fast.load(std::memory_order_relaxed)) return true;
    return false;
}

Target* matching(const D3D12_RESOURCE_DESC* d) {
    if (!d || d->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d->DepthOrArraySize != 1 || d->MipLevels > 1) return nullptr;
    if (d->Format != DXGI_FORMAT_B8G8R8A8_UNORM && d->Format != DXGI_FORMAT_B8G8R8A8_TYPELESS && d->Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return nullptr;
    for (auto& t : targets)
        if (d->Width == t.width && d->Height == t.height) return &t;
    return nullptr;
}

void releaseGpu() {
    for (auto& s : slots) {
        if (s.upload) { s.upload->Unmap(0, nullptr); s.upload->Release(); }
        if (s.list) s.list->Release();
        if (s.allocator) s.allocator->Release();
        s = Slot{};
    }
    if (fence) fence->Release();
    fence = nullptr; fenceValue = 0; gpuReady = false;
}

void adopt(ID3D12Device* dev, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, void** ppv, const char* how) {
    Target* t = matching(desc);
    if (!ppv || !*ppv || !t) return;
    ID3D12Resource* resource = nullptr;
    if (FAILED(static_cast<IUnknown*>(*ppv)->QueryInterface(IID_PPV_ARGS(&resource)))) return;
    std::lock_guard<std::mutex> lock(m);
    if (device && device != dev) { releaseGpu(); queue = nullptr; }
    if (t->resource) t->resource->Release();
    t->resource = resource;  // keep the reference from QueryInterface
    t->fast = resource;
    t->desc = *desc;
    t->state = state;
    device = dev;
    logf("stream: %s texture created via %s res=%p fmt=%d state=0x%x", t->name, how, resource, int(desc->Format), unsigned(state));
}

HRESULT STDMETHODCALLTYPE hookCommitted(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS f,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID riid, void** ppv) {
    HRESULT hr = origCommitted(d, hp, f, desc, s, c, riid, ppv);
    if (SUCCEEDED(hr)) adopt(d, desc, s, ppv, "CreateCommittedResource");
    return hr;
}
HRESULT STDMETHODCALLTYPE hookPlaced(ID3D12Device* d, ID3D12Heap* h, UINT64 o, const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID riid, void** ppv) {
    HRESULT hr = origPlaced(d, h, o, desc, s, c, riid, ppv);
    if (SUCCEEDED(hr)) adopt(d, desc, s, ppv, "CreatePlacedResource");
    return hr;
}
HRESULT STDMETHODCALLTYPE hookCommitted1(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS f,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, ID3D12ProtectedResourceSession* p, REFIID riid, void** ppv) {
    HRESULT hr = origCommitted1(d, hp, f, desc, s, c, p, riid, ppv);
    if (SUCCEEDED(hr)) adopt(d, desc, s, ppv, "CreateCommittedResource1");
    return hr;
}
HRESULT STDMETHODCALLTYPE hookCommitted2(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS f,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, ID3D12ProtectedResourceSession* p, REFIID riid, void** ppv) {
    HRESULT hr = origCommitted2(d, hp, f, desc, s, c, p, riid, ppv);
    if (SUCCEEDED(hr)) adopt(d, desc, s, ppv, "CreateCommittedResource2");
    return hr;
}
HRESULT STDMETHODCALLTYPE hookPlaced1(ID3D12Device* d, ID3D12Heap* h, UINT64 o, const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID riid, void** ppv) {
    HRESULT hr = origPlaced1(d, h, o, desc, s, c, riid, ppv);
    if (SUCCEEDED(hr)) adopt(d, desc, s, ppv, "CreatePlacedResource1");
    return hr;
}

// Track each texture's state from the engine's own barriers so ours match.
void STDMETHODCALLTYPE hookBarrier(ID3D12GraphicsCommandList* list, UINT n, const D3D12_RESOURCE_BARRIER* barriers) {
    if (!ownCall) {
        for (auto& t : targets) {
            ID3D12Resource* r = t.fast.load(std::memory_order_relaxed);
            if (!r) continue;
            for (UINT i = 0; i < n; i++) {
                if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || barriers[i].Transition.pResource != r) continue;
                std::lock_guard<std::mutex> lock(m);
                if (t.resource == r && t.state != barriers[i].Transition.StateAfter) {
                    t.state = barriers[i].Transition.StateAfter;
                    logf("stream: engine moved %s texture to state 0x%x", t.name, unsigned(t.state));
                }
            }
        }
    }
    origBarrier(list, n, barriers);
}

bool createGpu() {  // caller holds m
    if (gpuReady) return true;
    if (!device) return false;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buf.Width = UINT64(ROW_PITCH) * arcade::HEIGHT; buf.Height = 1;
    buf.DepthOrArraySize = 1; buf.MipLevels = 1; buf.SampleDesc.Count = 1; buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ownCall = true;
    bool ok = SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    for (auto& s : slots) {
        ok = ok && SUCCEEDED(origCommitted(device, &heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&s.upload)));
        ok = ok && SUCCEEDED(s.upload->Map(0, nullptr, reinterpret_cast<void**>(&s.mapped)));
        ok = ok && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator)));
        ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator, nullptr, IID_PPV_ARGS(&s.list)));
        ok = ok && SUCCEEDED(s.list->Close());
    }
    ownCall = false;
    if (!ok) { logf("stream: failed to create upload resources"); releaseGpu(); broken = true; return false; }
    gpuReady = true;
    logf("stream: upload path ready (%llu bytes x2, row pitch %u)", buf.Width, ROW_PITCH);
    return true;
}

// Worker: copy the newest host frame into a free upload slot.
void workerLoop() {
    LONG lastSerial = -1;
    bool wasActive = false;
    while (running) {
        Sleep(2);
        bool active = anyActive();
        if (active && !wasActive) lastSerial = -1;  // re-upload the current frame for a newly shown target
        wasActive = active;
        if (!active || broken || !shared || shared->latestFrame == LONG(arcade::NO_FRAME)) continue;
        LONG serial = shared->frameSerial;
        if (serial == lastSerial) continue;
        Slot* slot = nullptr;
        {
            std::lock_guard<std::mutex> lock(m);
            if (!createGpu()) continue;
            UINT64 done = fence->GetCompletedValue();
            for (auto& s : slots)
                if (s.state == SlotState::Free && done >= s.fenceValue) { slot = &s; break; }
            if (!slot) continue;
            slot->state = SlotState::Filling;
        }
        LONG index = shared->latestFrame;
        InterlockedExchange(&shared->readingFrame, index);
        bool ok = index >= 0 && index < LONG(arcade::FRAME_BUFFERS) && shared->latestFrame == index;
        if (ok) memcpy(slot->mapped, shared->frames[index], arcade::FRAME_BYTES);  // same pitch on both sides
        InterlockedExchange(&shared->readingFrame, LONG(arcade::NO_FRAME));
        std::lock_guard<std::mutex> lock(m);
        slot->serial = serial;
        slot->state = ok ? SlotState::Ready : SlotState::Free;
        if (ok) lastSerial = serial;
    }
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

void recordAndSubmit(ID3D12CommandQueue* q) {  // caller holds m
    Slot* ready = nullptr;
    for (auto& s : slots)
        if (s.state == SlotState::Ready && (!ready || s.serial - ready->serial > 0)) ready = &s;
    if (!ready) return;
    Target* live[TARGETS];
    int count = 0;
    for (auto& t : targets) if (t.active && t.resource) live[count++] = &t;
    if (!count) return;
    if (FAILED(ready->allocator->Reset())) return;
    ownCall = true;
    auto list = ready->list;
    list->Reset(ready->allocator, nullptr);
    for (int i = 0; i < count; i++) {
        Target& t = *live[i];
        transition(list, t.resource, t.state, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{t.resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{ready->upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint = {t.desc.Format, arcade::WIDTH, arcade::HEIGHT, 1, ROW_PITCH};
        list->CopyTextureRegion(&dst, t.x, t.y, 0, &src, nullptr);
        transition(list, t.resource, D3D12_RESOURCE_STATE_COPY_DEST, t.state);
    }
    bool closed = SUCCEEDED(list->Close());
    ownCall = false;
    if (!closed) { logf("stream: command list close failed; streaming disabled"); broken = true; return; }
    ID3D12CommandList* lists[] = {list};
    origExecute(q, 1, lists);
    q->Signal(fence, ++fenceValue);
    ready->fenceValue = fenceValue;
    ready->state = SlotState::Free;
    for (int i = 0; i < count; i++) {
        Target& t = *live[i];
        if (++t.uploads == 1 || t.uploads % 1800 == 0) logf("stream: %s: %llu frames uploaded (state 0x%x)", t.name, t.uploads, unsigned(t.state));
    }
}

// Pick the busiest direct queue on the textures' device: that is the frame queue.
void chooseQueue(ID3D12CommandQueue* q) {  // caller holds m
    static ID3D12CommandQueue* seen[8] = {};
    static unsigned counts[8] = {}, total = 0;
    int slot = -1;
    for (int i = 0; i < 8 && slot < 0; i++) if (seen[i] == q) slot = i;
    if (slot < 0) {
        D3D12_COMMAND_QUEUE_DESC desc = q->GetDesc();
        ID3D12Device* qd = nullptr;
        bool eligible = desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&qd))) && qd == device;
        if (qd) qd->Release();
        if (!eligible) return;
        for (int i = 0; i < 8 && slot < 0; i++) if (!seen[i]) { seen[i] = q; slot = i; }
        if (slot < 0) return;
    }
    counts[slot]++;
    if (++total < 240) return;
    int best = 0;
    for (int i = 1; i < 8; i++) if (counts[i] > counts[best]) best = i;
    queue = seen[best];
    logf("stream: using direct queue %p (%u of %u submissions)", queue, counts[best], total);
}

void STDMETHODCALLTYPE hookExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (!broken && !ownCall && anyActive()) {
        std::unique_lock<std::mutex> lock(m, std::try_to_lock);
        if (lock.owns_lock() && gpuReady) {
            if (!queue) chooseQueue(q);
            if (q == queue) recordAndSubmit(q);
        }
    }
    origExecute(q, n, lists);
}

bool hook(void* function, void* detour, void** original, const char* name) {
    MH_STATUS s = MH_CreateHook(function, detour, original);
    if (s == MH_OK) s = MH_EnableHook(function);
    if (s != MH_OK) { logf("stream: hook %s failed: %s", name, MH_StatusToString(s)); return false; }
    return true;
}

}  // namespace

bool install(arcade::Shared* s) {
    shared = s;
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        logf("stream: could not create a probe D3D12 device");
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* a = nullptr;
    ID3D12GraphicsCommandList* l = nullptr;
    bool ok = SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) &&
              SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) &&
              SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l)));
    if (ok) {
        auto dv = *reinterpret_cast<void***>(dev);
        auto qv = *reinterpret_cast<void***>(q);
        auto lv = *reinterpret_cast<void***>(l);
        ok = hook(dv[DEV_CREATE_COMMITTED], reinterpret_cast<void*>(hookCommitted), reinterpret_cast<void**>(&origCommitted), "CreateCommittedResource") &&
             hook(dv[DEV_CREATE_PLACED], reinterpret_cast<void*>(hookPlaced), reinterpret_cast<void**>(&origPlaced), "CreatePlacedResource") &&
             hook(qv[QUEUE_EXECUTE], reinterpret_cast<void*>(hookExecute), reinterpret_cast<void**>(&origExecute), "ExecuteCommandLists") &&
             hook(lv[LIST_RESOURCE_BARRIER], reinterpret_cast<void*>(hookBarrier), reinterpret_cast<void**>(&origBarrier), "ResourceBarrier");
        ID3D12Device4* d4 = nullptr;
        if (ok && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d4)))) {
            hook(dv[DEV4_CREATE_COMMITTED1], reinterpret_cast<void*>(hookCommitted1), reinterpret_cast<void**>(&origCommitted1), "CreateCommittedResource1");
            d4->Release();
        }
        ID3D12Device8* d8 = nullptr;
        if (ok && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d8)))) {
            hook(dv[DEV8_CREATE_COMMITTED2], reinterpret_cast<void*>(hookCommitted2), reinterpret_cast<void**>(&origCommitted2), "CreateCommittedResource2");
            hook(dv[DEV8_CREATE_PLACED1], reinterpret_cast<void*>(hookPlaced1), reinterpret_cast<void**>(&origPlaced1), "CreatePlacedResource1");
            d8->Release();
        }
    }
    if (l) l->Release();
    if (a) a->Release();
    if (q) q->Release();
    dev->Release();
    if (!ok) { logf("stream: D3D12 hooks not installed; the tablet will show the splash image only"); return false; }
    running = true;
    worker = std::thread(workerLoop);
    logf("stream: D3D12 hooks installed");
    return true;
}

void setVisible(bool v) {
    if (targets[TABLET].active.exchange(v) != v) logf("stream: page %s", v ? "visible, streaming" : "hidden, paused");
}

void setPosterActive(bool a) {
    // Taking the lock waits out a submission in progress, so once this returns no copy
    // into the poster texture is recorded (earlier ones are already on the game's queue,
    // ahead of anything that could free it).
    std::lock_guard<std::mutex> lock(m);
    if (targets[POSTER].active.exchange(a) != a) logf("stream: poster %s", a ? "docked, streaming" : "undocked, stopped");
}

bool posterTextureReady() { return targets[POSTER].fast.load() != nullptr; }

void shutdown() {
    running = false;
    if (worker.joinable()) worker.join();
}

}  // namespace stream
