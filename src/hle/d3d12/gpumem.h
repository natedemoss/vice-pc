#pragma once

// Title address -> real PC address translation.
//
// On Xbox the title chooses where things live: it creates buffers at GPU virtual
// addresses inside memory it reserved itself, and places descriptor heaps at CPU
// addresses it picked. On PC the driver picks those addresses. So the title keeps
// seeing its own addresses, and every call that hands one back to D3D12 goes
// through gm_translate() first.

#include <cstdint>

enum GmSpace {
    GM_GPU_VA = 0,     // buffer GPU virtual addresses
    GM_DESC_CPU = 1,   // CPU descriptor handles
    GM_DESC_GPU = 2,   // GPU descriptor handles
    GM_SPACES = 3,
};

// Register [game, game+size) -> [real, real+size).
void gm_add_range(GmSpace space, uint64_t game, uint64_t size, uint64_t real, const char* what,
                  void* owner = nullptr);

// Range lookup: owner object (e.g. the ID3D12Resource backing a GPU VA range) and the
// offset of `addr` inside the range. False if `addr` is not in a registered range.
bool gm_lookup(GmSpace space, uint64_t addr, void** owner, uint64_t* offset);

// Real address for a title address; returns `addr` unchanged if it is not in a registered range.
uint64_t gm_translate(GmSpace space, uint64_t addr);

// Per-object title address (descriptor heap start, resource GPU VA).
void gm_set_object(GmSpace space, const void* obj, uint64_t game);
bool gm_object(GmSpace space, const void* obj, uint64_t* game);

// Forget every range and object entry belonging to `owner` (called when it is destroyed).
void gm_remove_owner(const void* owner);

// Counters for the log summary.
uint64_t gm_translated_count();
