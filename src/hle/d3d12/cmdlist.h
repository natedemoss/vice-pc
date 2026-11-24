#pragma once

// Install address-translating overrides on a patched command list's vtable.
void cmdlist_install(void* list);

// Title-owned command list (Xbox packet-buffer layout, no-op methods). See cmdlist.cpp.
void* cmdlist_create_title_list(int type);
