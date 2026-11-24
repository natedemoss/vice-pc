// The title's D3D12 device: a real PC device with a patched vtable (see vtpatch.h).
//
// Create* methods are overridden so that (a) the Xbox IIDs the title passes are
// replaced with the PC IID for that method, (b) every object handed back gets its
// vtable patched too, and (c) every call is logged with its HRESULT, so we can see
// exactly which Xbox-specific parameters the PC runtime rejects.

#include "device.h"
#include "cmdlist.h"
#include "dxgi.h"
#include "gpumem.h"
#include "vtpatch.h"

#include "../hle.h"
#include "../../log.h"
#include "../../stubs.h"

#include <windows.h>
#include <intrin.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>

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

bool from_game(void* ra);

void patch_resource(void* r);

// ---- IUnknown ---------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device_QueryInterface(ID3D12Device* self, REFIID riid, void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, REFIID, void**);
    if (dxgi_is_device_iid(riid)) {
        *out = dxgi_device_for(self);
        LOG("[d3d12] Device::QueryInterface(IDXGIDevice) -> DXGI device stand-in");
        return S_OK;
    }
    HRESULT hr = orig<Fn>(self, 0)(self, riid, out);
    if (FAILED(hr) && from_game(_ReturnAddress())) {
        // Title callers only: the PC runtime also queries its own private IIDs and must get E_NOINTERFACE.
        // Xbox-only device IIDs (newer Xbox device versions): hand back the same device.
        char g[64];
        fmt_guid(&riid, g, sizeof(g));
        LOG("[d3d12] Device::QueryInterface(%s) unknown on PC -> returning device (from %p)", g, CALLER);
        self->AddRef();
        *out = self;
        hr = S_OK;
    }
    return hr;
}

// ---- object creation ----------------------------------------------------------

// Xbox ID3D12CommandQueue slot 18: GetTimestampFrequency(UINT64*) (game+0x24841a6 uses the
// result as ticks per second). On PC slot 18 is GetDesc, which writes 16 bytes into the
// title's 8-byte variable and corrupts its stack, so forward to PC slot 16 instead.
HRESULT STDMETHODCALLTYPE Queue_GetTimestampFrequencyX(ID3D12CommandQueue* self, UINT64* freq) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT64*);
    return orig<Fn>(self, 16)(self, freq);
}

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
    if (SUCCEEDED(hr)) {
        vt_patch(*out, kVtCommandQueue);
        vt_override(*out, 18, reinterpret_cast<void*>(&Queue_GetTimestampFrequencyX));
    }
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

bool from_game(void* ra) {
    uint64_t r;
    return hle_rva(ra, &r);
}

// ID3D12DescriptorHeap slot 9. Title (C ABI): handle returned in RAX. Anyone else
// (C++ ABI): hidden out pointer in rdx, pointer returned.
uint64_t STDMETHODCALLTYPE Heap_GetCPUStart(ID3D12DescriptorHeap* self, D3D12_CPU_DESCRIPTOR_HANDLE* out) {
    using Fn = D3D12_CPU_DESCRIPTOR_HANDLE*(STDMETHODCALLTYPE*)(ID3D12DescriptorHeap*, D3D12_CPU_DESCRIPTOR_HANDLE*);
    if (from_game(_ReturnAddress())) {
        uint64_t game;
        if (gm_object(GM_DESC_CPU, self, &game)) return game;
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        orig<Fn>(self, 9)(self, &h);
        return h.ptr;
    }
    orig<Fn>(self, 9)(self, out);
    return reinterpret_cast<uint64_t>(out);
}

// Xbox D3D12_DESCRIPTOR_HEAP_DESC carries two extra fields after the PC ones: the CPU
// address the title wants the heap at, and its size in bytes (count * increment).
struct XboxDescriptorHeapDesc {
    D3D12_DESCRIPTOR_HEAP_DESC pc;
    uint64_t address;
    uint64_t size;
};

