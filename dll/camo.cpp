#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <stdio.h>
#include <vector>
#include <algorithm>
#include <string>
#include <memory>
#include <cstdint>
#include "imgui.h"
#include "goh_replay.h"
#include "camo.h"
#include "camo_pixels.h"
extern "C" {
unsigned char *stbi_load_from_memory(const unsigned char *, int, int *, int *, int *, int);
int stbi_info_from_memory(const unsigned char *, int, int *, int *, int *);
void stbi_image_free(void *);
}
namespace {
constexpr uintptr_t G_DESKTOP = 0x159ca68;
uintptr_t base;
wchar_t folder[MAX_PATH], ini[MAX_PATH];
SRWLOCK lock = SRWLOCK_INIT;
struct Panel { bool shown; float x, y, w, h; } panel, replays_list, lobby_info, day_row;
bool floating, drawn;
volatile bool lobby_open, from_lobby_close;           // the lobby is on screen; a battle began
bool from_lobby;                                       // the window was opened from the lobby
volatile LONG importing;
HWND owner;
struct Import { std::vector<unsigned char> png; std::string name, error; } pending;
struct Preset { CamoSettings settings; std::string name; int id; };
std::vector<Preset> presets;
int next_picture;                                      // a deleted picture's number is not given again: textures are named by it
// A vehicle's camouflage: its picture (a preset's file) and how it lies, with the vehicle's textures, as found in its
// model: all of them take the picture, the main ones tell the vehicle in a battle.
// A sticker: one picture at one place on the vehicle, a projector from outside onto the surface around that point.
struct Sticker { int picture; float c[3], n[3], size, rotation, opacity, soft, wear; int flip_x, flip_y; };
struct Assign { std::string model; int picture; CamoSettings s; std::vector<std::string> all, main; std::vector<Sticker> stickers; };
std::vector<Assign> assigns;
wchar_t vehicles_ini[MAX_PATH];
int selected = -1;
std::string status;
bool dirty;

bool read(uintptr_t p, void *out, size_t n) {
    SIZE_T got = 0;
    return p && ReadProcessMemory(GetCurrentProcess(), (void *)p, out, n, &got) && got == n;
}
uintptr_t q(uintptr_t p) { uintptr_t v = 0; read(p, &v, 8); return v; }
struct GameString { union { char buf[16]; const char *ptr; }; size_t size, cap; };
void *find(void *layout, const char *name) {
    GameString s = {}; s.size = strlen(name); s.cap = 15;
    if (s.size > 15) { s.ptr = name; s.cap = s.size; } else memcpy(s.buf, name, s.size);
    uintptr_t fn = q(q((uintptr_t)layout) + 0x1e0);
    if (fn < base + 0x1000 || fn >= base + 0xfec000) return nullptr;
    return ((void *(*)(void *, GameString *))fn)(layout, &s);
}
bool bounds(uintptr_t d, Panel &p) {
    int size[2]; if (!read(d + 0xa0, size, sizeof size)) return false;
    p = {false, 0, 0, (float)size[0], (float)size[1]};
    for (int depth = 0; d && depth < 72; ++depth) {
        unsigned flags; int pos[2];
        if (!read(d + 0x48, &flags, 4) || !(flags & 2) || !read(d + 0x98, pos, 8)) return false;
        p.x += pos[0]; p.y += pos[1];
        if (q(d) == base + 0x1101800) { p.shown = p.w > 0 && p.h > 0; return p.shown; }
        d = q(d + 0xf0);
    }
    return false;
}
// ---- our own variant of a vehicle's texture -------------------------------------------------------------------
// The game finds a texture's variants by listing name#*.dds (55aef0, a list per texture). Ours is added to that list
// for the vehicles' colour textures; when the game then opens name#gohcamo.dds, the file opened is name.dds and what
// it holds is the same DDS with the pattern on it, every mip, same format and size. The alpha stays as it is.
constexpr uintptr_t FN_TEX_MODS = 0x55aef0, FN_MODS_GROW = 0x55c8d0, FN_MOD_IS = 0x556820, VT_ZIP = 0x1060678, VT_BUF = 0x10c6258;
constexpr uintptr_t TEX_MODS_CALLS[] = {0x55b606, 0x55c1bc, 0x71542a, 0x715b0a, 0x7174ba};
constexpr uintptr_t FN_MOD_NEW = 0x556310, FN_MOD_FREE = 0x5564e0;
// A vehicle's variant: gohcamo<picture>x<crc of model and settings>, so changed settings are a new texture for the game,
// which keeps textures by name. A second name, gohcamoid<crc of the model>, is only on the vehicle's main textures: a
// skeleton that has it is that vehicle (small textures are shared between vehicles). What the other threads may use of
// the camouflages is in `published`, under its lock.
struct Published { std::string token, id_token; int id; CamoSettings s; std::string model; std::vector<std::string> all, main; std::vector<Sticker> stickers; };
std::vector<Published> published;
SRWLOCK published_lock = SRWLOCK_INIT;
struct Vec { unsigned char *begin, *end, *cap; };
void *(*tex_mods_real)(void *, void *);
// A damaged part comes from the vehicle's _x model, a burned one from its _xx: the same layout of the texture, a texture of
// their own, is2_44_x beside is2_44. They are the same texture to the camouflage: without that it comes off where hit.
std::string undamaged(std::string s) {
    size_t n = s.size();
    if (n > 3 && !s.compare(n - 3, 3, "_xx")) s.resize(n - 3);
    else if (n > 2 && !s.compare(n - 2, 2, "_x")) s.resize(n - 2);
    return s;
}
// Tracks are not painted. A half-track's hull is: m3a1_halftrack is the vehicle, not its tracks.
bool is_track(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    for (size_t at; (at = s.find("halftrack")) != std::string::npos; ) s.erase(at, 9);
    return s.find("track") != std::string::npos;
}
bool is_camo_texture(const char *s, size_t n) {
    std::string k(s, n);
    for (auto &c : k) c = (char)tolower((unsigned char)c);
    if (k.find("_vehicle/") == std::string::npos || k.find("/tracks/") != std::string::npos || k.find('#') != std::string::npos) return false;
    size_t dot = k.rfind('.');
    if (dot != std::string::npos && dot + 5 >= k.size()) k.resize(dot);
    k = undamaged(k);
    for (const char *end : {"_bp", "_sp", "_lm", "_nm", "_ao"})
        if (k.size() > 3 && !k.compare(k.size() - 3, 3, end)) return false;
    return true;
}
std::string stem_of(const char *s, size_t n) {
    std::string k(s, n);
    for (auto &c : k) c = (char)tolower((unsigned char)c);
    size_t at = k.find("/texture/common/");
    if (at != std::string::npos) k = k.substr(at + 16);
    size_t dot = k.rfind('.');
    if (dot != std::string::npos && dot + 5 >= k.size()) k.resize(dot);
    return k;
}
void add_variant(Vec *v, const char *token) {
    auto is = (bool (*)(void *, const char *))(base + FN_MOD_IS);
    for (unsigned char *e = v->begin; e < v->end; e += 0x68) if (is(e, token)) return;
    alignas(16) unsigned char entry[0x80] = {};
    ((void *(*)(void *, const char *))(base + FN_MOD_NEW))(entry, token);
    ((void *(*)(Vec *, unsigned char *, void *))(base + FN_MODS_GROW))(v, v->end, entry);
    ((void (*)(void *))(base + FN_MOD_FREE))(entry);
}
void *tex_mods(void *map, void *key) {
    Vec *v = (Vec *)tex_mods_real(map, key);
    const GameString *k = (const GameString *)key;
    const char *s = k->cap > 15 ? k->ptr : k->buf;
    if (!v || k->size > 400 || !is_camo_texture(s, k->size)) return v;
    std::string stem = undamaged(stem_of(s, k->size));
    AcquireSRWLockShared(&published_lock);
    for (const auto &e : published) {
        if (std::find(e.all.begin(), e.all.end(), stem) != e.all.end()) add_variant(v, e.token.c_str());
        if (std::find(e.main.begin(), e.main.end(), stem) != e.main.end()) add_variant(v, e.id_token.c_str());
    }
    ReleaseSRWLockShared(&published_lock);
    return v;
}

// the pattern on the textures: for now a test pattern of three colours
struct Pattern;
// A sticker: a projector at its point, looking straight onto the surface there. Over the picture lies a grid: `chosen` is the
// floor the sticker lies on in a cell, `levels` all the heights there it paints (the floor and all that stands on it);
// `stretch` lengthens distances from the middle by what the surface bends away.
struct StickerImage {
    std::shared_ptr<Pattern> pic;
    float c[3], n[3], e1[3], e2[3], half_w, half_h, opacity, wear;
    bool flip_x = false, flip_y = false;
    int gx = 0, gy = 0;
    float cell = 1;
    std::vector<float> chosen, levels, stretch;
};
struct Pattern { std::vector<unsigned char> px; int w = 0, h = 0; CamoSettings s; std::string model; std::vector<StickerImage> stickers; };
struct VehicleMap;
void free_projection(VehicleMap *);
Pattern test_pattern() {
    Pattern p; p.w = p.h = 256; p.px.resize(256 * 256 * 4); p.s.opacity = .8f; p.s.scale = 3;
    static const unsigned char C[3][3] = {{58, 74, 40}, {116, 96, 58}, {36, 32, 26}};
    for (int y = 0; y < 256; y++) for (int x = 0; x < 256; x++) {
        float a = x / 256.f * 6.2831853f, b = y / 256.f * 6.2831853f;
        float n = sinf(a * 2 + sinf(b * 3)) + sinf(b * 2 + sinf(a * 3) * 1.5f) + .5f * sinf((a + b) * 5);
        const unsigned char *c = C[n > .6f ? 0 : n < -.6f ? 2 : 1];
        unsigned char *o = &p.px[(y * 256 + x) * 4];
        o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = 255;
    }
    return p;
}
void rgb_of(uint16_t c, int o[3]) { o[0] = (c >> 11) * 255 / 31; o[1] = ((c >> 5) & 63) * 255 / 63; o[2] = (c & 31) * 255 / 31; }
uint16_t c565(const int c[3]) { return (uint16_t)(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 | ((c[2] * 31 + 127) / 255)); }
// one 8-byte colour block: pattern over its 16 pixels, written back
struct VehicleMap;
struct Placement { float cx, cy, top, width, height, ca, sa, ox, oy; bool through, repeat, flip_x, flip_y; };
Placement place_picture(const float lo[3], const float hi[3], int pw, int ph, const CamoSettings &s);
bool on_picture(const Placement &l, const float *P, float &u, float &v);
const float *texel_place(const VehicleMap &pr, size_t at);
void stick(const VehicleMap &pr, size_t at, const std::vector<StickerImage> &list, uint8_t io[4]);
Placement place_on(const VehicleMap &pr, const Pattern &p);
bool texel(const VehicleMap &pr, int x, int y, int w, int h, size_t &at);
void paint_pixel(const int rgb[3], int x, int y, int w, int h, const Pattern &p, float average, const VehicleMap *pr, const Placement &pl, int o[3]) {
    float lum = rgb[0] * .3f + rgb[1] * .59f + rgb[2] * .11f, keep = lum / average;
    keep = keep < .45f ? .45f : keep > 1.6f ? 1.6f : keep;
    uint8_t flat[4], out[4];
    for (int c = 0; c < 3; c++) { float f = rgb[c] / keep; flat[c] = (uint8_t)(f > 255 ? 255 : f); }
    flat[3] = 255;
    if (pr) {
        size_t at;
        float u, v;
        memcpy(out, flat, 4);
        if (texel(*pr, x, y, w, h, at)) {
            if (p.w && on_picture(pl, texel_place(*pr, at), u, v)) camo_blend(p.px.data(), p.w, p.h, u, v, flat, p.s, out);
            stick(*pr, at, p.stickers, out);
        }
    } else if (p.w) camo_pixel(p.px.data(), p.w, p.h, (x + .5f) / w, (y + .5f) / h, flat, p.s, out);
    else memcpy(out, flat, 4);
    for (int c = 0; c < 3; c++) {
        int v = (int)(out[c] * keep);
        o[c] = v < 0 ? 0 : v > 255 ? 255 : v;
    }
}
void block(unsigned char *b, int bx, int by, int w, int h, const Pattern &p, float average, const VehicleMap *pr, const Placement &pl) {
    uint16_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    uint32_t bits = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
    int pal[4][3];
    rgb_of(c0, pal[0]); rgb_of(c1, pal[1]);
    for (int i = 0; i < 3; i++) {
        if (c0 > c1) { pal[2][i] = (2 * pal[0][i] + pal[1][i]) / 3; pal[3][i] = (pal[0][i] + 2 * pal[1][i]) / 3; }
        else { pal[2][i] = (pal[0][i] + pal[1][i]) / 2; pal[3][i] = 0; }
    }
    int px[16][3], lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
        int k = bits >> (2 * i) & 3, x = bx + (i & 3), y = by + (i >> 2);
        paint_pixel(pal[k], x, y, w, h, p, average, pr, pl, px[i]);
        for (int c = 0; c < 3; c++) {
            if (px[i][c] < lo[c]) lo[c] = px[i][c];
            if (px[i][c] > hi[c]) hi[c] = px[i][c];
        }
    }
    uint16_t a = c565(hi), z = c565(lo);
    if (a <= z) { uint32_t none = 0; b[0] = a & 255; b[1] = a >> 8; b[2] = z & 255; b[3] = z >> 8; memcpy(b + 4, &none, 4); return; }
    int e0[3], e1[3], d[3], len = 0;
    rgb_of(a, e0); rgb_of(z, e1);
    for (int c = 0; c < 3; c++) { d[c] = e1[c] - e0[c]; len += d[c] * d[c]; }
    uint32_t out = 0;
    for (int i = 0; i < 16; i++) {
        int dot = 0;
        for (int c = 0; c < 3; c++) dot += (px[i][c] - e0[c]) * d[c];
        int step = len ? (dot * 3 + len / 2) / len : 0;
        step = step < 0 ? 0 : step > 3 ? 3 : step;
        static const int INDEX[4] = {0, 2, 3, 1};
        out |= (uint32_t)INDEX[step] << (2 * i);
    }
    b[0] = a & 255; b[1] = a >> 8; b[2] = z & 255; b[3] = z >> 8; memcpy(b + 4, &out, 4);
}
// a mod's texture can be plain 32 bit pixels; each mask says where a colour is
int shift_of(uint32_t mask) { int s = 0; while (mask && !(mask & 1)) { mask >>= 1; s++; } return s; }
bool paint_plain(unsigned char *f, size_t n, const Pattern &p, const VehicleMap *pr, uint32_t w, uint32_t h, uint32_t mips) {
    uint32_t mask[3];
    memcpy(mask, f + 92, 12);
    if (!mask[0] || !mask[1] || !mask[2]) return false;
    int shift[3] = {shift_of(mask[0]), shift_of(mask[1]), shift_of(mask[2])};
    double sum = 0;
    size_t count = (size_t)w * h;
    if (128 + count * 4 > n) return false;
    for (size_t i = 0; i < count; i += 16) {
        uint32_t v;
        memcpy(&v, f + 128 + i * 4, 4);
        sum += (v >> shift[0] & 255) * .3 + (v >> shift[1] & 255) * .59 + (v >> shift[2] & 255) * .11;
    }
    float average = sum > 8.0 * (count / 16) ? (float)(sum / (count / 16 + 1)) : 90.f;
    Placement pl = {};
    if (pr) pl = place_on(*pr, p);
    size_t at = 128;
    for (uint32_t m = 0; m < mips && w && h; m++) {
        if (at + (size_t)w * h * 4 > n) break;
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            unsigned char *b = f + at + ((size_t)y * w + x) * 4;
            uint32_t v;
            memcpy(&v, b, 4);
            int rgb[3] = {(int)(v >> shift[0] & 255), (int)(v >> shift[1] & 255), (int)(v >> shift[2] & 255)}, o[3];
            paint_pixel(rgb, x, y, w, h, p, average, pr, pl, o);
            v &= ~(mask[0] | mask[1] | mask[2]);
            for (int c = 0; c < 3; c++) v |= (uint32_t)o[c] << shift[c];
            memcpy(b, &v, 4);
        }
        at += (size_t)w * h * 4;
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    return true;
}
bool paint_dds(unsigned char *f, size_t n, const Pattern &p, const VehicleMap *pr) {
    if (n < 128 || memcmp(f, "DDS ", 4)) return false;
    uint32_t h, w, mips, four, bits;
    memcpy(&h, f + 12, 4); memcpy(&w, f + 16, 4); memcpy(&mips, f + 28, 4); memcpy(&four, f + 84, 4); memcpy(&bits, f + 88, 4);
    int size = four == 0x31545844 ? 8 : four == 0x35545844 || four == 0x33545844 ? 16 : 0;       // DXT1, DXT5, DXT3
    if (!w || !h) return false;
    if (!mips) mips = 1;
    if (!four && bits == 32) return paint_plain(f, n, p, pr, w, h, mips);
    if (!size) return false;
    double sum = 0;
    size_t blocks = 0;
    for (size_t o = 128 + size - 8, top = 128 + (size_t)((w + 3) / 4) * ((h + 3) / 4) * size; o + 8 <= top && o + 8 <= n; o += size * 16) {
        int c[3];
        rgb_of((uint16_t)(f[o] | f[o + 1] << 8), c);
        sum += c[0] * .3 + c[1] * .59 + c[2] * .11;
        blocks++;
    }
    float average = blocks && sum > blocks * 8.0 ? (float)(sum / blocks) : 90.f;
    Placement pl = {};
    if (pr) pl = place_on(*pr, p);
    size_t at = 128;
    for (uint32_t m = 0; m < mips && w && h; m++) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (at + (size_t)bw * bh * size > n) break;
        for (uint32_t y = 0; y < bh; y++) for (uint32_t x = 0; x < bw; x++) {
            unsigned char *b = f + at + ((size_t)y * bw + x) * size + (size - 8);
            uint16_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
            uint32_t bits; memcpy(&bits, b + 4, 4);
            bool cut = false;                                                // DXT1's holes: left alone
            if (size == 8 && c0 <= c1) for (int i = 0; i < 16; i++) cut |= (bits >> (2 * i) & 3) == 3;
            if (!cut) block(b, x * 4, y * 4, w, h, p, average, pr, pl);
        }
        at += (size_t)bw * bh * size;
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    return true;
}
SRWLOCK paint_lock = SRWLOCK_INIT;
struct Painted { std::string name; std::vector<unsigned char> data; };
std::vector<Painted *> painted;
void filename(int id, wchar_t *out);
bool readfile(const wchar_t *path, std::vector<unsigned char> &out);
// The game's light is made for its own colour textures, and they are dark: 52 of 255 on average over the 687 main textures of its
// vehicles (the middle 80% of them 38 to 66). A picture of ordinary brightness, 120 to 160, comes out overexposed on a vehicle. So a
// brighter one comes down to that level as a whole, its colours keeping their proportions; a darker one stays. Multiply and overlay
// work from the texture's own colour, they do not need it.
constexpr float VEHICLE_LEVEL = 52;
float picture_level(const std::vector<unsigned char> &px) {
    double sum = 0, weight = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        double a = px[i + 3];
        sum += a * (px[i] * .3 + px[i + 1] * .59 + px[i + 2] * .11);
        weight += a;
    }
    float mean = weight > 0 ? (float)(sum / weight) : 0;
    return mean > VEHICLE_LEVEL ? VEHICLE_LEVEL / mean : 1;
}
std::shared_ptr<Pattern> load_picture(int id) {
    wchar_t path[MAX_PATH];
    std::vector<unsigned char> bytes;
    int c, w, h;
    filename(id, path);
    if (!readfile(path, bytes)) return nullptr;
    unsigned char *d = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &c, 4);
    if (!d) return nullptr;
    auto r = std::make_shared<Pattern>();
    r->w = w; r->h = h; r->px.assign(d, d + (size_t)w * h * 4);
    r->s.dim = picture_level(r->px);
    stbi_image_free(d);
    return r;
}
// A soft sticker: the picture's alpha blurred, with nothing beyond its frame, so both the frame and the outline of what is
// drawn (the digits of a number on a clear PNG) fade out gently. The colours stay as they are.
std::shared_ptr<Pattern> softened(std::shared_ptr<Pattern> pic, float soft) {
    if (!pic || soft <= .001f) return pic;
    const int w = pic->w, h = pic->h, r = (int)(soft * .1f * (w < h ? w : h)) + 1;
    auto out = std::make_shared<Pattern>(*pic);
    std::vector<float> a((size_t)w * h), b((size_t)w * h);
    for (size_t i = 0; i < a.size(); i++) a[i] = pic->px[i * 4 + 3];
    for (int pass = 0; pass < 2; pass++) {            // two box blurs, near enough a soft edge
        for (int y = 0; y < h; y++) {
            double sum = 0;
            for (int x = -r; x < w; x++) {
                if (x + r < w) sum += a[(size_t)y * w + x + r];
                if (x - r - 1 >= 0) sum -= a[(size_t)y * w + x - r - 1];
                if (x >= 0) b[(size_t)y * w + x] = (float)(sum / (2 * r + 1));
            }
        }
        for (int x = 0; x < w; x++) {
            double sum = 0;
            for (int y = -r; y < h; y++) {
                if (y + r < h) sum += b[(size_t)(y + r) * w + x];
                if (y - r - 1 >= 0) sum -= b[(size_t)(y - r - 1) * w + x];
                if (y >= 0) a[(size_t)y * w + x] = (float)(sum / (2 * r + 1));
            }
        }
    }
    for (size_t i = 0; i < a.size(); i++) {
        float keep = a[i] / 255;                      // where the picture is see-through its colour must not darken the edge
        out->px[i * 4 + 3] = (unsigned char)(fminf(a[i], 255.f) * (pic->px[i * 4 + 3] ? 1.f : fminf(keep * 2, 1.f)));
    }
    return out;
}
void normalize(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-9f;
    for (int i = 0; i < 3; i++) v[i] /= l;
}
void cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
// seen from outside: right along e1, down along e2, so the picture is the right way round
StickerImage sticker_image(const Sticker &s, std::shared_ptr<Pattern> pic) {
    StickerImage r;
    memcpy(r.c, s.c, sizeof r.c); memcpy(r.n, s.n, sizeof r.n);
    normalize(r.n);
    float up[3] = {0, 0, 1}, right[3], down[3];
    if (fabsf(r.n[2]) > .9f) { up[0] = 1; up[2] = 0; }
    cross(up, r.n, right); normalize(right);
    cross(right, r.n, down);
    float a = s.rotation * .017453292f, ca = cosf(a), sa = sinf(a);
    for (int i = 0; i < 3; i++) { r.e1[i] = ca * right[i] + sa * down[i]; r.e2[i] = ca * down[i] - sa * right[i]; }
    r.pic = pic;
    r.half_w = s.size / 2;
    r.half_h = pic && pic->w ? s.size / 2 * pic->h / pic->w : s.size / 2;
    r.opacity = s.opacity;
    r.wear = s.wear;
    r.flip_x = s.flip_x; r.flip_y = s.flip_y;
    return r;
}
Pattern pattern_of(const std::string &token) {
    Pattern p;
    int id = -1;
    bool known = false;
    std::vector<Sticker> stickers;
    AcquireSRWLockShared(&published_lock);
    for (auto &e : published) if (e.token == token) { id = e.id; p.s = e.s; p.model = e.model; stickers = e.stickers; known = true; }
    ReleaseSRWLockShared(&published_lock);
    for (const Sticker &s : stickers) if (auto pic = load_picture(s.picture)) p.stickers.push_back(sticker_image(s, softened(pic, s.soft)));
    if (id >= 0) {
        if (auto pic = load_picture(id)) { p.px = pic->px; p.w = pic->w; p.h = pic->h; p.s.dim = pic->s.dim; return p; }
    }
    if (known) return p;                                // stickers only
    rc_log("camo: no picture for %s, test pattern", token.c_str());
    return test_pattern();
}
bool make_projection(const std::string &file, const unsigned char *f, size_t n, const std::string &model, std::vector<StickerImage> &stickers, VehicleMap *&out);
Painted *painted_of(const std::string &name, const std::string &token, const unsigned char *f, size_t n) {
    AcquireSRWLockExclusive(&paint_lock);
    Painted *r = nullptr;
    std::string key = name + "#" + token;
    for (auto *e : painted) if (e->name == key && e->data.size() == n) r = e;
    if (!r) {
        DWORD t0 = GetTickCount();
        static std::string last;
        static Pattern p;
        if (last != token) { p = pattern_of(token); last = token; }
        r = new Painted{key, std::vector<unsigned char>(f, f + n)};
        VehicleMap *pr = nullptr;
        bool tracks = !p.s.mode && is_track(name);     // nobody paints those
        if (!p.s.mode && !tracks) make_projection(name, f, n, p.model, p.stickers, pr);
        bool ok = tracks || paint_dds(r->data.data(), n, p, pr);
        free_projection(pr);
        rc_log("camo: %s painted in %lu ms: %s", key.c_str(), GetTickCount() - t0, ok ? "ok" : "a kind of DDS not known, left as it is");
        painted.push_back(r);
    }
    ReleaseSRWLockExclusive(&paint_lock);
    return r;
}
// The engine's own per-vehicle texture variants ("texmods": the files name#winter.dds next to name.dds, the
// #burned of wrecks). eSkeleton::SetTexMod (714960) works on one skeleton, so on one vehicle: entity+0xb0.
// Selection: eGameControl [159e218], its selection at +0xd8, entries {entity, ...} 16 bytes from sel+8 to sel+0x10.
// Test, Shift+F12: the selected vehicles get our variant, or lose it.
constexpr uintptr_t GAME_CONTROL = 0x159e218, FN_MOD_VALID = 0x715180,
                    FN_SET_TEXMOD = 0x714960;
