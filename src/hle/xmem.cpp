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
// Placeholder flags matter: the title reserves with MEM_RESERVE_PLACEHOLDER and later
// splits the range with VirtualFree(MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER).
constexpr DWORD kReservePlaceholder = 0x40000;  // MEM_RESERVE_PLACEHOLDER
constexpr DWORD kReplacePlaceholder = 0x4000;   // MEM_REPLACE_PLACEHOLDER
constexpr DWORD kStdTypeBits =
    MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | kReservePlaceholder | kReplacePlaceholder;

using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
VirtualAlloc2Fn virtual_alloc2() {
    static VirtualAlloc2Fn fn = reinterpret_cast<VirtualAlloc2Fn>(
        GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2"));
    return fn;
}

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
    if (win_type & kReservePlaceholder) win_protect = PAGE_NOACCESS;  // required for placeholders
    void* p = (win_type & (kReservePlaceholder | kReplacePlaceholder)) && virtual_alloc2()
                  ? virtual_alloc2()(nullptr, address, size, win_type, win_protect, nullptr, 0)
                  : VirtualAlloc(address, size, win_type, win_protect);
    if (logged.fetch_add(1) < 64) {
        LOG("[xmem] XMemVirtualAlloc(addr=%p, size=0x%llx, type=0x%08lx, prot=0x%lx, extra=0x%lx) -> %p%s", address,
            (unsigned long long)size, type, protect, extra, p, p ? "" : " FAILED");
    }
    if (!p) LOG("[xmem]   VirtualAlloc error %lu", GetLastError());
    return p;
}

// ---- Physical pages -----------------------------------------------------------
//
// Xbox titles allocate 64 KB physical pages and map them into their own reserved
// ranges (like AllocateUserPhysicalPages / MapUserPhysicalPages). Observed call:
//   XMemAllocatePhysicalPages(0x280, &pageCount, pageArray) == TRUE, count unchanged.
// Emulation: page "frame numbers" are just unique ids; mapping commits fresh memory
// at the target address, unmapping decommits it. Contents are not shared between
// two mappings of the same page (aliasing) — revisit if the title relies on it.
constexpr uint64_t kPhysPageSize = 64 * 1024;
std::atomic<uint64_t> g_next_pfn{0x100000};
std::atomic<uint64_t> g_phys_pages_live{0};
std::atomic<int> g_phys_logged{0};

BOOL __cdecl XMemAllocatePhysicalPages_hle(uint64_t flags, uint64_t* page_count, uint64_t* page_array) {
    if (!page_count || !page_array) return FALSE;
    uint64_t n = *page_count;
    uint64_t first = g_next_pfn.fetch_add(n);
    for (uint64_t i = 0; i < n; ++i) page_array[i] = first + i;
    g_phys_pages_live += n;
    if (g_phys_logged.fetch_add(1) < 32) {
        LOG("[xmem] XMemAllocatePhysicalPages(flags=0x%llx, %llu pages = %llu MB) -> pfn 0x%llx..",
            (unsigned long long)flags, (unsigned long long)n, (unsigned long long)(n * kPhysPageSize >> 20),
            (unsigned long long)first);
    }
    return TRUE;
}

BOOL __cdecl XMemFreePhysicalPages_hle(uint64_t flags, uint64_t* page_count, uint64_t* page_array) {
    (void)flags;
    (void)page_array;
    if (page_count) g_phys_pages_live -= *page_count;
    return TRUE;
}

BOOL map_range(void* va, uint64_t pages, const uint64_t* page_array, const char* api) {
    size_t bytes = (size_t)(pages * kPhysPageSize);
    void* p;
    if (page_array) {
        p = VirtualAlloc(va, bytes, MEM_COMMIT, PAGE_READWRITE);
        if (!p) p = VirtualAlloc(va, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);  // not inside a reservation
    } else {
        p = VirtualFree(va, bytes, MEM_DECOMMIT) ? va : nullptr;  // NULL page array = unmap
    }
    if (g_phys_logged.fetch_add(1) < 32 || !p) {
        LOG("[xmem] %s(va=%p, %llu pages, %s) -> %s%s", api, va, (unsigned long long)pages,
            page_array ? "map" : "unmap", p ? "ok" : "FAILED", p ? "" : "");
    }
    if (!p) LOG("[xmem]   error %lu", GetLastError());
    return p ? TRUE : FALSE;
}

BOOL __cdecl XMemMapPhysicalPages_hle(void* va, uint64_t pages, const uint64_t* page_array) {
    return map_range(va, pages, page_array, "XMemMapPhysicalPages");
}

// BOOL XMemMapPhysicalPagesScatter(const uint64_t* vas, uint32_t count, const uint64_t* pfns)
// Reference title (game+0xf2cc3e): two parallel arrays, one 64 KB page each; pfn 0 = unmap.
std::atomic<int> g_scatter_logged{0};
BOOL __cdecl XMemMapPhysicalPagesScatter_hle(const uint64_t* vas, uint64_t count, const uint64_t* pfns) {
    uint32_t n = static_cast<uint32_t>(count);
    BOOL ok = TRUE;
    uint32_t mapped = 0;
    for (uint32_t i = 0; i < n; ++i) {
        void* va = reinterpret_cast<void*>(vas[i]);
        void* p;
        if (pfns[i]) {
            p = VirtualAlloc(va, kPhysPageSize, MEM_COMMIT, PAGE_READWRITE);
            if (!p) p = VirtualAlloc(va, kPhysPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p) ++mapped;
        } else {
            p = VirtualFree(va, kPhysPageSize, MEM_DECOMMIT) ? va : nullptr;
        }
        if (!p) {
            LOG("[xmem] XMemMapPhysicalPagesScatter: page %u at %p failed (error %lu)", i, va, GetLastError());
            ok = FALSE;
        }
    }
    if (g_scatter_logged.fetch_add(1) < 16) {
        uint64_t lo = ~0ull, hi = 0;
        for (uint32_t i = 0; i < n; ++i) { lo = vas[i] < lo ? vas[i] : lo; hi = vas[i] > hi ? vas[i] : hi; }
        LOG("[xmem] XMemMapPhysicalPagesScatter(%u pages at 0x%llx..0x%llx, %u mapped) -> %s", n,
            (unsigned long long)lo, (unsigned long long)hi, mapped, ok ? "ok" : "FAILED");
    }
    return ok;
}

const HleExport kExports[] = {
    {"xmem.dll", "XMemMapPhysicalPagesScatter", reinterpret_cast<void*>(&XMemMapPhysicalPagesScatter_hle)},
    {"xmem.dll", "XMemAllocatePhysicalPages", reinterpret_cast<void*>(&XMemAllocatePhysicalPages_hle)},
    {"xmem.dll", "XMemFreePhysicalPages", reinterpret_cast<void*>(&XMemFreePhysicalPages_hle)},
    {"xmem.dll", "XMemMapPhysicalPages", reinterpret_cast<void*>(&XMemMapPhysicalPages_hle)},
    {"xmem.dll", "XMemVirtualAlloc", reinterpret_cast<void*>(&XMemVirtualAlloc_hle)},
    {"xmem.dll", "XMemAlloc", reinterpret_cast<void*>(&XMemAlloc_hle)},
    {"xmem.dll", "XMemAllocDefault", reinterpret_cast<void*>(&XMemAlloc_hle)},
    {"xmem.dll", "XMemFree", reinterpret_cast<void*>(&XMemFree_hle)},
    {"xmem.dll", "XMemFreeDefault", reinterpret_cast<void*>(&XMemFree_hle)},
};

HleRegistrar g_reg(kExports);

}  // namespace
