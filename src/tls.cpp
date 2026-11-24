#include "tls.h"

#include "log.h"
#include "stubs.h"

#include <windows.h>
#include <atomic>
#include <cstring>
#include <vector>

// Implicit TLS for the mapped game image.
//
// Console titles are built assuming the main executable owns implicit-TLS slot 0,
// and read their thread-local data straight from ThreadLocalStoragePointer[0]
// without consulting _tls_index. Under the loader, slot 0 belongs to vice-pc.exe.
// So for every thread we swap the game's TLS block into slot 0 of the array the OS
// allocated, and swap the original back before the OS tears the thread down.
//
// Consequence: the host must never use implicit TLS itself (no thread_local,
// no thread-safe magic statics; see /Zc:threadSafeInit- in CMakeLists.txt).
// Host per-thread state lives in a dynamic TlsAlloc slot instead.

namespace {

constexpr size_t kTebTlsPointer = 0x58;  // TEB.ThreadLocalStoragePointer (x64)
constexpr DWORD kGameSlot = 0;

std::atomic<bool> g_ready{false};
const uint8_t* g_template = nullptr;
size_t g_template_size = 0;
size_t g_zero_fill = 0;
void* g_image_base = nullptr;
std::vector<PIMAGE_TLS_CALLBACK>& g_callbacks = *new std::vector<PIMAGE_TLS_CALLBACK>();

DWORD g_saved_slot = TLS_OUT_OF_INDEXES;  // dynamic TLS: the host's original slot-0 block

void** teb_tls_array() {
    return *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(NtCurrentTeb()) + kTebTlsPointer);
}

bool install_for_current_thread() {
    void** arr = teb_tls_array();
    if (!arr) return false;  // the host exe has a TLS directory, so the OS always allocates one
    void* block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, g_template_size + g_zero_fill + 16);
    if (!block) return false;
    memcpy(block, g_template, g_template_size);
    TlsSetValue(g_saved_slot, arr[kGameSlot]);
    arr[kGameSlot] = block;
    return true;
}

void restore_current_thread() {
    void** arr = teb_tls_array();
    void* original = TlsGetValue(g_saved_slot);
    if (!arr || !original) return;
    void* ours = arr[kGameSlot];
    arr[kGameSlot] = original;
    TlsSetValue(g_saved_slot, nullptr);
    HeapFree(GetProcessHeap(), 0, ours);
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

    g_saved_slot = TlsAlloc();
    if (g_saved_slot == TLS_OUT_OF_INDEXES) {
        LOG("[tls] TlsAlloc failed");
        return false;
    }
    if (tls->AddressOfIndex) *reinterpret_cast<DWORD*>(tls->AddressOfIndex) = kGameSlot;

    if (tls->AddressOfCallBacks) {
        for (auto cb = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks); *cb; ++cb) {
            g_callbacks.push_back(*cb);
        }
    }

    if (!install_for_current_thread()) return false;
    g_ready.store(true);
    LOG("[tls] game block in slot %lu, template %zu bytes + %zu zero-fill, %zu callbacks", kGameSlot,
        g_template_size, g_zero_fill, g_callbacks.size());
    return true;
}

void tls_run_process_attach() {
    run_callbacks(DLL_PROCESS_ATTACH);
}
