#include "tls.h"

#include "log.h"
#include "stubs.h"

#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace {

// Offset of ThreadLocalStoragePointer in the x64 TEB.
constexpr size_t kTebTlsPointer = 0x58;
// Headroom so DLLs the OS loads later (which get the next free index) don't collide with us.
constexpr DWORD kSlotHeadroom = 64;

std::atomic<bool> g_ready{false};
DWORD g_slot = 0;
const uint8_t* g_template = nullptr;
size_t g_template_size = 0;
size_t g_zero_fill = 0;
void* g_image_base = nullptr;
std::vector<PIMAGE_TLS_CALLBACK> g_callbacks;

// Per-thread bookkeeping (the host's own implicit TLS, set up by the OS loader).
// Windows 10+ keeps a hidden header in front of the TLS array it allocates and
// frees the array through that header at thread exit, so we must hand the
// original array back before ntdll tears the thread down.
thread_local void** t_original_array = nullptr;
thread_local void** t_our_array = nullptr;
thread_local void* t_our_block = nullptr;

void** teb_tls_array() {
    return *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(NtCurrentTeb()) + kTebTlsPointer);
}

void set_teb_tls_array(void** arr) {
    *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(NtCurrentTeb()) + kTebTlsPointer) = arr;
}

// Highest implicit-TLS index the OS loader has assigned to any module in the process.
bool highest_os_tls_index(DWORD& out) {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return false;
    size_t n = std::min<size_t>(needed / sizeof(HMODULE), 1024);
    bool any = false;
    out = 0;
    for (size_t i = 0; i < n; ++i) {
        auto base = reinterpret_cast<uint8_t*>(mods[i]);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        auto d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (!d.Size) continue;
        auto tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(base + d.VirtualAddress);
        if (!tls->AddressOfIndex) continue;
        DWORD idx = *reinterpret_cast<DWORD*>(tls->AddressOfIndex);
        if (!any || idx > out) out = idx;
        any = true;
    }
    return true;
}

bool install_for_current_thread() {
    void** old = teb_tls_array();
    size_t count = g_slot + 1;
    auto arr = static_cast<void**>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(void*)));
    void* block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, g_template_size + g_zero_fill + 16);
    if (!arr || !block) return false;
    if (old) {
        // Every slot below g_slot - kSlotHeadroom was handed out by the OS loader, so the
        // old array is at least that long. Copy only those entries.
        memcpy(arr, old, (g_slot - kSlotHeadroom) * sizeof(void*));
    }
    memcpy(block, g_template, g_template_size);
    arr[g_slot] = block;
    t_original_array = old;
    t_our_array = arr;
    t_our_block = block;
    set_teb_tls_array(arr);
    return true;
}

void restore_current_thread() {
    if (!t_our_array) return;
    if (teb_tls_array() == t_our_array) set_teb_tls_array(t_original_array);
    HeapFree(GetProcessHeap(), 0, t_our_array);
    HeapFree(GetProcessHeap(), 0, t_our_block);
    t_our_array = nullptr;
    t_our_block = nullptr;
}

void run_callbacks(DWORD reason) {
    for (auto cb : g_callbacks) cb(g_image_base, reason, nullptr);
}

// Host TLS callback: runs on every new thread before its start routine.
void NTAPI host_tls_callback(PVOID, DWORD reason, PVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        stub_print_summary();
        log_flush();
        if (g_ready.load()) restore_current_thread();
        return;
    }
    if (!g_ready.load()) return;
    if (reason == DLL_THREAD_ATTACH) {
        if (!install_for_current_thread()) LOG("[tls] failed to set up TLS for thread %lu", GetCurrentThreadId());
        run_callbacks(DLL_THREAD_ATTACH);
    } else if (reason == DLL_THREAD_DETACH) {
        run_callbacks(DLL_THREAD_DETACH);
        restore_current_thread();
    }
}

}  // namespace

// Force the host exe to have a TLS directory and register our callback in it.
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:scarlett_tls_callback")
#pragma const_seg(".CRT$XLB")
extern "C" const PIMAGE_TLS_CALLBACK scarlett_tls_callback = host_tls_callback;
#pragma const_seg()

bool tls_setup(LoadedImage& img) {
    auto d = img.dir(IMAGE_DIRECTORY_ENTRY_TLS);
    if (!d.Size) {
        LOG("[tls] image has no TLS directory");
        return true;
    }
    auto tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(img.base + d.VirtualAddress);
    // These are VAs and were already fixed up by relocation.
    g_template = reinterpret_cast<const uint8_t*>(tls->StartAddressOfRawData);
    g_template_size = (size_t)(tls->EndAddressOfRawData - tls->StartAddressOfRawData);
    g_zero_fill = tls->SizeOfZeroFill;
    g_image_base = img.base;

    DWORD highest = 0;
    if (!highest_os_tls_index(highest)) {
        LOG("[tls] EnumProcessModules failed");
        return false;
    }
    g_slot = highest + 1 + kSlotHeadroom;
    if (tls->AddressOfIndex) *reinterpret_cast<DWORD*>(tls->AddressOfIndex) = g_slot;

    if (tls->AddressOfCallBacks) {
        for (auto cb = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks); *cb; ++cb) {
            g_callbacks.push_back(*cb);
        }
    }

    if (!install_for_current_thread()) return false;
    g_ready.store(true);
    LOG("[tls] slot %lu, template %zu bytes + %zu zero-fill, %zu callbacks", g_slot, g_template_size, g_zero_fill,
        g_callbacks.size());
    return true;
}

void tls_run_process_attach() {
    run_callbacks(DLL_PROCESS_ATTACH);
}
