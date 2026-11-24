#include "gpumem.h"

#include "../../log.h"

#include <atomic>
#include <map>
#include <shared_mutex>
#include <unordered_map>

namespace {

struct Range {
    uint64_t size;
    uint64_t real;
    void* owner;
};

struct Space {
    std::shared_mutex mu;
    std::map<uint64_t, Range> ranges;                     // keyed by game start
    std::unordered_map<const void*, uint64_t> objects;
};

// Leaked on purpose: used from game threads that can outlive static destruction.
Space* g_spaces = new Space[GM_SPACES];
std::atomic<uint64_t> g_translated{0};

const char* kSpaceName[GM_SPACES] = {"gpu-va", "desc-cpu", "desc-gpu"};

}  // namespace

void gm_add_range(GmSpace space, uint64_t game, uint64_t size, uint64_t real, const char* what, void* owner) {
    Space& s = g_spaces[space];
    std::unique_lock<std::shared_mutex> lk(s.mu);
    s.ranges[game] = Range{size, real, owner};
    LOG("[gpumem] %s %s: title 0x%llx..0x%llx -> real 0x%llx", kSpaceName[space], what, (unsigned long long)game,
        (unsigned long long)(game + size), (unsigned long long)real);
}

uint64_t gm_translate(GmSpace space, uint64_t addr) {
    if (!addr) return addr;
    Space& s = g_spaces[space];
    std::shared_lock<std::shared_mutex> lk(s.mu);
    auto it = s.ranges.upper_bound(addr);
    if (it == s.ranges.begin()) return addr;
    --it;
    if (addr - it->first >= it->second.size) return addr;
    g_translated.fetch_add(1, std::memory_order_relaxed);
    return it->second.real + (addr - it->first);
}

bool gm_lookup(GmSpace space, uint64_t addr, void** owner, uint64_t* offset) {
    Space& s = g_spaces[space];
    std::shared_lock<std::shared_mutex> lk(s.mu);
    auto it = s.ranges.upper_bound(addr);
    if (it == s.ranges.begin()) return false;
    --it;
    if (addr - it->first >= it->second.size) return false;
    *owner = it->second.owner;
    *offset = addr - it->first;
    return true;
}

void gm_set_object(GmSpace space, const void* obj, uint64_t game) {
    Space& s = g_spaces[space];
    std::unique_lock<std::shared_mutex> lk(s.mu);
    s.objects[obj] = game;
}

bool gm_object(GmSpace space, const void* obj, uint64_t* game) {
    Space& s = g_spaces[space];
    std::shared_lock<std::shared_mutex> lk(s.mu);
    auto it = s.objects.find(obj);
    if (it == s.objects.end()) return false;
    *game = it->second;
    return true;
}

uint64_t gm_translated_count() { return g_translated.load(); }
