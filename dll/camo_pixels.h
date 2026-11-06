// CPU reference for the texture compositor; independent of the game and graphics API.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <vector>
// The whole vehicle, the hull and the turret take one sheet from side to side; a side takes the whole picture for itself.
enum { CAMO_ALL, CAMO_LEFT, CAMO_RIGHT, CAMO_HULL, CAMO_TURRET };
struct CamoSettings {
    float offset[2] = {0, 0};
    float scale = 5, rotation = 0, opacity = 1;
    int blend = 0;
    int mode = 0;                                   // 0 one picture on the vehicle (camo.cpp), 1 a pattern over the texture
    int repeat = 1;                                 // mode 0: the picture goes on mirrored beyond its edges
    float size = 1;                                 // mode 0: 1 = the smallest picture that covers the whole vehicle
    int part = 0;                                   // mode 0: where it goes, CAMO_ above
    int gun = 0;                                    // mode 0: 1 the barrels take it too
    int seam = 0;                                   // mode 0: 1 the old cover, torn along the middle of the vehicle
    int flip_x = 1, flip_y = 0;                     // mode 0: the picture turned over; left to right is how it reads right from the front
    float dim = 1;                                  // the picture's level, in plain blending (camo.cpp, picture_level); not a setting
    float bright = 1;                               // the player's brightness over that level
};
// The pixels a selection holds, corners 0 to 1 either way round: left, top, right, bottom. False: the whole picture, or too little
// to be a selection.
static bool camo_crop_rect(float ax, float ay, float bx, float by, int w, int h, int r[4]) {
    r[0] = (int)floorf(fminf(ax, bx) * w + .5f); r[2] = (int)floorf(fmaxf(ax, bx) * w + .5f);
    r[1] = (int)floorf(fminf(ay, by) * h + .5f); r[3] = (int)floorf(fmaxf(ay, by) * h + .5f);
    if (r[0] < 0) r[0] = 0;
    if (r[1] < 0) r[1] = 0;
    if (r[2] > w) r[2] = w;
    if (r[3] > h) r[3] = h;
    if (r[2] - r[0] < 2 || r[3] - r[1] < 2) return false;
    return r[0] > 0 || r[1] > 0 || r[2] < w || r[3] < h;
}
// A DXT1 or DXT5 file twice as big each way, with a new biggest mip on top of the ones it has. A block's four quarters become four
// blocks with its colours, each texel twice over: what is not painted on looks as it did. False: not a file this can do.
static bool camo_double_dds(std::vector<unsigned char> &f) {
    if (f.size() < 128 || memcmp(f.data(), "DDS ", 4)) return false;
    uint32_t h, w, mips, four;
    memcpy(&h, &f[12], 4); memcpy(&w, &f[16], 4); memcpy(&mips, &f[28], 4); memcpy(&four, &f[84], 4);
    const size_t size = four == 0x31545844 ? 8 : four == 0x35545844 ? 16 : 0;
    if (!size || !w || !h || w % 4 || h % 4) return false;
    const size_t bw = w / 4, bh = h / 4;
    if (f.size() < 128 + bw * bh * size) return false;
    std::vector<unsigned char> o(128 + bw * bh * 4 * size + (f.size() - 128));
    memcpy(o.data(), f.data(), 128);
    const uint32_t nw = w * 2, nh = h * 2, linear = (uint32_t)(bw * bh * 4 * size), more = (mips ? mips : 1) + 1, flags = 0x20000 | 0x80000;
    uint32_t have; memcpy(&have, &o[8], 4); have |= flags;
    memcpy(&o[8], &have, 4); memcpy(&o[12], &nh, 4); memcpy(&o[16], &nw, 4); memcpy(&o[20], &linear, 4); memcpy(&o[28], &more, 4);
    for (size_t y = 0; y < bh * 2; y++) for (size_t x = 0; x < bw * 2; x++) {
        const unsigned char *s = &f[128 + ((y / 2) * bw + x / 2) * size];
        unsigned char *d = &o[128 + (y * bw * 2 + x) * size];
        const int qx = (int)(x & 1) * 2, qy = (int)(y & 1) * 2;
        if (size == 16) {
            uint64_t from = 0, to = 0;
            memcpy(&from, s + 2, 6);
            for (int i = 0; i < 16; i++) to |= (from >> (3 * ((qy + (i >> 3)) * 4 + qx + ((i & 3) >> 1))) & 7) << (3 * i);
            d[0] = s[0]; d[1] = s[1];
            memcpy(d + 2, &to, 6);
            s += 8; d += 8;
        }
        uint32_t from, to = 0;
        memcpy(d, s, 4);
        memcpy(&from, s + 4, 4);
        for (int i = 0; i < 16; i++) to |= (from >> (2 * ((qy + (i >> 3)) * 4 + qx + ((i & 3) >> 1))) & 3) << (2 * i);
        memcpy(d + 4, &to, 4);
    }
    memcpy(&o[128 + bw * bh * 4 * size], &f[128], f.size() - 128);
    f.swap(o);
    return true;
}
// How far across the vehicle a place is on a sheet from side to side, y from the middle, drop down from the top of the vehicle. The
// cover goes over the top and down the sides, but at the middle the drop has to be nothing: a roof lies well below the top of a
// turret, and counted in full the drop puts the two halves of the roof that far apart, with a tear along the vehicle. So it comes
// in with the distance from the middle, all of it at the sides.
static float camo_across(float y, float drop, float half) {
    float d = fabsf(y), k = half > 0 ? d / half : 1;
    k = k > 1 ? 1 : k;
    k = k * k * (3 - 2 * k);
    float b = d + drop * k;
    return y < 0 ? -b : b;
}
// one texel of the picture between its pixels, the picture repeating
static void camo_sample(const uint8_t *pattern, int w, int h, float u, float v, float out[4]) {
    float x = (u - floorf(u)) * w - .5f, y = (v - floorf(v)) * h - .5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = (x0 + w) % w; x1 = x1 % w; y0 = (y0 + h) % h; y1 = y1 % h;
    const uint8_t *a = pattern + ((size_t)y0 * w + x0) * 4, *b = pattern + ((size_t)y0 * w + x1) * 4;
    const uint8_t *c = pattern + ((size_t)y1 * w + x0) * 4, *d = pattern + ((size_t)y1 * w + x1) * 4;
    for (int i = 0; i < 4; ++i)
        out[i] = (a[i] * (1 - fx) + b[i] * fx) * (1 - fy) + (c[i] * (1 - fx) + d[i] * fx) * fy;
}
// base with the picture's point u, v on it
static void camo_blend(const uint8_t *pattern, int w, int h, float u, float v,
                       const uint8_t base[4], const CamoSettings &s, uint8_t out[4]) {
    float p[4];
    camo_sample(pattern, w, h, u, v, p);
    float alpha = s.opacity * p[3] / 255.f;
    for (int c = 0; c < 3; ++c) {
        float b = base[c] / 255.f, a = fminf(1, p[c] / 255.f * (s.blend == 0 ? s.dim : 1) * s.bright);
        float mix = s.blend == 1 ? b * a : s.blend == 2 ?
            (b <= .5f ? 2*b*a : 1-2*(1-b)*(1-a)) : a;
        out[c] = (uint8_t)(fminf(255, fmaxf(0, 255 * (b + alpha * (mix - b)))) + .5f);
    }
    out[3] = base[3]; // foliage/mesh cutout alpha belongs to the original material
}
static void camo_pixel(const uint8_t *pattern, int w, int h, float u, float v,
                       const uint8_t base[4], const CamoSettings &s, uint8_t out[4]) {
    float angle = s.rotation * 0.017453292519943295f;
    float x = (u - .5f) * s.scale, y = (v - .5f) * s.scale;
    float aspect = h > 0 ? (float)w / h : 1;        // the picture keeps its shape: scale is how many fit in height
    u = (cosf(angle) * x - sinf(angle) * y) / aspect + .5f + s.offset[0];
    v = sinf(angle) * x + cosf(angle) * y + .5f + s.offset[1];
    camo_blend(pattern, w, h, u, v, base, s, out);
}
