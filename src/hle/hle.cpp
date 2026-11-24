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

void* hle_lookup(const char* dll_lower, const char* name) {
    for (const auto& e : registry()) {
        if (strcmp(e.dll, dll_lower) == 0 && strcmp(e.name, name) == 0) return e.fn;
    }
    return nullptr;
}
