// A picture on its way to other players, and what comes back from them. No PNG leaves or enters here: the sender decodes its own
// file, makes it smaller and writes the pixels as DXT5 blocks, which have no structure a bad file could use: a size, then a fixed
// number of bytes, whatever they hold is colours. What arrives is checked for its size and unpacked into a buffer of exactly that size.
//   "GSK1", width and height (16 bits each, multiples of 4, 4 to SKIN_MAX_SIDE), then zlib of the blocks
// Include after stb_dxt.h and stb_image_write.h.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <vector>
extern "C" int stbi_zlib_decode_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);
constexpr int SKIN_MAX_SIDE = 1024;
constexpr size_t SKIN_MAX_BYTES = 900 * 1024;

// w x h of RGBA averaged down to nw x nh; colour counts by its alpha, so a see-through edge does not go dark
static void skin_shrink(const unsigned char *px, int w, int h, int nw, int nh, std::vector<unsigned char> &out) {
    out.resize((size_t)nw * nh * 4);
    for (int y = 0; y < nh; y++) {
        const int y0 = (int)((int64_t)y * h / nh), y1 = (int)((int64_t)(y + 1) * h / nh);
        for (int x = 0; x < nw; x++) {
            const int x0 = (int)((int64_t)x * w / nw), x1 = (int)((int64_t)(x + 1) * w / nw);
            double c[3] = {0, 0, 0}, a = 0;
            int n = 0;
            for (int j = y0; j < (y1 > y0 ? y1 : y0 + 1); j++) for (int i = x0; i < (x1 > x0 ? x1 : x0 + 1); i++) {
                const unsigned char *p = px + ((size_t)(j < h ? j : h - 1) * w + (i < w ? i : w - 1)) * 4;
                for (int k = 0; k < 3; k++) c[k] += (double)p[k] * (p[3] + 1);
                a += p[3] + 1;
                n++;
            }
            unsigned char *o = &out[((size_t)y * nw + x) * 4];
            for (int k = 0; k < 3; k++) o[k] = (unsigned char)(c[k] / a + .5);
            o[3] = (unsigned char)(a / n - 1 + .5);
        }
    }
}
// the picture as a package of at most SKIN_MAX_BYTES: as big as it can be and still fit
static bool skin_encode(const unsigned char *px, int w, int h, std::vector<unsigned char> &out) {
    if (w < 1 || h < 1) return false;
    for (int side = SKIN_MAX_SIDE; side >= 64; side = side * 3 / 4) {
        const int big = w > h ? w : h;
        const double k = big > side ? (double)side / big : 1;
        int nw = (int)(w * k) & ~3, nh = (int)(h * k) & ~3;
        nw = nw < 4 ? 4 : nw; nh = nh < 4 ? 4 : nh;
        std::vector<unsigned char> small;
        skin_shrink(px, w, h, nw, nh, small);
        std::vector<unsigned char> blocks((size_t)(nw / 4) * (nh / 4) * 16);
        size_t at = 0;
        for (int by = 0; by < nh; by += 4) for (int bx = 0; bx < nw; bx += 4) {
            unsigned char rgba[64];
            for (int j = 0; j < 4; j++) memcpy(rgba + j * 16, &small[((size_t)(by + j) * nw + bx) * 4], 16);
            stb_compress_dxt_block(&blocks[at], rgba, 1, STB_DXT_HIGHQUAL);
            at += 16;
        }
        int n = 0;
        unsigned char *z = stbi_zlib_compress(blocks.data(), (int)blocks.size(), &n, 9);
        if (!z) return false;
        if ((size_t)n + 8 <= SKIN_MAX_BYTES) {
            out.resize((size_t)n + 8);
            memcpy(out.data(), "GSK1", 4);
            out[4] = (unsigned char)nw; out[5] = (unsigned char)(nw >> 8); out[6] = (unsigned char)nh; out[7] = (unsigned char)(nh >> 8);
            memcpy(out.data() + 8, z, n);
            free(z);
            return true;
        }
        free(z);
    }
    return false;
}
// what came from the server to RGBA; false: not one of ours
static bool skin_decode(const unsigned char *d, size_t n, std::vector<unsigned char> &px, int &w, int &h) {
    if (n <= 8 || n > SKIN_MAX_BYTES || memcmp(d, "GSK1", 4)) return false;
    w = d[4] | d[5] << 8; h = d[6] | d[7] << 8;
    if (w < 4 || h < 4 || w > SKIN_MAX_SIDE || h > SKIN_MAX_SIDE || w % 4 || h % 4) return false;
    const int size = (w / 4) * (h / 4) * 16;
    std::vector<unsigned char> blocks((size_t)size);
    if (stbi_zlib_decode_buffer((char *)blocks.data(), size, (const char *)d + 8, (int)n - 8) != size) return false;
    px.resize((size_t)w * h * 4);
    const unsigned char *b = blocks.data();
    for (int by = 0; by < h; by += 4) for (int bx = 0; bx < w; bx += 4, b += 16) {
        int alpha[8] = {b[0], b[1]};
        for (int i = 2; i < 8; i++) alpha[i] = b[0] > b[1] ? ((8 - i) * b[0] + (i - 1) * b[1]) / 7 : i < 6 ? ((6 - i) * b[0] + (i - 1) * b[1]) / 5 : i == 6 ? 0 : 255;
        uint64_t abits = 0;
        memcpy(&abits, b + 2, 6);
        const uint16_t c0 = b[8] | b[9] << 8, c1 = b[10] | b[11] << 8;
        uint32_t bits; memcpy(&bits, b + 12, 4);
        int pal[4][3];
        pal[0][0] = (c0 >> 11) * 255 / 31; pal[0][1] = ((c0 >> 5) & 63) * 255 / 63; pal[0][2] = (c0 & 31) * 255 / 31;
        pal[1][0] = (c1 >> 11) * 255 / 31; pal[1][1] = ((c1 >> 5) & 63) * 255 / 63; pal[1][2] = (c1 & 31) * 255 / 31;
        for (int i = 0; i < 3; i++) { pal[2][i] = (2 * pal[0][i] + pal[1][i]) / 3; pal[3][i] = (pal[0][i] + 2 * pal[1][i]) / 3; }
        for (int i = 0; i < 16; i++) {
            unsigned char *o = &px[((size_t)(by + (i >> 2)) * w + bx + (i & 3)) * 4];
            const int *c = pal[bits >> (2 * i) & 3];
            o[0] = (unsigned char)c[0]; o[1] = (unsigned char)c[1]; o[2] = (unsigned char)c[2];
            o[3] = (unsigned char)alpha[abits >> (3 * i) & 7];
        }
    }
    return true;
}
// Names that come from other players go into the game's file lookups: only the plain characters of its vehicle paths
static bool skin_plain(const std::string &s, size_t most) {
    if (s.empty() || s.size() > most || s.find("..") != std::string::npos) return false;
    for (unsigned char c : s) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '/' || c == '.')) return false;
    return true;
}
static bool skin_model_ok(const std::string &s) {
    return skin_plain(s, 200) && !s.compare(0, 17, "/entity/-vehicle/") && s.size() > 21 && !s.compare(s.size() - 4, 4, ".mdl");
}
static bool skin_stem_ok(const std::string &s) { return skin_plain(s, 160) && !s.compare(0, 9, "_vehicle/"); }
static bool skin_hash_ok(const std::string &s) {
    if (s.size() != 64) return false;
    for (unsigned char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
