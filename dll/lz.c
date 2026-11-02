// A small LZ77 codec in the spirit of LZ4, for snapshot pages: fast both ways, and runs of zeros (what the
// XOR of two versions of a page mostly is) shrink to a few bytes.
//
// Stream: sequences of [token][literal length ext][literals][offset u16][match length ext].
// token = literal length (high 4 bits, 15 = more bytes follow) | match length - 4 (low 4 bits, 15 = more);
// the last sequence has literals only. Lengths past 15 add bytes of 255 and a final byte < 255.
#include <stdint.h>
#include <string.h>

#define MIN_MATCH 4
#define HASH_BITS 12

static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static uint8_t *put_len(uint8_t *op, uint32_t n) {
    while (n >= 255) { *op++ = 255; n -= 255; }
    *op++ = (uint8_t)n;
    return op;
}

static uint8_t *emit(uint8_t *op, const uint8_t *lit, uint32_t nlit, uint32_t off, uint32_t mlen) {
    uint8_t *tok = op++;
    *tok = (uint8_t)((nlit >= 15 ? 15 : nlit) << 4);
    if (nlit >= 15) op = put_len(op, nlit - 15);
    memcpy(op, lit, nlit);
    op += nlit;
    if (mlen) {
        op[0] = (uint8_t)off; op[1] = (uint8_t)(off >> 8);
        op += 2;
        uint32_t m = mlen - MIN_MATCH;
        *tok |= (uint8_t)(m >= 15 ? 15 : m);
        if (m >= 15) op = put_len(op, m - 15);
    }
    return op;
}

// worst case output: n + n / 255 + 16 bytes
uint32_t lz_pack(const uint8_t *src, uint32_t n, uint8_t *dst) {
    uint32_t table[1 << HASH_BITS];
    memset(table, 0xff, sizeof table);
    uint8_t *op = dst;
    uint32_t ip = 0, anchor = 0, misses = 0;
    if (n >= 16) {
        uint32_t last = n - 12;                 // matches start before this and end 5 bytes before the end
        while (ip <= last) {
            uint32_t seq = rd32(src + ip), h = (seq * 2654435761u) >> (32 - HASH_BITS), cand = table[h];
            table[h] = ip;
            if (cand != 0xffffffffu && ip - cand <= 65535 && rd32(src + cand) == seq) {
                uint32_t m = ip + MIN_MATCH, c = cand + MIN_MATCH;
                while (m < n - 5 && src[m] == src[c]) { m++; c++; }
                op = emit(op, src + anchor, ip - anchor, ip - cand, m - ip);
                ip = anchor = m;
                misses = 0;
            } else {
                ip += 1 + (misses++ >> 5);          // incompressible stretches are skipped faster
            }
        }
    }
    return (uint32_t)(emit(op, src + anchor, n - anchor, 0, 0) - dst);
}

// 1 when exactly dn bytes came out
int lz_unpack(const uint8_t *src, uint32_t sn, uint8_t *dst, uint32_t dn) {
    const uint8_t *ip = src, *iend = src + sn;
    uint8_t *op = dst, *oend = dst + dn;
    while (ip < iend) {
        uint32_t tok = *ip++, nlit = tok >> 4;
        if (nlit == 15) { uint32_t b; do { if (ip >= iend) return 0; b = *ip++; nlit += b; } while (b == 255); }
        if (nlit > (uint32_t)(iend - ip) || nlit > (uint32_t)(oend - op)) return 0;
        memcpy(op, ip, nlit);
        op += nlit; ip += nlit;
        if (ip >= iend) break;                      // the last sequence has no match
        if (iend - ip < 2) return 0;
        uint32_t off = ip[0] | (uint32_t)ip[1] << 8, mlen = (tok & 15) + MIN_MATCH;
        ip += 2;
        if ((tok & 15) == 15) { uint32_t b; do { if (ip >= iend) return 0; b = *ip++; mlen += b; } while (b == 255); }
        if (!off || off > (uint32_t)(op - dst) || mlen > (uint32_t)(oend - op)) return 0;
        const uint8_t *from = op - off;
        if (off >= mlen) { memcpy(op, from, mlen); op += mlen; }
        else while (mlen--) *op++ = *from++;        // overlapping: runs
    }
    return op == oend;
}
