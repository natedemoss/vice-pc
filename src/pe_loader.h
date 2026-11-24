#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct LoadedImage {
    uint8_t* base = nullptr;
    size_t size = 0;
    uint64_t preferred_base = 0;
    IMAGE_NT_HEADERS64* nt = nullptr;
    uint8_t* entry = nullptr;

    bool contains(const void* p) const {
        auto a = reinterpret_cast<const uint8_t*>(p);
        return a >= base && a < base + size;
    }
    uint64_t rva_of(const void* p) const {
        return static_cast<uint64_t>(reinterpret_cast<const uint8_t*>(p) - base);
    }
    IMAGE_DATA_DIRECTORY dir(int index) const {
        return nt->OptionalHeader.DataDirectory[index];
    }
};

struct ImportOptions {
    // DLL names (lower-case) that must never be loaded for real; every function is stubbed/HLE'd.
    std::vector<std::string> stub_dlls;
    // When a stub is called: log and terminate instead of returning 0.
    bool stop_on_stub = false;
};

struct ImportStats {
    int dlls = 0;
    int resolved_native = 0;
    int resolved_hle = 0;
    int stubbed = 0;
};

// Reads the file, maps sections at the preferred base (or relocates), returns false on error.
bool pe_map(const std::wstring& path, LoadedImage& img);

// Fills the import address table: native Windows DLL, host HLE implementation, or logging stub.
bool pe_resolve_imports(LoadedImage& img, const ImportOptions& opt, ImportStats& stats);

// Registers .pdata so SEH / C++ exceptions can unwind through game code.
bool pe_register_unwind(LoadedImage& img);

// Applies final page protections per section (call after imports are written).
void pe_protect_sections(LoadedImage& img);