HRESULT STDMETHODCALLTYPE Device_CreateDescriptorHeap(ID3D12Device* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                                      REFIID, void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
    uint64_t want = 0;
    if (from_game(_ReturnAddress())) {
        auto* x = reinterpret_cast<const XboxDescriptorHeapDesc*>(desc);
        UINT inc = self->GetDescriptorHandleIncrementSize(desc->Type);
        if (x->address && x->size == uint64_t(inc) * desc->NumDescriptors) want = x->address;
    }
    HRESULT hr = orig<Fn>(self, 14)(self, desc, __uuidof(ID3D12DescriptorHeap), out);
    if (should_log(hr))
        LOG("[d3d12] CreateDescriptorHeap(type=%d n=%u flags=0x%x at=0x%llx) -> 0x%08lx", desc->Type,
            desc->NumDescriptors, desc->Flags, (unsigned long long)want, hr);
    if (SUCCEEDED(hr)) {
        auto* heap = static_cast<ID3D12DescriptorHeap*>(*out);
        vt_patch(heap, kVtDescriptorHeap);
        vt_override(heap, 9, reinterpret_cast<void*>(&Heap_GetCPUStart));
        if (want) {
            using StartFn = D3D12_CPU_DESCRIPTOR_HANDLE*(STDMETHODCALLTYPE*)(ID3D12DescriptorHeap*,
                                                                             D3D12_CPU_DESCRIPTOR_HANDLE*);
            D3D12_CPU_DESCRIPTOR_HANDLE real;
            orig<StartFn>(heap, 9)(heap, &real);
            UINT inc = self->GetDescriptorHandleIncrementSize(desc->Type);
            gm_add_range(GM_DESC_CPU, want, uint64_t(inc) * desc->NumDescriptors, real.ptr, "descriptor heap");
            gm_set_object(GM_DESC_CPU, heap, want);
        }
    }
    return hr;
}

// ---- views: destination handles (and CBV buffer addresses) are title addresses ----

D3D12_CPU_DESCRIPTOR_HANDLE tcpu(D3D12_CPU_DESCRIPTOR_HANDLE h) {
    h.ptr = static_cast<SIZE_T>(gm_translate(GM_DESC_CPU, h.ptr));
    return h;
}

void STDMETHODCALLTYPE Device_CreateCBV(ID3D12Device* self, const D3D12_CONSTANT_BUFFER_VIEW_DESC* d,
                                        D3D12_CPU_DESCRIPTOR_HANDLE h) {
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*,
                                        D3D12_CPU_DESCRIPTOR_HANDLE);
    D3D12_CONSTANT_BUFFER_VIEW_DESC t;
    if (d) { t = *d; t.BufferLocation = gm_translate(GM_GPU_VA, t.BufferLocation); }
    orig<Fn>(self, 17)(self, d ? &t : nullptr, tcpu(h));
}

void STDMETHODCALLTYPE Device_CreateSRV(ID3D12Device* self, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d,
                                        D3D12_CPU_DESCRIPTOR_HANDLE h) {
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*,
                                        D3D12_CPU_DESCRIPTOR_HANDLE);
    D3D12_SHADER_RESOURCE_VIEW_DESC t;
    if (d && d->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE) {
        t = *d;
        t.RaytracingAccelerationStructure.Location = gm_translate(GM_GPU_VA, t.RaytracingAccelerationStructure.Location);
        d = &t;
    }
    orig<Fn>(self, 18)(self, r, d, tcpu(h));
}

void STDMETHODCALLTYPE Device_CreateUAV(ID3D12Device* self, ID3D12Resource* r, ID3D12Resource* counter,
                                        const D3D12_UNORDERED_ACCESS_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*,
                                        const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    orig<Fn>(self, 19)(self, r, counter, d, tcpu(h));
}

void STDMETHODCALLTYPE Device_CreateRTV(ID3D12Device* self, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* d,
                                        D3D12_CPU_DESCRIPTOR_HANDLE h) {
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                        D3D12_CPU_DESCRIPTOR_HANDLE);
    orig<Fn>(self, 20)(self, r, d, tcpu(h));
}

