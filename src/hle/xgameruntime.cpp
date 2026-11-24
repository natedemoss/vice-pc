// xgameruntime.dll stand-in (Xbox game runtime: XUser, XTaskQueue, XGameSave, ...).
//
// The title's GDK static library loads "xgameruntime.dll" itself and resolves five
// exports by name (strings next to each other in the title image):
//   InitializeApiImpl / InitializeApiImplEx / InitializeApiImplEx2 / UninitializeApiImpl
//   QueryApiImpl(providerGuid, interfaceGuid, out)
// QueryApiImpl hands back internal per-feature interfaces whose layouts aren't public,
// so each one is a probe object (every method call logged) until it is decoded.
// Without this module the title retried the load thousands of times per minute.

#include "hle.h"
#include "probe.h"
#include "../log.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>

namespace {

alignas(64) uint8_t g_fake_module[64];  // address used as the pseudo HMODULE

std::string guid_str(const GUID* g) {
    char b[64] = "(null)";
    if (g)
        snprintf(b, sizeof(b), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g->Data1, g->Data2, g->Data3,
                 g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6],
                 g->Data4[7]);
    return b;
}

HRESULT __cdecl InitializeApiImpl_hle(uint64_t a, uint64_t b) {
    LOG("[xgr] InitializeApiImpl(0x%llx, 0x%llx) -> S_OK", (unsigned long long)a, (unsigned long long)b);
    return S_OK;
}
HRESULT __cdecl InitializeApiImplEx_hle(uint64_t a, uint64_t b, uint64_t c) {
    LOG("[xgr] InitializeApiImplEx(0x%llx, 0x%llx, 0x%llx) -> S_OK", (unsigned long long)a, (unsigned long long)b,
        (unsigned long long)c);
    return S_OK;
}
HRESULT __cdecl InitializeApiImplEx2_hle(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    LOG("[xgr] InitializeApiImplEx2(0x%llx, 0x%llx, 0x%llx, 0x%llx) -> S_OK", (unsigned long long)a,
        (unsigned long long)b, (unsigned long long)c, (unsigned long long)d);
    return S_OK;
}
HRESULT __cdecl UninitializeApiImpl_hle() { return S_OK; }

// void XErrorReport(HRESULT hr, const char* message): the title reports failures here.
void __cdecl XErrorReport_hle(HRESULT hr, const char* message) {
    static std::atomic<int> n{0};
    int k = n.fetch_add(1);
    if (k < 200) {
        char msg[512] = "";
        if (message) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(message, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) strncpy_s(msg, message, _TRUNCATE);
        }
        uint64_t rva = 0;
        bool g = hle_rva(_ReturnAddress(), &rva);
        LOG("[XErrorReport] hr=0x%08lx \"%s\" (from %s0x%llx)", hr, msg, g ? "game+" : "", (unsigned long long)rva);
    } else if (k == 200) {
        LOG("[XErrorReport] further reports not logged");
    }
}

std::mutex g_mu;
std::map<std::string, void*>& g_objects = *new std::map<std::string, void*>();

// ---- networking (interface {BF2346B2-...}) --------------------------------------------
// Layout matches the GDK's XNetworkingConnectivityHint: level, cost, interface type,
// then networkInitialized at +0xC (the title's callback checks exactly that byte).
struct ConnectivityHint {
    uint32_t level;          // 1 = none (offline)
    uint32_t cost;           // 0 = unknown
    uint32_t iana_type;
    uint8_t initialized;     // +0xC
    uint8_t approaching_limit, over_limit, roaming;
};

void fill_hint(ConnectivityHint* h) {
    memset(h, 0, sizeof(*h));
    h->level = 1;  // XNetworkingConnectivityLevelHint::None: initialised, no network (offline play)
    h->initialized = 1;
}

// slot 15: XNetworkingGetConnectivityHint(hint*)  (game+0x55e0e78)
HRESULT STDMETHODCALLTYPE Net_GetConnectivityHint(ProbeObject*, ConnectivityHint* hint) {
    if (!hint) return E_POINTER;
    fill_hint(hint);
    LOG("[xgr] XNetworkingGetConnectivityHint -> initialised, offline");
    return S_OK;
}

