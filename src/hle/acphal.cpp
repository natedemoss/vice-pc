// AcpHal.dll — Xbox audio co-processor (ACP) HAL and APU memory.
//
// The APU heap is shared memory the audio hardware reads; here it is one host
// allocation, and APU "addresses" are offsets into it. Shape contexts (per-voice
// hardware state: SRC, filters, volume, ...) become zeroed host arrays. No audio is
// produced yet; this lets the title's audio engine initialise.
//
// Shapes follow the GDK's public apu.h:
//   HRESULT ApuCreateHeap(UINT32 cachedSizeBytes, UINT32 nonCachedSizeBytes)
//   HRESULT ApuAlloc(void** va, APU_ADDRESS* pa, UINT32 size, UINT32 align, UINT32 flags)
//   HRESULT ApuFree(void* va)
//   BOOL    ApuIsVirtualAddressValid(const void* va, UINT32 physicalAlignment)
//   void*   ApuMapApuAddress(APU_ADDRESS pa)
//   APU_ADDRESS ApuMapVirtualAddress(const void* va)

#include "hle.h"
#include "probe.h"
#include "../log.h"
#include "../stubs.h"

#include <cstdio>
#include <cstring>

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace {

std::mutex g_mu;
uint8_t* g_heap = nullptr;
size_t g_heap_size = 0;
size_t g_heap_used = 0;

HRESULT __cdecl ApuCreateHeap_hle(uint32_t cached, uint32_t noncached) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_heap) return S_OK;
    g_heap_size = size_t(cached) + noncached;
    if (g_heap_size == 0) g_heap_size = 64 << 20;
    g_heap = static_cast<uint8_t*>(VirtualAlloc(nullptr, g_heap_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    LOG("[apu] ApuCreateHeap(cached=%u, noncached=%u) -> %p", cached, noncached, g_heap);
    return g_heap ? S_OK : E_OUTOFMEMORY;
}

HRESULT __cdecl ApuAlloc_hle(void** va, uint32_t* pa, uint32_t size, uint32_t align, uint32_t flags) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_heap) return E_FAIL;
    if (align < 64) align = 64;
    size_t off = (g_heap_used + align - 1) & ~size_t(align - 1);
    if (off + size > g_heap_size) {
        LOG("[apu] ApuAlloc(%u) out of APU heap", size);
        return E_OUTOFMEMORY;
    }
    g_heap_used = off + size;
    if (va) *va = g_heap + off;
    if (pa) *pa = static_cast<uint32_t>(off);
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 16) LOG("[apu] ApuAlloc(%u, align %u, flags 0x%x) -> +0x%zx", size, align, flags, off);
    return S_OK;
}

HRESULT __cdecl ApuFree_hle(void*) { return S_OK; }  // bump allocator: freed memory is not reused

BOOL __cdecl ApuIsVirtualAddressValid_hle(const void* va, uint32_t) {
    auto* p = static_cast<const uint8_t*>(va);
    return g_heap && p >= g_heap && p < g_heap + g_heap_size;
}

void* __cdecl ApuMapApuAddress_hle(uint32_t pa) { return g_heap ? g_heap + pa : nullptr; }

uint32_t __cdecl ApuMapVirtualAddress_hle(const void* va) {
    return g_heap ? static_cast<uint32_t>(static_cast<const uint8_t*>(va) - g_heap) : 0;
}

// HRESULT AcpHalAllocateShapeContexts(SHAPE_CONTEXTS* contexts)
// Reference title (game+0x43aec73): struct at 0x149ae98c8 with a context count of 350 at
// +0x18 and receives an array pointer at +0x40 (350 x 64 bytes, memset by the title).
// The exact field list isn't known, so every pointer field from +0x20 to +0x78 gets a
// zeroed array large enough for any per-voice context type.
HRESULT __cdecl AcpHalAllocateShapeContexts_hle(uint8_t* ctx) {
    if (!ctx) return E_POINTER;
    constexpr size_t kArray = 1 << 20;
    for (size_t off = 0x20; off <= 0x78; off += 8) {
        void* p = VirtualAlloc(nullptr, kArray, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        *reinterpret_cast<void**>(ctx + off) = p;
    }
    LOG("[apu] AcpHalAllocateShapeContexts(count@+0x18=%u) -> arrays at +0x20..+0x78",
        *reinterpret_cast<uint32_t*>(ctx + 0x18));
    return S_OK;
}

HRESULT __cdecl AcpHalReleaseShapeContexts_hle() { return S_OK; }

// HRESULT AcpHalCreate(IAcpHal** hal). IAcpHal is not a COM interface: the title calls
// slot 0 as Initialize(0x1000, 0x1000) and slot 2 with five arguments (game+0x43aedba,
// game+0x43aee2f). Every slot is a logged no-op returning S_OK.
HRESULT __cdecl AcpHalCreate_hle(void** out) {
    if (!out) return E_POINTER;
    static void** vt = [] {
        constexpr int kSlots = 48;
        void** t = new void*[kSlots];
        for (int i = 0; i < kSlots; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "slot%d", i);
            t[i] = stub_create("IAcpHal", _strdup(name), false);
        }
        return t;
    }();
    auto* obj = new void*[64]();
    obj[0] = vt;
    *out = obj;
    LOG("[apu] AcpHalCreate -> no-op HAL object");
    return S_OK;
}

const HleExport kExports[] = {
    {"acphal.dll", "ApuCreateHeap", reinterpret_cast<void*>(&ApuCreateHeap_hle)},
    {"acphal.dll", "ApuAlloc", reinterpret_cast<void*>(&ApuAlloc_hle)},
    {"acphal.dll", "ApuFree", reinterpret_cast<void*>(&ApuFree_hle)},
    {"acphal.dll", "ApuIsVirtualAddressValid", reinterpret_cast<void*>(&ApuIsVirtualAddressValid_hle)},
    {"acphal.dll", "ApuMapApuAddress", reinterpret_cast<void*>(&ApuMapApuAddress_hle)},
    {"acphal.dll", "ApuMapVirtualAddress", reinterpret_cast<void*>(&ApuMapVirtualAddress_hle)},
    {"acphal.dll", "AcpHalAllocateShapeContexts", reinterpret_cast<void*>(&AcpHalAllocateShapeContexts_hle)},
    {"acphal.dll", "AcpHalReleaseShapeContexts", reinterpret_cast<void*>(&AcpHalReleaseShapeContexts_hle)},
    {"acphal.dll", "AcpHalCreate", reinterpret_cast<void*>(&AcpHalCreate_hle)},
};

HleRegistrar g_reg(kExports);

}  // namespace