void STDMETHODCALLTYPE Device_CreateDSV(ID3D12Device* self, ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* d,
                                        D3D12_CPU_DESCRIPTOR_HANDLE h) {
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                        D3D12_CPU_DESCRIPTOR_HANDLE);
    orig<Fn>(self, 21)(self, r, d, tcpu(h));
}

void STDMETHODCALLTYPE Device_CreateSampler(ID3D12Device* self, const D3D12_SAMPLER_DESC* d,
                                            D3D12_CPU_DESCRIPTOR_HANDLE h) {
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_SAMPLER_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    orig<Fn>(self, 22)(self, d, tcpu(h));
}

void STDMETHODCALLTYPE Device_CopyDescriptors(ID3D12Device* self, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* ds,
                                              const UINT* dsz, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* ss,
                                              const UINT* ssz, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                        const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
    D3D12_CPU_DESCRIPTOR_HANDLE sd[64], sss[64];
    D3D12_CPU_DESCRIPTOR_HANDLE* td = nd <= 64 ? sd : new D3D12_CPU_DESCRIPTOR_HANDLE[nd];
    D3D12_CPU_DESCRIPTOR_HANDLE* ts = ns <= 64 ? sss : new D3D12_CPU_DESCRIPTOR_HANDLE[ns];
    for (UINT i = 0; i < nd; ++i) td[i] = tcpu(ds[i]);
    for (UINT i = 0; i < ns; ++i) ts[i] = tcpu(ss[i]);
    orig<Fn>(self, 23)(self, nd, td, dsz, ns, ts, ssz, type);
    if (td != sd) delete[] td;
    if (ts != sss) delete[] ts;
}

void STDMETHODCALLTYPE Device_CopyDescriptorsSimple(ID3D12Device* self, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE d,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE s, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
                                        D3D12_DESCRIPTOR_HEAP_TYPE);
    orig<Fn>(self, 24)(self, n, tcpu(d), tcpu(s), type);
}

UINT element_bytes(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
        case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT: return 12;
        case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT: return 8;
        case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R16_UNORM: return f >= DXGI_FORMAT_R16_TYPELESS ? 2 : 4;
        case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_R8_SNORM: return 1;
        default: return 4;
    }
}

// Xbox SRV desc: the PC fields, then the GPU address of the data (no resource object).
struct XboxSrvDesc {
    D3D12_SHADER_RESOURCE_VIEW_DESC pc;  // 0x28 bytes
    uint64_t gpu_va;
};

std::atomic<int> g_srvx_logged{0};

// Xbox-only device slot 70: SRV straight from a GPU address.
// Reference title: (resource desc of the buffer, SRV desc + GPU VA, destination handle).
void STDMETHODCALLTYPE Device_CreateSRVAtAddressX(ID3D12Device* self, const D3D12_RESOURCE_DESC* rdesc,
                                                  const XboxSrvDesc* x, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*,
                                        D3D12_CPU_DESCRIPTOR_HANDLE);
    D3D12_SHADER_RESOURCE_VIEW_DESC d = x->pc;
    void* owner = nullptr;
    uint64_t off = 0;
    ID3D12Resource* res = nullptr;
    bool ok = gm_lookup(GM_GPU_VA, x->gpu_va, &owner, &off) && owner;
    if (ok && d.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
        res = static_cast<ID3D12Resource*>(owner);
        uint64_t res_va = orig<UINT64(STDMETHODCALLTYPE*)(ID3D12Resource*)>(res, 11)(res);
        uint64_t byte_off = gm_translate(GM_GPU_VA, x->gpu_va) - res_va;
        UINT el = d.Buffer.StructureByteStride ? d.Buffer.StructureByteStride
                  : (d.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) ? 4 : element_bytes(d.Format);
        d.Buffer.FirstElement += byte_off / el;
    } else if (ok && off == 0 && d.ViewDimension != D3D12_SRV_DIMENSION_BUFFER) {
        res = static_cast<ID3D12Resource*>(owner);  // texture view on the texture created at that address
        if (!vt_is_patched(res)) {
            static std::atomic<int> bad{0};
            if (bad.fetch_add(1) < 10)
                LOG("[d3d12]   SRVAtAddressX: owner %p of va 0x%llx no longer looks like a resource (vptr %p)", res,
                    (unsigned long long)x->gpu_va, *reinterpret_cast<void**>(res));
            res = nullptr;
        }
        if (res && res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) res = nullptr;
    }
    if (g_srvx_logged.fetch_add(1) < 50)
        LOG("[d3d12] CreateSRVAtAddressX(va=0x%llx dim=%d fmt=%d n=%u width=%llu) -> %s", (unsigned long long)x->gpu_va,
            d.ViewDimension, d.Format, d.Buffer.NumElements, (unsigned long long)(rdesc ? rdesc->Width : 0),
            res ? "view on backing buffer" : "no backing resource, null view");
    if (!res) {
        // Null descriptor: keeps the slot valid for the GPU (reads return zero).
        D3D12_SHADER_RESOURCE_VIEW_DESC n{};
        n.Format = DXGI_FORMAT_R32_UINT;
        n.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        n.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        orig<Fn>(self, 18)(self, nullptr, &n, tcpu(h));
        return;
    }
    orig<Fn>(self, 18)(self, res, &d, tcpu(h));
}