bool has_mod(uintptr_t skeleton, const char *name) {
    alignas(16) unsigned char mod[0x80];
    ((void *(*)(void *, const char *))(base + FN_MOD_NEW))(mod, name);
    bool valid = ((bool (*)(uintptr_t, void *))(base + FN_MOD_VALID))(skeleton, mod);
    ((void (*)(void *))(base + FN_MOD_FREE))(mod);
    return valid;
}
void set_texmod(uintptr_t skeleton, const char *name) {
    alignas(16) unsigned char mod[0x80];
    ((void *(*)(void *, const char *))(base + FN_MOD_NEW))(mod, name);
    ((void (*)(uintptr_t, void *, bool, bool))(base + FN_SET_TEXMOD))(skeleton, mod, false, true);
    ((void (*)(void *))(base + FN_MOD_FREE))(mod);
}
// Every actor passes here on its turn in the game's thread (eActor's slot 23). One of the local player's vehicles with
// a camouflage set for it gets it, once: a wreck's own #burned comes after. a464f0 is the game's "local player".
constexpr uintptr_t VT_ACTOR = 0x10de138, FN_ACTOR_QUANT = 0x7efd10, G_LOCAL_PLAYER = 0x1487a88, GAME_MODE_OBJ = 0x159c760;
volatile LONG battle_number, published_count;
void *(*actor_quant_real)(void *);
void camouflage(unsigned char *actor) {
    static std::vector<uintptr_t> done;               // sorted skeletons; this thread only
    static LONG battle;
    static int logged;
    if (battle != battle_number) { battle = battle_number; done.clear(); }
    if (!published_count || actor[0xda8] != *(unsigned char *)(base + G_LOCAL_PLAYER) || !(actor[0x30] & 2)) return;
    uintptr_t skeleton = *(uintptr_t *)(actor + 0xb0);
    if (!skeleton) return;
    auto at = std::lower_bound(done.begin(), done.end(), skeleton);
    if (at != done.end() && *at == skeleton) return;
    done.insert(at, skeleton);
    // has_mod comes back to tex_mods, which takes the lock: not under it
    AcquireSRWLockShared(&published_lock);
    std::vector<std::pair<std::string, std::string>> list;
    for (const auto &e : published) list.push_back({e.id_token, e.token});
    ReleaseSRWLockShared(&published_lock);
    std::string token;
    for (const auto &e : list) if (has_mod(skeleton, e.first.c_str())) { token = e.second; break; }
    if (token.empty()) return;
    set_texmod(skeleton, token.c_str());
    if (logged++ < 8) rc_log("camo: %s on a vehicle of player %d", token.c_str(), actor[0xda8]);
}
void *actor_quant(void *self) {
    camouflage((unsigned char *)self);
    return actor_quant_real(self);
}
volatile LONG always_day;                              // the box in the lobby; always_day.c does the rest
bool day_host;                                         // the last lobby was our own
void filename(int id, wchar_t *out) { swprintf(out, MAX_PATH, L"%s\\%03d.png", folder, id); }
bool readfile(const wchar_t *path, std::vector<unsigned char> &out) {
    FILE *f = _wfopen(path, L"rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    if (n < 24 || n > 32*1024*1024) { fclose(f); return false; }
    out.resize(n); bool ok = fread(out.data(), 1, n, f) == (size_t)n; fclose(f); return ok;
}
bool validate(const std::vector<unsigned char> &data, int &w, int &h) {
    static const unsigned char magic[] = {137,80,78,71,13,10,26,10};
    int c;
    return data.size() >= 24 && !memcmp(data.data(), magic, 8) &&
        stbi_info_from_memory(data.data(), (int)data.size(), &w, &h, &c) && w > 0 && h > 0 && w <= 4096 && h <= 4096;
}
std::shared_ptr<Pattern> picture;                     // the picture on the vehicle in the tab
void load_image(int index) {
    picture.reset(); dirty = true;
    if (index < 0 || index >= (int)presets.size()) return;
    wchar_t path[MAX_PATH]; filename(presets[index].id, path);
    std::vector<unsigned char> bytes;
    int w, h, c;
    if (!readfile(path, bytes) || !validate(bytes, w, h)) { status = TR("PNG не найден или повреждён", "PNG missing or invalid"); return; }
    unsigned char *p = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &c, 4);
    if (!p) { status = TR("Не удалось прочитать PNG", "Could not decode PNG"); return; }
    auto r = std::make_shared<Pattern>();
    r->w = w; r->h = h; r->px.assign(p, p + (size_t)w * h * 4); r->s.dim = picture_level(r->px); stbi_image_free(p); status.clear();
    picture = r;
}
std::string utf8(const wchar_t *s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr); out.pop_back(); return out;
}
uint32_t fnv(const void *data, size_t n, uint32_t h = 2166136261u) {
    const unsigned char *b = (const unsigned char *)data;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}
