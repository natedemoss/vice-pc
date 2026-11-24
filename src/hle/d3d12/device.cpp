// The title's D3D12 device: a real PC device with a patched vtable (see vtpatch.h).
//
// Create* methods are overridden so that (a) the Xbox IIDs the title passes are
// replaced with the PC IID for that method, (b) every object handed back gets its
// vtable patched too, and (c) every call is logged with its HRESULT, so we can see
// exactly which Xbox-specific parameters the PC runtime rejects.

#include "device.h"
#include "dxgi.h"
#include "vtpatch.h"

#include "../hle.h"
#include "../../log.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <cstdio>

namespace {

ID3D12Device* g_device = nullptr;
VtInterface g_device_iface = kVtDevice;

std::atomic<int> g_logged{0};
bool should_log(HRESULT hr) { return FAILED(hr) || g_logged.fetch_add(1) < 5000; }

template <typename T>
T orig(void* self, int slot) {
    return reinterpret_cast<T>(vt_original(self, slot));
}

void fmt_guid(const GUID* g, char* out, size_t n) {
    if (!g) { snprintf(out, n, "(null)"); return; }
    snprintf(out, n, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g->Data1, g->Data2, g->Data3, g->Data4[0],
             g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

void* ret_addr_rva(void* ra) {
    uint64_t r;
    return hle_rva(ra, &r) ? reinterpret_cast<void*>(r) : ra;
}

#define CALLER ret_addr_rva(_ReturnAddress())

// ---- IUnknown ---------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device_QueryInterface(ID3D12Device* self, REFIID riid, void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, REFIID, void**);
    if (dxgi_is_device_iid(riid)) {
        *out = dxgi_device_for(self);
        LOG("[d3d12] Device::QueryInterface(IDXGIDevice) -> DXGI device stand-in");
        return S_OK;
    }
    HRESULT hr = orig<Fn>(self, 0)(self, riid, out);
    if (FAILED(hr)) {
        // Xbox-only device IIDs (newer Xbox device versions): hand back the same device.
        char g[64];
        fmt_guid(&riid, g, sizeof(g));
        LOG("[d3d12] Device::QueryInterface(%s) unknown on PC -> returning device", g);
        self->AddRef();
        *out = self;
        hr = S_OK;
    }
    return hr;
}

// ---- object creation ----------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device_CreateCommandQueue(ID3D12Device* self, const D3D12_COMMAND_QUEUE_DESC* desc,
                                                    REFIID, void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
    D3D12_COMMAND_QUEUE_DESC d = *desc;
    d.Flags &= D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;  // drop Xbox-only flags
    d.Priority = d.Priority > D3D12_COMMAND_QUEUE_PRIORITY_HIGH ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : d.Priority;
    HRESULT hr = orig<Fn>(self, 8)(self, &d, __uuidof(ID3D12CommandQueue), out);
    if (should_log(hr))
        LOG("[d3d12] CreateCommandQueue(type=%d prio=%d flags=0x%x) -> 0x%08lx", desc->Type, desc->Priority,
            desc->Flags, hr);
    if (SUCCEEDED(hr)) vt_patch(*out, kVtCommandQueue);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device_CreateCommandAllocator(ID3D12Device* self, D3D12_COMMAND_LIST_TYPE type, REFIID,
                                                        void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_COMMAND_LIST_TYPE, REFIID, void**);
    HRESULT hr = orig<Fn>(self, 9)(self, type, __uuidof(ID3D12CommandAllocator), out);
    if (should_log(hr)) LOG("[d3d12] CreateCommandAllocator(type=%d) -> 0x%08lx", type, hr);
    if (SUCCEEDED(hr)) vt_patch(*out, kVtCommandAllocator);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device_CreateGraphicsPipelineState(ID3D12Device* self,
                                                             const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID,
                                                             void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
    HRESULT hr = orig<Fn>(self, 10)(self, desc, __uuidof(ID3D12PipelineState), out);
    if (should_log(hr))
        LOG("[d3d12] CreateGraphicsPipelineState(VS %zu bytes, PS %zu bytes) -> 0x%08lx", desc->VS.BytecodeLength,
            desc->PS.BytecodeLength, hr);
    if (SUCCEEDED(hr)) vt_patch(*out, kVtPipelineState);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device_CreateComputePipelineState(ID3D12Device* self,
                                                            const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID,
                                                            void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
    HRESULT hr = orig<Fn>(self, 11)(self, desc, __uuidof(ID3D12PipelineState), out);
    if (should_log(hr))
        LOG("[d3d12] CreateComputePipelineState(CS %zu bytes) -> 0x%08lx", desc->CS.BytecodeLength, hr);
    if (SUCCEEDED(hr)) vt_patch(*out, kVtPipelineState);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device_CreateCommandList(ID3D12Device* self, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                                   ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso, REFIID,
                                                   void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                                           ID3D12PipelineState*, REFIID, void**);
    HRESULT hr = orig<Fn>(self, 12)(self, mask, type, alloc, pso, __uuidof(ID3D12GraphicsCommandList), out);
    if (should_log(hr)) LOG("[d3d12] CreateCommandList(type=%d) -> 0x%08lx", type, hr);
    if (SUCCEEDED(hr)) {
        // Upgrade to the newest list interface so slots past List1 exist.
        auto* l = static_cast<ID3D12GraphicsCommandList*>(*out);
        ID3D12GraphicsCommandList7* l7 = nullptr;
        if (SUCCEEDED(l->QueryInterface(IID_PPV_ARGS(&l7)))) {
            l->Release();
            *out = l7;
        }
        vt_patch(*out, kVtGraphicsCommandList);
    }
    return hr;
}

// Xbox device slot 73: CreateCommandList from a 16-byte description.
// Reference title: desc = {0 (DIRECT), 2, 1 (node mask), 0}, then (allocator, pso, riid, out).
HRESULT STDMETHODCALLTYPE Device_CreateCommandListX(ID3D12Device* self, const uint32_t* desc,
                                                    ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso,
                                                    REFIID riid, void** out) {
    LOG("[d3d12] CreateCommandListX(desc={%u,%u,%u,%u})", desc[0], desc[1], desc[2], desc[3]);
    return Device_CreateCommandList(self, 0, static_cast<D3D12_COMMAND_LIST_TYPE>(desc[0]), alloc, pso, riid, out);
}

HRESULT STDMETHODCALLTYPE Device_CreateDescriptorHeap(ID3D12Device* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                                      REFIID, void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
    HRESULT hr = orig<Fn>(self, 14)(self, desc, __uuidof(ID3D12DescriptorHeap), out);
    if (should_log(hr))
        LOG("[d3d12] CreateDescriptorHeap(type=%d n=%u flags=0x%x) -> 0x%08lx", desc->Type, desc->NumDescriptors,
            desc->Flags, hr);
    if (SUCCEEDED(hr)) vt_patch(*out, kVtDescriptorHeap);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device_CreateRootSignature(ID3D12Device* self, UINT mask, const void* blob, SIZE_T len,
                                                     REFIID, void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
    HRESULT hr = orig<Fn>(self, 16)(self, mask, blob, len, __uuidof(ID3D12RootSignature), out);
    if (should_log(hr)) LOG("[d3d12] CreateRootSignature(%zu bytes) -> 0x%08lx", len, hr);
    if (SUCCEEDED(hr)) vt_patch(*out, kVtRootSignature);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device_CreateCommittedResource(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* hp,
                                                         D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* desc,
                                                         D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* clear,
                                                         REFIID, void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                           const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                           const D3D12_CLEAR_VALUE*, REFIID, void**);
    HRESULT hr = orig<Fn>(self, 27)(self, hp, hf, desc, st, clear, __uuidof(ID3D12Resource), out);
    if (should_log(hr))
        LOG("[d3d12] CreateCommittedResource(heap=%d dim=%d %llux%u fmt=%d flags=0x%x) -> 0x%08lx", hp->Type,
            desc->Dimension, (unsigned long long)desc->Width, desc->Height, desc->Format, desc->Flags, hr);
    if (SUCCEEDED(hr)) vt_patch(*out, kVtResource);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device_CreateHeap(ID3D12Device* self, const D3D12_HEAP_DESC* desc, REFIID, void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, REFIID, void**);
    HRESULT hr = orig<Fn>(self, 28)(self, desc, __uuidof(ID3D12Heap), out);
    if (should_log(hr))
        LOG("[d3d12] CreateHeap(%llu bytes, type=%d flags=0x%x) -> 0x%08lx", (unsigned long long)desc->SizeInBytes,
            desc->Properties.Type, desc->Flags, hr);
    if (SUCCEEDED(hr)) vt_patch(*out, kVtHeap);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device_CreatePlacedResource(ID3D12Device* self, ID3D12Heap* heap, UINT64 offset,
                                                      const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES st,
                                                      const D3D12_CLEAR_VALUE* clear, REFIID, void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
                                           D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
    HRESULT hr = orig<Fn>(self, 29)(self, heap, offset, desc, st, clear, __uuidof(ID3D12Resource), out);
    if (should_log(hr))
        LOG("[d3d12] CreatePlacedResource(dim=%d %llux%u fmt=%d) -> 0x%08lx", desc->Dimension,
            (unsigned long long)desc->Width, desc->Height, desc->Format, hr);
    if (SUCCEEDED(hr)) vt_patch(*out, kVtResource);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device_CreateFence(ID3D12Device* self, UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID,
                                             void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT64, D3D12_FENCE_FLAGS, REFIID, void**);
    HRESULT hr = orig<Fn>(self, 36)(self, initial, flags, __uuidof(ID3D12Fence), out);
    if (should_log(hr)) LOG("[d3d12] CreateFence(%llu, flags=0x%x) -> 0x%08lx", (unsigned long long)initial, flags, hr);
    if (SUCCEEDED(hr)) vt_patch(*out, kVtFence);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device_CreateQueryHeap(ID3D12Device* self, const D3D12_QUERY_HEAP_DESC* desc, REFIID,
                                                 void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_QUERY_HEAP_DESC*, REFIID, void**);
    HRESULT hr = orig<Fn>(self, 39)(self, desc, __uuidof(ID3D12QueryHeap), out);
    if (should_log(hr)) LOG("[d3d12] CreateQueryHeap(type=%d n=%u) -> 0x%08lx", desc->Type, desc->Count, hr);
    if (SUCCEEDED(hr)) vt_patch(*out, kVtQueryHeap);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device_CreateCommandSignature(ID3D12Device* self, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                                        ID3D12RootSignature* rs, REFIID, void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*,
                                           REFIID, void**);
    HRESULT hr = orig<Fn>(self, 41)(self, desc, rs, __uuidof(ID3D12CommandSignature), out);
    if (should_log(hr)) LOG("[d3d12] CreateCommandSignature(%u args) -> 0x%08lx", desc->NumArgumentDescs, hr);
    if (SUCCEEDED(hr)) vt_patch(*out, kVtCommandSignature);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device_CheckFeatureSupport(ID3D12Device* self, D3D12_FEATURE f, void* data, UINT size) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_FEATURE, void*, UINT);
    HRESULT hr = orig<Fn>(self, 13)(self, f, data, size);
    if (should_log(hr)) LOG("[d3d12] CheckFeatureSupport(feature=%d, %u bytes) -> 0x%08lx", f, size, hr);
    return hr;
}

// Upgrade to the newest device interface the PC runtime offers and size the
// patched vtable to match (slot counts per version, from d3d12.h).
ID3D12Device* upgrade(ID3D12Device* dev, int* slots) {
    struct V { const IID& iid; int slots; const char* name; };
    const V versions[] = {
        {__uuidof(ID3D12Device10), 79, "ID3D12Device10"}, {__uuidof(ID3D12Device9), 76, "ID3D12Device9"},
        {__uuidof(ID3D12Device8), 73, "ID3D12Device8"},   {__uuidof(ID3D12Device5), 65, "ID3D12Device5"},
    };
    for (const auto& v : versions) {
        void* p = nullptr;
        if (SUCCEEDED(dev->QueryInterface(v.iid, &p))) {
            dev->Release();
            *slots = v.slots;
            LOG("[d3d12] using %s", v.name);
            return static_cast<ID3D12Device*>(p);
        }
    }
    *slots = 44;
    return dev;
}

}  // namespace

HRESULT d3d12_create_title_device(void** out) {
    if (g_device) {
        g_device->AddRef();
        *out = g_device;
        return S_OK;
    }
    char dbg[8] = "";
    if (GetEnvironmentVariableA("VICEPC_D3D_DEBUG", dbg, sizeof(dbg)) && dbg[0] == '1') {
        ID3D12Debug* d = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d)))) {
            d->EnableDebugLayer();
            d->Release();
            LOG("[d3d12] debug layer enabled");
        }
    }
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr)) {
        LOG("[d3d12] D3D12CreateDevice failed 0x%08lx", hr);
        return hr;
    }
    int slots = 44;
    dev = upgrade(dev, &slots);
    g_device_iface.pc_slots = slots;

    vt_patch(dev, g_device_iface);
#define OV(slot, fn) vt_override(dev, slot, reinterpret_cast<void*>(&fn))
    OV(0, Device_QueryInterface);
    OV(8, Device_CreateCommandQueue);
    OV(9, Device_CreateCommandAllocator);
    OV(10, Device_CreateGraphicsPipelineState);
    OV(11, Device_CreateComputePipelineState);
    OV(12, Device_CreateCommandList);
    OV(13, Device_CheckFeatureSupport);
    OV(14, Device_CreateDescriptorHeap);
    OV(16, Device_CreateRootSignature);
    OV(27, Device_CreateCommittedResource);
    OV(28, Device_CreateHeap);
    OV(29, Device_CreatePlacedResource);
    OV(36, Device_CreateFence);
    OV(39, Device_CreateQueryHeap);
    OV(41, Device_CreateCommandSignature);
    // Xbox device layout diverges from PC after the core methods:
    OV(48, Device_CreateCommandQueue);   // Xbox CreateCommandQueue variant (desc, riid, out)
    OV(73, Device_CreateCommandListX);   // Xbox CreateCommandList(desc*, alloc, pso, riid, out)
#undef OV
    g_device = dev;
    g_device->AddRef();
    *out = dev;
    return S_OK;
}