// ---- resources at title-chosen GPU addresses ------------------------------------

// ID3D12Resource slot 11: title sees the GPU VA it chose.
UINT64 STDMETHODCALLTYPE Resource_GetGPUVirtualAddress(ID3D12Resource* self) {
    uint64_t game;
    if (gm_object(GM_GPU_VA, self, &game)) return game;
    return orig<UINT64(STDMETHODCALLTYPE*)(ID3D12Resource*)>(self, 11)(self);
}

// ID3D12Resource slot 2: drop title address mappings when the resource dies.
ULONG STDMETHODCALLTYPE Resource_Release(ID3D12Resource* self) {
    uint64_t game;
    bool mapped = gm_object(GM_GPU_VA, self, &game);
    ULONG n = orig<ULONG(STDMETHODCALLTYPE*)(ID3D12Resource*)>(self, 2)(self);
    if (n == 0 && mapped) gm_remove_owner(self);
    return n;
}

void patch_resource(void* r) {
    vt_patch(r, kVtResource);
    vt_override(r, 2, reinterpret_cast<void*>(&Resource_Release));
    vt_override(r, 11, reinterpret_cast<void*>(&Resource_GetGPUVirtualAddress));
}

// System-memory heaps opened over title memory, keyed by allocation base.
std::mutex g_heap_mu;
std::unordered_map<uint64_t, ID3D12Heap*>& g_addr_heaps = *new std::unordered_map<uint64_t, ID3D12Heap*>();

ID3D12Heap* heap_over(ID3D12Device* dev, void* alloc_base) {
    std::lock_guard<std::mutex> lk(g_heap_mu);
    auto key = reinterpret_cast<uint64_t>(alloc_base);
    auto it = g_addr_heaps.find(key);
    if (it != g_addr_heaps.end()) return it->second;
    // ID3D12Device3::OpenExistingHeapFromAddress (PC slot 48; overridden for the title, so call the original).
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const void*, REFIID, void**);
    ID3D12Heap* heap = nullptr;
    HRESULT hr = orig<Fn>(dev, 48)(dev, alloc_base, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&heap));
    if (FAILED(hr)) heap = nullptr;
    LOG("[d3d12] OpenExistingHeapFromAddress(%p) -> 0x%08lx%s", alloc_base, hr,
        heap ? "" : " (falling back to GPU-only memory)");
    g_addr_heaps[key] = heap;
    return heap;
}

