#pragma once

// Creates an executable thunk for an import we cannot satisfy. When the game calls it,
// the call is logged (first call with caller address, then counted) and 0 is returned,
// or the process stops if stop_on_stub is set.
void* stub_create(const char* dll, const char* name, bool stop_on_stub);

// Prints every stub that was called, with call counts.
void stub_print_summary();

// Lets the stub logger translate return addresses into game RVAs.
void stub_set_image_range(const void* base, size_t size);