using HintCallback = void(__cdecl*)(void* context, const ConnectivityHint* hint);

// slot 16: XNetworkingRegisterConnectivityHintChanged(queue, context, callback, token*)
// (game+0x55e0f0c). The title waits on an event its callback sets once the network is
// initialised, so the callback runs right away with an "initialised, offline" hint.
HRESULT STDMETHODCALLTYPE Net_RegisterConnectivityHintChanged(ProbeObject*, void* queue, void* context,
                                                              HintCallback cb, uint64_t* token) {
    if (token) *token = 1;
    ConnectivityHint h;
    fill_hint(&h);
    LOG("[xgr] XNetworkingRegisterConnectivityHintChanged(queue %p, context %p, callback %p) -> notified (offline)",
        queue, context, cb);
    if (cb) cb(context, &h);
    return S_OK;
}

void install_known_overrides(ProbeType* t, const GUID* iface) {
    if (iface && iface->Data1 == 0xBF2346B2) {
        probe_override(t, 15, reinterpret_cast<void*>(&Net_GetConnectivityHint), "XNetworkingGetConnectivityHint");
        probe_override(t, 16, reinterpret_cast<void*>(&Net_RegisterConnectivityHintChanged),
                       "XNetworkingRegisterConnectivityHintChanged");
    }
}

HRESULT __cdecl QueryApiImpl_hle(const GUID* provider, const GUID* iface, void** out) {
    if (!out) return E_POINTER;
    std::string key = guid_str(provider) + "/" + guid_str(iface);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_objects.find(key);
    if (it == g_objects.end()) {
        std::string name = "XGR" + guid_str(iface).substr(1, 8);
        ProbeType* type = probe_type(_strdup(name.c_str()));
        install_known_overrides(type, iface);
        void* obj = probe_create(type);
        it = g_objects.emplace(key, obj).first;
        LOG("[xgr] QueryApiImpl(provider %s, interface %s) -> probe %s", guid_str(provider).c_str(),
            guid_str(iface).c_str(), name.c_str());
    }
    *out = it->second;
    return S_OK;
}

}  // namespace

HMODULE xgr_fake_module(const wchar_t* name) {
    if (!name) return nullptr;
    const wchar_t* base = wcsrchr(name, L'\\');
    base = base ? base + 1 : name;
    if (_wcsicmp(base, L"xgameruntime.dll") == 0 || _wcsicmp(base, L"xgameruntime") == 0) {
        static bool logged = false;
        if (!logged) { logged = true; LOG("[xgr] xgameruntime.dll -> built-in stand-in"); }
        return reinterpret_cast<HMODULE>(g_fake_module);
    }
    return nullptr;
}

bool xgr_is_fake(HMODULE m) { return m == reinterpret_cast<HMODULE>(g_fake_module); }

FARPROC xgr_proc(const char* name) {
    if (!name || reinterpret_cast<uintptr_t>(name) < 0x10000) return nullptr;  // ordinals unsupported
    struct E { const char* n; void* f; };
    static const E kProcs[] = {
        {"InitializeApiImpl", reinterpret_cast<void*>(&InitializeApiImpl_hle)},
        {"InitializeApiImplEx", reinterpret_cast<void*>(&InitializeApiImplEx_hle)},
        {"InitializeApiImplEx2", reinterpret_cast<void*>(&InitializeApiImplEx2_hle)},
        {"UninitializeApiImpl", reinterpret_cast<void*>(&UninitializeApiImpl_hle)},
        {"QueryApiImpl", reinterpret_cast<void*>(&QueryApiImpl_hle)},
        {"XErrorReport", reinterpret_cast<void*>(&XErrorReport_hle)},
    };
    for (const auto& e : kProcs)
        if (strcmp(e.n, name) == 0) return reinterpret_cast<FARPROC>(e.f);
    LOG("[xgr] GetProcAddress(xgameruntime, \"%s\") -> not provided", name);
    return nullptr;
}
