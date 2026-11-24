#pragma once

// Install address-translating overrides on a patched command list's vtable.
void cmdlist_install(void* list);

// Title-owned command list (Xbox packet-buffer layout, no-op methods). See cmdlist.cpp.
void* cmdlist_create_title_list(int type);

#include <cstddef>
#include <cstdint>

// Packets recorded so far in a title command list (from its buffer start to the cursor).
// False if list isn't a title list.
bool cmdlist_title_packets(void* list, const uint32_t** start, size_t* dwords);