volatile LONG warm_needed;                             // camouflages changed: paint them ahead of the battle
void publish() {
    InterlockedExchange(&warm_needed, 1);
    std::vector<Published> list;
    for (const auto &a : assigns) {
        uint32_t id = fnv(a.model.data(), a.model.size()), crc = fnv(&a.s, sizeof a.s, id);
        if (!a.stickers.empty()) crc = fnv(a.stickers.data(), a.stickers.size() * sizeof a.stickers[0], crc);
        char token[48], id_token[48];
        snprintf(token, sizeof token, "gohcamo%dx%08x", a.picture, crc);
        snprintf(id_token, sizeof id_token, "gohcamoid%08x", id);
        list.push_back({token, id_token, a.picture, a.s, a.model, a.all, a.main, a.stickers});
    }
    AcquireSRWLockExclusive(&published_lock);
    published = std::move(list);
    InterlockedExchange(&published_count, (LONG)published.size());
    ReleaseSRWLockExclusive(&published_lock);
}
std::string joined(const std::vector<std::string> &v) {
    std::string r;
    for (const auto &s : v) { if (!r.empty()) r += '|'; r += s; }
    return r;
}
std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> r;
    for (size_t at = 0; at <= s.size();) {
        size_t e = s.find('|', at);
        if (e == std::string::npos) e = s.size();
        if (e > at) r.push_back(s.substr(at, e - at));
        at = e + 1;
    }
    return r;
}
void save_vehicles() {
    wchar_t tmp[MAX_PATH]; swprintf(tmp, MAX_PATH, L"%s.tmp", vehicles_ini);
    FILE *f = _wfopen(tmp, L"wb"); if (!f) { status = TR("Не удалось сохранить настройки техники", "Cannot save vehicles"); return; }
    fprintf(f, "GOH_CAMO_V2\n");
    for (const auto &a : assigns) {
        fprintf(f, "%d %.9g %.9g %.9g %.9g %d %d %.9g %d %d %d\t%s\t%s\t%s\n", a.picture,
            a.s.offset[0], a.s.offset[1], a.s.rotation, a.s.opacity, a.s.blend, a.s.repeat, a.s.size, a.s.through, a.s.flip_x, a.s.flip_y,
            a.model.c_str(), joined(a.all).c_str(), joined(a.main).c_str());
        for (const auto &s : a.stickers) fprintf(f, "S %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %d %d\n", s.picture,
            s.c[0], s.c[1], s.c[2], s.n[0], s.n[1], s.n[2], s.size, s.rotation, s.opacity, s.soft, s.wear, s.flip_x, s.flip_y);
    }
    bool ok = !ferror(f); if (fclose(f)) ok = false;
    if (!ok || !MoveFileExW(tmp, vehicles_ini, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) status = TR("Не удалось сохранить настройки техники", "Cannot save vehicles");
}
void load_vehicles() {
    FILE *f = _wfopen(vehicles_ini, L"rb");
    if (!f) return;
    static char line[8192];
    if (fgets(line, sizeof line, f) && (!strcmp(line, "GOH_CAMO_V1\n") || !strcmp(line, "GOH_CAMO_V2\n"))) while (assigns.size() < 2000 && fgets(line, sizeof line, f)) {
        if (line[0] == 'S' && line[1] == ' ') {
            Sticker k = {};
            if (!assigns.empty() && assigns.back().stickers.size() < 64 && sscanf(line + 2, "%d %f %f %f %f %f %f %f %f %f %f %f %d %d", &k.picture, &k.c[0], &k.c[1], &k.c[2],
                       &k.n[0], &k.n[1], &k.n[2], &k.size, &k.rotation, &k.opacity, &k.soft, &k.wear, &k.flip_x, &k.flip_y) >= 10 && isfinite(k.size) && k.size > 0 && isfinite(k.c[0]) &&
                isfinite(k.c[1]) && isfinite(k.c[2]) && isfinite(k.n[0]) && isfinite(k.n[1]) && isfinite(k.n[2]) && isfinite(k.opacity) && isfinite(k.soft) && isfinite(k.wear))
                assigns.back().stickers.push_back(k);
            continue;
        }
        Assign a; auto &s = a.s;
        if (sscanf(line, "%d %f %f %f %f %d %d %f %d %d %d", &a.picture, &s.offset[0], &s.offset[1], &s.rotation, &s.opacity,
                   &s.blend, &s.repeat, &s.size, &s.through, &s.flip_x, &s.flip_y) != 11) continue;
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        size_t t0 = l.find('\t'), t1 = t0 == std::string::npos ? t0 : l.find('\t', t0 + 1), t2 = t1 == std::string::npos ? t1 : l.find('\t', t1 + 1);
        if (t2 == std::string::npos || !isfinite(s.size) || s.size < .05f || s.size > 20 || !isfinite(s.opacity) || s.opacity < 0 || s.opacity > 1 ||
            !isfinite(s.offset[0]) || !isfinite(s.offset[1]) || !isfinite(s.rotation) || s.blend < 0 || s.blend > 2) continue;
        a.model = l.substr(t0 + 1, t1 - t0 - 1);
        a.all = split(l.substr(t1 + 1, t2 - t1 - 1));
        a.main = split(l.substr(t2 + 1));
        assigns.push_back(a);
    }
    fclose(f);
}
void save() {
    publish();
    save_vehicles();
    wchar_t tmp[MAX_PATH]; swprintf(tmp, MAX_PATH, L"%s.tmp", ini);
    FILE *f = _wfopen(tmp, L"wb"); if (!f) { status = TR("Не удалось сохранить список картинок", "Cannot save presets"); return; }
    fprintf(f, "GOH_CAMO_2\n");
    for (const auto &p : presets) fprintf(f, "%d %.9g %.9g %.9g %.9g %.9g %d %d %d %.9g %s\n", p.id,
        p.settings.offset[0], p.settings.offset[1], p.settings.scale, p.settings.rotation,
        p.settings.opacity, p.settings.blend, p.settings.mode, p.settings.repeat, p.settings.size, p.name.c_str());
    bool ok = !ferror(f); if (fclose(f)) ok = false;
    if (!ok || !MoveFileExW(tmp, ini, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) status = TR("Не удалось сохранить список картинок", "Cannot save presets");
}
DWORD WINAPI pick(LPVOID) {
    wchar_t path[32768] = {};
    OPENFILENAMEW of = {}; of.lStructSize = sizeof of; of.hwndOwner = owner;
    of.lpstrFilter = L"PNG images\0*.png\0\0"; of.lpstrFile = path; of.nMaxFile = 32768;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    Import result;
    if (GetOpenFileNameW(&of)) {
        int w, h;
        if (!readfile(path, result.png) || !validate(result.png, w, h)) {
            result.png.clear(); result.error = TR("Нужен PNG до 4096 x 4096 и до 32 МБ", "Use a valid PNG, up to 4096 x 4096 and 32 MB");
        } else {
            const wchar_t *name = wcsrchr(path, L'\\'); result.name = utf8(name ? name + 1 : path);
            for (char &c : result.name) if (c == '\n' || c == '\r') c = ' ';
        }
    } else if (CommDlgExtendedError()) result.error = TR("Не удалось открыть окно выбора файла", "Could not open the file picker");
    AcquireSRWLockExclusive(&lock); pending = std::move(result); ReleaseSRWLockExclusive(&lock);
    InterlockedExchange(&importing, 2); return 0;
}
bool imported;                                         // a picture just came in: it goes on the vehicle in the tab
void accept_import() {
    if (InterlockedCompareExchange(&importing, 2, 2) != 2) return;
    AcquireSRWLockExclusive(&lock); Import r = std::move(pending); ReleaseSRWLockExclusive(&lock);
    InterlockedExchange(&importing, 0);
    if (!r.error.empty()) { status = r.error; return; }
    if (r.png.empty()) return;
    int id = next_picture; for (const auto &p : presets) if (p.id >= id) id = p.id + 1;
    wchar_t path[MAX_PATH]; filename(id, path);
    FILE *f = _wfopen(path, L"wb"); if (!f) { status = TR("Не удалось сохранить загруженный PNG", "Cannot store the imported PNG"); return; }
    bool ok = fwrite(r.png.data(), 1, r.png.size(), f) == r.png.size(); if (fclose(f)) ok = false;
    if (!ok) { DeleteFileW(path); status = TR("Не удалось сохранить загруженный PNG", "Cannot store the imported PNG"); return; }
    presets.push_back({{}, r.name, id}); selected = (int)presets.size()-1; imported = true; save(); load_image(selected);
}
}
extern "C" void camo_init(const char *dir) {
    base = (uintptr_t)GetModuleHandleW(nullptr);
    wchar_t path[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, dir, -1, path, MAX_PATH);
    swprintf(folder, MAX_PATH, L"%sgoh_camo", path); CreateDirectoryW(folder, nullptr);
    swprintf(ini, MAX_PATH, L"%s\\presets.txt", folder);
    swprintf(vehicles_ini, MAX_PATH, L"%s\\vehicles.txt", folder);
    load_vehicles();
    FILE *f = _wfopen(ini, L"rb");
    if (f) {
        char line[1024];
        bool v2 = false;
        if (fgets(line, sizeof line, f) && (!strcmp(line, "GOH_CAMO_1\n") || (v2 = !strcmp(line, "GOH_CAMO_2\n")))) {
            while (presets.size() < 256 && fgets(line, sizeof line, f)) {
                Preset p; char name[512]; auto &s = p.settings;
                if (v2 ? sscanf(line, "%d %f %f %f %f %f %d %d %d %f %511[^\r\n]", &p.id, &s.offset[0], &s.offset[1],
                        &s.scale, &s.rotation, &s.opacity, &s.blend, &s.mode, &s.repeat, &s.size, name) != 11
                       : sscanf(line, "%d %f %f %f %f %f %d %511[^\r\n]", &p.id, &s.offset[0], &s.offset[1],
                        &s.scale, &s.rotation, &s.opacity, &s.blend, name) != 8) continue;
                if (!isfinite(s.size) || s.size < .05f || s.size > 20) s.size = 1;
                if (!v2) s.offset[0] = s.offset[1] = s.rotation = 0;
                if (p.id < 0 || p.id > 999999 || !isfinite(s.offset[0]) || !isfinite(s.offset[1]) ||
                    !isfinite(s.scale) || !isfinite(s.rotation) || !isfinite(s.opacity) || s.scale < .05f ||
                    s.scale > 32 || s.opacity < 0 || s.opacity > 1 || s.blend < 0 || s.blend > 2 ||
                    fabsf(s.offset[0]) > 8 || fabsf(s.offset[1]) > 8 || fabsf(s.rotation) > 180) continue;
                p.name = name; presets.push_back(p);
            }
        }
        fclose(f);
    }
    publish();
    rc_log("camo: editor initialized, %u presets", (unsigned)presets.size());
}
extern "C" void camo_page(void *) {}
extern "C" void camo_selftest_tick(const char *);
extern "C" void lobby_dump_tick(const char *);
extern "C" int dialog_covered(void *dialog);
void lowest_row(uintptr_t d, const Panel &area, Panel &last, int depth, int &budget) {
    if (!d || depth > 24 || --budget < 0) return;
    unsigned flags = 0;
    if (!read(d + 0x48, &flags, 4) || !(flags & 2)) return;
    Panel here = {};
    if (depth && bounds(d, here) && here.h >= 20 && here.h <= 60 && here.w > area.w * .6f && here.y + here.h <= area.y + area.h + 1 &&
        (!last.shown || here.y + here.h > last.y + last.h)) last = here;
    uintptr_t b = q(d + 0xd8), e = q(d + 0xe0);
    if (!b || e < b || e - b > 8 * 4096) return;
    for (uintptr_t k = b; k < e; k += 8) lowest_row(q(k), area, last, depth + 1, budget);
}
void warm_start();
extern "C" void camo_tick() {
    static bool in_game;
    uintptr_t mode = q(base + GAME_MODE_OBJ);
    unsigned char on = 0;
    if (mode) read(mode + 0x908, &on, 1);
    if ((on == 1) != in_game) { in_game = on == 1; InterlockedIncrement(&battle_number); }
    { char path[MAX_PATH]; snprintf(path, sizeof path, "%ls\\..\\goh_snapshots.ini", folder); camo_selftest_tick(path); lobby_dump_tick(path); }
    // our tab, wherever the page is: looked for a few times a second, it is a walk through the screen's dialogs
    static DWORD next;
    static Panel p, list, info, day;
    DWORD now = GetTickCount();
    if ((LONG)(now - next) >= 0) {
        next = now + 200;
        p = {};
        void *root = (void *)q(base + G_DESKTOP), *tab = root ? find(root, "camo_root") : nullptr;
        if (tab) bounds((uintptr_t)tab, p);
        if (tab && dialog_covered(tab)) p = {};       // the game's own window over it: exit, ESC
        list = {};
        void *replays = root ? find(root, "lb_replays") : nullptr;
        if (replays) bounds((uintptr_t)replays, list);
        if (replays && dialog_covered(replays)) list = {};
        info = {};
        void *lobby = root ? find(root, "btn_game_info") : nullptr;   // only the lobby has it
        if (lobby) bounds((uintptr_t)lobby, info);
        if (lobby && dialog_covered(lobby)) info = {};
        day = {};
        void *start = root ? find(root, "btn_start") : nullptr, *settings = root ? find(root, "panel_settings") : nullptr;
        Panel button = {}, area = {};
        if (start) bounds((uintptr_t)start, button);
        if (button.shown) day_host = true;
        else if (info.shown) day_host = false;
        day_publish(always_day && day_host);
        if (button.shown && settings && !dialog_covered(settings) && bounds((uintptr_t)settings, area)) {
            // under the last of the settings' rows; they are made by the game, as many as the mode has
            Panel last = {};
            int budget = 600;
            lowest_row((uintptr_t)settings, area, last, 0, budget);
            float h = last.shown ? last.h : 33;
            day = {true, last.shown ? last.x : area.x, last.shown ? last.y + last.h + 1 : area.y + area.h - h - 8, last.shown ? last.w : area.w, h};
            if (day.y + day.h > area.y + area.h) day.y = area.y + area.h - day.h;
        }
    }
    AcquireSRWLockExclusive(&lock); panel = p; replays_list = list; lobby_info = info; day_row = day; ReleaseSRWLockExclusive(&lock);
    lobby_open = info.shown;
    from_lobby_close = from_lobby_close || in_game;
    if (warm_needed && !in_game && !drawn) warm_start();   // not while sliders move: each new setting is a new texture
}
extern "C" int camo_shown() { return drawn || floating || replays_list.shown || lobby_open; }
namespace {
// A name of ours has to be a file of the game's repository: textures are found (62f990), kept and opened by their
// entry there, and a name without one gives the checker dummy or "Resource not found". So the first time the game
// looks for one it gets an entry: a storage of ours, the game's eStorage (a base of its own, eAbstractStorage, with
// the file's name at +0x20, as 62fc70 makes them) whose Open (slot 1) opens the plain texture and paints what it reads.
// A copy of the plain file's entry does not do: it tried the file on disk. A .ebm is a one line { reference "name" }
// to another texture: followed. The game then treats name#gohcamo... as any other variant.
constexpr uintptr_t FN_TEX_FIND = 0x62f990, FN_FILE_ENTRY = 0x663960, FN_FILE_ADD = 0x664820, FN_PATH_KEY = 0x674f30,
                    FN_PATH_NEW = 0x6741b0, FN_PATH_FREE = 0x674210, FN_STR_SET = 0xa50b0, FN_NEW = 0x36b6a0,
                    VT_ABSTRACT_STORAGE = 0x10101b0, FN_BITMAP = 0x62b680, FN_FILE_EXISTS = 0x663830,
                    VT_FILE = 0x10c6088, FN_BUF_STREAM = 0x6796c0, FN_BITMAP_LOAD = 0x629cd0;
constexpr uintptr_t TEX_FIND_CALLS[] = {0x62b6ec, 0x632935, 0x64c730, 0x64eefd};
unsigned char (*tex_find_real)(void *, void *, char *);
void *(*open_prev)(void *, unsigned);
struct GamePath { GameString s; void *wide; };
GamePath path_of(const std::string &s) {
    GamePath p = {};
    p.s.ptr = (char *)s.c_str(); p.s.size = s.size(); p.s.cap = s.size() < 16 ? 16 : s.size();
    return p;
}
// the file a texture name stands for, through .ebm references; "" = none
std::string texture_file(std::string name) {
    for (int hop = 0; hop < 4; hop++) {
        alignas(16) unsigned char out[0x40];
        char source = 0;
        ((void *(*)(void *))(base + FN_PATH_NEW))(out);
        GamePath in = path_of(name);
        unsigned char found = tex_find_real(&in, out, &source);
        const GameString *o = (const GameString *)out;
        std::string file(o->cap > 15 ? o->ptr : o->buf, o->size);
        ((void (*)(void *))(base + FN_PATH_FREE))(out);
        // a .dds comes with source set; a texture already loaded has a .ebm of the game's own making beside it
        if (!found) { rc_log("camo: %s: no file", name.c_str()); return ""; }
        if (file.size() < 4 || _stricmp(file.c_str() + file.size() - 4, ".ebm")) return file;
        std::string dds = file.substr(0, file.size() - 4) + ".dds";
        GamePath d = path_of(dds);
        if (((unsigned char (*)(void *))(base + FN_FILE_EXISTS))(&d)) return dds;
        if (!open_prev) return "";
        GamePath f = path_of(file);
        unsigned char *st = (unsigned char *)open_prev(&f, 0x201);       // a few bytes, the stream is left to the game's heap
        if (!st) return "";
        const char *b = *(const char **)(st + 8), *e = *(const char **)(st + 0x28);
        std::string text(b, b && e > b && e - b < 4096 ? e - b : 0);
        rc_log("camo: %s: %s", file.c_str(), text.c_str());
        size_t q0 = text.find('"'), q1 = q0 == std::string::npos ? q0 : text.find('"', q0 + 1);
        if (q1 == std::string::npos) return "";
        std::string to = text.substr(q0 + 1, q1 - q0 - 1);
        if (!to.compare(0, 2, "$/")) to = "/texture/common/" + to.substr(2);
        else if (to[0] != '/') to = file.substr(0, file.rfind('/') + 1) + to;
        name = to;
    }
    return "";
}
// ---- the picture on the vehicle itself ---------------------------------------------------------------------------
// A vehicle's texture is its parts laid out flat: a picture put on the texture comes out in pieces. So every texel's
// place on the vehicle is worked out from the model (the .mdl's bones, the .ply meshes), and the picture is projected
// on that from the side, from above and from the front, like a stencil. Parts that share a piece of texture (the two
// sides of a hull, the road wheels) show the same thing.
struct Node { std::string key; std::vector<std::string> values; std::vector<Node> kids; };
struct Tokens {
    const char *p, *end;
    std::string next() {                              // "" at the end
        for (;;) {
            while (p < end && (unsigned char)*p <= 32) p++;
            if (p < end && *p == ';') { while (p < end && *p != '\n') p++; continue; }
            break;
        }
        if (p >= end) return "";
        if (*p == '{' || *p == '}') return std::string(1, *p++);
        if (*p == '"') {
            const char *s = ++p;
            while (p < end && *p != '"') p++;
            std::string r(s, p);
            if (p < end) p++;
            return r.empty() ? std::string(" ") : r;
        }
        const char *s = p;
        while (p < end && (unsigned char)*p > 32 && !strchr("{}\";", *p)) p++;
        return std::string(s, p);
    }
};
void read_node(Tokens &t, Node &n, int depth) {
    for (bool first = true;; first = false) {
        std::string k = t.next();
        if (k.empty() || k == "}") return;
        if (k == "{") {
            if (depth > 64) return;
            n.kids.emplace_back();
            read_node(t, n.kids.back(), depth + 1);
        } else if (first) n.key = k;
        else n.values.push_back(k);
    }
}
struct Mat { float r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, t[3] = {0, 0, 0}; };
void turn(const Mat &m, const float *v, float *out) {
    for (int c = 0; c < 3; c++) out[c] = v[0] * m.r[0][c] + v[1] * m.r[1][c] + v[2] * m.r[2][c];
}
Mat after(const Mat &l, const Mat &m) {               // l first, then m (row vectors)
    Mat o;
    for (int i = 0; i < 3; i++) turn(m, l.r[i], o.r[i]);
    turn(m, l.t, o.t);
    for (int c = 0; c < 3; c++) o.t[c] += m.t[c];
    return o;
}
struct View { std::string file; Mat m; };
bool same_key(const std::string &a, const char *b) { return !_stricmp(a.c_str(), b); }
void walk_bones(const Node &n, const Mat &parent, std::vector<View> &views) {
    Mat l;
    for (const Node &k : n.kids) {
        const auto &v = k.values;
        if (same_key(k.key, "orientation") && v.size() >= 9) {
            for (int i = 0; i < 9; i++) l.r[i / 3][i % 3] = (float)atof(v[i].c_str());
        } else if (same_key(k.key, "position") && v.size() >= 3) {
            for (int i = 0; i < 3; i++) l.t[i] = (float)atof(v[i].c_str());
        } else if (same_key(k.key, "matrix34") && v.size() >= 12) {
            for (int i = 0; i < 9; i++) l.r[i / 3][i % 3] = (float)atof(v[i].c_str());
            for (int i = 0; i < 3; i++) l.t[i] = (float)atof(v[9 + i].c_str());
        }
    }
    Mat w = after(l, parent);
    for (const Node &k : n.kids) {
        if (same_key(k.key, "bone")) walk_bones(k, w, views);
        else if (same_key(k.key, "volumeview") && !k.values.empty()) views.push_back({k.values[0], w});
        else if (same_key(k.key, "lodview"))
            for (const Node &v : k.kids)
                if (same_key(v.key, "volumeview") && !v.values.empty()) { views.push_back({v.values[0], w}); break; }   // the best one
    }
}
struct Triangle { float p[3][3], n[3][3], uv[3][2]; int layer; bool gun, skin; };    // layer: which of the model's textures, -1 none
// A barrel takes no sticker: gun.ply, gun2.ply, gun_lod1.ply, and the machine guns' mgun*.ply. Not the mantlet (gun_rot,
// mantlet.ply), a shield or a gunner's hatch.
bool is_barrel(std::string file) {
    for (auto &c : file) c = (char)tolower((unsigned char)c);
    size_t slash = file.rfind('/');
    if (slash != std::string::npos) file = file.substr(slash + 1);
    if (file.find("shield") != std::string::npos) return false;
    if (!file.compare(0, 4, "mgun") || !file.compare(0, 5, "hmgun")) return true;
    if (file.compare(0, 3, "gun")) return false;
    char next = file.size() > 3 ? file[3] : 0;
    return next == '.' || !next || (next >= '0' && next <= '9') || !file.compare(3, 4, "_lod");
}
struct VehicleMap {
    int w = 0, h = 0;
    std::vector<float> pos;                           // x, y, z of a texel
    std::vector<unsigned char> kind;                  // 0 not on the outside of the vehicle, 1 one place, 2 used at several
    std::vector<float> owner;                         // the size of the triangle whose place a texel holds
    std::vector<unsigned char> both;                  // 1: no sticker here: the texel is on both sides of the vehicle, or on a barrel
    std::vector<signed char> normal;                  // which way the surface looks there
    std::vector<float> twin;                          // x, y, z of the second place of a texel that two parts on top of the vehicle share
    std::vector<unsigned char> twinned;               // two hatches of one texture: 1 where twin holds a place
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
};
struct Model {
    std::vector<Triangle> all; std::vector<std::string> stems; float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
};
struct Mesh { unsigned first, tris; std::string material; };
struct Ply { std::vector<Mesh> meshes; const unsigned char *vert = nullptr; unsigned count = 0, stride = 0, uv = 24; const uint16_t *index = nullptr; unsigned indices = 0; bool skinned = false; };
bool read_ply(const std::vector<unsigned char> &d, Ply &o) {
    size_t p = 4, n = d.size();
    if (n < 8 || memcmp(d.data(), "EPLY", 4)) return false;
    while (p + 8 <= n) {
        const unsigned char *b = d.data() + p;
        if (!memcmp(b, "BNDS", 4)) p += 28;
        else if (!memcmp(b, "MESH", 4)) {
            if (p + 26 > n) return false;
            uint32_t fl, first, tris;
            memcpy(&fl, b + 4, 4); memcpy(&first, b + 8, 4); memcpy(&tris, b + 12, 4);
            // then a colour, then the material's name; flag 0x40: the vertices have a colour of their own before their uv
            size_t q = p + 24;
            auto named = [&](size_t at) { return at < n && d[at] > 4 && at + 1 + d[at] <= n && !memcmp(&d[at + 1 + d[at] - 4], ".mtl", 4); };
            if (!named(q)) q = p + 20;
            if (q >= n || q + 1 + d[q] > n) return false;
            o.meshes.push_back({first, tris, std::string((const char *)&d[q + 1], d[q])});
            if (fl & 0x40) o.uv = 28;
            p = q + 1 + d[q];
            if (fl & 0x1000) { if (p >= n) return false; p += 1 + d[p]; }
        } else if (!memcmp(b, "VERT", 4)) {
            uint32_t cnt; uint16_t stride, fl;
            memcpy(&cnt, b + 4, 4); memcpy(&stride, b + 8, 2); memcpy(&fl, b + 10, 2);
            if (stride < o.uv + 8 || (fl & 7) != 7 || p + 12 + (size_t)cnt * stride > n) return false;
            o.vert = b + 12; o.count = cnt; o.stride = stride;
            p += 12 + (size_t)cnt * stride;
        } else if (!memcmp(b, "INDX", 4)) {
            uint32_t cnt; memcpy(&cnt, b + 4, 4);
            if (p + 8 + (size_t)cnt * 2 > n) return false;
            o.index = (const uint16_t *)(b + 8); o.indices = cnt;
            p += 8 + (size_t)cnt * 2;
        } else if (!memcmp(b, "SKIN", 4)) {           // the bones of tracks and suspensions: names, then a mesh like any other
            uint32_t cnt; memcpy(&cnt, b + 4, 4);
            if (cnt > 4096) return false;
            o.skinned = true;
            p += 8;
            for (uint32_t i = 0; i < cnt; i++) { if (p >= n) return false; p += 1 + d[p]; }
        } else return false;
    }
    return o.vert && o.index;
}
// A texture is shared: the same texels serve both sides of the hull, all the wheels, and hidden parts like the inside of
// the engine room. The two sides mirror each other, so y counts by its size; what else lands far apart can't hold a drawing.
void fill_triangle(VehicleMap &pr, const Triangle &t, bool gap_only) {
    float wrap = floorf(fminf(t.uv[0][0], fminf(t.uv[1][0], t.uv[2][0]))), wrapv = floorf(fminf(t.uv[0][1], fminf(t.uv[1][1], t.uv[2][1])));
    float x[3], y[3];
    for (int i = 0; i < 3; i++) { x[i] = (t.uv[i][0] - wrap) * pr.w; y[i] = (t.uv[i][1] - wrapv) * pr.h; }
    float den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2]);
    if (fabsf(den) < 1e-6f) return;
    int x0 = (int)floorf(fminf(x[0], fminf(x[1], x[2]))) - 1, x1 = (int)ceilf(fmaxf(x[0], fmaxf(x[1], x[2]))) + 1;
    int y0 = (int)floorf(fminf(y[0], fminf(y[1], y[2]))) - 1, y1 = (int)ceilf(fmaxf(y[0], fmaxf(y[1], y[2]))) + 1;
    if (x1 - x0 > 2 * pr.w || y1 - y0 > 2 * pr.h) return;
    float cy = (pr.lo[1] + pr.hi[1]) / 2, apart = (pr.hi[0] - pr.lo[0]) * .04f;
    float e0[3], e1[3], ex[3];
    for (int i = 0; i < 3; i++) { e0[i] = t.p[1][i] - t.p[0][i]; e1[i] = t.p[2][i] - t.p[0][i]; }
    cross(e0, e1, ex);
    const float area = sqrtf(ex[0] * ex[0] + ex[1] * ex[1] + ex[2] * ex[2]);
    for (int py = y0; py <= y1; py++) for (int px = x0; px <= x1; px++) {
        float gx = px + .5f, gy = py + .5f;
        float a = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / den;
        float b = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / den;
        float c = 1 - a - b;
        if (a < -.03f || b < -.03f || c < -.03f) continue;
        int tx = ((px % pr.w) + pr.w) % pr.w, ty = ((py % pr.h) + pr.h) % pr.h;
        size_t at = (size_t)ty * pr.w + tx;
        if (t.gun) pr.both[at] = 1;
        float q[3];
        for (int i = 0; i < 3; i++) q[i] = a * t.p[0][i] + b * t.p[1][i] + c * t.p[2][i];
        float *o = &pr.pos[at * 3];
        if (!pr.kind[at]) {
            memcpy(o, q, sizeof q); pr.kind[at] = 1; pr.owner[at] = area;
            for (int i = 0; i < 3; i++) {
                float n = a * t.n[0][i] + b * t.n[1][i] + c * t.n[2][i];
                pr.normal[at * 3 + i] = (signed char)(n < -1 ? -127 : n > 1 ? 127 : n * 127);
            }
            continue;
        }
        if (gap_only || a < 0 || b < 0 || c < 0) continue;      // the rim around a triangle, for the filtering
        // Copies of a part on top of the vehicle, hatches mostly: they keep the camouflage and can take a sticker, see stick()
        float nz = a * t.n[0][2] + b * t.n[1][2] + c * t.n[2][2];
        const float middle = (pr.lo[2] + pr.hi[2]) / 2;      // not the tops of road wheels
        const bool on_top = !t.gun && nz > .5f && pr.normal[at * 3 + 2] > 64 && q[2] > middle && o[2] > middle;
        auto second_place = [&] { if (!pr.twinned[at]) { memcpy(&pr.twin[at * 3], q, sizeof q); pr.twinned[at] = 1; } };
        if ((q[1] - cy) * (o[1] - cy) < 0 && fabsf(q[1] - o[1]) > apart * .5f) {
            pr.both[at] = 1;
            if (on_top) second_place();
        }
        if (fabsf(q[0] - o[0]) > apart || fabsf(q[2] - o[2]) > apart || fabsf(fabsf(q[1] - cy) - fabsf(o[1] - cy)) > apart) {
            // Two places want the texel. Exact copies of one part (road wheels) can't hold a drawing; otherwise the bigger
            // surface keeps it: a detail reusing a bit of the hull's texture leaves no hole in the hull.
            float was = pr.owner[at];
            if (area > was * 1.05f) {
                memcpy(o, q, sizeof q); pr.kind[at] = 1; pr.owner[at] = area;
                for (int i = 0; i < 3; i++) {
                    float n = a * t.n[0][i] + b * t.n[1][i] + c * t.n[2][i];
                    pr.normal[at * 3 + i] = (signed char)(n < -1 ? -127 : n > 1 ? 127 : n * 127);
                }
            } else if (area > was * .95f) {
                if (on_top) second_place();
                else pr.kind[at] = 2;
            }
        }
    }
}
// Which triangles anyone can see: the model looked at from all around, each view with a depth buffer.
void look_from(const std::vector<Triangle> &all, const float eye[3], const float centre[3], float radius, std::vector<unsigned char> &seen) {
    constexpr int R = 320;
    std::vector<int> ids(R * R, -1);
    std::vector<float> depth(R * R, 1e30f);
    float r[3] = {eye[1], -eye[0], 0}, len = sqrtf(r[0] * r[0] + r[1] * r[1]);
    if (len < 1e-3f) { r[0] = 1; r[1] = 0; } else { r[0] /= len; r[1] /= len; }
    float u[3] = {r[1] * eye[2] - r[2] * eye[1], r[2] * eye[0] - r[0] * eye[2], r[0] * eye[1] - r[1] * eye[0]};
    for (size_t n = 0; n < all.size(); n++) {
        float x[3], y[3], d[3];
        for (int i = 0; i < 3; i++) {
            float q[3] = {all[n].p[i][0] - centre[0], all[n].p[i][1] - centre[1], all[n].p[i][2] - centre[2]};
            x[i] = ((q[0] * r[0] + q[1] * r[1] + q[2] * r[2]) / radius * .5f + .5f) * R;
            y[i] = ((q[0] * u[0] + q[1] * u[1] + q[2] * u[2]) / radius * .5f + .5f) * R;
            d[i] = -(q[0] * eye[0] + q[1] * eye[1] + q[2] * eye[2]);
        }
        float den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2]);
        if (fabsf(den) < 1e-7f) continue;
        int x0 = (int)fmaxf(fminf(x[0], fminf(x[1], x[2])), 0), x1 = (int)fminf(fmaxf(x[0], fmaxf(x[1], x[2])) + 2, R);
        int y0 = (int)fmaxf(fminf(y[0], fminf(y[1], y[2])), 0), y1 = (int)fminf(fmaxf(y[0], fmaxf(y[1], y[2])) + 2, R);
        for (int py = y0; py < y1; py++) for (int px = x0; px < x1; px++) {
            float gx = px + .5f, gy = py + .5f;
            float a = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / den;
            float b = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / den;
            float c = 1 - a - b;
            if (a < 0 || b < 0 || c < 0) continue;
            float z = a * d[0] + b * d[1] + c * d[2];
            if (z < depth[py * R + px]) { depth[py * R + px] = z; ids[py * R + px] = (int)n; }
        }
    }
    for (int id : ids) if (id >= 0) seen[id] = 1;
    float close = radius * .008f;
    for (size_t n = 0; n < all.size(); n++) {
        if (seen[n]) continue;
        float q[3] = {0, 0, 0};
        for (int i = 0; i < 3; i++) for (int c = 0; c < 3; c++) q[c] += (all[n].p[i][c] - centre[c]) / 3;
        int x = (int)(((q[0] * r[0] + q[1] * r[1] + q[2] * r[2]) / radius * .5f + .5f) * R);
        int y = (int)(((q[0] * u[0] + q[1] * u[1] + q[2] * u[2]) / radius * .5f + .5f) * R);
        if (x < 0 || y < 0 || x >= R || y >= R) continue;
        if (-(q[0] * eye[0] + q[1] * eye[1] + q[2] * eye[2]) <= depth[y * R + x] + close) seen[n] = 2;     // a maybe: it only fills gaps
    }
}
// What of the model is on the outside.
std::vector<unsigned char> seen_of(const Model &m) {
    std::vector<unsigned char> seen(m.all.size());
    float centre[3], radius = 1;
    for (int i = 0; i < 3; i++) { centre[i] = (m.lo[i] + m.hi[i]) / 2; radius += (m.hi[i] - m.lo[i]) * (m.hi[i] - m.lo[i]) / 4; }
    radius = sqrtf(radius);
    for (int e = 0; e <= 2; e++) for (int a = 0; a < 8; a++) {
        float el = e * .5235988f, az = a * .7853982f;
        float eye[3] = {cosf(el) * cosf(az), cosf(el) * sinf(az), sinf(el)};
        look_from(m.all, eye, centre, radius, seen);
    }
    float up[3] = {0, 0, 1};
    look_from(m.all, up, centre, radius, seen);
    return seen;
}
// The place of every texel of a texture of the model seen from outside.
void bake_layer(VehicleMap &pr, const Model &m, const std::vector<unsigned char> &seen, int layer) {
    memcpy(pr.lo, m.lo, sizeof pr.lo); memcpy(pr.hi, m.hi, sizeof pr.hi);
    pr.pos.assign((size_t)pr.w * pr.h * 3, 0);
    pr.kind.assign((size_t)pr.w * pr.h, 0);
    pr.both.assign((size_t)pr.w * pr.h, 0);
    pr.twin.assign((size_t)pr.w * pr.h * 3, 0);
    pr.twinned.assign((size_t)pr.w * pr.h, 0);
    pr.normal.assign((size_t)pr.w * pr.h * 3, 0);
    pr.owner.assign((size_t)pr.w * pr.h, 0);
    for (size_t i = 0; i < m.all.size(); i++) if (m.all[i].layer == layer && seen[i] == 1 && !m.all[i].skin) fill_triangle(pr, m.all[i], false);
    for (size_t i = 0; i < m.all.size(); i++) if (m.all[i].layer == layer && seen[i] == 2 && !m.all[i].skin) fill_triangle(pr, m.all[i], true);
    const int w = pr.w, h = pr.h;
    for (int pass = 0; pass < 3; pass++) {
        std::vector<int> grow;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            size_t at = (size_t)y * w + x;
            if (pr.kind[at]) continue;
            static const int DX[4] = {1, -1, 0, 0}, DY[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; k++) {
                int nx = x + DX[k], ny = y + DY[k];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h || pr.kind[(size_t)ny * w + nx] != 1) continue;
                grow.push_back((int)at); grow.push_back((int)((size_t)ny * w + nx));
                break;
            }
        }
        for (size_t i = 0; i < grow.size(); i += 2) {
            size_t to = grow[i], from = grow[i + 1];
            memcpy(&pr.pos[to * 3], &pr.pos[from * 3], 12);
            memcpy(&pr.normal[to * 3], &pr.normal[from * 3], 3);
            pr.both[to] = pr.both[from];
            pr.twinned[to] = pr.twinned[from];
            memcpy(&pr.twin[to * 3], &pr.twin[from * 3], 3 * sizeof(float));
            pr.kind[to] = 1;
        }
    }
}
bool read_game_file(const std::string &path, std::vector<unsigned char> &out) {
    GamePath p = path_of(path);
    if (!open_prev || !((unsigned char (*)(void *))(base + FN_FILE_EXISTS))(&p)) return false;
    unsigned char *st = (unsigned char *)open_prev(&p, 0x201);
    if (!st) return false;
    uintptr_t vt = *(uintptr_t *)st - base;
    bool ok = false;
    static int odd;
    if (vt != VT_FILE && vt != VT_ZIP && vt != VT_BUF && odd++ < 8) rc_log("camo: %s: a stream of kind %llx", path.c_str(), (unsigned long long)vt);
    if (vt == VT_FILE) {
        const GameString *g = (const GameString *)(st + 0x48);
        std::string disk(g->cap > 15 ? g->ptr : g->buf, g->size);
        wchar_t wide[MAX_PATH * 2];
        ok = MultiByteToWideChar(CP_UTF8, 0, disk.c_str(), -1, wide, MAX_PATH * 2) && readfile(wide, out);
    } else if (vt == VT_ZIP || vt == VT_BUF) {
        unsigned char *b = *(unsigned char **)(st + 8), *e = *(unsigned char **)(st + 0x28);
        if (b && e > b) { out.assign(b, e); ok = true; }
    }
    (*(void (**)(void *, int))*(uintptr_t *)st)(st, 1);
    return ok;
}
// mdl: the model's file in the game. Its triangles in the resting pose and the textures they are on, "_vehicle/t34_76_41".
bool load_model(const std::string &mdl, Model &o) {
    std::vector<unsigned char> text;
    if (!read_game_file(mdl, text)) { rc_log("camo: no model %s", mdl.c_str()); return false; }
    Tokens t{(const char *)text.data(), (const char *)text.data() + text.size()};
    Node root;
    read_node(t, root, 0);
    const Node *skeleton = &root;
    for (const Node &k : root.kids) if (same_key(k.key, "skeleton")) skeleton = &k;
    std::vector<View> views;
    walk_bones(*skeleton, Mat(), views);
    std::string dir = mdl.substr(0, mdl.rfind('/') + 1);
    std::vector<std::pair<std::string, int>> materials;
    for (const View &v : views) {
        std::vector<unsigned char> data;
        Ply m;
        if (!read_game_file(dir + v.file, data) || !read_ply(data, m)) continue;
        const bool barrel = is_barrel(v.file);
        if (m.skinned) {                              // tracks and suspensions lie where they are; a canvas and such follow their bones
            std::string name = v.file;
            for (auto &c : name) c = (char)tolower((unsigned char)c);
            bool running_gear = false;
            for (const char *k : {"track", "susp", "torz", "spring", "rod"}) running_gear |= name.find(k) != std::string::npos;
            if (!running_gear) continue;
        }
        for (const Mesh &mesh : m.meshes) {
            if (!_strnicmp(mesh.material.c_str(), "number", 6)) continue;     // plates the game fills in per vehicle
            int layer = -1;
            bool known = false;
            for (auto &e : materials) if (e.first == mesh.material) { layer = e.second; known = true; }
            if (!known) {
                std::vector<unsigned char> mt;
                if (read_game_file(dir + mesh.material, mt)) {
                    std::string s(mt.begin(), mt.end());
                    for (auto &c : s) c = (char)tolower((unsigned char)c);
                    size_t d = s.find("diffuse"), q0 = d == std::string::npos ? d : s.find('"', d), q1 = q0 == std::string::npos ? q0 : s.find('"', q0 + 1);
                    if (q1 != std::string::npos) {
                        std::string tex = s.substr(q0 + 1, q1 - q0 - 1);
                        if (!tex.compare(0, 2, "$/")) tex = tex.substr(2);
                        auto at = std::find(o.stems.begin(), o.stems.end(), tex);
                        layer = (int)(at - o.stems.begin());
                        if (at == o.stems.end()) o.stems.push_back(tex);
                    }
                }
                materials.push_back({mesh.material, layer});
            }
            // first counts triangles
            for (unsigned i = 0; i < mesh.tris && ((size_t)mesh.first + i) * 3 + 2 < m.indices; i++) {
                Triangle tr;
                tr.layer = layer;
                tr.gun = barrel;
                tr.skin = m.skinned;
                bool good = true;
                for (int c = 0; c < 3; c++) {
                    unsigned k = m.index[((size_t)mesh.first + i) * 3 + c];
                    if (k >= m.count) { good = false; break; }
                    float raw[8];
                    memcpy(raw, m.vert + (size_t)k * m.stride, 24);
                    memcpy(raw + 6, m.vert + (size_t)k * m.stride + m.uv, 8);
                    turn(v.m, raw, tr.p[c]);
                    turn(v.m, raw + 3, tr.n[c]);
                    for (int a = 0; a < 3; a++) {
                        tr.p[c][a] += v.m.t[a];
                        o.lo[a] = fminf(o.lo[a], tr.p[c][a]); o.hi[a] = fmaxf(o.hi[a], tr.p[c][a]);
                    }
                    tr.uv[c][0] = raw[6]; tr.uv[c][1] = raw[7];
                }
                if (good) o.all.push_back(tr);
            }
        }
    }
    return !o.all.empty();
}
// Where a texel of the vehicle falls on the picture; false = beside it. The picture is laid over the vehicle like a cover:
// along its length, and across: over the top and down the sides. That is one function of the place on the vehicle,
// whichever way a face looks, so no part of the drawing tears from the next. Either both sides get the same, the
// picture starting at the middle of the roof, or it is one sheet from side to side. Where a texture serves both sides
// (only a few parts) one sheet shows the same on both.
Placement place_picture(const float lo[3], const float hi[3], int pw, int ph, const CamoSettings &s) {
    Placement l;
    l.cx = (lo[0] + hi[0]) / 2; l.cy = (lo[1] + hi[1]) / 2; l.top = hi[2];
    float half = (hi[1] - lo[1]) / 2 + hi[2] - lo[2], along = hi[0] - lo[0];
    l.height = fmaxf(s.through ? 2 * half : half, along * ph / pw) * s.size;
    l.width = l.height * pw / ph;
    float angle = s.rotation * 0.017453292519943295f;
    l.ca = cosf(angle); l.sa = sinf(angle);
    l.ox = s.offset[0]; l.oy = s.offset[1];
    l.through = s.through; l.repeat = s.repeat; l.flip_x = s.flip_x; l.flip_y = s.flip_y;
    return l;
}
const float *texel_place(const VehicleMap &pr, size_t at) { return &pr.pos[at * 3]; }
void face_normal(const Triangle &t, float n[3]) {
    float u[3], v[3];
    for (int i = 0; i < 3; i++) { u[i] = t.p[1][i] - t.p[0][i]; v[i] = t.p[2][i] - t.p[0][i]; }
    cross(u, v, n);
    normalize(n);
    float s[3] = {t.n[0][0] + t.n[1][0] + t.n[2][0], t.n[0][1] + t.n[1][1] + t.n[2][1], t.n[0][2] + t.n[1][2] + t.n[2][2]};
    if (n[0] * s[0] + n[1] * s[1] + n[2] * s[2] < 0) for (int i = 0; i < 3; i++) n[i] = -n[i];
}
float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
// A sticker is a stencil at its point, looking along the surface's normal there, as decals are in games. Over the picture lies
// a grid; each cell keeps the heights of the surfaces the stencil meets, several layers. From the middle a floor flows
// over the grid: it goes down only gently, so from a turret's roof nothing falls on the hull, and having gone down a
// side it does not lie flat again on what is below. Whatever stands on the floor inside the frame takes the picture too:
// hatches, boxes, cables, a gun, whatever meshes they are. Everything is relative to the plane of the point, so nothing
// can fly off.
constexpr int STICKER_GRID = 128, STICKER_LAYERS = 4;
void sticker_lay(StickerImage &s, const Model &m, const std::vector<unsigned char> &seen) {
    const float big = fmaxf(s.half_w, s.half_h), cell = 2 * big / STICKER_GRID;
    const int gx = (int)ceilf(2 * s.half_w / cell) + 1, gy = (int)ceilf(2 * s.half_h / cell) + 1;
    s.cell = cell; s.gx = gx; s.gy = gy;
    const size_t cells = (size_t)gx * gy;
    std::vector<float> height(cells * STICKER_LAYERS), facing(cells * STICKER_LAYERS);
    std::vector<unsigned char> count(cells, 0);
    std::vector<float> wall_lo(cells, 1e30f), wall_hi(cells, -1e30f);     // how far up and down a wall runs in a cell
    for (size_t i = 0; i < m.all.size(); i++) {
        if (!seen[i] || m.all[i].gun || m.all[i].skin) continue;
        const Triangle &t = m.all[i];
        float f[3];
        face_normal(t, f);
        float looks = dot3(f, s.n);
        float x[3], y[3], z[3];
        for (int k = 0; k < 3; k++) {
            float d[3] = {t.p[k][0] - s.c[0], t.p[k][1] - s.c[1], t.p[k][2] - s.c[2]};
            x[k] = (dot3(d, s.e1) + s.half_w) / cell;
            y[k] = (dot3(d, s.e2) + s.half_h) / cell;
            z[k] = dot3(d, s.n);
        }
        if (looks < .35f) {                           // a side of something: where it stands
            float len = 0;
            for (int k = 0; k < 3; k++) {
                float ex = x[(k + 1) % 3] - x[k], ey = y[(k + 1) % 3] - y[k], ez = (z[(k + 1) % 3] - z[k]) / cell;
                len = fmaxf(len, sqrtf(ex * ex + ey * ey + ez * ez));
            }
            int n = (int)ceilf(len) + 1;
            n = n > 96 ? 96 : n;
            for (int a = 0; a <= n; a++) for (int b = 0; b <= n - a; b++) {
                float wa = (float)a / n, wb = (float)b / n, wc = 1 - wa - wb;
                int cx = (int)(wa * x[0] + wb * x[1] + wc * x[2]), cy = (int)(wa * y[0] + wb * y[1] + wc * y[2]);
                if (cx < 0 || cy < 0 || cx >= gx || cy >= gy) continue;
                float h = wa * z[0] + wb * z[1] + wc * z[2];
                size_t at = (size_t)cy * gx + cx;
                wall_lo[at] = fminf(wall_lo[at], h); wall_hi[at] = fmaxf(wall_hi[at], h);
            }
        }
        if (looks < .1f) continue;
        int x0 = (int)fmaxf(floorf(fminf(x[0], fminf(x[1], x[2]))), 0), x1 = (int)fminf(ceilf(fmaxf(x[0], fmaxf(x[1], x[2]))) + 1, (float)gx);
        int y0 = (int)fmaxf(floorf(fminf(y[0], fminf(y[1], y[2]))), 0), y1 = (int)fminf(ceilf(fmaxf(y[0], fmaxf(y[1], y[2]))) + 1, (float)gy);
        if (x1 <= x0 || y1 <= y0) continue;
        float den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2]);
        if (fabsf(den) < 1e-7f) continue;
        for (int py = y0; py < y1; py++) for (int px = x0; px < x1; px++) {
            float gxc = px + .5f, gyc = py + .5f;
            float a = ((y[1] - y[2]) * (gxc - x[2]) + (x[2] - x[1]) * (gyc - y[2])) / den;
            float b = ((y[2] - y[0]) * (gxc - x[2]) + (x[0] - x[2]) * (gyc - y[2])) / den;
            float c = 1 - a - b;
            if (a < -.1f || b < -.1f || c < -.1f) continue;
            float depth = a * z[0] + b * z[1] + c * z[2];
            size_t at = (size_t)py * gx + px;
            float *h = &height[at * STICKER_LAYERS], *l = &facing[at * STICKER_LAYERS];
            int n = count[at], same = -1;
            for (int k = 0; k < n; k++) if (fabsf(h[k] - depth) < cell * 1.5f) same = k;
            if (same >= 0) { if (depth > h[same]) { h[same] = depth; l[same] = looks; } continue; }
            if (n < STICKER_LAYERS) { h[n] = depth; l[n] = looks; count[at]++; continue; }
            int worst = 0;
            for (int k = 1; k < n; k++) if (fabsf(h[k]) > fabsf(h[worst])) worst = k;
            if (fabsf(depth) < fabsf(h[worst])) { h[worst] = depth; l[worst] = looks; }
        }
    }
    s.chosen.assign(cells, 1e30f);
    s.stretch.assign(cells, 1);
    // the middle: the layer at the sticker's point, on the nearest cell that has one
    int mx = gx / 2, my = gy / 2, start = -1, layer = 0;
    for (int r = 0; r < 16 && start < 0; r++) for (int dy = -r; dy <= r && start < 0; dy++) for (int dx = -r; dx <= r; dx++) {
        int x = mx + dx, y = my + dy;
        if (x < 0 || y < 0 || x >= gx || y >= gy || !count[(size_t)y * gx + x]) continue;
        start = y * gx + x;
        break;
    }
    if (start < 0) return;
    for (int k = 1; k < count[start]; k++) if (fabsf(height[(size_t)start * STICKER_LAYERS + k]) < fabsf(height[(size_t)start * STICKER_LAYERS + layer])) layer = k;
    const float small = fmaxf(cell * 4, 1.5f), level = cell * 3;
    std::vector<float> turned(cells, 1);
    std::vector<int> queue;
    queue.reserve(cells);
    s.chosen[start] = height[(size_t)start * STICKER_LAYERS + layer];
    turned[start] = facing[(size_t)start * STICKER_LAYERS + layer];
    queue.push_back(start);
    static const int WAYS[16][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}, {3, 0}, {-3, 0}, {0, 3}, {0, -3}, {3, 3}, {-3, 3}, {3, -3}, {-3, -3}};
    for (size_t at = 0; at < queue.size(); at++) {
        int p = queue[at], px = p % gx, py = p / gx;
        float here = s.chosen[p];
        for (const auto &w : WAYS) {
            int x = px + w[0], y = py + w[1];
            if (x < 0 || y < 0 || x >= gx || y >= gy) continue;
            int q = y * gx + x;
            if (s.chosen[q] < 1e29f || !count[q]) continue;
            // the lowest layer the floor can go on to; over a gap only what lies level with here
            bool gap = abs(w[0]) > 1 || abs(w[1]) > 1;
            float lowest = gap ? here - level : here - small, best = 1e30f, look = 1;
            for (int k = 0; k < count[q]; k++) {
                float h = height[(size_t)q * STICKER_LAYERS + k];
                if (h >= lowest && h < best) { best = h; look = facing[(size_t)q * STICKER_LAYERS + k]; }
            }
            if (best > 1e29f || (gap && best > here + level)) continue;
            if (best <= here && look > turned[p] + .2f) continue;   // paper bends one way
            s.chosen[q] = fminf(best, here);                         // standing on something, the floor stays where it was
            turned[q] = best <= here ? fminf(turned[p], look) : turned[p];
            queue.push_back(q);
        }
    }
    // The sticker paints the floor, and what stands on it: a level above joined to the floor by a wall, a hatch, a box. What
    // only hangs over the floor (a turret's bustle over the deck) has no wall down to it, and takes nothing.
    const float standing = fminf(big * .5f, 8.f), touching = fmaxf(cell * 3, 1.5f);
    std::vector<unsigned char> painted(cells * STICKER_LAYERS, 0);
    std::vector<int> states;
    states.reserve(cells);
    for (size_t i = 0; i < cells; i++) {
        if (s.chosen[i] > 1e29f) continue;
        for (int k = 0; k < count[i]; k++)
            if (fabsf(height[i * STICKER_LAYERS + k] - s.chosen[i]) <= small) { painted[i * STICKER_LAYERS + k] = 1; states.push_back((int)(i * STICKER_LAYERS + k)); }
    }
    auto wall_between = [&](int x, int y, float lo, float hi) {
        float a = 1e30f, b = -1e30f;                  // the wall around here, however it runs from cell to cell
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int cx = x + dx, cy = y + dy;
            if (cx < 0 || cy < 0 || cx >= gx || cy >= gy) continue;
            a = fminf(a, wall_lo[(size_t)cy * gx + cx]); b = fmaxf(b, wall_hi[(size_t)cy * gx + cx]);
        }
        return a <= lo + touching && b >= hi - touching;
    };
    for (size_t at = 0; at < states.size(); at++) {
        int state = states[at], p = state / STICKER_LAYERS, px = p % gx, py = p / gx;
        float here = height[state];
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int x = px + dx, y = py + dy;
            if (x < 0 || y < 0 || x >= gx || y >= gy) continue;
            int q = y * gx + x;
            if (s.chosen[q] > 1e29f) continue;
            for (int k = 0; k < count[q]; k++) {
                int next = q * STICKER_LAYERS + k;
                float h = height[next];
                if (painted[next] || h > s.chosen[q] + standing) continue;
                if (fabsf(h - here) > small && !(h > here && wall_between(x, y, here, h))) continue;
                painted[next] = 1;
                states.push_back(next);
            }
        }
    }
    s.levels.assign(cells * STICKER_LAYERS, 1e30f);
    for (size_t i = 0; i < cells * STICKER_LAYERS; i++) if (painted[i]) s.levels[i] = height[i];
    // Distances from the middle along the surface: what bends away still takes its part of the picture, not a smear.
    const int sx = start % gx, sy = start / gx;
    std::vector<float> raw(cells, 1);
    for (int y = 0; y < gy; y++) for (int x = 0; x < gx; x++) {
        if (s.chosen[(size_t)y * gx + x] > 1e29f) continue;
        int dx = x - sx, dy = y - sy, steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        if (!steps) continue;
        float straight = sqrtf((float)(dx * dx + dy * dy)) * cell, part = straight / steps, along = 0, prev = s.chosen[start];
        for (int k = 1; k <= steps; k++) {
            int cx = sx + dx * k / steps, cy = sy + dy * k / steps;
            float h = s.chosen[(size_t)cy * gx + cx], d = 0;
            if (h < 1e29f) { d = h - prev; prev = h; if (fabsf(d) > part * 4) d = 0; }
            along += sqrtf(part * part + d * d);
        }
        raw[(size_t)y * gx + x] = fminf(along / straight, 2.5f);
    }
    // Rivets and edges are no reason for a jump in the picture: the stretch is smoothed wide, so the sticker's edges stay smooth.
    std::vector<float> sum(cells), weight(cells), sum2(cells), weight2(cells);
    for (size_t i = 0; i < cells; i++) { bool on = s.chosen[i] < 1e29f; sum[i] = on ? raw[i] : 0; weight[i] = on ? 1.f : 0; }
    const int R = 10;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < gy; y++) for (int x = 0; x < gx; x++) {
            float a = 0, b = 0;
            for (int d = -R; d <= R; d++) { int c = x + d; if (c >= 0 && c < gx) { a += sum[(size_t)y * gx + c]; b += weight[(size_t)y * gx + c]; } }
            sum2[(size_t)y * gx + x] = a; weight2[(size_t)y * gx + x] = b;
        }
        for (int y = 0; y < gy; y++) for (int x = 0; x < gx; x++) {
            float a = 0, b = 0;
            for (int d = -R; d <= R; d++) { int c = y + d; if (c >= 0 && c < gy) { a += sum2[(size_t)c * gx + x]; b += weight2[(size_t)c * gx + x]; } }
            sum[(size_t)y * gx + x] = a; weight[(size_t)y * gx + x] = b;
        }
    }
    for (size_t i = 0; i < cells; i++) s.stretch[i] = weight[i] > 0 ? sum[i] / weight[i] : 1;
}
// where a point of the surface falls on the sticker; false = it does not
bool on_sticker(const StickerImage &s, const float *P, float look, float &u, float &v) {
    if (look < .25f || s.levels.empty()) return false;
    float d[3] = {P[0] - s.c[0], P[1] - s.c[1], P[2] - s.c[2]};
    float a = dot3(d, s.e1), b = dot3(d, s.e2);
    if (fabsf(a) >= s.half_w || fabsf(b) >= s.half_h) return false;
    int x = (int)((a + s.half_w) / s.cell), y = (int)((b + s.half_h) / s.cell);
    float depth = dot3(d, s.n), slope = sqrtf(fmaxf(0, 1 - look * look)) / look, room = s.cell * (1.5f + 2 * slope), nearest = 1e30f;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        int cx = x + dx, cy = y + dy;
        if (cx < 0 || cy < 0 || cx >= s.gx || cy >= s.gy) continue;
        const float *h = &s.levels[((size_t)cy * s.gx + cx) * STICKER_LAYERS];
        for (int k = 0; k < STICKER_LAYERS; k++) {
            if (h[k] > 1e29f) continue;
            float off = fabsf(h[k] - depth) + (dx || dy ? s.cell * .5f : 0);
            if (off < nearest) nearest = off;
        }
    }
    if (nearest > room) return false;
    float fx = (a + s.half_w) / s.cell - .5f, fy = (b + s.half_h) / s.cell - .5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    fx -= x0; fy -= y0;
    auto stretch_at = [&](int cx, int cy) {
        cx = cx < 0 ? 0 : cx >= s.gx ? s.gx - 1 : cx; cy = cy < 0 ? 0 : cy >= s.gy ? s.gy - 1 : cy;
        return s.stretch[(size_t)cy * s.gx + cx];
    };
    float k = (stretch_at(x0, y0) * (1 - fx) + stretch_at(x0 + 1, y0) * fx) * (1 - fy) + (stretch_at(x0, y0 + 1) * (1 - fx) + stretch_at(x0 + 1, y0 + 1) * fx) * fy;
    u = a * k / (2 * s.half_w) + .5f;
    v = b * k / (2 * s.half_h) + .5f;
    return u >= 0 && u < 1 && v >= 0 && v < 1;
}
// Worn paint: patches of it gone, by a noise of three sizes over the picture.
float wear_noise(float u, float v) {
    auto hash = [](int x, int y) { uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u; h = (h ^ h >> 13) * 1274126177u; return (float)((h ^ h >> 16) & 0xffff) / 65535.f; };
    float sum = 0, weight = 0, amp = 1, scale = 11;
    for (int o = 0; o < 4; o++, amp *= .55f, scale *= 2.3f) {
        float x = u * scale, y = v * scale;
        int ix = (int)floorf(x), iy = (int)floorf(y);
        float fx = x - ix, fy = y - iy;
        fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
        float a = hash(ix, iy), b = hash(ix + 1, iy), c = hash(ix, iy + 1), d = hash(ix + 1, iy + 1);
        sum += amp * ((a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy);
        weight += amp;
    }
    return sum / weight;
}
void sticker_pixel(const StickerImage &s, float u, float v, uint8_t io[4]) {
    CamoSettings blend;
    blend.opacity = s.opacity;
    blend.dim = s.pic->s.dim;
    if (s.wear > .001f) {
        float k = (wear_noise(u, v) - (s.wear * .75f + .12f)) / .1f + .5f;
        k = k < 0 ? 0 : k > 1 ? 1 : k;
        blend.opacity *= k * k * (3 - 2 * k);
    }
    uint8_t base[4] = {io[0], io[1], io[2], io[3]};
    camo_blend(s.pic->px.data(), s.pic->w, s.pic->h, s.flip_x ? 1 - u : u, s.flip_y ? 1 - v : v, base, blend, io);
}
// Stickers over a texel, only those in one place of the vehicle: what both sides share would show it twice. A texel two
// hatches share shows on both, so a sticker takes it only when it lies over both: a sticker on one hatch would appear on the other.
void stick(const VehicleMap &pr, size_t at, const std::vector<StickerImage> &list, uint8_t io[4]) {
    if (list.empty() || (pr.both[at] && !pr.twinned[at])) return;
    const float *P = &pr.pos[at * 3];
    const signed char *N = &pr.normal[at * 3];
    for (const StickerImage &s : list) {
        float u, v, look = (N[0] * s.n[0] + N[1] * s.n[1] + N[2] * s.n[2]) / 127.f;
        if (!on_sticker(s, P, look, u, v)) continue;
        float u2, v2;
        if (pr.twinned[at] && !on_sticker(s, &pr.twin[at * 3], look, u2, v2)) continue;
        sticker_pixel(s, u, v, io);
    }
}
Placement place_on(const VehicleMap &pr, const Pattern &p) { return place_picture(pr.lo, pr.hi, p.w ? p.w : 1, p.h ? p.h : 1, p.s); }
bool on_picture(const Placement &l, const float *P, float &u, float &v) {
    float a = P[0] - l.cx, b = fabsf(P[1] - l.cy) + l.top - P[2];
    if (!l.through) b -= l.height / 2;
    else if (P[1] < l.cy) b = -b;
    u = (l.ca * a - l.sa * b) / l.width + .5f + l.ox;
    v = (l.sa * a + l.ca * b) / l.height + .5f + l.oy;
    if (l.repeat) {
        u -= 2 * floorf(u / 2); v -= 2 * floorf(v / 2);
        if (u > 1) u = 2 - u;
        if (v > 1) v = 2 - v;
    } else if (u < 0 || u >= 1 || v < 0 || v >= 1) return false;
    if (l.flip_x) u = 1 - u;
    if (l.flip_y) v = 1 - v;
    return true;
}
bool texel(const VehicleMap &pr, int x, int y, int w, int h, size_t &at) {
    int tx = (int)((int64_t)x * pr.w / w), ty = (int)((int64_t)y * pr.h / h);
    if (tx >= pr.w || ty >= pr.h) return false;
    at = (size_t)ty * pr.w + tx;
    return pr.kind[at] == 1;
}
struct Variant { std::string file, token; };
void *variant_vtable[40];
void *variant_open(unsigned char *self, unsigned mode) {
    const Variant *v = *(Variant **)(self + 0x48);
    GamePath f = path_of(v->file);
    void *r = open_prev(&f, mode);
    if (!r) { rc_log("camo: %s not there (mode %x)", v->file.c_str(), mode); return r; }
    unsigned char *st = (unsigned char *)r;
    uintptr_t vt = *(uintptr_t *)st - base;
    unsigned char *b = *(unsigned char **)(st + 8), *e = *(unsigned char **)(st + 0x28);
    if (vt == VT_FILE) {
        // a file of its own on the disk, read a piece at a time: read here, and the game gets a stream over our copy
        const GameString *g = (const GameString *)(st + 0x48);
        std::string disk(g->cap > 15 ? g->ptr : g->buf, g->size);
        wchar_t wide[MAX_PATH * 2];
        std::vector<unsigned char> bytes;
        if (!MultiByteToWideChar(CP_UTF8, 0, disk.c_str(), -1, wide, MAX_PATH * 2) || !readfile(wide, bytes)) {
            rc_log("camo: cannot read %s, plain texture", disk.c_str());
            return r;
        }
        Painted *p = painted_of(v->file, v->token, bytes.data(), bytes.size());
        unsigned char *o = (unsigned char *)((void *(*)(size_t))(base + FN_NEW))(0x48);
        if (!o) return r;
        ((void *(*)(void *, void *, unsigned, int))(base + FN_BUF_STREAM))(o, p->data.data(), (unsigned)p->data.size(), -1);
        (*(void (**)(void *, int))*(uintptr_t *)st)(st, 1);
        return o;
    }
    if (!b || e <= b || *(uint64_t *)(st + 0x20) || *(unsigned char **)(st + 0x30) != b || (vt != VT_ZIP && vt != VT_BUF)) {
        rc_log("camo: %s: the stream is not whole (vtable %llx), plain texture", v->file.c_str(), (unsigned long long)vt);
        return r;
    }
    Painted *p = painted_of(v->file, v->token, b, (size_t)(e - b));
    unsigned char *c = p->data.data();
    size_t n = p->data.size();
    if (vt == VT_ZIP) memcpy(b, c, n);
    else {
        *(unsigned char **)(st + 8) = c; *(unsigned char **)(st + 0x10) = c + n; *(uint64_t *)(st + 0x18) = n;
        *(unsigned char **)(st + 0x30) = c; *(unsigned char **)(st + 0x28) = c + n;
    }
    return r;
}
void free_projection(VehicleMap *) {}               // the maps stay in make_projection's cache
// Which model a texture belongs to: every material of the game's vehicles, read once. The repository is a std::map of
// file names (root at [[1556ee8]+8]; node: +0 left, +0x10 right, +0x19 nil, +0x20 the name); its lock is 1556ec0.
constexpr uintptr_t G_REPOSITORY = 0x1556ee8, G_REPOSITORY_LOCK = 0x1556ec0;
void collect_names(uintptr_t node, std::vector<std::string> &out, int depth) {
    if (!node || depth > 200 || *(unsigned char *)(node + 0x19)) return;
    collect_names(*(uintptr_t *)node, out, depth + 1);
    const GameString *g = (const GameString *)(node + 0x20);
    const char *s = g->cap > 15 ? g->ptr : g->buf;
    if (g->size > 12 && g->size < 300 && (!memcmp(s + g->size - 4, ".mtl", 4) || !memcmp(s + g->size - 4, ".mdl", 4)) &&
        strstr(s, "vehicle")) out.emplace_back(s, g->size);
    collect_names(*(uintptr_t *)(node + 0x10), out, depth + 1);
}
std::vector<std::pair<std::string, std::string>> index;              // texture, the material's folder
std::vector<std::string> models;
void build_index() {
    if (index.empty()) {
        DWORD t0 = GetTickCount();
        std::vector<std::string> names;
        CRITICAL_SECTION *lock = (CRITICAL_SECTION *)(base + G_REPOSITORY_LOCK);
        EnterCriticalSection(lock);
        collect_names(*(uintptr_t *)(*(uintptr_t *)(base + G_REPOSITORY) + 8), names, 0);
        LeaveCriticalSection(lock);
        for (const auto &n : names) {
            size_t at = n.find("/entity/");
            if (at == std::string::npos) continue;
            std::string path = n.substr(at);
            if (!path.compare(path.size() - 4, 4, ".mdl")) { models.push_back(path); continue; }
            std::vector<unsigned char> m;
            if (!read_game_file(path, m)) continue;
            std::string s(m.begin(), m.end());
            for (auto &c : s) c = (char)tolower((unsigned char)c);
            size_t d = s.find("diffuse"), q0 = d == std::string::npos ? d : s.find('"', d), q1 = q0 == std::string::npos ? q0 : s.find('"', q0 + 1);
            if (q1 == std::string::npos) continue;
            std::string tex = s.substr(q0 + 1, q1 - q0 - 1);
            if (!tex.compare(0, 2, "$/")) tex = tex.substr(2);
            index.push_back({tex, path.substr(0, path.rfind('/') + 1)});
        }
        rc_log("camo: %d names, %d materials, %d models read in %lu ms", (int)names.size(), (int)index.size(), (int)models.size(), GetTickCount() - t0);
    }
}
std::vector<std::string> models_of_texture(std::string stem) {
    build_index();
    for (auto &c : stem) c = (char)tolower((unsigned char)c);
    std::vector<std::string> found;
    for (const auto &e : index) {
        if (e.first != stem) continue;
        for (const auto &m : models)
            if (!m.compare(0, e.second.size(), e.second) && m.find('/', e.second.size()) == std::string::npos &&
                std::find(found.begin(), found.end(), m) == found.end()) found.push_back(m);
    }
    return found;
}
bool make_projection(const std::string &file, const unsigned char *f, size_t n, const std::string &model, std::vector<StickerImage> &stickers, VehicleMap *&out) {
    uint32_t h, w;
    if (n < 128) return false;
    memcpy(&h, f + 12, 4); memcpy(&w, f + 16, 4);
    if (w > 4096 || h > 4096) return false;
    size_t at = file.find("/texture/common/");
    if (at == std::string::npos) return false;
    std::string stem = undamaged(stem_of(file.c_str(), file.size()));
    // several models can use a texture, some only for a few parts: the one that covers most of it, if it isn't known
    std::vector<std::string> list;
    if (!model.empty()) list.push_back(model);
    else list = models_of_texture(stem);
    // A vehicle's model is read once, the place of a texture's texels worked out once: the plain, _x and _xx textures of a
    // vehicle and all its camouflages use the same ones. The caller holds paint_lock.
    struct Read { std::string name; std::shared_ptr<Model> m; std::vector<unsigned char> seen; };
    static std::vector<std::shared_ptr<Read>> models_read;
    struct Baked { std::string key; std::unique_ptr<VehicleMap> map; };
    static Baked baked[2];
    static int baked_next;
    std::shared_ptr<Read> best;
    int most = 0, layer = -1, tried = 0;
    for (const auto &name : list) {
        if (++tried > 16) break;
        std::shared_ptr<Read> r;
        for (auto &e : models_read) if (e->name == name) r = e;
        if (!r) {
            r = std::make_shared<Read>();
            r->name = name;
            r->m = std::make_shared<Model>();
            if (!load_model(name, *r->m)) continue;
            if (models_read.size() >= 6) models_read.erase(models_read.begin());
            models_read.push_back(r);
        }
        int l = (int)(std::find(r->m->stems.begin(), r->m->stems.end(), stem) - r->m->stems.begin()), count = 0;
        for (const Triangle &t : r->m->all) count += t.layer == l;
        if (l < (int)r->m->stems.size() && count > most) { most = count; layer = l; best = r; }
    }
    if (!best) { rc_log("camo: no model for %s: the picture goes on as a pattern", stem.c_str()); return false; }
    if (best->seen.empty()) best->seen = seen_of(*best->m);
    char size[32];
    snprintf(size, sizeof size, "|%ux%u", w, h);
    std::string key = best->name + "|" + stem + size;
    VehicleMap *r = nullptr;
    for (auto &b : baked) if (b.key == key) r = b.map.get();
    if (!r) {
        Baked &b = baked[baked_next];
        baked_next = (baked_next + 1) % 2;
        b.key = key;
        b.map.reset(new VehicleMap);
        r = b.map.get();
        r->w = w; r->h = h;
        bake_layer(*r, *best->m, best->seen, layer);
    }
    for (StickerImage &s : stickers) sticker_lay(s, *best->m, best->seen);
    out = r;
    return true;
}
void link_variant(const std::string &name, const std::string &plain, const std::string &token) {
    static std::vector<std::string> done;
    AcquireSRWLockExclusive(&paint_lock);
    bool first = std::find(done.begin(), done.end(), name) == done.end();
    if (first) done.push_back(name);
    ReleaseSRWLockExclusive(&paint_lock);
    if (!first) return;
    rc_log("camo: linking %s", name.c_str());
    std::string file = texture_file(plain);
    if (file.empty()) return;
    rc_log("camo: its file is %s", file.c_str());
    std::string alias = name + file.substr(file.rfind('.'));
    GamePath f = path_of(file), a = path_of(alias);
    uintptr_t entry = (uintptr_t)((void *(*)(void *))(base + FN_FILE_ENTRY))(&f);
    rc_log("camo: entry %p, vtable %llx", (void *)entry, (unsigned long long)(entry ? *(uintptr_t *)entry - base : 0));
    if (!variant_vtable[2]) {
        memcpy(variant_vtable, (void **)(base + VT_ABSTRACT_STORAGE) - 1, sizeof variant_vtable);
        variant_vtable[2] = (void *)variant_open;
    }
    unsigned char *o = (unsigned char *)((void *(*)(size_t))(base + FN_NEW))(0x60);
    if (!o) return;
    memset(o, 0, 0x60);
    *(void **)o = &variant_vtable[1];
    if (entry) *(uint64_t *)(o + 0x18) = *(uint64_t *)(entry + 0x18);
    ((void *(*)(void *))(base + FN_PATH_NEW))(o + 0x20);
    ((void *(*)(void *, const char *, size_t))(base + FN_STR_SET))(o + 0x20, alias.c_str(), alias.size());
    *(Variant **)(o + 0x48) = new Variant{file, token};
    alignas(16) unsigned char key[0x40];
    ((void *(*)(void *))(base + FN_PATH_NEW))(key);
    void *k = ((void *(*)(void *, void *))(base + FN_PATH_KEY))(&a, key);
    ((void (*)(void *, void *))(base + FN_FILE_ADD))(o, k);
    ((void (*)(void *))(base + FN_PATH_FREE))(key);
    rc_log("camo: %s is in the repository", alias.c_str());
}
unsigned char tex_find(void *in, void *out, char *flag) {
    const GameString *g = (const GameString *)in;
    if (g->size >= 8 && g->size <= 400) {
        std::string name(g->cap > 15 ? g->ptr : g->buf, g->size);
        size_t at = name.find("#gohcamo");
        if (at != std::string::npos) {
            size_t end = name.find_first_of("#.", at + 1);
            std::string stem = name.substr(0, end == std::string::npos ? name.size() : end);
            link_variant(stem, stem.substr(0, at), stem.substr(at + 1));
        }
    }
    return tex_find_real(in, out, flag);
}

}

