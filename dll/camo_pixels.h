// CPU reference for the texture compositor; independent of the game and graphics API.
#pragma once
#include <math.h>
#include <stdint.h>
struct CamoSettings {
    float offset[2] = {0, 0};
    float scale = 5, rotation = 0, opacity = 1;
    int blend = 0;
    int mode = 0;                                   // 0 one picture on the vehicle (camo.cpp), 1 a pattern over the texture
    int repeat = 1;                                 // mode 0: the picture goes on mirrored beyond its edges
    float size = 1;                                 // mode 0: 1 = the smallest picture that covers the whole vehicle
    int through = 0;                                // mode 0: 0 both sides alike, mirrored along the roof; 1 one sheet over it all
    int flip_x = 0, flip_y = 0;                     // mode 0: the picture turned over
    float dim = 1;                                  // the picture's level, in plain blending (camo.cpp, picture_level); not a setting
};
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
        float b = base[c] / 255.f, a = p[c] / 255.f * (s.blend == 0 ? s.dim : 1);
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
