#pragma once

// High-level emulation (HLE) registry: host-side reimplementations of Xbox-only
// functions. Each module (xmem.cpp, ...) registers its exports with HLE_EXPORT.
// The import resolver checks this table before native DLLs and stubs.

struct HleExport {
    const char* dll;   // lower-case, e.g. "xmem.dll"
    const char* name;  // exact export name
    void* fn;
};

void* hle_lookup(const char* dll_lower, const char* name);
void hle_register(const HleExport* table, int count);

// Registers a table at static-init time.
struct HleRegistrar {
    template <int N>
    explicit HleRegistrar(const HleExport (&t)[N]) { hle_register(t, N); }
};