// Xbox-only device slot 133: create a resource at a title-chosen GPU address.
// Reference title: (gpu_va, desc, state = -1, clear = null, IID_ID3D12Resource, out).
HRESULT STDMETHODCALLTYPE Device_CreateResourceAtAddressX(ID3D12Device* self, uint64_t game_va,
                                                          const D3D12_RESOURCE_DESC* xdesc, UINT state,
                                                          const D3D12_CLEAR_VALUE* clear, REFIID, void** out) {
    D3D12_RESOURCE_DESC d = *xdesc;
    d.Flags &= static_cast<D3D12_RESOURCE_FLAGS>(0x3F);  // drop Xbox-only flags (e.g. 0x20000)
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        // Xbox alignment / layout values describe console tiling; let the PC driver choose.
        d.Alignment = 0;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    // Clear values are only legal on render-target / depth-stencil resources.
    if (!(d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) clear = nullptr;
    using Placed = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
                                               D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
    using Committed = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                  const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                  const D3D12_CLEAR_VALUE*, REFIID, void**);
    HRESULT hr = E_FAIL;
    const char* how = "";
    uint64_t delta = 0;
    *out = nullptr;

    MEMORY_BASIC_INFORMATION mbi{};
    bool cpu_backed = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                      VirtualQuery(reinterpret_cast<void*>(game_va), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT;
    if (cpu_backed) {
        // Title memory is ordinary committed memory here: let the GPU see it directly.
        if (ID3D12Heap* heap = heap_over(self, mbi.AllocationBase)) {
            uint64_t off = game_va - reinterpret_cast<uint64_t>(mbi.AllocationBase);
            uint64_t aligned = off & ~uint64_t(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT - 1);
            delta = off - aligned;
            D3D12_RESOURCE_DESC pd = d;
            pd.Width += delta;
            pd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
            hr = orig<Placed>(self, 29)(self, heap, aligned, &pd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                        __uuidof(ID3D12Resource), out);
            how = "shared with title memory";
            if (FAILED(hr)) LOG("[d3d12]   placed resource over title memory failed 0x%08lx", hr);
        }
    }
    if (!*out) {
        delta = 0;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        hr = orig<Committed>(self, 27)(self, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                       clear,
                                       __uuidof(ID3D12Resource), out);
        how = "GPU-only";
    }
    LOG("[d3d12] CreateResourceAtAddressX(va=0x%llx dim=%d %llux%u fmt=%d flags=0x%x state=0x%x) -> 0x%08lx, %s",
        (unsigned long long)game_va, xdesc->Dimension, (unsigned long long)xdesc->Width, xdesc->Height, xdesc->Format,
        xdesc->Flags, state, hr, how);
    if (FAILED(hr)) return hr;
    auto* res = static_cast<ID3D12Resource*>(*out);
    patch_resource(res);
    uint64_t real = orig<UINT64(STDMETHODCALLTYPE*)(ID3D12Resource*)>(res, 11)(res) + delta;
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        // Textures: register the title address so views made from it find this resource.
        D3D12_RESOURCE_ALLOCATION_INFO ai = self->GetResourceAllocationInfo(0, 1, &d);
        gm_add_range(GM_GPU_VA, game_va, ai.SizeInBytes ? ai.SizeInBytes : 1, real, "texture", res);
        // Diagnostic: hardware write-watch on the newest texture objects (VICEPC_WATCH=1).
        {
            static int slot = 0;
            char w[4] = "";
            if (GetEnvironmentVariableA("VICEPC_WATCH", w, sizeof(w)) && w[0] == '1') {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                GetThreadContext(GetCurrentThread(), &ctx);
                DWORD64* dr[4] = {&ctx.Dr0, &ctx.Dr1, &ctx.Dr2, &ctx.Dr3};
                *dr[slot] = reinterpret_cast<DWORD64>(res);
                ctx.Dr7 |= (1ull << (slot * 2));                    // local enable
                ctx.Dr7 &= ~(0xFull << (16 + slot * 4));
                ctx.Dr7 |= (0x1ull | (0x2ull << 2)) << (16 + slot * 4);  // write, 8 bytes
                SetThreadContext(GetCurrentThread(), &ctx);
                slot = (slot + 1) & 3;
            }
        }
        static std::atomic<int> tex_logged{0};
        if (tex_logged.fetch_add(1) < 400) LOG("[d3d12]   texture object %p at title 0x%llx", res, (unsigned long long)game_va);
        gm_set_object(GM_GPU_VA, res, game_va);
    }
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        gm_add_range(GM_GPU_VA, game_va, d.Width, real, "buffer", res);
        gm_set_object(GM_GPU_VA, res, game_va);
        if (delta) LOG("[d3d12]   note: buffer starts 0x%llx into its placement (offsets need adjusting)",
                       (unsigned long long)delta);
    }
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
    if (SUCCEEDED(hr)) patch_resource(*out);
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
    if (SUCCEEDED(hr)) patch_resource(*out);
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
    {
        // Diagnostic (VICEPC_WATCH=2): watch the title's per-thread render context
        // (TLS block + 0x12A8), which is set just before this call during device setup.
        char w[4] = "";
        static bool armed = false;
        if (!armed && GetEnvironmentVariableA("VICEPC_WATCH", w, sizeof(w)) && w[0] == '2') {
            armed = true;
            auto** tls = reinterpret_cast<uint8_t**>(__readgsqword(0x58));
            uint8_t* field = tls[0] + 0x12A8;
            LOG("[watch] render context field %p = %p (thread %lu)", field, *reinterpret_cast<void**>(field), GetCurrentThreadId());
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            GetThreadContext(GetCurrentThread(), &ctx);
            ctx.Dr0 = reinterpret_cast<DWORD64>(field);
            ctx.Dr7 = 1 | ((0x1ull | (0x2ull << 2)) << 16);
            SetThreadContext(GetCurrentThread(), &ctx);
        }
    }
    HRESULT hr = orig<Fn>(self, 41)(self, desc, rs, __uuidof(ID3D12CommandSignature), out);
    char types[96] = "";
    size_t len = 0;
    for (UINT i = 0; i < desc->NumArgumentDescs && i < 12; ++i)
        len += snprintf(types + len, sizeof(types) - len, "%d ", desc->pArgumentDescs[i].Type);
    if (should_log(hr))
        LOG("[d3d12] CreateCommandSignature(stride=%u, args: %s, rootsig=%p) -> 0x%08lx", desc->ByteStride, types, rs, hr);
    if (FAILED(hr)) {
        // Mesh-shader dispatch (and anything newer) isn't available on every PC GPU: keep an
        // object of the same stride with a plain compute dispatch so the title's setup completes.
        // Indirect calls through it are wrong until mesh shading is translated.
        D3D12_INDIRECT_ARGUMENT_DESC args[16];
        UINT n = desc->NumArgumentDescs < 16 ? desc->NumArgumentDescs : 16;
        bool changed = false;
        for (UINT i = 0; i < n; ++i) {
            args[i] = desc->pArgumentDescs[i];
            if (args[i].Type >= D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS) {
                args[i].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
                changed = true;
            }
        }
        if (changed) {
            D3D12_COMMAND_SIGNATURE_DESC d = *desc;
            d.pArgumentDescs = args;
            d.NumArgumentDescs = n;
            if (d.ByteStride < sizeof(D3D12_DISPATCH_ARGUMENTS)) d.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
            hr = orig<Fn>(self, 41)(self, &d, rs, __uuidof(ID3D12CommandSignature), out);
            LOG("[d3d12]   retried with mesh/ray dispatch -> compute dispatch: 0x%08lx", hr);
        }
    }
    if (SUCCEEDED(hr)) vt_patch(*out, kVtCommandSignature);
    return hr;
}

