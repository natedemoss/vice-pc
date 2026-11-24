// Command list methods that take title addresses (GPU VAs, descriptor handles):
// translate them to real PC addresses before forwarding (see gpumem.h).

#include "cmdlist.h"
#include "gpumem.h"
#include "vtpatch.h"

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

void cmdlist_install(void* list) {
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
