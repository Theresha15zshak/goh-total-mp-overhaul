// Round-trip test of the snapshot codec (dll/lz.c) on the kinds of pages a game heap holds.
//   python -m ziglang cc -target x86_64-windows-gnu -O2 tests/lz_test.c dll/lz.c -o build/lz_test.exe
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

uint32_t lz_pack(const uint8_t *src, uint32_t n, uint8_t *dst);
int lz_unpack(const uint8_t *src, uint32_t sn, uint8_t *dst, uint32_t dn);

static uint32_t s = 12345;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

static int round_trip(const uint8_t *page, uint32_t n, uint32_t *packed) {
    static uint8_t out[1 << 20], back[1 << 20];
    uint32_t c = lz_pack(page, n, out);
    *packed = c;
    memset(back, 0xcd, n);
    return lz_unpack(out, c, back, n) && !memcmp(back, page, n);
}

int main(void) {
    static uint8_t page[1 << 18];
    const char *kind[] = {"zeros", "random", "structs", "text", "sparse xor", "short runs", "big blob"};
    uint64_t total[7] = {0}, packed[7] = {0};
    int bad = 0;
    for (int k = 0; k < 7; k++) {
        for (int t = 0; t < 3000; t++) {
            uint32_t n = k == 6 ? 20000 + rnd() % 200000 : 4096;
            memset(page, 0, n);
            if (k == 1) for (uint32_t i = 0; i < n; i++) page[i] = (uint8_t)rnd();
            if (k == 2) for (uint32_t i = 0; i + 48 <= n; i += 48) {   // objects: vtable, pointers, floats, flags
                uint64_t v[6] = {0x14105e8a8ull, 0x3724133a0ull + (rnd() % 64) * 16, rnd() % 7, 0x3f800000, rnd() & 1, 0};
                memcpy(page + i, v, 48);
            }
            if (k == 3) for (uint32_t i = 0; i < n; i++) page[i] = "squad_rifle_late_irr_1 "[(i + t) % 23];
            if (k == 4) for (int j = 0; j < 12; j++) page[rnd() % n] = (uint8_t)rnd();
            if (k == 5) for (uint32_t i = 0; i < n; i++) page[i] = (uint8_t)((i / (1 + rnd() % 9)) & 3);
            if (k == 6) for (uint32_t i = 0; i < n; i++) page[i] = (i % 1000 < 600) ? (uint8_t)rnd() : 0;
            uint32_t c;
            if (!round_trip(page, n, &c)) { printf("%s: round trip failed (n %u)\n", kind[k], n); bad++; break; }
            total[k] += n; packed[k] += c;
        }
        printf("%-10s %6.1f%% of the size\n", kind[k], 100.0 * packed[k] / total[k]);
    }
    // speed on struct-like pages
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    static uint8_t out[8192], back[4096];
    memset(page, 0, 4096);
    for (uint32_t i = 0; i + 48 <= 4096; i += 48) { uint64_t v[6] = {0x14105e8a8ull, 0x3724133a0ull + i, i % 7, 0x3f800000, i & 1, 0}; memcpy(page + i, v, 48); }
    uint32_t c = 0;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 20000; i++) { page[i % 4096] ^= 1; c = lz_pack(page, 4096, out); }
    QueryPerformanceCounter(&b);
    double pack_mb = 20000 * 4096.0 / 1e6 / ((b.QuadPart - a.QuadPart) / (double)f.QuadPart);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 20000; i++) bad += !lz_unpack(out, c, back, 4096);
    QueryPerformanceCounter(&b);
    double unpack_mb = 20000 * 4096.0 / 1e6 / ((b.QuadPart - a.QuadPart) / (double)f.QuadPart);
    printf("speed: pack %.0f MB/s, unpack %.0f MB/s\n", pack_mb, unpack_mb);
    printf("%s\n", bad ? "FAILED" : "ok");
    return bad;
}