// ID3D12Device2::CreatePipelineState (slot 47): log the sub-object stream before the PC
// runtime parses it. Each sub-object starts with a D3D12_PIPELINE_STATE_SUBOBJECT_TYPE
// (u32) and is pointer-aligned; unknown types are Xbox-only.
std::atomic<int> g_pso_logged{0};

HRESULT STDMETHODCALLTYPE Device_CreatePipelineState(ID3D12Device* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                                     REFIID riid, void** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
    char types[256] = "";
    if (desc && g_pso_logged.load() < 40) {
        size_t pos = 0, len = 0;
        const auto* base = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
        int n = 0;
        while (pos + 4 <= desc->SizeInBytes && n < 40) {
            uint32_t t = *reinterpret_cast<const uint32_t*>(base + pos);
            len += snprintf(types + len, sizeof(types) - len, "%u ", t);
            if (len >= sizeof(types) - 8) break;
            // Size of each sub-object (from d3dx12.h CD3DX12_PIPELINE_STATE_STREAM_* layouts), padded to 8.
            static const uint32_t kSize[] = {
                16, 16, 24, 24, 24, 24, 24, 40,   // root sig, VS, PS, DS, HS, GS, CS, stream output
                328, 8, 48, 48, 24, 8, 8, 8,      // blend, sample mask, raster, depth-stencil, input layout, IB strip, topology, RT formats
                8, 16, 8, 16, 24, 8, 64, 8,       // DS format, sample desc, node mask, cached PSO, flags, depth-stencil1, view instancing
                24, 24, 64, 8, 64};
            if (t >= sizeof(kSize) / sizeof(kSize[0])) break;  // unknown (Xbox) type: stop walking
            if (t == 10) pos += 28 + 4;                         // RT formats: type + 8 formats + count
            pos += kSize[t] ? kSize[t] : 8;
            pos = (pos + 7) & ~size_t(7);
            ++n;
        }
    }
    if (g_pso_logged.fetch_add(1) < 40) {
        LOG("[d3d12] CreatePipelineState(%zu bytes, subobjects: %s) ...", desc ? desc->SizeInBytes : 0, types);
        log_flush();
    }
    HRESULT hr = orig<Fn>(self, 47)(self, desc, __uuidof(ID3D12PipelineState), out);
    if (should_log(hr)) LOG("[d3d12] CreatePipelineState -> 0x%08lx", hr);
    if (SUCCEEDED(hr)) vt_patch(*out, kVtPipelineState);
    (void)riid;
    return hr;
}

