// A texture of a mod can come with its biggest picture only. The game takes that as a plain texture, but not as a variant of one:
// it wants mips there ("No MIP levels!", 141510), and shows the grey checker instead. So what has one level gets the rest, each the
// level above it averaged. Plain 32 bit pixels, DXT1, DXT3 and DXT5. Include after stb_dxt.h.
#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>
static void dds_decode_block(const unsigned char *b, int size, uint32_t four, unsigned char out[16][4]) {
    const unsigned char *c = b + size - 8;
    const uint16_t c0 = c[0] | c[1] << 8, c1 = c[2] | c[3] << 8;
    uint32_t bits; memcpy(&bits, c + 4, 4);
    int pal[4][3];
    pal[0][0] = (c0 >> 11) * 255 / 31; pal[0][1] = ((c0 >> 5) & 63) * 255 / 63; pal[0][2] = (c0 & 31) * 255 / 31;
    pal[1][0] = (c1 >> 11) * 255 / 31; pal[1][1] = ((c1 >> 5) & 63) * 255 / 63; pal[1][2] = (c1 & 31) * 255 / 31;
    for (int i = 0; i < 3; i++) {
        if (size == 16 || c0 > c1) { pal[2][i] = (2 * pal[0][i] + pal[1][i]) / 3; pal[3][i] = (pal[0][i] + 2 * pal[1][i]) / 3; }
        else { pal[2][i] = (pal[0][i] + pal[1][i]) / 2; pal[3][i] = 0; }
    }
    int alpha[8] = {255, 255};
    uint64_t abits = 0;
    if (four == 0x35545844) {
        alpha[0] = b[0]; alpha[1] = b[1];
        for (int i = 2; i < 8; i++) alpha[i] = b[0] > b[1] ? ((8 - i) * b[0] + (i - 1) * b[1]) / 7 : i < 6 ? ((6 - i) * b[0] + (i - 1) * b[1]) / 5 : i == 6 ? 0 : 255;
        memcpy(&abits, b + 2, 6);
    }
    for (int i = 0; i < 16; i++) {
        const int *p = pal[bits >> (2 * i) & 3];
        out[i][0] = (unsigned char)p[0]; out[i][1] = (unsigned char)p[1]; out[i][2] = (unsigned char)p[2];
        out[i][3] = four == 0x33545844 ? (unsigned char)((b[i / 2] >> (i & 1) * 4 & 15) * 17) : four == 0x35545844 ? (unsigned char)alpha[abits >> (3 * i) & 7] : 255;
    }
}
static void dds_encode_block(const unsigned char rgba[64], int size, uint32_t four, unsigned char *b) {
    if (four == 0x35545844) { stb_compress_dxt_block(b, rgba, 1, STB_DXT_HIGHQUAL); return; }
    if (four == 0x33545844) {
        for (int i = 0; i < 16; i += 2) b[i / 2] = (unsigned char)((rgba[i * 4 + 3] + 8) / 17 | ((rgba[i * 4 + 7] + 8) / 17) << 4);
        stb_compress_dxt_block(b + 8, rgba, 0, STB_DXT_HIGHQUAL);
        return;
    }
    stb_compress_dxt_block(b, rgba, 0, STB_DXT_HIGHQUAL);
}
// false: a kind of file this does not know; true: it has its mips
static bool dds_add_mips(std::vector<unsigned char> &f) {
    if (f.size() < 128 || memcmp(f.data(), "DDS ", 4)) return false;
    uint32_t h, w, mips, four, bits;
    memcpy(&h, &f[12], 4); memcpy(&w, &f[16], 4); memcpy(&mips, &f[28], 4); memcpy(&four, &f[84], 4); memcpy(&bits, &f[88], 4);
    if (mips > 1) return true;
    if (!w || !h || w > 16384 || h > 16384) return false;
    const int size = four == 0x31545844 ? 8 : four == 0x35545844 || four == 0x33545844 ? 16 : !four && bits == 32 ? 0 : -1;
    if (size < 0) return false;
    const size_t bw = (w + 3) / 4, bh = (h + 3) / 4, top = size ? bw * bh * size : (size_t)w * h * 4;
    if (f.size() < 128 + top) return false;
    if (w == 1 && h == 1) return true;
    std::vector<unsigned char> px((size_t)w * h * 4);
    if (!size) memcpy(px.data(), &f[128], px.size());
    else for (size_t by = 0; by < bh; by++) for (size_t bx = 0; bx < bw; bx++) {
        unsigned char o[16][4];
        dds_decode_block(&f[128 + (by * bw + bx) * size], size, four, o);
        for (int i = 0; i < 16; i++) {
            const size_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
            if (x < w && y < h) memcpy(&px[(y * w + x) * 4], o[i], 4);
        }
    }
    f.resize(128 + top);
    uint32_t levels = 1, cw = w, ch = h;
    while (cw > 1 || ch > 1) {
        const uint32_t nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
        std::vector<unsigned char> small((size_t)nw * nh * 4);
        for (uint32_t y = 0; y < nh; y++) for (uint32_t x = 0; x < nw; x++) {
            const uint32_t x0 = x * 2, y0 = y * 2, x1 = x0 + 1 < cw ? x0 + 1 : x0, y1 = y0 + 1 < ch ? y0 + 1 : y0;
            for (int c = 0; c < 4; c++)
                small[((size_t)y * nw + x) * 4 + c] = (unsigned char)((px[((size_t)y0 * cw + x0) * 4 + c] + px[((size_t)y0 * cw + x1) * 4 + c] + px[((size_t)y1 * cw + x0) * 4 + c] + px[((size_t)y1 * cw + x1) * 4 + c] + 2) / 4);
        }
        px.swap(small); cw = nw; ch = nh; levels++;
        if (!size) { f.insert(f.end(), px.begin(), px.end()); continue; }
        const size_t at = f.size(), lw = (cw + 3) / 4, lh = (ch + 3) / 4;
        f.resize(at + lw * lh * size);
        for (size_t by = 0; by < lh; by++) for (size_t bx = 0; bx < lw; bx++) {
            unsigned char rgba[64];
            for (int i = 0; i < 16; i++) {
                size_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                x = x < cw ? x : cw - 1; y = y < ch ? y : ch - 1;
                memcpy(rgba + i * 4, &px[(y * cw + x) * 4], 4);
            }
            dds_encode_block(rgba, size, four, &f[at + (by * lw + bx) * size]);
        }
    }
    uint32_t flags, caps;
    memcpy(&flags, &f[8], 4); memcpy(&caps, &f[108], 4);
    flags |= 0x20000; caps |= 0x400008;
    memcpy(&f[8], &flags, 4); memcpy(&f[108], &caps, 4); memcpy(&f[28], &levels, 4);
    return true;
}