// pvp.c's file open: 1 = ours, what the game gets is in *out
extern "C" int camo_open(void *, unsigned, void *(*prev)(void *, unsigned), void **) {
    open_prev = prev;
    return 0;
}

// Every texture of every camouflage set, with its damaged versions, is painted here, in the background, in the menus. A
// vehicle coming into a battle then finds them ready: painting at that moment stalls the game.
volatile LONG warming;
DWORD WINAPI warm_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    AcquireSRWLockShared(&published_lock);
    std::vector<Published> list = published;
    ReleaseSRWLockShared(&published_lock);
    DWORD t0 = GetTickCount();
    int done = 0;
    for (const Published &e : list) for (const std::string &stem : e.all) {
        if (is_track(stem)) continue;
        for (const char *end : {"", "_x", "_xx"}) {
            std::string file = texture_file("/texture/common/" + stem + end);
            std::vector<unsigned char> bytes;
            if (file.empty() || !read_game_file(file, bytes)) continue;
            painted_of(file, e.token, bytes.data(), bytes.size());
            done++;
        }
    }
    if (done) rc_log("camo: %d textures painted ahead, %lu ms", done, GetTickCount() - t0);
    InterlockedExchange(&warming, 0);
    return 0;
}
void warm_start() {
    static DWORD started;
    if (!started) started = GetTickCount();
    if (!open_prev || GetTickCount() - started < 8000) return;      // the game's files first
    if (InterlockedCompareExchange(&warming, 1, 0)) return;
    InterlockedExchange(&warm_needed, 0);
    HANDLE h = CreateThread(nullptr, 1 << 20, warm_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h); else InterlockedExchange(&warming, 0);
}
// [debug] camo_selftest=1 in goh_snapshots.ini: in the menu, what a vehicle would do with our variant, without a battle
void selftest() {
    std::string token = "gohcamoselftest";
    std::string name = "/texture/common/_vehicle/t34_76_41#" + token;
    rc_log("camo selftest: %s", name.c_str());
    GamePath p = path_of(name);
    alignas(16) unsigned char out[0x40];
    char flag = 0;
    ((void *(*)(void *))(base + FN_PATH_NEW))(out);
    unsigned char found = tex_find(&p, out, &flag);
    const GameString *o = (const GameString *)out;
    std::string file(o->cap > 15 ? o->ptr : o->buf, o->size);
    ((void (*)(void *))(base + FN_PATH_FREE))(out);
    rc_log("camo selftest: found %d, flag %d, file %s", found, flag, file.c_str());
    if (!found) return;
    GamePath f = path_of(file);
    void *st = open_prev(&f, 0x201);
    rc_log("camo selftest: opened %p", st);
    void *bitmap = ((void *(*)(void *))(base + FN_BITMAP))(&p);
    rc_log("camo selftest: bitmap %p, vtable %llx", bitmap, (unsigned long long)(bitmap ? *(uintptr_t *)bitmap - base : 0));
    if (bitmap) {
        ((void (*)(void *, int, bool))(base + FN_BITMAP_LOAD))(bitmap, 1, true);
        rc_log("camo selftest: bitmap loaded");
        if (!painted.empty()) {
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%s\\selftest.dds", folder);
            FILE *d = _wfopen(path, L"wb");
            if (d) { fwrite(painted.back()->data.data(), 1, painted.back()->data.size(), d); fclose(d); }
        }
    }
}
void preview_test();
extern "C" void camo_selftest_tick(const char *ini_path) {
    static int state;                            // 0 asks, 1 waits, 2 done
    static DWORD at;
    if (state == 2) return;
    if (!state) {
        if (!GetPrivateProfileIntA("debug", "camo_selftest", 0, ini_path)) { state = 2; return; }
        state = 1; at = GetTickCount() + 15000;
    }
    if ((LONG)(GetTickCount() - at) >= 0 && open_prev) { state = 2; selftest(); preview_test(); }
}

