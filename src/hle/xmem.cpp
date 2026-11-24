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

namespace {

std::atomic<uint64_t> g_live_allocs{0};

size_t pick_alignment(size_t size) {
    return size >= 4096 ? 4096 : 64;
}

void* __cdecl XMemAlloc_hle(SIZE_T size, uint64_t attributes) {
    (void)attributes;
    void* p = _aligned_malloc(size ? size : 1, pick_alignment(size));
    if (p) {
        memset(p, 0, size);
        ++g_live_allocs;
    }
    return p;
}

void __cdecl XMemFree_hle(void* p, uint64_t attributes) {
    (void)attributes;
    if (!p) return;
    _aligned_free(p);
    --g_live_allocs;
}

const HleExport kExports[] = {
    {"xmem.dll", "XMemAlloc", reinterpret_cast<void*>(&XMemAlloc_hle)},
    {"xmem.dll", "XMemAllocDefault", reinterpret_cast<void*>(&XMemAlloc_hle)},
    {"xmem.dll", "XMemFree", reinterpret_cast<void*>(&XMemFree_hle)},
    {"xmem.dll", "XMemFreeDefault", reinterpret_cast<void*>(&XMemFree_hle)},
};

HleRegistrar g_reg(kExports);

}  // namespace
