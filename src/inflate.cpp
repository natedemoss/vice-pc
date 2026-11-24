#include "inflate.h"

#include <cstring>

namespace {

struct Bits {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buf = 0;
    int n = 0;
    bool overrun = false;

    void need(int k) {
        while (n < k) {
            uint64_t b = 0;
            if (p < end) b = *p++;
            else overrun = true;
            buf |= b << n;
            n += 8;
        }
    }
    uint32_t get(int k) {
        if (k == 0) return 0;
        need(k);
        uint32_t v = static_cast<uint32_t>(buf & ((1ull << k) - 1));
        buf >>= k;
        n -= k;
        return v;
    }
    void align() {
        int drop = n & 7;
        buf >>= drop;
        n -= drop;
    }
};

// Canonical Huffman table: counts per length + symbols sorted by code.
struct Huff {
    uint16_t count[16];
    uint16_t sym[288];
};

bool build(Huff& h, const uint8_t* lens, int n) {
    memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; ++i) h.count[lens[i]]++;
    h.count[0] = 0;
    int left = 1;
    for (int l = 1; l < 16; ++l) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return false;  // over-subscribed
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = offs[l] + h.count[l];
    for (int i = 0; i < n; ++i)
        if (lens[i]) h.sym[offs[lens[i]]++] = static_cast<uint16_t>(i);
    return true;
}

int decode(Bits& b, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; ++l) {
        code |= static_cast<int>(b.get(1));
        int c = h.count[l];
        if (code - c < first) return h.sym[index + (code - first)];
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits& b, const Huff& lit, const Huff& dist, uint8_t* out, size_t cap, size_t& pos) {
    for (;;) {
        int s = decode(b, lit);
        if (s < 0 || b.overrun) return false;
        if (s < 256) {
            if (pos >= cap) return false;
            out[pos++] = static_cast<uint8_t>(s);
        } else if (s == 256) {
            return true;
        } else {
            s -= 257;
            if (s >= 29) return false;
            size_t len = kLenBase[s] + b.get(kLenExtra[s]);
            int d = decode(b, dist);
            if (d < 0 || d >= 30) return false;
            size_t back = kDistBase[d] + b.get(kDistExtra[d]);
            if (back > pos || pos + len > cap) return false;
            for (size_t i = 0; i < len; ++i, ++pos) out[pos] = out[pos - back];  // may overlap
        }
    }
}

long long inflate_impl(Bits& b, uint8_t* out, size_t cap) {
    size_t pos = 0;
    static Huff fixed_lit, fixed_dist;
    static bool fixed_ready = [] {
        uint8_t l[288];
        int i = 0;
        for (; i < 144; ++i) l[i] = 8;
        for (; i < 256; ++i) l[i] = 9;
        for (; i < 280; ++i) l[i] = 7;
        for (; i < 288; ++i) l[i] = 8;
        build(fixed_lit, l, 288);
        uint8_t d[30];
        for (int j = 0; j < 30; ++j) d[j] = 5;
        build(fixed_dist, d, 30);
        return true;
    }();
    (void)fixed_ready;

    int last;
    do {
        last = static_cast<int>(b.get(1));
        int type = static_cast<int>(b.get(2));
        if (type == 0) {
            b.align();
            uint32_t len = b.get(16);
            uint32_t nlen = b.get(16);
            if ((len ^ 0xFFFF) != nlen || pos + len > cap) return -1;
            for (uint32_t i = 0; i < len; ++i) out[pos++] = static_cast<uint8_t>(b.get(8));
        } else if (type == 1) {
            if (!codes(b, fixed_lit, fixed_dist, out, cap, pos)) return -1;
        } else if (type == 2) {
            int nlen = static_cast<int>(b.get(5)) + 257;
            int ndist = static_cast<int>(b.get(5)) + 1;
            int ncode = static_cast<int>(b.get(4)) + 4;
            if (nlen > 286 || ndist > 30) return -1;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lens[320] = {};
            for (int i = 0; i < ncode; ++i) lens[order[i]] = static_cast<uint8_t>(b.get(3));
            Huff lencode;
            if (!build(lencode, lens, 19)) return -1;
            int idx = 0;
            while (idx < nlen + ndist) {
                int s = decode(b, lencode);
                if (s < 0) return -1;
                if (s < 16) {
                    lens[idx++] = static_cast<uint8_t>(s);
                } else {
                    uint8_t val = 0;
                    int rep;
                    if (s == 16) {
                        if (idx == 0) return -1;
                        val = lens[idx - 1];
                        rep = 3 + static_cast<int>(b.get(2));
                    } else if (s == 17) {
                        rep = 3 + static_cast<int>(b.get(3));
                    } else {
                        rep = 11 + static_cast<int>(b.get(7));
                    }
                    if (idx + rep > nlen + ndist) return -1;
                    while (rep--) lens[idx++] = val;
                }
            }
            if (lens[256] == 0) return -1;
            Huff lit, dist;
            if (!build(lit, lens, nlen) || !build(dist, lens + nlen, ndist)) return -1;
            if (!codes(b, lit, dist, out, cap, pos)) return -1;
        } else {
            return -1;
        }
        if (b.overrun) return -1;
    } while (!last);
    return static_cast<long long>(pos);
}

}  // namespace

long long raw_inflate(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap) {
    Bits b{in, in + in_len};
    return inflate_impl(b, out, out_cap);
}

long long zlib_inflate(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap) {
    if (in_len < 2) return -1;
    if ((in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) return -1;
    return raw_inflate(in + 2, in_len - 2, out, out_cap);
}
