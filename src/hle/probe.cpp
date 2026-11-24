#include "probe.h"

#include "hle.h"
#include "../log.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

struct ProbeType {
    std::string name;
    void* vtbl[kProbeSlots];
    const char* slot_names[kProbeSlots];
    std::atomic<uint32_t> calls[kProbeSlots];
};

namespace {

std::vector<ProbeType*>& types() {
    static std::vector<ProbeType*>* t = new std::vector<ProbeType*>();
    return *t;
}

void format_guid(const GUID* g, char* out, size_t n) {
    if (!g) {
        strncpy_s(out, n, "(null)", _TRUNCATE);
        return;
    }
    snprintf(out, n, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g->Data1, g->Data2, g->Data3,
             g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

void format_caller(const void* ret, char* out, size_t n) {
    uint64_t rva;
    if (hle_rva(ret, &rva)) snprintf(out, n, "game+0x%llx", (unsigned long long)rva);
    else snprintf(out, n, "%p", ret);
}

uintptr_t dispatch(ProbeObject* self, int slot, uintptr_t a, uintptr_t b, uintptr_t c, const void* ret) {
    ProbeType* t = self->type;
    uint32_t n = t->calls[slot].fetch_add(1);
    char caller[64];
    format_caller(ret, caller, sizeof(caller));

    switch (slot) {
    case 0: {  // QueryInterface(riid, ppv)
        char g[64];
        format_guid(reinterpret_cast<const GUID*>(a), g, sizeof(g));
        LOG("[probe] %s::QueryInterface(%s) from %s -> self", t->name.c_str(), g, caller);
        if (b) *reinterpret_cast<void**>(b) = self;
        _InterlockedIncrement(&self->refs);
        return S_OK;
    }
    case 1:
        return (uintptr_t)_InterlockedIncrement(&self->refs);
    case 2: {
        long r = _InterlockedDecrement(&self->refs);
        // Never actually free: a stale pointer the game keeps would otherwise crash in
        // a confusing place. Leaking a few probe objects is fine while we explore.
        return (uintptr_t)(r < 0 ? 0 : r);
    }
    default:
        if (n < 8) {
            LOG("[probe] %s::slot%d(0x%llx, 0x%llx, 0x%llx) from %s -> S_OK", t->name.c_str(), slot,
                (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, caller);
        }
        return S_OK;
    }
}

template <int N>
uintptr_t __cdecl slot_fn(ProbeObject* self, uintptr_t a, uintptr_t b, uintptr_t c) {
    return dispatch(self, N, a, b, c, _ReturnAddress());
}

template <int... Is>
void fill_defaults(void** vtbl, std::integer_sequence<int, Is...>) {
    ((vtbl[Is] = reinterpret_cast<void*>(&slot_fn<Is>)), ...);
}

}  // namespace

ProbeType* probe_type(const char* name) {
    for (auto* t : types())
        if (t->name == name) return t;
    auto* t = new ProbeType();
    t->name = name;
    fill_defaults(t->vtbl, std::make_integer_sequence<int, kProbeSlots>{});
    for (auto& s : t->slot_names) s = nullptr;
    for (auto& c : t->calls) c = 0;
    types().push_back(t);
    return t;
}

ProbeObject* probe_create(ProbeType* type, void* user) {
    auto* o = new ProbeObject();
    o->vtbl = type->vtbl;
    o->type = type;
    o->refs = 1;
    o->user = user;
    return o;
}

void probe_override(ProbeType* type, int index, void* fn, const char* slot_name) {
    if (index < 3 || index >= kProbeSlots) return;
    type->vtbl[index] = fn;
    type->slot_names[index] = slot_name;
}

const char* probe_type_name(const ProbeType* type) {
    return type->name.c_str();
}