extern "C" void camo_install(unsigned char *exe, unsigned char *(*alloc)(size_t), void (*patch)(void *, const void *, size_t)) {
    base = (uintptr_t)exe;
    for (uintptr_t site : TEX_MODS_CALLS) {
        int32_t rel; memcpy(&rel, exe + site + 1, 4);
        if (exe[site] != 0xe8 || site + 5 + rel != FN_TEX_MODS) { rc_log("camo: the texture lists are not as expected, no camouflage"); return; }
    }
    tex_mods_real = (void *(*)(void *, void *))(exe + FN_TEX_MODS);
    unsigned char *s = alloc(16);
    if (!s) return;
    uint64_t fn = (uint64_t)(uintptr_t)tex_mods;
    s[0] = 0x48; s[1] = 0xb8; memcpy(s + 2, &fn, 8); s[10] = 0xff; s[11] = 0xe0;
    for (uintptr_t site : TEX_MODS_CALLS) {
        int32_t rel = (int32_t)(s - (exe + site + 5));
        patch(exe + site + 1, &rel, 4);
    }
    for (uintptr_t site : TEX_FIND_CALLS) {
        int32_t rel; memcpy(&rel, exe + site + 1, 4);
        if (exe[site] != 0xe8 || site + 5 + rel != FN_TEX_FIND) { rc_log("camo: the texture search is not as expected"); return; }
    }
    tex_find_real = (unsigned char (*)(void *, void *, char *))(exe + FN_TEX_FIND);
    unsigned char *f = alloc(16);
    if (!f) return;
    fn = (uint64_t)(uintptr_t)tex_find;
    f[0] = 0x48; f[1] = 0xb8; memcpy(f + 2, &fn, 8); f[10] = 0xff; f[11] = 0xe0;
    for (uintptr_t site : TEX_FIND_CALLS) {
        int32_t rel = (int32_t)(f - (exe + site + 5));
        patch(exe + site + 1, &rel, 4);
    }
    void *slot = nullptr;
    memcpy(&slot, exe + VT_ACTOR + 8 * 23, 8);
    if (slot == exe + FN_ACTOR_QUANT) {
        actor_quant_real = (void *(*)(void *))slot;
        void *ours = (void *)actor_quant;
        patch(exe + VT_ACTOR + 8 * 23, &ours, 8);
    } else rc_log("camo: the actors are not as expected, no camouflage by itself");
    rc_log("camo: texture variants ready");
}

