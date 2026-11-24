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
void hle_set_image_base(void* base) { g_image_base = base; }
void* hle_image_base() { return g_image_base; }

void* hle_lookup(const char* dll_lower, const char* name) {
    for (const auto& e : registry()) {
        if (strcmp(e.dll, dll_lower) == 0 && strcmp(e.name, name) == 0) return e.fn;
    }
    return nullptr;
}
