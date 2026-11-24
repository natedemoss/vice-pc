// xmem.dll — Xbox GDK memory allocator.
//
// Implemented from the public GDK documentation signatures:
//   PVOID XMemAlloc(SIZE_T dwSize, UINT64 dwAttributes);
//   VOID  XMemFree(PVOID lpAddress, UINT64 dwAttributes);
//   XMemAllocDefault / XMemFreeDefault have the same signatures.
//
// The attribute word encodes alignment, memory type and allocator id. For now we
// ignore it and use a generous alignment. Physical-page APIs (XMemAllocatePhysicalPages,
// XMemMapPhysicalPages, ...) remain stubs until we see how the game uses them.

#include "hle.h"
#include "../log.h"

#include <windows.h>
#include <malloc.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {

std::atomic<uint64_t> g_live_allocs{0};
std::atomic<int> g_alloc_logged{0};
std::atomic<int> g_free_logged{0};

// Track our own blocks so a free of a pointer we didn't hand out is caught and logged
// instead of corrupting the heap.
std::mutex g_mu;
std::unordered_map<void*, size_t>& g_blocks = *new std::unordered_map<void*, size_t>();

size_t pick_alignment(size_t size) {
    return size >= 4096 ? 4096 : 64;
}

void* __cdecl XMemAlloc_hle(SIZE_T size, uint64_t attributes) {
    void* p = _aligned_malloc(size ? size : 1, pick_alignment(size));
    if (p) {
        memset(p, 0, size);
        ++g_live_allocs;
        std::lock_guard<std::mutex> lk(g_mu);
        g_blocks[p] = size;
    }
    if (g_alloc_logged.fetch_add(1) < 40) {
        LOG("[xmem] XMemAlloc(size=0x%zx, attr=0x%016llx) -> %p", size, (unsigned long long)attributes, p);
    }
    return p;
}

void __cdecl XMemFree_hle(void* p, uint64_t attributes) {
    if (!p) return;
    bool ours;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ours = g_blocks.erase(p) != 0;
    }
    if (g_free_logged.fetch_add(1) < 40 || !ours) {
        LOG("[xmem] XMemFree(%p, attr=0x%016llx)%s", p, (unsigned long long)attributes,
            ours ? "" : " NOT FROM XMemAlloc - ignored");
    }
    if (!ours) return;
    _aligned_free(p);
    --g_live_allocs;
}

// XMemVirtualAlloc(address, size, allocationType, protect, extra)
// Observed in the reference title right after D3D12ConfigureVirtualMemoryX:
// multi-GB MEM_RESERVE calls with Xbox-only flag bits (0x20400000) and Xbox page
// protections (e.g. 0x21). We keep the standard Windows bits and pick a sane protection.
constexpr DWORD kStdTypeBits = MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN;

DWORD translate_protect(DWORD xbox_protect, DWORD type) {
    if (!(type & MEM_COMMIT)) return PAGE_NOACCESS;  // reservation only
    if (xbox_protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
        return PAGE_EXECUTE_READWRITE;
    return PAGE_READWRITE;
}

void* __cdecl XMemVirtualAlloc_hle(void* address, SIZE_T size, DWORD type, DWORD protect, DWORD extra) {
    static std::atomic<int> logged{0};
    DWORD win_type = type & kStdTypeBits;
    if (!(win_type & (MEM_COMMIT | MEM_RESERVE | MEM_RESET))) win_type |= MEM_RESERVE;
    DWORD win_protect = translate_protect(protect, win_type);
    void* p = VirtualAlloc(address, size, win_type, win_protect);
    if (logged.fetch_add(1) < 64) {
        LOG("[xmem] XMemVirtualAlloc(addr=%p, size=0x%llx, type=0x%08lx, prot=0x%lx, extra=0x%lx) -> %p%s", address,
            (unsigned long long)size, type, protect, extra, p, p ? "" : " FAILED");
    }
    if (!p) LOG("[xmem]   VirtualAlloc error %lu", GetLastError());
    return p;
}

const HleExport kExports[] = {
    {"xmem.dll", "XMemVirtualAlloc", reinterpret_cast<void*>(&XMemVirtualAlloc_hle)},
    {"xmem.dll", "XMemAlloc", reinterpret_cast<void*>(&XMemAlloc_hle)},
    {"xmem.dll", "XMemAllocDefault", reinterpret_cast<void*>(&XMemAlloc_hle)},
    {"xmem.dll", "XMemFree", reinterpret_cast<void*>(&XMemFree_hle)},
    {"xmem.dll", "XMemFreeDefault", reinterpret_cast<void*>(&XMemFree_hle)},
};

HleRegistrar g_reg(kExports);

}  // namespace
