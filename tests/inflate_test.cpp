// Unit test for src/inflate.cpp against streams produced by Python's zlib
// (dynamic Huffman at level 9, stored blocks at level 0, fixed Huffman).

#include "../src/inflate.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "inflate_data.inc"

static int check(const char* name, const uint8_t* z, size_t zn, const uint8_t* want, size_t wn) {
    static uint8_t out[1 << 16];
    long long n = zlib_inflate(z, zn, out, sizeof(out));
    bool ok = n == static_cast<long long>(wn) && memcmp(out, want, wn) == 0;
    printf("%-8s %s (%lld bytes)\n", name, ok ? "ok" : "FAILED", n);
    return ok ? 0 : 1;
}

int main() {
    int fails = 0;
    fails += check("level9", kLevel9, sizeof(kLevel9), kPlain, sizeof(kPlain));
    fails += check("level0", kLevel0, sizeof(kLevel0), kPlain, sizeof(kPlain));
    fails += check("fixed", kFixed, sizeof(kFixed), kSmall, sizeof(kSmall));
    // Truncated input must fail cleanly, not crash.
    uint8_t out[64];
    if (zlib_inflate(kLevel9, 10, out, sizeof(out)) >= 0) { printf("truncated FAILED\n"); ++fails; }
    // Output overflow must fail cleanly.
    if (zlib_inflate(kLevel9, sizeof(kLevel9), out, sizeof(out)) >= 0) { printf("overflow FAILED\n"); ++fails; }
    return fails;
}
