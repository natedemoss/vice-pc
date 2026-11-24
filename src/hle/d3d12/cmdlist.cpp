// Command list methods that take title addresses (GPU VAs, descriptor handles):
// translate them to real PC addresses before forwarding (see gpumem.h).

#include "cmdlist.h"
#include "gpumem.h"
#include "vtpatch.h"

#include "../../log.h"
#include "../../stubs.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <windows.h>
#include <d3d12.h>

namespace {

using List = ID3D12GraphicsCommandList;

template <typename T>
T orig(void* self, int slot) {
    return reinterpret_cast<T>(vt_original(self, slot));
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu(D3D12_CPU_DESCRIPTOR_HANDLE h) {
    h.ptr = static_cast<SIZE_T>(gm_translate(GM_DESC_CPU, h.ptr));
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE gpu(D3D12_GPU_DESCRIPTOR_HANDLE h) {
    h.ptr = gm_translate(GM_DESC_GPU, h.ptr);
    return h;
}
D3D12_GPU_VIRTUAL_ADDRESS va(D3D12_GPU_VIRTUAL_ADDRESS a) { return gm_translate(GM_GPU_VA, a); }

// 30 / 31: Set{Compute,Graphics}RootDescriptorTable
template <int Slot>
void STDMETHODCALLTYPE SetRootTable(List* self, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    orig<void(STDMETHODCALLTYPE*)(List*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(self, Slot)(self, i, gpu(h));
}

// 36..41: Set{Compute,Graphics}Root{ConstantBuffer,ShaderResource,UnorderedAccess}View
template <int Slot>
void STDMETHODCALLTYPE SetRootView(List* self, UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) {
    orig<void(STDMETHODCALLTYPE*)(List*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(self, Slot)(self, i, va(a));
}

void STDMETHODCALLTYPE IASetIndexBuffer(List* self, const D3D12_INDEX_BUFFER_VIEW* v) {
    D3D12_INDEX_BUFFER_VIEW t;
    if (v) { t = *v; t.BufferLocation = va(t.BufferLocation); }
    orig<void(STDMETHODCALLTYPE*)(List*, const D3D12_INDEX_BUFFER_VIEW*)>(self, 42)(self, v ? &t : nullptr);
}

void STDMETHODCALLTYPE IASetVertexBuffers(List* self, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) {
    D3D12_VERTEX_BUFFER_VIEW t[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    if (v && n <= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) {
        for (UINT i = 0; i < n; ++i) { t[i] = v[i]; t[i].BufferLocation = va(t[i].BufferLocation); }
        v = t;
    }
    orig<void(STDMETHODCALLTYPE*)(List*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*)>(self, 43)(self, start, n, v);
}

void STDMETHODCALLTYPE OMSetRenderTargets(List* self, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, BOOL single,
                                          const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    D3D12_CPU_DESCRIPTOR_HANDLE t[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
    D3D12_CPU_DESCRIPTOR_HANDLE d;
    if (rts) {
        UINT m = single ? (n ? 1u : 0u) : (n < 8 ? n : 8);
        for (UINT i = 0; i < m; ++i) t[i] = cpu(rts[i]);
        rts = t;
    }
    if (ds) { d = cpu(*ds); ds = &d; }
    orig<void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                  const D3D12_CPU_DESCRIPTOR_HANDLE*)>(self, 45)(self, n, rts, single, ds);
}

void STDMETHODCALLTYPE ClearDSV(List* self, D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT depth,
                                UINT8 stencil, UINT nr, const D3D12_RECT* r) {
    orig<void(STDMETHODCALLTYPE*)(List*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT,
                                  const D3D12_RECT*)>(self, 46)(self, cpu(h), f, depth, stencil, nr, r);
}

void STDMETHODCALLTYPE ClearRTV(List* self, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT* c, UINT nr,
                                const D3D12_RECT* r) {
    orig<void(STDMETHODCALLTYPE*)(List*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*)>(
        self, 47)(self, cpu(h), c, nr, r);
}

template <int Slot, typename V>
void STDMETHODCALLTYPE ClearUAV(List* self, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c,
                                ID3D12Resource* res, const V* vals, UINT nr, const D3D12_RECT* r) {
    orig<void(STDMETHODCALLTYPE*)(List*, D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*,
                                  const V*, UINT, const D3D12_RECT*)>(self, Slot)(self, gpu(g), cpu(c), res, vals,
                                                                                   nr, r);
}

}  // namespace

// ---- title-owned command lists ---------------------------------------------------
//
// Xbox command lists are user-mode GPU packet buffers: the title's inlined fast paths
// write packets at [list+0x18] (cursor) up to [list+0x30] (limit) and poke state fields
// in the object (e.g. +0x140, +0x544). On a PC list object that corrupts the driver.
// Until packets are decoded, every title command list is a title-owned object: a large
// zeroed body plus a packet buffer, with no-op methods. When the buffer fills, the title
// calls slot 25 to get space; the cursor is rewound (packets are discarded).

namespace {

constexpr size_t kFakeBody = 1 << 20;     // object body (title writes fields well past 0x500)
constexpr size_t kFakePackets = 1 << 20;  // packet buffer
constexpr size_t kFakeSlack = 64 << 10;   // headroom past the limit for one packet burst

struct FakeList {
    void** vtbl;
};

uint8_t* fake_buf(FakeList* l) { return reinterpret_cast<uint8_t*>(l) + kFakeBody; }
void fake_rewind(FakeList* l) {
    auto* o = reinterpret_cast<uint8_t*>(l);
    uint8_t* b = fake_buf(l);
    *reinterpret_cast<uint8_t**>(o + 0x18) = b;
    *reinterpret_cast<uint8_t**>(o + 0x30) = b + kFakePackets - kFakeSlack;
}

std::atomic<uint64_t> g_fake_rewinds{0};

HRESULT STDMETHODCALLTYPE Fake_QueryInterface(FakeList* self, REFIID, void** out) {
    *out = self;
    return S_OK;
}
ULONG STDMETHODCALLTYPE Fake_AddRef(FakeList*) { return 2; }
ULONG STDMETHODCALLTYPE Fake_Release(FakeList*) { return 1; }  // never freed: the title may hold stale pointers
HRESULT STDMETHODCALLTYPE Fake_Close(FakeList*) { return S_OK; }
HRESULT STDMETHODCALLTYPE Fake_Reset(FakeList* self, void*, void*) {
    fake_rewind(self);
    return S_OK;
}
void STDMETHODCALLTYPE Fake_MakeSpace(FakeList* self) {
    uint64_t n = g_fake_rewinds.fetch_add(1) + 1;
    if ((n & (n - 1)) == 0) LOG("[d3d12] title command lists: %llu packet buffers filled (discarded: packets not decoded yet)",
                                (unsigned long long)n);
    fake_rewind(self);
}

void** fake_vtable() {
    static void** vt = [] {
        constexpr int kSlots = 256;
        void** t = new void*[kSlots];
        for (int i = 0; i < kSlots; ++i) {
            char name[48];
            snprintf(name, sizeof(name), "slot%d", i);
            t[i] = stub_create("XboxCommandList", _strdup(name), stub_stop_mode());
        }
        t[0] = reinterpret_cast<void*>(&Fake_QueryInterface);
        t[1] = reinterpret_cast<void*>(&Fake_AddRef);
        t[2] = reinterpret_cast<void*>(&Fake_Release);
        t[8] = reinterpret_cast<void*>(&Fake_Close);
        t[9] = reinterpret_cast<void*>(&Fake_Reset);
        t[25] = reinterpret_cast<void*>(&Fake_MakeSpace);
        return t;
    }();
    return vt;
}

}  // namespace

bool cmdlist_title_packets(void* list, const uint32_t** start, size_t* dwords) {
    if (!list || *static_cast<void***>(list) != fake_vtable()) return false;
    auto* o = static_cast<uint8_t*>(list);
    uint8_t* b = o + kFakeBody;  // the title may reuse +0x10, so use the fixed buffer location
    uint8_t* cur = *reinterpret_cast<uint8_t**>(o + 0x18);
    *start = reinterpret_cast<const uint32_t*>(b);
    *dwords = (cur > b && cur < b + kFakePackets) ? static_cast<size_t>(cur - b) / 4 : 0;
    static std::atomic<int> logged{0};
    if (*dwords == 0 && logged.fetch_add(1) < 4)
        LOG("[pm4] list %p: +0x10=%p +0x18=%p +0x20=%p +0x28=%p +0x30=%p (buffer %p)", list,
            *reinterpret_cast<void**>(o + 0x10), cur, *reinterpret_cast<void**>(o + 0x20),
            *reinterpret_cast<void**>(o + 0x28), *reinterpret_cast<void**>(o + 0x30), b);
    return true;
}

void* cmdlist_create_title_list(int type) {
    auto* o = static_cast<uint8_t*>(VirtualAlloc(nullptr, kFakeBody + kFakePackets, MEM_RESERVE | MEM_COMMIT,
                                                 PAGE_READWRITE));
    if (!o) return nullptr;
    auto* l = reinterpret_cast<FakeList*>(o);
    l->vtbl = fake_vtable();
    *reinterpret_cast<uint8_t**>(o + 0x10) = o + kFakeBody;
    fake_rewind(l);
    static std::atomic<int> n{0};
    int k = n.fetch_add(1);
    if (k < 4 || (k & (k - 1)) == 0) LOG("[d3d12] title command list #%d (type %d) at %p: packets kept in title memory", k + 1, type, o);
    return l;
}

void cmdlist_install(void* list) {
    // Only the base ID3D12GraphicsCommandList methods (slots 0-58) keep PC's meaning on
    // Xbox. Extension slots differ (title calls slot 75 with a bool; PC slot 75 is
    // DispatchRays(desc*)), so they become logged no-ops until each is identified.
    static bool stubbed = false;
    for (int s = 59; s < 79; ++s) {
        static void* stubs[79] = {};
        if (!stubbed) {
            char name[48];
            snprintf(name, sizeof(name), "slot%d (Xbox-only)", s);
            stubs[s] = stub_create("ID3D12GraphicsCommandList", _strdup(name), stub_stop_mode());
        }
        vt_override(list, s, stubs[s]);
    }
    stubbed = true;
#define OV(slot, fn) vt_override(list, slot, reinterpret_cast<void*>(&fn))
    OV(30, SetRootTable<30>);
    OV(31, SetRootTable<31>);
    OV(36, SetRootView<36>);
    OV(37, SetRootView<37>);
    OV(38, SetRootView<38>);
    OV(39, SetRootView<39>);
    OV(40, SetRootView<40>);
    OV(41, SetRootView<41>);
    OV(42, IASetIndexBuffer);
    OV(43, IASetVertexBuffers);
    OV(45, OMSetRenderTargets);
    OV(46, ClearDSV);
    OV(47, ClearRTV);
    OV(48, (ClearUAV<48, UINT>));
    OV(49, (ClearUAV<49, FLOAT>));
#undef OV
}
