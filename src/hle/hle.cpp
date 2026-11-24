#include "hle.h"

#include <cstring>
#include <vector>

static std::vector<HleExport>& registry() {
    static std::vector<HleExport> r;
    return r;
}

void hle_register(const HleExport* table, int count) {
    registry().insert(registry().end(), table, table + count);
}

static void* g_image_base = nullptr;
static size_t g_image_size = 0;
void hle_set_image_base(void* base, size_t size) {
    g_image_base = base;
    g_image_size = size;
}
void* hle_image_base() { return g_image_base; }

static std::wstring& game_path_storage() {
    static std::wstring* s = new std::wstring();
    return *s;
}
void hle_set_game_path(const std::wstring& path) { game_path_storage() = path; }
const std::wstring& hle_game_path() { return game_path_storage(); }

static std::wstring& package_root_storage() {
    static std::wstring* s = new std::wstring();
    return *s;
}
void hle_set_package_root(const std::wstring& path) { package_root_storage() = path; }
const std::wstring& hle_package_root() { return package_root_storage(); }
bool hle_rva(const void* p, uint64_t* rva) {
    auto a = static_cast<const uint8_t*>(p), b = static_cast<const uint8_t*>(g_image_base);
    if (!b || a < b || a >= b + g_image_size) return false;
    *rva = static_cast<uint64_t>(a - b);
    return true;
}

void* hle_lookup(const char* dll_lower, const char* name) {
    for (const auto& e : registry()) {
        if (strcmp(e.dll, dll_lower) == 0 && strcmp(e.name, name) == 0) return e.fn;
    }
    return nullptr;
}
