#include "stubs.h"

#include "log.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>

namespace {

struct StubInfo {
    std::string dll;
    std::string name;
    bool stop = false;
    std::atomic<uint64_t> calls{0};
};

// deque: stable addresses, since thunks embed a pointer to their StubInfo.
// Intentionally leaked so it outlives static destructors (the summary prints at process detach).
std::deque<StubInfo>& g_stubs = *new std::deque<StubInfo>();

const uint8_t* g_img_base = nullptr;
size_t g_img_size = 0;

constexpr size_t kThunkSize = 32;
constexpr size_t kPageThunks = 4096 / kThunkSize;
uint8_t* g_page = nullptr;
size_t g_page_used = kPageThunks;

// Called (via thunk) with rcx = StubInfo*. The game's original first argument is lost,
// which is fine because we never forward the call. Return value lands in rax = 0.
extern "C" uint64_t stub_handler(StubInfo* s) {
    uint64_t n = s->calls.fetch_add(1) + 1;
    if (n == 1 || s->stop) {
        auto ret = reinterpret_cast<const uint8_t*>(_ReturnAddress());
        if (g_img_base && ret >= g_img_base && ret < g_img_base + g_img_size) {
            LOG("[stub] %s!%s called from game+0x%llx (returning 0)", s->dll.c_str(), s->name.c_str(),
                (unsigned long long)(ret - g_img_base));
        } else {
            LOG("[stub] %s!%s called from 0x%p (returning 0)", s->dll.c_str(), s->name.c_str(), ret);
        }
    }
    if (s->stop) {
        LOG("[stub] --stop-on-stub set: stopping at first unimplemented call");
        stub_print_summary();
        log_flush();
        TerminateProcess(GetCurrentProcess(), 3);
    }
    return 0;
}

uint8_t* alloc_thunk() {
    if (g_page_used == kPageThunks) {
        // RWX: stubs are also created while the title runs (D3D12 vtable patching), so we
        // can't flip protections on a page other threads may be executing from.
        g_page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (!g_page) return nullptr;
        memset(g_page, 0xCC, 4096);
        g_page_used = 0;
    }
    return g_page + kThunkSize * g_page_used++;
}

}  // namespace

static bool g_stop_mode = false;
void stub_set_stop_mode(bool stop) { g_stop_mode = stop; }
bool stub_stop_mode() { return g_stop_mode; }

void stub_set_image_range(const void* base, size_t size) {
    g_img_base = static_cast<const uint8_t*>(base);
    g_img_size = size;
}

void* stub_create(const char* dll, const char* name, bool stop_on_stub) {
    StubInfo& info = g_stubs.emplace_back();
    info.dll = dll;
    info.name = name;
    info.stop = stop_on_stub;

    uint8_t* t = alloc_thunk();
    if (!t) return nullptr;

    uint64_t info_ptr = reinterpret_cast<uint64_t>(&info);
    uint64_t handler = reinterpret_cast<uint64_t>(&stub_handler);
    uint8_t* p = t;
    *p++ = 0x48; *p++ = 0xB9; memcpy(p, &info_ptr, 8); p += 8;  // mov rcx, imm64
    *p++ = 0x48; *p++ = 0xB8; memcpy(p, &handler, 8); p += 8;   // mov rax, imm64
    *p++ = 0xFF; *p++ = 0xE0;                                   // jmp rax

    FlushInstructionCache(GetCurrentProcess(), t, kThunkSize);
    return t;
}

void stub_print_summary() {
    LOG("---- stub call summary ----");
    size_t called = 0;
    for (auto& s : g_stubs) {
        uint64_t c = s.calls.load();
        if (c) {
            LOG("  %10llu  %s!%s", (unsigned long long)c, s.dll.c_str(), s.name.c_str());
            ++called;
        }
    }
    LOG("  %zu of %zu stubs were called", called, g_stubs.size());
}