// ---- the tab: the vehicles, one of them in 3D with its camouflage ------------------------------------------------
namespace {
struct Layer {
    std::string stem;
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    float average = 128;
    VehicleMap map;                                   // empty: takes no picture
};
struct Preview {
    Model m;
    std::vector<unsigned char> seen;
    std::string model;
    std::vector<std::shared_ptr<Layer>> layers;
    std::vector<std::string> all, main;
    size_t shared = 0, single = 0;                    // texels on both sides / in one place: for the log
};
// a DDS's first level, DXT1 and DXT5 as all the vehicles have
bool decode_dds(const std::vector<unsigned char> &f, Layer &l) {
    if (f.size() < 128 || memcmp(f.data(), "DDS ", 4)) return false;
    uint32_t h, w, four, pixel_bits;
    memcpy(&h, &f[12], 4); memcpy(&w, &f[16], 4); memcpy(&four, &f[84], 4); memcpy(&pixel_bits, &f[88], 4);
    int size = four == 0x31545844 ? 8 : four == 0x35545844 || four == 0x33545844 ? 16 : 0;
    if (!w || !h || w > 4096 || h > 4096) return false;
    if (!four && pixel_bits == 32) {                  // plain pixels
        uint32_t mask[4];
        memcpy(mask, &f[92], 16);
        if (!mask[0] || !mask[1] || !mask[2] || 128 + (size_t)w * h * 4 > f.size()) return false;
        int sr = shift_of(mask[0]), sg = shift_of(mask[1]), sb = shift_of(mask[2]), sa = shift_of(mask[3]);
        l.w = w; l.h = h;
        l.rgba.resize((size_t)w * h * 4);
        double all = 0;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint32_t v;
            memcpy(&v, &f[128 + i * 4], 4);
            unsigned char *o = &l.rgba[i * 4];
            o[0] = v >> sr & 255; o[1] = v >> sg & 255; o[2] = v >> sb & 255; o[3] = mask[3] ? v >> sa & 255 : 255;
            all += o[0] * .3 + o[1] * .59 + o[2] * .11;
        }
        l.average = (float)(all / ((double)w * h));
        if (l.average < 1) l.average = 1;
        return true;
    }
    if (!size) return false;
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (128 + (size_t)bw * bh * size > f.size()) return false;
    l.w = w; l.h = h;
    l.rgba.assign((size_t)w * h * 4, 255);
    double sum = 0;
    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        const unsigned char *b = &f[128 + ((size_t)by * bw + bx) * size];
        int alpha[8];
        uint64_t abits = 0;
        bool sharp = four == 0x33545844;              // DXT3: four bits of alpha per pixel
        const unsigned char *sharp_alpha = b;
        if (size == 16 && sharp) b += 8;
        else if (size == 16) {
            alpha[0] = b[0]; alpha[1] = b[1];
            for (int i = 2; i < 8; i++) alpha[i] = alpha[0] > alpha[1] ? ((8 - i) * alpha[0] + (i - 1) * alpha[1]) / 7 :
                i < 6 ? ((6 - i) * alpha[0] + (i - 1) * alpha[1]) / 5 : i == 6 ? 0 : 255;
            memcpy(&abits, b + 2, 6);
            b += 8;
        }
        uint16_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
        uint32_t bits = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
        int pal[4][3];
        rgb_of(c0, pal[0]); rgb_of(c1, pal[1]);
        for (int i = 0; i < 3; i++) {
            if (c0 > c1 || size == 16) { pal[2][i] = (2 * pal[0][i] + pal[1][i]) / 3; pal[3][i] = (pal[0][i] + 2 * pal[1][i]) / 3; }
            else { pal[2][i] = (pal[0][i] + pal[1][i]) / 2; pal[3][i] = 0; }
        }
        for (int i = 0; i < 16; i++) {
            int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
            if (x >= (int)w || y >= (int)h) continue;
            int k = bits >> (2 * i) & 3;
            unsigned char *o = &l.rgba[((size_t)y * w + x) * 4];
            o[0] = (unsigned char)pal[k][0]; o[1] = (unsigned char)pal[k][1]; o[2] = (unsigned char)pal[k][2];
            o[3] = sharp ? (unsigned char)((sharp_alpha[i / 2] >> (i & 1) * 4 & 15) * 17) : size == 16 ? (unsigned char)alpha[abits >> (3 * i) & 7] : 255;
            sum += pal[k][0] * .3 + pal[k][1] * .59 + pal[k][2] * .11;
        }
    }
    l.average = (float)(sum / ((double)w * h));
    if (l.average < 1) l.average = 1;
    return true;
}
std::shared_ptr<Preview> load_preview(const std::string &mdl) {
    DWORD t0 = GetTickCount();
    auto p = std::make_shared<Preview>();
    p->model = mdl;
    if (!load_model(mdl, p->m)) return nullptr;
    std::vector<unsigned char> &seen = p->seen;
    seen = seen_of(p->m);
    std::vector<int> count(p->m.stems.size());
    for (const Triangle &t : p->m.all) if (t.layer >= 0) count[t.layer]++;
    int most = 0;
    for (size_t i = 0; i < count.size(); i++) if (!is_track(p->m.stems[i]) && count[i] > most) most = count[i];
    for (size_t i = 0; i < p->m.stems.size(); i++) {
        auto l = std::make_shared<Layer>();
        l->stem = p->m.stems[i];
        p->layers.push_back(l);
        std::string file = texture_file("/texture/common/" + l->stem);
        std::vector<unsigned char> bytes;
        bool got = !file.empty() && read_game_file(file, bytes);
        if (!got || !decode_dds(bytes, *l)) { rc_log("camo: %s not shown: file \"%s\", read %d, %d bytes", l->stem.c_str(), file.c_str(), got, (int)bytes.size()); continue; }
        if (is_track(l->stem) || !is_camo_texture(("_vehicle/" + l->stem).c_str(), l->stem.size() + 9)) continue;
        p->all.push_back(l->stem);
        if (count[i] * 10 >= most * 3) p->main.push_back(l->stem);
        l->map.w = l->w > 1024 ? 1024 : l->w;
        l->map.h = l->h > 1024 ? 1024 : l->h;
        bake_layer(l->map, p->m, seen, (int)i);
        for (size_t k = 0; k < l->map.kind.size(); k++) if (l->map.kind[k] == 1) { if (l->map.both[k]) p->shared++; else p->single++; }
    }
    rc_log("camo: %s for the tab: %d triangles, %d textures, %lu ms, both sides share %d%% of the outside", mdl.c_str(), (int)p->m.all.size(),
           (int)p->layers.size(), GetTickCount() - t0, (int)(p->shared * 100 / (p->shared + p->single + 1)));
    return p;
}
// a texture coordinate as a row or column; the models have some that are no numbers
int cell(float t, int size) {
    float f = t - floorf(t);
    int i = f >= 0 && f < 1 ? (int)(f * size) : 0;
    return i < size ? i : size - 1;
}
struct View3D { float yaw = 300, pitch = 22, zoom = 1.2f; };
// The vehicle as the game will paint it: the same place on the picture, the same mix, per pixel of the view. The rows
// are shared out between the cores; `fine` draws twice the pixels and averages them, for when nothing moves.
struct Screen { float x[3], y[3], d[3]; };
struct Buffers {
    std::vector<int> ids;
    std::vector<float> depth, bary;
    std::vector<Screen> screen;
    std::vector<unsigned char> out;
    std::vector<float> pick;                          // per pixel of the view: place and normal, x 1e30 for nothing
};
struct Band {
    const Preview *p; const Pattern *pic; const CamoSettings *s; Placement place;
    const std::vector<StickerImage> *stickers;
    Buffers *b; int w, h, ss, W, y0, y1;
    float light[3];
};
bool debug_kinds;
DWORD WINAPI draw_band(LPVOID arg) {
    const Band &B = *(const Band *)arg;
    const Preview &p = *B.p;
    Buffers &buf = *B.b;
    const int w = B.w, h = B.h;
    for (int y = B.y0; y < B.y1; y++) {
        std::fill(buf.ids.begin() + (size_t)y * w, buf.ids.begin() + (size_t)(y + 1) * w, -1);
        std::fill(buf.depth.begin() + (size_t)y * w, buf.depth.begin() + (size_t)(y + 1) * w, 1e30f);
    }
    for (size_t n = 0; n < p.m.all.size(); n++) {
        const Screen &q = buf.screen[n];
        const float *x = q.x, *y = q.y, *d = q.d;
        int y0 = (int)fmaxf(fminf(y[0], fminf(y[1], y[2])), (float)B.y0), y1 = (int)fminf(fmaxf(y[0], fmaxf(y[1], y[2])) + 2, (float)B.y1);
        if (y1 <= y0) continue;
        float den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2]);
        if (fabsf(den) < 1e-6f) continue;
        int x0 = (int)fmaxf(fminf(x[0], fminf(x[1], x[2])), 0), x1 = (int)fminf(fmaxf(x[0], fmaxf(x[1], x[2])) + 2, (float)w);
        const Triangle &t = p.m.all[n];
        const Layer *l = t.layer >= 0 ? p.layers[t.layer].get() : nullptr;
        bool cut = l && !l->rgba.empty();
        float ax = (y[1] - y[2]) / den, ay = (x[2] - x[1]) / den, bx = (y[2] - y[0]) / den, by = (x[0] - x[2]) / den;
        for (int py = y0; py < y1; py++) {
            float gx = x0 + .5f - x[2], gy = py + .5f - y[2];
            float a = ax * gx + ay * gy, b = bx * gx + by * gy;
            for (int px = x0; px < x1; px++, a += ax, b += bx) {
                float c = 1 - a - b;
                if (a < 0 || b < 0 || c < 0) continue;
                float z = a * d[0] + b * d[1] + c * d[2];
                size_t at = (size_t)py * w + px;
                if (z >= buf.depth[at]) continue;
                if (cut) {                            // {blend test}: the holes of grilles and the like
                    float tu = a * t.uv[0][0] + b * t.uv[1][0] + c * t.uv[2][0], tv = a * t.uv[0][1] + b * t.uv[1][1] + c * t.uv[2][1];
                    if (l->rgba[((size_t)cell(tv, l->h) * l->w + cell(tu, l->w)) * 4 + 3] < 25) continue;
                }
                buf.depth[at] = z; buf.ids[at] = (int)n; buf.bary[at * 2] = a; buf.bary[at * 2 + 1] = b;
            }
        }
    }
    const float *light = B.light;
    const Pattern *pic = B.pic;
    CamoSettings shown = *B.s;
    if (pic) shown.dim = pic->s.dim;
    unsigned char row[2][4096 * 3];
    for (int y = B.y0; y < B.y1; y++) {
        unsigned char *line = row[(y - B.y0) % B.ss];
        for (int x = 0; x < w; x++) {
            size_t at = (size_t)y * w + x;
            unsigned char *o = &line[x * 3];
            if (buf.ids[at] < 0) {
                o[0] = o[1] = o[2] = (unsigned char)(46 - 16 * y / h);
                if (y % B.ss == 0 && x % B.ss == 0) buf.pick[((size_t)(y / B.ss) * B.W + x / B.ss) * 6] = 1e30f;
                continue;
            }
            const Triangle &t = p.m.all[buf.ids[at]];
            float a = buf.bary[at * 2], b = buf.bary[at * 2 + 1], c = 1 - a - b;
            float nx = a * t.n[0][0] + b * t.n[1][0] + c * t.n[2][0], ny = a * t.n[0][1] + b * t.n[1][1] + c * t.n[2][1], nz = a * t.n[0][2] + b * t.n[1][2] + c * t.n[2][2];
            float nl = sqrtf(nx * nx + ny * ny + nz * nz) + 1e-6f, dot = (nx * light[0] + ny * light[1] + nz * light[2]) / nl;
            float shade = .5f + .62f * (dot > 0 ? dot : 0);
            const Layer *l = t.layer >= 0 ? p.layers[t.layer].get() : nullptr;
            unsigned char col[3] = {96, 96, 96};
            if (l && !l->rgba.empty()) {
                float tu = a * t.uv[0][0] + b * t.uv[1][0] + c * t.uv[2][0], tv = a * t.uv[0][1] + b * t.uv[1][1] + c * t.uv[2][1];
                const unsigned char *e = &l->rgba[((size_t)cell(tv, l->h) * l->w + cell(tu, l->w)) * 4];
                memcpy(col, e, 3);
                if (!l->map.kind.empty() && (pic || !B.stickers->empty())) {
                    size_t m = (size_t)cell(tv, l->map.h) * l->map.w + cell(tu, l->map.w);
                    if (l->map.kind[m] == 1) {
                        float pu, pv;
                        float lum = e[0] * .3f + e[1] * .59f + e[2] * .11f, keep = lum / l->average;
                        keep = keep < .45f ? .45f : keep > 1.6f ? 1.6f : keep;
                        uint8_t flat[4], out[4];
                        for (int i = 0; i < 3; i++) { float f = e[i] / keep; flat[i] = (uint8_t)(f > 255 ? 255 : f); }
                        flat[3] = 255;
                        memcpy(out, flat, 4);
                        if (pic && on_picture(B.place, texel_place(l->map, m), pu, pv)) camo_blend(pic->px.data(), pic->w, pic->h, pu, pv, flat, shown, out);
                        if (l->map.twinned[m]) stick(l->map, m, *B.stickers, out);
                        else if (!l->map.both[m] && !t.gun) for (const StickerImage &k : *B.stickers) {      // exact: per pixel, not per texel
                            float place[3], su, sv, look = (nx * k.n[0] + ny * k.n[1] + nz * k.n[2]) / nl;
                            for (int i = 0; i < 3; i++) place[i] = a * t.p[0][i] + b * t.p[1][i] + c * t.p[2][i];
                            if (on_sticker(k, place, look, su, sv)) sticker_pixel(k, su, sv, out);
                        }
                        for (int i = 0; i < 3; i++) { float f = out[i] * keep; col[i] = (unsigned char)(f > 255 ? 255 : f); }
                    }
                }
            }
            if (debug_kinds && l) {                   // self test: red not outside, blue used at several places, green both sides
                float tu = a * t.uv[0][0] + b * t.uv[1][0] + c * t.uv[2][0], tv = a * t.uv[0][1] + b * t.uv[1][1] + c * t.uv[2][1];
                int k = l->map.kind.empty() ? 9 : 0;
                size_t m = 0;
                if (!k) { m = (size_t)cell(tv, l->map.h) * l->map.w + cell(tu, l->map.w); k = l->map.kind[m]; if (k == 1 && l->map.both[m]) k = 3; }
                static const unsigned char TINT[4][3] = {{220, 40, 40}, {120, 120, 120}, {40, 70, 230}, {40, 200, 70}};
                if (k < 4) memcpy(col, TINT[k], 3);
            }
            if (y % B.ss == 0 && x % B.ss == 0) {     // where the mouse would be on the vehicle
                float *k = &buf.pick[((size_t)(y / B.ss) * B.W + x / B.ss) * 6];
                for (int i = 0; i < 3; i++) k[i] = a * t.p[0][i] + b * t.p[1][i] + c * t.p[2][i];
                k[3] = nx / nl; k[4] = ny / nl; k[5] = nz / nl;
            }
            for (int i = 0; i < 3; i++) { float f = col[i] * shade; o[i] = (unsigned char)(f > 255 ? 255 : f); }
        }
        if ((y - B.y0) % B.ss != B.ss - 1) continue;
        unsigned char *dst = &buf.out[(size_t)(y / B.ss) * B.W * 4];
        if (B.ss == 1) for (int x = 0; x < B.W; x++) { memcpy(dst + x * 4, &line[x * 3], 3); dst[x * 4 + 3] = 255; }
        else for (int x = 0; x < B.W; x++) {
            for (int i = 0; i < 3; i++) {
                const unsigned char *u = &row[0][x * 6 + i], *v = &row[1][x * 6 + i];
                dst[x * 4 + i] = (unsigned char)((u[0] + u[3] + v[0] + v[3] + 2) / 4);
            }
            dst[x * 4 + 3] = 255;
        }
    }
    return 0;
}
void render_view(Buffers &buf, const Preview &p, const Pattern *pic, const std::vector<StickerImage> &stickers, const CamoSettings &s, int W, int H, const View3D &view, bool fine) {
    const int ss = fine ? 2 : 1, w = W * ss, h = H * ss;
    float yaw = view.yaw * .017453292f, pitch = view.pitch * .017453292f;
    float eye[3] = {cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), sinf(pitch)};
    float r[3] = {eye[1], -eye[0], 0}, len = sqrtf(r[0] * r[0] + r[1] * r[1]);
    if (len < 1e-3f) { r[0] = 1; r[1] = 0; } else { r[0] /= len; r[1] /= len; }
    float u[3] = {r[1] * eye[2] - r[2] * eye[1], r[2] * eye[0] - r[0] * eye[2], r[0] * eye[1] - r[1] * eye[0]};
    float centre[3], radius = 1;
    for (int i = 0; i < 3; i++) { centre[i] = (p.m.lo[i] + p.m.hi[i]) / 2; radius += (p.m.hi[i] - p.m.lo[i]) * (p.m.hi[i] - p.m.lo[i]) / 4; }
    radius = sqrtf(radius);
    float scale = (w < h * 1.6f ? w / 1.6f : h) / (2 * radius) * 1.45f * view.zoom;
    buf.ids.resize((size_t)w * h); buf.depth.resize((size_t)w * h); buf.bary.resize((size_t)w * h * 2);
    buf.out.resize((size_t)W * H * 4);
    buf.pick.resize((size_t)W * H * 6);
    buf.screen.resize(p.m.all.size());
    for (size_t n = 0; n < p.m.all.size(); n++) for (int i = 0; i < 3; i++) {
        const float *v = p.m.all[n].p[i];
        float q[3] = {v[0] - centre[0], v[1] - centre[1], v[2] - centre[2]};
        buf.screen[n].x[i] = w / 2.f - (q[0] * r[0] + q[1] * r[1] + q[2] * r[2]) * scale;
        buf.screen[n].y[i] = h / 2.f - (q[0] * u[0] + q[1] * u[1] + q[2] * u[2]) * scale;
        buf.screen[n].d[i] = -(q[0] * eye[0] + q[1] * eye[1] + q[2] * eye[2]);
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors - 1;
    n = n < 1 ? 1 : n > 8 ? 8 : n;
    Band bands[8];
    HANDLE threads[8];
    int rows = (H + n - 1) / n * ss, started = 0;
    for (int i = 0; i < n && i * rows < h; i++) {
        Band &b = bands[i];
        b = {&p, pic, &s, Placement(), &stickers, &buf, w, h, ss, W, i * rows, i * rows + rows < h ? i * rows + rows : h, {}};
        if (pic) b.place = place_picture(p.m.lo, p.m.hi, pic->w, pic->h, s);
        for (int k = 0; k < 3; k++) b.light[k] = eye[k] * .55f + r[k] * .35f + u[k] * .6f;
        threads[started] = i + 1 < n && (i + 1) * rows < h ? CreateThread(nullptr, 64 << 10, draw_band, &b, 0, nullptr) : nullptr;
        if (!threads[started]) draw_band(&b); else started++;
    }
    if (started) WaitForMultipleObjects(started, threads, TRUE, 2000);
    for (int i = 0; i < started; i++) CloseHandle(threads[i]);
}
struct Vehicle { std::string model, name, country, kind; };
std::vector<Vehicle> vehicles;
volatile LONG vehicles_state;                          // 0 not asked, 1 reading, 2 ready
DWORD WINAPI read_vehicles(LPVOID) {
    AcquireSRWLockExclusive(&paint_lock);
    build_index();
    std::vector<std::string> list = models;
    ReleaseSRWLockExclusive(&paint_lock);
    std::vector<Vehicle> out;
    for (const auto &m : list) {
        // /entity/-vehicle/ussr/tank_heavy/is2_44/is2_44.mdl
        size_t at = m.find("/-vehicle/");
        if (at == std::string::npos || m.find("/x/") != std::string::npos) continue;
        std::vector<std::string> parts;
        for (size_t i = at + 10; i < m.size();) {
            size_t e = m.find('/', i);
            if (e == std::string::npos) e = m.size();
            parts.push_back(m.substr(i, e - i));
            i = e + 1;
        }
        if (parts.size() != 4 || parts[3] != parts[2] + ".mdl") continue;
        const std::string &k = parts[1];
        if (k != "tank_light" && k != "tank_medium" && k != "tank_heavy" && k != "btr" && k != "car" && k.find("spg") == std::string::npos) continue;
        out.push_back({m, parts[2], parts[0], k});
    }
    std::sort(out.begin(), out.end(), [](const Vehicle &a, const Vehicle &b) {
        return a.country != b.country ? a.country < b.country : a.kind != b.kind ? a.kind < b.kind : a.name < b.name; });
    AcquireSRWLockExclusive(&lock);
    vehicles = std::move(out);
    ReleaseSRWLockExclusive(&lock);
    InterlockedExchange(&vehicles_state, 2);
    return 0;
}
// The vehicle in the view is read by one thread at a time, the last one asked for; the picture is drawn by another.
std::string wanted, shown_model;
std::shared_ptr<Preview> shown, arrived;
bool loader_running;
DWORD WINAPI loader(LPVOID) {
    for (;;) {
        AcquireSRWLockExclusive(&lock);
        std::string m = wanted;
        if (m == shown_model) { loader_running = false; ReleaseSRWLockExclusive(&lock); return 0; }
        shown_model = m;
        ReleaseSRWLockExclusive(&lock);
        std::shared_ptr<Preview> p = m.empty() ? nullptr : load_preview(m);
        AcquireSRWLockExclusive(&lock);
        arrived = p;
        ReleaseSRWLockExclusive(&lock);
    }
}
void want_model(const std::string &m) {
    AcquireSRWLockExclusive(&lock);
    wanted = m;
    bool start = !loader_running;
    loader_running = true;
    ReleaseSRWLockExclusive(&lock);
    if (start) {
        HANDLE t = CreateThread(nullptr, 1 << 20, loader, nullptr, 0, nullptr);
        if (t) CloseHandle(t); else { AcquireSRWLockExclusive(&lock); loader_running = false; ReleaseSRWLockExclusive(&lock); }
    }
}
struct Job { std::shared_ptr<Preview> p; std::shared_ptr<Pattern> pic; std::vector<StickerImage> stickers; CamoSettings s; int w, h; View3D view; bool fine; };
volatile LONG drawing;
std::vector<unsigned char> frame;
std::vector<float> frame_pick, view_pick;              // the place under each pixel: drawn / on screen
int frame_w, frame_h, view_w, view_h;
bool frame_new;
DWORD WINAPI draw_job(LPVOID arg) {
    std::unique_ptr<Job> j((Job *)arg);
    static Buffers buf;
    static std::vector<StickerImage> laid;
    static const Preview *laid_on;
    if (laid_on != j->p.get()) { laid.clear(); laid_on = j->p.get(); }
    for (StickerImage &s : j->stickers) {
        bool same = false;
        for (const StickerImage &o : laid)
            if (!memcmp(o.c, s.c, sizeof s.c) && !memcmp(o.n, s.n, sizeof s.n) && !memcmp(o.e1, s.e1, sizeof s.e1) && o.half_w == s.half_w && o.half_h == s.half_h) {
                s.gx = o.gx; s.gy = o.gy; s.cell = o.cell; s.chosen = o.chosen; s.levels = o.levels; s.stretch = o.stretch; same = true;
                break;
            }
        if (!same) sticker_lay(s, j->p->m, j->p->seen);
    }
    laid = j->stickers;
    render_view(buf, *j->p, j->pic.get(), j->stickers, j->s, j->w, j->h, j->view, j->fine);
    AcquireSRWLockExclusive(&lock);
    frame = buf.out; frame_w = j->w; frame_h = j->h; frame_new = true;
    frame_pick.swap(buf.pick);
    ReleaseSRWLockExclusive(&lock);
    InterlockedExchange(&drawing, 0);
    return 0;
}
ID3D11ShaderResourceView *view_image;
void upload_frame(ID3D11Device *dev) {
    AcquireSRWLockExclusive(&lock);
    bool fresh = frame_new;
    std::vector<unsigned char> f;
    int w = frame_w, h = frame_h;
    if (fresh) { f = std::move(frame); frame_new = false; view_pick.swap(frame_pick); view_w = w; view_h = h; }
    ReleaseSRWLockExclusive(&lock);
    if (!fresh || f.empty()) return;
    D3D11_TEXTURE2D_DESC desc = {}; desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {f.data(), (UINT)w * 4, 0}; ID3D11Texture2D *tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&desc, &data, &tex))) return;
    ID3D11ShaderResourceView *v = nullptr;
    if (SUCCEEDED(dev->CreateShaderResourceView(tex, nullptr, &v))) {
        if (view_image) view_image->Release();
        view_image = v;
    }
    tex->Release();
}
std::string current;                                   // the model open in the tab
CamoSettings fresh_settings;
View3D view3d;
bool unsaved;
int sticker_at = -1;                                   // the sticker chosen in the list
bool import_sticker;                                   // the picture being imported is for a new sticker
bool add_soft;                                         // new stickers start soft
int ask_delete = -1;                                   // the picture whose deletion waits for a yes
struct StickerPicture { int id, soft; std::shared_ptr<Pattern> pic; };
std::vector<StickerPicture> sticker_pictures;
std::shared_ptr<Pattern> sticker_picture(int id, float soft) {
    int step = (int)(soft * 50 + .5f);                // a slider's small moves share one blur
    for (auto &e : sticker_pictures) if (e.id == id && e.soft == step) return e.pic;
    auto p = softened(load_picture(id), step / 50.f);
    if (sticker_pictures.size() > 24) sticker_pictures.erase(sticker_pictures.begin());
    sticker_pictures.push_back({id, step, p});
    return p;
}
const float *picked(int x, int y) {                   // the place under a pixel of the view
    if (x < 0 || y < 0 || x >= view_w || y >= view_h || view_pick.size() < (size_t)view_w * view_h * 6) return nullptr;
    const float *k = &view_pick[((size_t)y * view_w + x) * 6];
    return k[0] > 1e29f ? nullptr : k;
}
Assign *assign_of(const std::string &m) {
    for (auto &a : assigns) if (a.model == m) return &a;
    return nullptr;
}
int preset_of(int id) {
    for (int i = 0; i < (int)presets.size(); i++) if (presets[i].id == id) return i;
    return -1;
}
int picture_uses(int id) {                             // on how many vehicles
    int n = 0;
    for (const Assign &a : assigns) {
        bool used = a.picture == id;
        for (const Sticker &k : a.stickers) used |= k.picture == id;
        n += used;
    }
    return n;
}
// A picture out of the list and off the disk, and off every vehicle it is on
void delete_picture(int id) {
    int at = preset_of(id);
    if (at < 0) return;
    for (size_t i = assigns.size(); i-- > 0; ) {
        Assign &a = assigns[i];
        a.stickers.erase(std::remove_if(a.stickers.begin(), a.stickers.end(), [&](const Sticker &k) { return k.picture == id; }), a.stickers.end());
        if (a.picture == id) a.picture = -1;
        if (a.picture < 0 && a.stickers.empty()) assigns.erase(assigns.begin() + i);
    }
    presets.erase(presets.begin() + at);
    if (next_picture <= id) next_picture = id + 1;
    sticker_pictures.erase(std::remove_if(sticker_pictures.begin(), sticker_pictures.end(), [&](const StickerPicture &e) { return e.id == id; }), sticker_pictures.end());
    Assign *a = assign_of(current);
    selected = a ? preset_of(a->picture) : -1;
    sticker_at = -1;
    load_image(selected);
    save();
    wchar_t path[MAX_PATH];
    filename(id, path);
    DeleteFileW(path);
    dirty = true;
}
// The pictures in a menu, each with its X; pick(i) when one is chosen
template <class F> void picture_rows(int chosen, F pick) {
    int gone = -1;
    for (int i = 0; i < (int)presets.size(); i++) {
        const Preset &e = presets[i];
        ImGui::PushID(i);
        if (e.id == ask_delete) {
            ImGui::TextWrapped(TR("%s: стоит на технике (%d), снимется с неё. Удалить?", "%s: on %d vehicle(s), it will come off them. Delete?"), e.name.c_str(), picture_uses(e.id));
            if (ImGui::SmallButton(TR("Удалить", "Delete"))) gone = e.id;
            ImGui::SameLine();
            if (ImGui::SmallButton(TR("Отмена", "Cancel"))) ask_delete = -1;
        } else {
            float del = ImGui::CalcTextSize("X").x + 20, room = ImGui::GetContentRegionAvail().x - del, text = ImGui::CalcTextSize(e.name.c_str()).x;
            int id = e.id;
            if (ImGui::Selectable(e.name.c_str(), i == chosen, 0, ImVec2(room > text ? room : text, 0))) pick(i);
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                if (picture_uses(id)) ask_delete = id;
                else gone = id;
            }
        }
        ImGui::PopID();
    }
    if (gone >= 0) { delete_picture(gone); ask_delete = -1; }
}
void open_vehicle(const std::string &m) {
    if (m == current) return;
    current = m;
    shown.reset();
    want_model(m);
    Assign *a = assign_of(m);
    selected = a ? preset_of(a->picture) : -1;
    sticker_at = -1;
    load_image(selected);
}
void put_picture(int index) {                         // on the vehicle in the tab; -1 takes it off
    Assign *a = assign_of(current);
    if (index < 0) {
        if (a && a->stickers.empty()) assigns.erase(assigns.begin() + (a - assigns.data()));
        else if (a) a->picture = -1;
    } else if (a) a->picture = presets[index].id;
    else if (shown) assigns.push_back({current, presets[index].id, CamoSettings(), shown->all, shown->main, {}});
    selected = index;
    load_image(selected);
    save();
}
// a new sticker, on what is nearest to the middle of the view
void add_sticker(int picture) {
    if (!shown) return;
    Assign *a = assign_of(current);
    if (!a) { assigns.push_back({current, -1, CamoSettings(), shown->all, shown->main, {}}); a = &assigns.back(); }
    if (a->stickers.size() >= 64) return;
    const float *at = nullptr;
    int best = 1 << 30;
    for (int y = 0; y < view_h; y += 4) for (int x = 0; x < view_w; x += 4) {
        int d = abs(x - view_w / 2) + abs(y - view_h / 2);
        if (d < best) if (const float *k = picked(x, y)) { best = d; at = k; }
    }
    Sticker s = {};
    s.picture = picture;
    for (int i = 0; i < 3; i++) {
        s.c[i] = at ? at[i] : (shown->m.lo[i] + shown->m.hi[i]) / 2;
        s.n[i] = at ? at[3 + i] : i == 2;
    }
    s.size = (shown->m.hi[0] - shown->m.lo[0]) * .2f;
    s.rotation = 0; s.opacity = 1;
    s.soft = add_soft ? .4f : 0; s.wear = add_soft ? .2f : 0;
    a->stickers.push_back(s);
    sticker_at = (int)a->stickers.size() - 1;
    dirty = true;
    save();
}
// the folders of the game's files, as names
const char *group_name(const std::string &k) {
    static const char *const NAMES[][3] = {
        {"eng", "Британия", "Britain"}, {"finland", "Финляндия", "Finland"}, {"generic", "Общая", "Generic"}, {"germany", "Германия", "Germany"},
        {"usa", "США", "USA"}, {"ussr", "СССР", "USSR"}, {"japan", "Япония", "Japan"}, {"italy", "Италия", "Italy"}, {"france", "Франция", "France"},
        {"btr", "Бронетранспортёры", "Armoured carriers"}, {"car", "Автомобили", "Cars"}, {"tank_light", "Лёгкие танки", "Light tanks"},
        {"tank_medium", "Средние танки", "Medium tanks"}, {"tank_heavy", "Тяжёлые танки", "Heavy tanks"}, {"spg", "САУ", "Self-propelled guns"},
    };
    for (const auto &n : NAMES) if (k == n[0]) return TR(n[1], n[2]);
    return k.c_str();
}
void vehicle_row(const Vehicle &v) {
    const Assign *a = assign_of(v.model);
    bool has = a && (a->picture >= 0 || !a->stickers.empty());
    std::string label = (has ? "* " : "") + v.name;
    ImGui::PushID(v.model.c_str());
    if (ImGui::Selectable(label.c_str(), v.model == current)) open_vehicle(v.model);
    ImGui::PopID();
}
void vehicle_list() {
    static char search[64];
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", TR("Поиск техники", "Find a vehicle"), search, sizeof search);
    if (InterlockedCompareExchange(&vehicles_state, 1, 0) == 0) {
        HANDLE t = CreateThread(nullptr, 1 << 20, read_vehicles, nullptr, 0, nullptr);
        if (t) CloseHandle(t); else InterlockedExchange(&vehicles_state, 0);
    }
    if (vehicles_state != 2) { ImGui::TextDisabled(TR("Читаю список...", "Reading the list...")); return; }
    ImGui::BeginChild("list");
    std::string find_text(search);
    for (auto &c : find_text) c = (char)tolower((unsigned char)c);
    if (!find_text.empty()) {
        for (const auto &v : vehicles) if (v.name.find(find_text) != std::string::npos) vehicle_row(v);
    } else for (size_t i = 0; i < vehicles.size();) {
        const std::string &country = vehicles[i].country;
        size_t e = i;
        while (e < vehicles.size() && vehicles[e].country == country) e++;
        if (ImGui::TreeNode(country.c_str(), "%s", group_name(country))) {
            for (size_t k = i; k < e;) {
                const std::string &kind = vehicles[k].kind;
                size_t ke = k;
                while (ke < e && vehicles[ke].kind == kind) ke++;
                if (ImGui::TreeNode(kind.c_str(), "%s", group_name(kind))) {
                    for (size_t n = k; n < ke; n++) vehicle_row(vehicles[n]);
                    ImGui::TreePop();
                }
                k = ke;
            }
            ImGui::TreePop();
        }
        i = e;
    }
    ImGui::EndChild();
}
void vehicle_view(ID3D11Device *device, const CamoSettings &s) {
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.y -= ImGui::GetTextLineHeightWithSpacing();
    if (size.x < 64 || size.y < 64) return;
    int w = size.x > 2000 ? 2000 : (int)size.x, h = (int)size.y;
    static int last_w, last_h;
    if (w != last_w || h != last_h) { last_w = w; last_h = h; dirty = true; }
    AcquireSRWLockExclusive(&lock);
    std::shared_ptr<Preview> next = std::move(arrived);
    bool loading = wanted != shown_model || loader_running;
    ReleaseSRWLockExclusive(&lock);
    if (next && next->model == current) {
        shown = next; dirty = true;
        // what was saved for this vehicle may be from a build that read fewer of its textures
        Assign *a = assign_of(current);
        if (a && (a->all != shown->all || a->main != shown->main)) { a->all = shown->all; a->main = shown->main; save(); }
    }
    bool moving = ImGui::IsAnyItemActive();           // the mouse on the view or a slider
    static bool plain;                                // the last frame was the quick kind
    if (plain && !moving) dirty = true;
    if (shown && dirty && InterlockedCompareExchange(&drawing, 1, 0) == 0) {
        dirty = false;
        plain = moving;
        std::vector<StickerImage> images;
        if (Assign *a = assign_of(current)) for (const Sticker &k : a->stickers) if (auto pic = sticker_picture(k.picture, k.soft)) images.push_back(sticker_image(k, pic));
        Job *j = new Job{shown, picture, std::move(images), s, w, h, view3d, !moving};
        HANDLE t = CreateThread(nullptr, 1 << 20, draw_job, j, 0, nullptr);
        if (t) CloseHandle(t); else { delete j; InterlockedExchange(&drawing, 0); }
    }
    upload_frame(device);
    ImGui::InvisibleButton("view", size);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (view_image && !current.empty()) dl->AddImage((ImTextureID)view_image, a, b);
    else dl->AddRectFilled(a, b, IM_COL32(38, 38, 38, 255));
    if (current.empty()) dl->AddText(ImVec2(a.x + 16, a.y + 16), IM_COL32(200, 200, 200, 255), TR("Выбери технику в списке слева", "Choose a vehicle on the left"));
    else if (loading || !shown) dl->AddText(ImVec2(a.x + 16, a.y + 16), IM_COL32(200, 200, 200, 255), TR("Загружаю модель...", "Loading the model..."));
    ImGuiIO &io = ImGui::GetIO();
    // on a sticker the mouse takes it: it follows the surface under the mouse; anywhere else it turns the vehicle
    static bool carrying;
    int mx = (int)((io.MousePos.x - a.x) * view_w / (b.x - a.x + 1)), my = (int)((io.MousePos.y - a.y) * view_h / (b.y - a.y + 1));
    Assign *owner_of = assign_of(current);
    if (ImGui::IsItemActivated()) {
        carrying = false;
        const float *k = picked(mx, my);
        for (int i = owner_of && k ? (int)owner_of->stickers.size() - 1 : -1; i >= 0; i--) {
            const Sticker &s = owner_of->stickers[i];
            float d = 0;
            for (int c = 0; c < 3; c++) d += (k[c] - s.c[c]) * (k[c] - s.c[c]);
            if (sqrtf(d) < s.size * .6f) { sticker_at = i; carrying = true; break; }
        }
    }
    if (ImGui::IsItemActive() && carrying && owner_of && sticker_at >= 0 && sticker_at < (int)owner_of->stickers.size()) {
        if (const float *k = picked(mx, my)) {
            Sticker &s = owner_of->stickers[sticker_at];
            if (memcmp(s.c, k, sizeof s.c)) { memcpy(s.c, k, sizeof s.c); memcpy(s.n, k + 3, sizeof s.n); dirty = true; unsaved = true; }
        }
    } else if (ImGui::IsItemActive() && (io.MouseDelta.x || io.MouseDelta.y)) {
        view3d.yaw -= io.MouseDelta.x * .5f;
        view3d.pitch = fminf(85, fmaxf(-20, view3d.pitch + io.MouseDelta.y * .4f));
        dirty = true;
    }
    if (ImGui::IsItemHovered() && io.MouseWheel) {
        view3d.zoom = fminf(6, fmaxf(.5f, view3d.zoom * (1 + io.MouseWheel * .1f)));
        dirty = true;
    }
    ImGui::TextDisabled(TR("Левая кнопка: вращать, на наклейке: двигать её. Колесо: приблизить.", "Left button: turn, on a sticker: move it. Wheel: zoom."));
}
void vehicle_settings() {
    const bool disable_import = importing || presets.size() >= 256;
    if (disable_import) ImGui::BeginDisabled();
    if (ImGui::Button(TR("Загрузить PNG", "Import PNG"), ImVec2(-1,0))) {
        InterlockedExchange(&importing,1);
        HANDLE thread = CreateThread(nullptr, 4<<20, pick, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (thread) CloseHandle(thread); else { InterlockedExchange(&importing,0); status = TR("Не удалось открыть окно выбора файла", "Cannot open file picker"); }
    }
    if (disable_import) ImGui::EndDisabled();
    if (current.empty()) { if (!status.empty()) ImGui::TextWrapped("%s", status.c_str()); return; }
    {
        std::string title = current.substr(current.rfind('/') + 1);
        title.resize(title.size() > 4 ? title.size() - 4 : 0);        // without .mdl
        ImGui::TextWrapped("%s", title.c_str());
    }
    if (!shown) ImGui::BeginDisabled();
    const char *none = TR("Без камуфляжа", "No camouflage");
    bool menu_open = false;
    if (ImGui::BeginCombo(TR("Камуфляж", "Camouflage"), selected >= 0 ? presets[selected].name.c_str() : none)) {
        if (ImGui::Selectable(none, selected < 0)) put_picture(-1);
        picture_rows(selected, [](int i) { put_picture(i); });
        menu_open = true;
        ImGui::EndCombo();
    }
    if (!shown) ImGui::EndDisabled();
    Assign *a = assign_of(current);
    if (a && a->picture >= 0) {
        auto &s = a->s;
        bool changed = false;
        ImGui::SeparatorText(TR("Положение", "Position"));
        changed |= ImGui::SliderFloat(TR("Вдоль танка", "Along"), &s.offset[0], -1, 1, "%.3f");
        changed |= ImGui::SliderFloat(TR("Вверх-вниз", "Up and down"), &s.offset[1], -1, 1, "%.3f");
        changed |= ImGui::SliderFloat(TR("Размер", "Size"), &s.size, .2f, 4, "%.2f");
        changed |= ImGui::SliderFloat(TR("Поворот", "Rotation"), &s.rotation, -180, 180, "%.0f");
        ImGui::SeparatorText(TR("Зеркало", "Mirror"));
        changed |= ImGui::Combo(TR("На бортах", "On the sides"), &s.through, TR("Одинаково, зеркало по центру\0Сквозной рисунок, без зеркала\0", "The same, mirrored at the middle\0One sheet, no mirror\0"));
        bool fx = s.flip_x, fy = s.flip_y, rep = s.repeat;
        if (ImGui::Checkbox(TR("Отразить слева направо", "Flip left to right"), &fx)) { s.flip_x = fx; changed = true; }
        if (ImGui::Checkbox(TR("Отразить сверху вниз", "Flip top to bottom"), &fy)) { s.flip_y = fy; changed = true; }
        if (ImGui::Checkbox(TR("Продолжать за краями картинки", "Go on beyond its edges"), &rep)) { s.repeat = rep; changed = true; }
        ImGui::SeparatorText(TR("Цвет", "Colour"));
        changed |= ImGui::SliderFloat(TR("Непрозрачность", "Opacity"), &s.opacity, 0, 1, "%.2f");
        changed |= ImGui::Combo(TR("Наложение", "Blend"), &s.blend, TR("Обычное\0Умножение\0Перекрытие\0", "Normal\0Multiply\0Overlay\0"));
        if (ImGui::Button(TR("Сбросить настройки", "Reset settings"), ImVec2(-1,0))) { s = CamoSettings(); changed = true; }
        if (changed) { dirty = true; unsaved = true; }
        ImGui::Spacing();
        ImGui::TextWrapped("%s", TR("В бою камуфляж появится на твоей технике сам.", "In a battle your own vehicles get it by themselves."));
    }
    ImGui::SeparatorText(TR("Наклейки", "Stickers"));
    a = assign_of(current);
    if (a) {
        bool changed = false;
        for (int i = 0; i < (int)a->stickers.size(); i++) {
            ImGui::PushID(1000 + i);
            int pi = preset_of(a->stickers[i].picture);
            char row[160];
            snprintf(row, sizeof row, "%d. %s", i + 1, pi >= 0 ? presets[pi].name.c_str() : "?");
            float del = ImGui::CalcTextSize("X").x + 20;
            if (ImGui::Selectable(row, i == sticker_at, 0, ImVec2(ImGui::GetContentRegionAvail().x - del, 0))) sticker_at = i;
            ImGui::SameLine();
            bool gone = ImGui::SmallButton("X");
            ImGui::PopID();
            if (gone) {
                a->stickers.erase(a->stickers.begin() + i);
                if (sticker_at >= (int)a->stickers.size()) sticker_at = (int)a->stickers.size() - 1;
                changed = true;
                break;
            }
        }
        if (sticker_at >= 0 && sticker_at < (int)a->stickers.size()) {
            Sticker &k = a->stickers[sticker_at];
            float big = shown ? shown->m.hi[0] - shown->m.lo[0] : 200;
            changed |= ImGui::SliderFloat(TR("Размер наклейки", "Sticker size"), &k.size, big * .02f, big, "%.1f");
            changed |= ImGui::SliderFloat(TR("Поворот наклейки", "Sticker rotation"), &k.rotation, -180, 180, "%.0f");
            changed |= ImGui::SliderFloat(TR("Непрозрачность наклейки", "Sticker opacity"), &k.opacity, 0, 1, "%.2f");
            changed |= ImGui::SliderFloat(TR("Мягкие края", "Soft edges"), &k.soft, 0, 1, "%.2f");
            changed |= ImGui::SliderFloat(TR("Потёртость", "Wear"), &k.wear, 0, 1, "%.2f");
            bool fx = k.flip_x, fy = k.flip_y;
            if (ImGui::Checkbox(TR("Отразить наклейку слева направо", "Flip the sticker left to right"), &fx)) { k.flip_x = fx; changed = true; }
            if (ImGui::Checkbox(TR("Отразить наклейку сверху вниз", "Flip the sticker top to bottom"), &fy)) { k.flip_y = fy; changed = true; }
        }
        if (changed) { dirty = true; unsaved = true; }
    }
    ImGui::TextWrapped("%s", TR("Наклейка лежит только в одном месте. На деталях, где текстура общая для двух бортов, её не будет.", "A sticker is in one place only. It does not show on parts that share their texture between the two sides."));
    bool can_add = shown && !importing && presets.size() < 256;
    if (!can_add) ImGui::BeginDisabled();
    if (ImGui::Button(TR("Добавить наклейку", "Add a sticker"), ImVec2(-1,0))) ImGui::OpenPopup("##sticker_from");
    if (!can_add) ImGui::EndDisabled();
    if (ImGui::BeginPopup("##sticker_from")) {
        ImGui::Checkbox(TR("Мягкая наклейка: плавные края и лёгкий износ", "Soft sticker: gentle edges and a little wear"), &add_soft);
        ImGui::Separator();
        if (ImGui::Selectable(TR("Загрузить PNG...", "Import a PNG..."))) {
            import_sticker = true;
            InterlockedExchange(&importing,1);
            HANDLE thread = CreateThread(nullptr, 4<<20, pick, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            if (thread) CloseHandle(thread); else { InterlockedExchange(&importing,0); import_sticker = false; status = TR("Не удалось открыть окно выбора файла", "Cannot open file picker"); }
        }
        picture_rows(-1, [](int i) { add_sticker(presets[i].id); });
        menu_open = true;
        ImGui::EndPopup();
    }
    if (!menu_open) ask_delete = -1;
    if (unsaved && !ImGui::IsAnyItemActive()) { unsaved = false; status.clear(); save(); }
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
}
}
// The Replays tab: a button over the picture, just above the list, for the folder the replays are in
void replays_button() {
    Panel l; AcquireSRWLockShared(&lock); l = replays_list; ReleaseSRWLockShared(&lock);
    if (!l.shown) return;
    ImGui::SetNextWindowPos(ImVec2(l.x + 24, l.y - 76));
    ImGui::SetNextWindowBgAlpha(0);
    if (ImGui::Begin("###goh_replays_folder", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing)) {
        if (ImGui::Button(TR("Открыть папку реплеев", "Open replays folder"), ImVec2(0, 34))) replays_open_folder();
    }
    ImGui::End();
}
// The lobby: the camouflage in a window of its own over it, in the header left of the back button. A battle takes it away.
void lobby_button() {
    if (from_lobby_close) {
        from_lobby_close = false;
        if (from_lobby) { from_lobby = false; floating = false; }
    }
    Panel l; AcquireSRWLockShared(&lock); l = lobby_info; ReleaseSRWLockShared(&lock);
    if (!l.shown) return;
    ImGui::SetNextWindowPos(ImVec2(l.x + l.w - 64, l.y - 4), 0, ImVec2(1, 1));
    ImGui::SetNextWindowBgAlpha(0);
    if (ImGui::Begin("###goh_lobby_camo", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing)) {
        if (ImGui::Button(TR("Камуфляж", "Camouflage"))) {
            floating = !floating;
            from_lobby = floating;
            if (floating) ImGui::SetWindowFocus("###goh_camo");
        }
    }
    ImGui::End();
}
ImFont *ui_game_font();
// The host's row under the session's settings, drawn as the game draws those: a name, and a box in the column of the values
void day_setting() {
    static int loaded;
    if (!loaded) { loaded = 1; InterlockedExchange(&always_day, online_list_opt("always_day")); }
    Panel r; AcquireSRWLockShared(&lock); r = day_row; ReleaseSRWLockShared(&lock);
    if (!r.shown) return;
    ImGui::SetNextWindowPos(ImVec2(r.x, r.y));
    ImGui::SetNextWindowSize(ImVec2(r.w, r.h));
    ImGui::SetNextWindowBgAlpha(0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    if (ImGui::Begin("###goh_always_day", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        if (ImGui::InvisibleButton("##day", ImVec2(r.w, r.h))) {
            InterlockedExchange(&always_day, !always_day);
            online_set_list_opt("always_day", always_day);
        }
        const bool hot = ImGui::IsItemHovered();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const char *name = TR("Всегда день", "Always Day");
        ImFont *font = ui_game_font();
        if (font) ImGui::PushFont(font);
        float size = ImGui::GetTextLineHeight(), mid = r.y + r.h / 2;
        if (hot) dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h - 1), IM_COL32(44, 42, 38, 200));
        dl->AddLine(ImVec2(r.x, r.y + r.h - 1), ImVec2(r.x + r.w, r.y + r.h - 1), IM_COL32(70, 70, 66, 120));
        dl->AddLine(ImVec2(r.x + 199, r.y), ImVec2(r.x + 199, r.y + r.h), IM_COL32(70, 70, 66, 120));
        dl->AddText(ImVec2(floorf(r.x + 5), floorf(mid - size / 2)), hot ? IM_COL32(235, 178, 108, 255) : IM_COL32(190, 172, 140, 255), name);
        if (font) ImGui::PopFont();
        float s = 16, x = r.x + 202, y = r.y + 8;              // where the game has its boxes in a row
        dl->AddRect(ImVec2(x, y), ImVec2(x + s, y + s), IM_COL32(215, 215, 210, 255));
        if (always_day) dl->AddRectFilled(ImVec2(x + 3, y + 3), ImVec2(x + s - 3, y + s - 3), IM_COL32(190, 190, 185, 255));
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
extern "C" void camo_draw(ID3D11Device *device, void *window) {
    owner = (HWND)window;
    replays_button();
    lobby_button();
    day_setting();
    static bool was;
    bool key = GetForegroundWindow() == owner && (GetAsyncKeyState(VK_SHIFT)&0x8000) && (GetAsyncKeyState(VK_F11)&0x8000);
    if (key && !was) floating = !floating; was = key;
    accept_import();
    if (importing == 0 && import_sticker && !imported) import_sticker = false;      // the file window was closed
    if (imported) {
        imported = false;
        int fresh = selected;
        if (import_sticker) {                         // the new picture is a sticker: the camouflage stays as it was
            import_sticker = false;
            Assign *was = assign_of(current);
            selected = was ? preset_of(was->picture) : -1;
            load_image(selected);
            add_sticker(presets[fresh].id);
        } else if (!current.empty() && shown) put_picture(selected);
        else { selected = -1; load_image(-1); }
    }
    Panel p; AcquireSRWLockShared(&lock); p = panel; ReleaseSRWLockShared(&lock);
    drawn = p.shown || floating;
    if (!drawn) return;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings;
    if (p.shown) {
        ImGui::SetNextWindowPos(ImVec2(p.x+8, p.y+8));
        ImGui::SetNextWindowSize(ImVec2(p.w-16, p.h-16));
        ImGui::SetNextWindowBgAlpha(1);
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
    } else ImGui::SetNextWindowSize(ImVec2(1100, 680), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(TR("КАМУФЛЯЖ###goh_camo", "CAMOUFLAGE###goh_camo"), p.shown ? nullptr : &floating, flags)) { ImGui::End(); return; }
    float all = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("vehicles", ImVec2(all * .22f, 0), true);
    vehicle_list();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("view", ImVec2(all * .50f, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    Assign *a = assign_of(current);
    vehicle_view(device, a ? a->s : fresh_settings);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("settings", ImVec2(0, 0), true);
    vehicle_settings();
    ImGui::EndChild();
    ImGui::End();
}
// the self test's look at the tab: a vehicle with the first picture, both ways over the sides, in goh_camo
void preview_test() {
    if (presets.empty()) return;
    // camouflage on vehicles, as it is and as a map of the texels: red not outside, blue used at several places, green both sides
    static const struct { const char *model; float yaw, pitch, zoom; } CASES[] = {
        {"/entity/-vehicle/germany/btr/sdkfz251d_16/sdkfz251d_16.mdl", 300, 22, 1.3f},
        {"/entity/-vehicle/ussr/tank_heavy/is2_45/is2_45.mdl", 300, 30, 1.3f},
        {"/entity/-vehicle/usa/tank_light/m18_hellcat/m18_hellcat.mdl", 300, 30, 1.3f},
        {"/entity/-vehicle/germany/tank_medium/stug3g/stug3g.mdl", 300, 30, 1.3f},
    };
    load_image(0);
    for (int i = 0; i < 4; i++) {
        std::shared_ptr<Preview> p = load_preview(CASES[i].model);
        if (!p) continue;
        for (int look = 0; look < 2; look++) {
            CamoSettings s;
            View3D v;
            v.yaw = CASES[i].yaw; v.pitch = CASES[i].pitch; v.zoom = CASES[i].zoom;
            Buffers buf;
            std::vector<StickerImage> none;
            debug_kinds = look == 1;
            render_view(buf, *p, picture.get(), none, s, 720, 460, v, true);
            debug_kinds = false;
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%s\\preview%d.ppm", folder, i * 2 + look);
            FILE *d = _wfopen(path, L"wb");
            if (!d) continue;
            fprintf(d, "P6 720 460 255\n");
            for (size_t b = 0; b < buf.out.size(); b += 4) fwrite(&buf.out[b], 1, 3, d);
            fclose(d);
        }
    }
    load_image(-1);
    rc_log("camo selftest: stems done");
}
