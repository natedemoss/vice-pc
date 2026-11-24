#pragma once

#include <cstddef>
#include <cstdint>

// Minimal DEFLATE / zlib decoder (RFC 1950/1951): stored, fixed and dynamic blocks.
// Used for DirectStorage requests, which the reference title compresses with zlib.

// Decodes a zlib stream (2-byte header, deflate data, Adler-32 not verified).
// Returns the number of bytes written to `out`, or -1 on malformed input / overflow.
long long zlib_inflate(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap);

// Same for a raw deflate stream (no zlib header).
long long raw_inflate(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap);