// Xbox-only device slot 62: pipeline state from a precompiled console pipeline blob.
// Reference title (game+0x247fe66): (desc {blob, ?, ?, 1}, 0, 0, IID_ID3D12PipelineState, out).
// The blob holds console GPU code, which PC D3D12 cannot run: fail cleanly (the title
// checks the HRESULT and carries on without that pipeline).
std::atomic<int> g_pso_x_logged{0};
HRESULT STDMETHODCALLTYPE Device_CreatePipelineStateX(ID3D12Device*, const uint64_t* desc, uint64_t a2, uint64_t a3,
                                                      REFIID, void** out) {
    int n = g_pso_x_logged.fetch_add(1);
    if (n < 8 && desc)
        LOG("[d3d12] CreatePipelineStateX(desc={%llx %llx %llx %llx}, %llx, %llx) -> E_NOTIMPL (console pipeline blob)",
            (unsigned long long)desc[0], (unsigned long long)desc[1], (unsigned long long)desc[2],
            (unsigned long long)desc[3], (unsigned long long)a2, (unsigned long long)a3);
    else if (n == 8) LOG("[d3d12] CreatePipelineStateX: further calls not logged");
    if (out) *out = nullptr;
    return E_NOTIMPL;
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
    // The Xbox device keeps PC's order only for the core methods (ID3D12Device, slots
    // 0-43). From slot 44 on the meanings differ, and forwarding them into the PC runtime
    // ends in a fail-fast. They become logged stubs; known ones are overridden below.
    for (int s = 44; s < slots; ++s) {
        char name[48];
        snprintf(name, sizeof(name), "slot%d (Xbox-only)", s);
        vt_override(dev, s, stub_create("ID3D12Device", _strdup(name), stub_stop_mode()));
    }
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
    OV(62, Device_CreatePipelineStateX);  // Xbox: PSO from a precompiled console blob
    OV(70, Device_CreateSRVAtAddressX);    // Xbox: buffer SRV from a GPU VA
    OV(133, Device_CreateResourceAtAddressX);  // Xbox: resource at a title-chosen GPU VA
    OV(17, Device_CreateCBV);
    OV(18, Device_CreateSRV);
    OV(19, Device_CreateUAV);
    OV(20, Device_CreateRTV);
    OV(21, Device_CreateDSV);
    OV(22, Device_CreateSampler);
    OV(23, Device_CopyDescriptors);
    OV(24, Device_CopyDescriptorsSimple);
#undef OV
    g_device = dev;
    g_device->AddRef();
    *out = dev;
    return S_OK;
}
