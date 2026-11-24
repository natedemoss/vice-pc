#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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

// Base address of the mapped game image (set by the loader before imports are resolved).
void hle_set_image_base(void* base, size_t size);
void* hle_image_base();
// True if p lies inside the game image; *rva receives its offset.
bool hle_rva(const void* p, uint64_t* rva);

// Full path of the game executable (what GetModuleFileName(NULL) reports to the title).
void hle_set_game_path(const std::wstring& path);
const std::wstring& hle_game_path();

// Folder that Xbox's G:\ (the mounted title package) maps to.
void hle_set_package_root(const std::wstring& path);
const std::wstring& hle_package_root();

// Registers a table at static-init time.
struct HleRegistrar {
    template <int N>
    explicit HleRegistrar(const HleExport (&t)[N]) { hle_register(t, N); }
};
