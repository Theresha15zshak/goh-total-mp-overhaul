#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <stdio.h>
#include <vector>
#include <algorithm>
#include <string>
#include <memory>
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
struct Panel { bool shown; float x, y, w, h; } panel;
bool floating, drawn;
volatile LONG importing;
HWND owner;
struct Import { std::vector<unsigned char> png; std::string name, error; } pending;
struct Preset { CamoSettings settings; std::string name; int id; };
std::vector<Preset> presets;
// A vehicle's camouflage: its picture (a preset's file) and how it lies, with the vehicle's textures, as found in its
// model: all of them take the picture, the main ones tell the vehicle in a battle.
struct Assign { std::string model; int picture; CamoSettings s; std::vector<std::string> all, main; };
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
struct Published { std::string token, id_token; int id; CamoSettings s; std::string model; std::vector<std::string> all, main; };
std::vector<Published> published;
SRWLOCK published_lock = SRWLOCK_INIT;
struct Vec { unsigned char *begin, *end, *cap; };
void *(*tex_mods_real)(void *, void *);
bool is_camo_texture(const char *s, size_t n) {
    std::string k(s, n);
    for (auto &c : k) c = (char)tolower((unsigned char)c);
    if (k.find("_vehicle/") == std::string::npos || k.find("/tracks/") != std::string::npos || k.find('#') != std::string::npos) return false;
    size_t dot = k.rfind('.');
    if (dot != std::string::npos && dot + 5 >= k.size()) k.resize(dot);
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
    std::string stem = stem_of(s, k->size);
    AcquireSRWLockShared(&published_lock);
    for (const auto &e : published) {
        if (std::find(e.all.begin(), e.all.end(), stem) != e.all.end()) add_variant(v, e.token.c_str());
        if (std::find(e.main.begin(), e.main.end(), stem) != e.main.end()) add_variant(v, e.id_token.c_str());
    }
    ReleaseSRWLockShared(&published_lock);
    return v;
}

// the pattern on the textures: for now a test pattern of three colours
struct Pattern { std::vector<unsigned char> px; int w = 0, h = 0; CamoSettings s; std::string model; };
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
Placement place_on(const VehicleMap &pr, const Pattern &p);
bool texel(const VehicleMap &pr, int x, int y, int w, int h, size_t &at);
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
        float lum = pal[k][0] * .3f + pal[k][1] * .59f + pal[k][2] * .11f, keep = lum / average;
        keep = keep < .45f ? .45f : keep > 1.6f ? 1.6f : keep;
        uint8_t flat[4], out[4];
        for (int c = 0; c < 3; c++) { float f = pal[k][c] / keep; flat[c] = (uint8_t)(f > 255 ? 255 : f); }
        flat[3] = 255;
        if (pr) {
            size_t at;
            float u, v;
            if (texel(*pr, x, y, w, h, at) && on_picture(pl, texel_place(*pr, at), u, v)) camo_blend(p.px.data(), p.w, p.h, u, v, flat, p.s, out);
            else memcpy(out, flat, 4);
        } else camo_pixel(p.px.data(), p.w, p.h, (x + .5f) / w, (y + .5f) / h, flat, p.s, out);
        for (int c = 0; c < 3; c++) {
            int v = (int)(out[c] * keep);
            px[i][c] = v < 0 ? 0 : v > 255 ? 255 : v;
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
bool paint_dds(unsigned char *f, size_t n, const Pattern &p, const VehicleMap *pr) {
    if (n < 128 || memcmp(f, "DDS ", 4)) return false;
    uint32_t h, w, mips, four;
    memcpy(&h, f + 12, 4); memcpy(&w, f + 16, 4); memcpy(&mips, f + 28, 4); memcpy(&four, f + 84, 4);
    int size = four == 0x31545844 ? 8 : four == 0x35545844 ? 16 : 0;       // DXT1, DXT5
    if (!size || !w || !h) return false;
    if (!mips) mips = 1;
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
Pattern pattern_of(const std::string &token) {
    Pattern p;
    int id = -1;
    AcquireSRWLockShared(&published_lock);
    for (auto &e : published) if (e.token == token) { id = e.id; p.s = e.s; p.model = e.model; }
    ReleaseSRWLockShared(&published_lock);
    wchar_t path[MAX_PATH];
    std::vector<unsigned char> bytes;
    int c;
    if (id >= 0) {
        filename(id, path);
        if (readfile(path, bytes)) {
            unsigned char *d = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &p.w, &p.h, &c, 4);
            if (d) { p.px.assign(d, d + (size_t)p.w * p.h * 4); stbi_image_free(d); return p; }
        }
    }
    rc_log("camo: no picture for %s, test pattern", token.c_str());
    return test_pattern();
}
bool make_projection(const std::string &file, const unsigned char *f, size_t n, const std::string &model, VehicleMap *&out);
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
        bool tracks = !p.s.mode && name.find("track") != std::string::npos;     // nobody paints those
        if (!p.s.mode && !tracks) make_projection(name, f, n, p.model, pr);
        bool ok = tracks || paint_dds(r->data.data(), n, p, pr);
        free_projection(pr);
        rc_log("camo: %s painted in %lu ms: %s", key.c_str(), GetTickCount() - t0, ok ? "ok" : "not a DXT1/DXT5 DDS, left as it is");
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
    if (!readfile(path, bytes) || !validate(bytes, w, h)) { status = "PNG missing or invalid"; return; }
    unsigned char *p = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &c, 4);
    if (!p) { status = "Could not decode PNG"; return; }
    auto r = std::make_shared<Pattern>();
    r->w = w; r->h = h; r->px.assign(p, p + (size_t)w * h * 4); stbi_image_free(p); status.clear();
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
void publish() {
    std::vector<Published> list;
    for (const auto &a : assigns) {
        uint32_t id = fnv(a.model.data(), a.model.size()), crc = fnv(&a.s, sizeof a.s, id);
        char token[48], id_token[48];
        snprintf(token, sizeof token, "gohcamo%dx%08x", a.picture, crc);
        snprintf(id_token, sizeof id_token, "gohcamoid%08x", id);
        list.push_back({token, id_token, a.picture, a.s, a.model, a.all, a.main});
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
    FILE *f = _wfopen(tmp, L"wb"); if (!f) { status = "Cannot save vehicles"; return; }
    fprintf(f, "GOH_CAMO_V1\n");
    for (const auto &a : assigns) fprintf(f, "%d %.9g %.9g %.9g %.9g %d %d %.9g %d %d %d\t%s\t%s\t%s\n", a.picture,
        a.s.offset[0], a.s.offset[1], a.s.rotation, a.s.opacity, a.s.blend, a.s.repeat, a.s.size, a.s.through, a.s.flip_x, a.s.flip_y,
        a.model.c_str(), joined(a.all).c_str(), joined(a.main).c_str());
    bool ok = !ferror(f); if (fclose(f)) ok = false;
    if (!ok || !MoveFileExW(tmp, vehicles_ini, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) status = "Cannot save vehicles";
}
void load_vehicles() {
    FILE *f = _wfopen(vehicles_ini, L"rb");
    if (!f) return;
    static char line[8192];
    if (fgets(line, sizeof line, f) && !strcmp(line, "GOH_CAMO_V1\n")) while (assigns.size() < 2000 && fgets(line, sizeof line, f)) {
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
    FILE *f = _wfopen(tmp, L"wb"); if (!f) { status = "Cannot save presets"; return; }
    fprintf(f, "GOH_CAMO_2\n");
    for (const auto &p : presets) fprintf(f, "%d %.9g %.9g %.9g %.9g %.9g %d %d %d %.9g %s\n", p.id,
        p.settings.offset[0], p.settings.offset[1], p.settings.scale, p.settings.rotation,
        p.settings.opacity, p.settings.blend, p.settings.mode, p.settings.repeat, p.settings.size, p.name.c_str());
    bool ok = !ferror(f); if (fclose(f)) ok = false;
    if (!ok || !MoveFileExW(tmp, ini, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) status = "Cannot save presets";
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
            result.png.clear(); result.error = "Use a valid PNG, up to 4096 x 4096 and 32 MB";
        } else {
            const wchar_t *name = wcsrchr(path, L'\\'); result.name = utf8(name ? name + 1 : path);
            for (char &c : result.name) if (c == '\n' || c == '\r') c = ' ';
        }
    } else if (CommDlgExtendedError()) result.error = "Could not open the file picker";
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
    int id = 0; for (const auto &p : presets) if (p.id >= id) id = p.id + 1;
    wchar_t path[MAX_PATH]; filename(id, path);
    FILE *f = _wfopen(path, L"wb"); if (!f) { status = "Cannot store the imported PNG"; return; }
    bool ok = fwrite(r.png.data(), 1, r.png.size(), f) == r.png.size(); if (fclose(f)) ok = false;
    if (!ok) { DeleteFileW(path); status = "Cannot store the imported PNG"; return; }
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
extern "C" void camo_tick() {
    static bool in_game;
    uintptr_t mode = q(base + GAME_MODE_OBJ);
    unsigned char on = 0;
    if (mode) read(mode + 0x908, &on, 1);
    if ((on == 1) != in_game) { in_game = on == 1; InterlockedIncrement(&battle_number); }
    { char path[MAX_PATH]; snprintf(path, sizeof path, "%ls\\..\\goh_snapshots.ini", folder); camo_selftest_tick(path); lobby_dump_tick(path); }
    // our tab, wherever the page is: looked for a few times a second, it is a walk through the screen's dialogs
    static DWORD next;
    static Panel p;
    DWORD now = GetTickCount();
    if ((LONG)(now - next) >= 0) {
        next = now + 200;
        p = {};
        void *root = (void *)q(base + G_DESKTOP), *tab = root ? find(root, "camo_root") : nullptr;
        if (tab) bounds((uintptr_t)tab, p);
        if (tab && dialog_covered(tab)) p = {};       // the game's own window over it: exit, ESC
    }
    AcquireSRWLockExclusive(&lock); panel = p; ReleaseSRWLockExclusive(&lock);
}
extern "C" int camo_shown() { return drawn || floating; }
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
struct Triangle { float p[3][3], n[3][3], uv[3][2]; int layer; };    // layer: which of the model's textures, -1 none
struct VehicleMap {
    int w = 0, h = 0;
    std::vector<float> pos;                           // x, y, z of a texel
    std::vector<unsigned char> kind;                  // 0 not on the outside of the vehicle, 1 one place, 2 used at several
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
};
struct Model { std::vector<Triangle> all; std::vector<std::string> stems; float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f}; };
struct Mesh { unsigned first, tris; std::string material; };
struct Ply { std::vector<Mesh> meshes; const unsigned char *vert = nullptr; unsigned count = 0, stride = 0; const uint16_t *index = nullptr; unsigned indices = 0; };
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
            size_t q = p + 20 + (fl & 0x40 ? 0 : 4);
            if (q >= n || q + 1 + d[q] > n) return false;
            o.meshes.push_back({first, tris, std::string((const char *)&d[q + 1], d[q])});
            p = q + 1 + d[q];
            if (fl & 0x1000) { if (p >= n) return false; p += 1 + d[p]; }
        } else if (!memcmp(b, "VERT", 4)) {
            uint32_t cnt; uint16_t stride, fl;
            memcpy(&cnt, b + 4, 4); memcpy(&stride, b + 8, 2); memcpy(&fl, b + 10, 2);
            if (stride < 32 || (fl & 7) != 7 || p + 12 + (size_t)cnt * stride > n) return false;
            o.vert = b + 12; o.count = cnt; o.stride = stride;
            p += 12 + (size_t)cnt * stride;
        } else if (!memcmp(b, "INDX", 4)) {
            uint32_t cnt; memcpy(&cnt, b + 4, 4);
            if (p + 8 + (size_t)cnt * 2 > n) return false;
            o.index = (const uint16_t *)(b + 8); o.indices = cnt;
            p += 8 + (size_t)cnt * 2;
        } else return false;                          // SKIN (tracks) and the like: no part of ours
    }
    return o.vert && o.index;
}
// A texture is shared: the same texels serve both sides of the hull, all the wheels, and hidden parts like the inside of
// the engine room. The two sides mirror each other, so y counts by its size; what else lands far apart can't hold a drawing.
void fill_triangle(VehicleMap &pr, const Triangle &t) {
    float wrap = floorf(fminf(t.uv[0][0], fminf(t.uv[1][0], t.uv[2][0]))), wrapv = floorf(fminf(t.uv[0][1], fminf(t.uv[1][1], t.uv[2][1])));
    float x[3], y[3];
    for (int i = 0; i < 3; i++) { x[i] = (t.uv[i][0] - wrap) * pr.w; y[i] = (t.uv[i][1] - wrapv) * pr.h; }
    float den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2]);
    if (fabsf(den) < 1e-6f) return;
    int x0 = (int)floorf(fminf(x[0], fminf(x[1], x[2]))) - 1, x1 = (int)ceilf(fmaxf(x[0], fmaxf(x[1], x[2]))) + 1;
    int y0 = (int)floorf(fminf(y[0], fminf(y[1], y[2]))) - 1, y1 = (int)ceilf(fmaxf(y[0], fmaxf(y[1], y[2]))) + 1;
    if (x1 - x0 > 2 * pr.w || y1 - y0 > 2 * pr.h) return;
    float cy = (pr.lo[1] + pr.hi[1]) / 2, apart = (pr.hi[0] - pr.lo[0]) * .04f;
    for (int py = y0; py <= y1; py++) for (int px = x0; px <= x1; px++) {
        float gx = px + .5f, gy = py + .5f;
        float a = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / den;
        float b = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / den;
        float c = 1 - a - b;
        if (a < -.03f || b < -.03f || c < -.03f) continue;
        int tx = ((px % pr.w) + pr.w) % pr.w, ty = ((py % pr.h) + pr.h) % pr.h;
        size_t at = (size_t)ty * pr.w + tx;
        float q[3];
        for (int i = 0; i < 3; i++) q[i] = a * t.p[0][i] + b * t.p[1][i] + c * t.p[2][i];
        float *o = &pr.pos[at * 3];
        if (!pr.kind[at]) { memcpy(o, q, sizeof q); pr.kind[at] = 1; continue; }
        if (a < 0 || b < 0 || c < 0) continue;        // the rim around a triangle, for the filtering
        if (fabsf(q[0] - o[0]) > apart || fabsf(q[2] - o[2]) > apart || fabsf(fabsf(q[1] - cy) - fabsf(o[1] - cy)) > apart) pr.kind[at] = 2;
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
    for (size_t i = 0; i < m.all.size(); i++) if (m.all[i].layer == layer && seen[i]) fill_triangle(pr, m.all[i]);
}
bool read_game_file(const std::string &path, std::vector<unsigned char> &out) {
    GamePath p = path_of(path);
    if (!open_prev || !((unsigned char (*)(void *))(base + FN_FILE_EXISTS))(&p)) return false;
    unsigned char *st = (unsigned char *)open_prev(&p, 0x201);
    if (!st) return false;
    uintptr_t vt = *(uintptr_t *)st - base;
    bool ok = false;
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
        for (const Mesh &mesh : m.meshes) {
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
                bool good = true;
                for (int c = 0; c < 3; c++) {
                    unsigned k = m.index[((size_t)mesh.first + i) * 3 + c];
                    if (k >= m.count) { good = false; break; }
                    float raw[8];
                    memcpy(raw, m.vert + (size_t)k * m.stride, sizeof raw);
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
Placement place_on(const VehicleMap &pr, const Pattern &p) { return place_picture(pr.lo, pr.hi, p.w, p.h, p.s); }
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
void free_projection(VehicleMap *p) { delete p; }
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
bool make_projection(const std::string &file, const unsigned char *f, size_t n, const std::string &model, VehicleMap *&out) {
    uint32_t h, w;
    if (n < 128) return false;
    memcpy(&h, f + 12, 4); memcpy(&w, f + 16, 4);
    if (w > 4096 || h > 4096) return false;
    size_t at = file.find("/texture/common/");
    if (at == std::string::npos) return false;
    std::string stem = stem_of(file.c_str(), file.size());
    // several models can use a texture, some only for a few parts: the one that covers most of it, if it isn't known
    std::vector<std::string> list;
    if (!model.empty()) list.push_back(model);
    else list = models_of_texture(stem);
    std::unique_ptr<Model> best;
    int most = 0, layer = -1, tried = 0;
    for (const auto &m : list) {
        if (++tried > 16) break;
        std::unique_ptr<Model> p(new Model);
        if (!load_model(m, *p)) continue;
        int l = (int)(std::find(p->stems.begin(), p->stems.end(), stem) - p->stems.begin()), count = 0;
        for (const Triangle &t : p->all) count += t.layer == l;
        if (l < (int)p->stems.size() && count > most) { most = count; layer = l; best = std::move(p); }
    }
    if (!best) { rc_log("camo: no model for %s: the picture goes on as a pattern", stem.c_str()); return false; }
    VehicleMap *r = new VehicleMap;
    r->w = w; r->h = h;
    bake_layer(*r, *best, seen_of(*best), layer);
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
    std::string model;
    std::vector<std::shared_ptr<Layer>> layers;
    std::vector<std::string> all, main;
};
// a DDS's first level, DXT1 and DXT5 as all the vehicles have
bool decode_dds(const std::vector<unsigned char> &f, Layer &l) {
    if (f.size() < 128 || memcmp(f.data(), "DDS ", 4)) return false;
    uint32_t h, w, four;
    memcpy(&h, &f[12], 4); memcpy(&w, &f[16], 4); memcpy(&four, &f[84], 4);
    int size = four == 0x31545844 ? 8 : four == 0x35545844 ? 16 : 0;
    if (!size || !w || !h || w > 4096 || h > 4096) return false;
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (128 + (size_t)bw * bh * size > f.size()) return false;
    l.w = w; l.h = h;
    l.rgba.assign((size_t)w * h * 4, 255);
    double sum = 0;
    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        const unsigned char *b = &f[128 + ((size_t)by * bw + bx) * size];
        int alpha[8];
        uint64_t abits = 0;
        if (size == 16) {
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
            o[3] = size == 16 ? (unsigned char)alpha[abits >> (3 * i) & 7] : 255;
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
    std::vector<unsigned char> seen = seen_of(p->m);
    std::vector<int> count(p->m.stems.size());
    for (const Triangle &t : p->m.all) if (t.layer >= 0) count[t.layer]++;
    int most = 0;
    for (size_t i = 0; i < count.size(); i++) if (p->m.stems[i].find("track") == std::string::npos && count[i] > most) most = count[i];
    for (size_t i = 0; i < p->m.stems.size(); i++) {
        auto l = std::make_shared<Layer>();
        l->stem = p->m.stems[i];
        p->layers.push_back(l);
        std::string file = texture_file("/texture/common/" + l->stem);
        std::vector<unsigned char> bytes;
        if (file.empty() || !read_game_file(file, bytes) || !decode_dds(bytes, *l)) { rc_log("camo: %s not shown", l->stem.c_str()); continue; }
        if (l->stem.find("track") != std::string::npos || !is_camo_texture(("_vehicle/" + l->stem).c_str(), l->stem.size() + 9)) continue;
        p->all.push_back(l->stem);
        if (count[i] * 10 >= most * 3) p->main.push_back(l->stem);
        l->map.w = l->w > 1024 ? 1024 : l->w;
        l->map.h = l->h > 1024 ? 1024 : l->h;
        bake_layer(l->map, p->m, seen, (int)i);
    }
    rc_log("camo: %s for the tab: %d triangles, %d textures, %lu ms", mdl.c_str(), (int)p->m.all.size(), (int)p->layers.size(), GetTickCount() - t0);
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
};
struct Band {
    const Preview *p; const Pattern *pic; const CamoSettings *s; Placement place;
    Buffers *b; int w, h, ss, W, y0, y1;
    float light[3];
};
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
    unsigned char row[2][4096 * 3];
    for (int y = B.y0; y < B.y1; y++) {
        unsigned char *line = row[(y - B.y0) % B.ss];
        for (int x = 0; x < w; x++) {
            size_t at = (size_t)y * w + x;
            unsigned char *o = &line[x * 3];
            if (buf.ids[at] < 0) { o[0] = o[1] = o[2] = (unsigned char)(46 - 16 * y / h); continue; }
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
                if (pic && !l->map.kind.empty()) {
                    size_t m = (size_t)cell(tv, l->map.h) * l->map.w + cell(tu, l->map.w);
                    float pu, pv;
                    if (l->map.kind[m] == 1 && on_picture(B.place, texel_place(l->map, m), pu, pv)) {
                        float lum = e[0] * .3f + e[1] * .59f + e[2] * .11f, keep = lum / l->average;
                        keep = keep < .45f ? .45f : keep > 1.6f ? 1.6f : keep;
                        uint8_t flat[4], out[4];
                        for (int i = 0; i < 3; i++) { float f = e[i] / keep; flat[i] = (uint8_t)(f > 255 ? 255 : f); }
                        flat[3] = 255;
                        camo_blend(pic->px.data(), pic->w, pic->h, pu, pv, flat, *B.s, out);
                        for (int i = 0; i < 3; i++) { float f = out[i] * keep; col[i] = (unsigned char)(f > 255 ? 255 : f); }
                    }
                }
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
void render_view(Buffers &buf, const Preview &p, const Pattern *pic, const CamoSettings &s, int W, int H, const View3D &view, bool fine) {
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
        b = {&p, pic, &s, Placement(), &buf, w, h, ss, W, i * rows, i * rows + rows < h ? i * rows + rows : h, {}};
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
struct Job { std::shared_ptr<Preview> p; std::shared_ptr<Pattern> pic; CamoSettings s; int w, h; View3D view; bool fine; };
volatile LONG drawing;
std::vector<unsigned char> frame;
int frame_w, frame_h;
bool frame_new;
DWORD WINAPI draw_job(LPVOID arg) {
    std::unique_ptr<Job> j((Job *)arg);
    static Buffers buf;
    render_view(buf, *j->p, j->pic.get(), j->s, j->w, j->h, j->view, j->fine);
    AcquireSRWLockExclusive(&lock);
    frame = buf.out; frame_w = j->w; frame_h = j->h; frame_new = true;
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
    if (fresh) { f = std::move(frame); frame_new = false; }
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
Assign *assign_of(const std::string &m) {
    for (auto &a : assigns) if (a.model == m) return &a;
    return nullptr;
}
int preset_of(int id) {
    for (int i = 0; i < (int)presets.size(); i++) if (presets[i].id == id) return i;
    return -1;
}
void open_vehicle(const std::string &m) {
    if (m == current) return;
    current = m;
    shown.reset();
    want_model(m);
    Assign *a = assign_of(m);
    selected = a ? preset_of(a->picture) : -1;
    load_image(selected);
}
void put_picture(int index) {                         // on the vehicle in the tab; -1 takes it off
    Assign *a = assign_of(current);
    if (index < 0) {
        if (a) assigns.erase(assigns.begin() + (a - assigns.data()));
    } else if (a) a->picture = presets[index].id;
    else if (shown) assigns.push_back({current, presets[index].id, CamoSettings(), shown->all, shown->main});
    selected = assign_of(current) ? index : -1;
    load_image(selected);
    save();
}
void vehicle_row(const Vehicle &v) {
    bool has = assign_of(v.model) != nullptr;
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
        if (ImGui::TreeNode(country.c_str())) {
            for (size_t k = i; k < e;) {
                const std::string &kind = vehicles[k].kind;
                size_t ke = k;
                while (ke < e && vehicles[ke].kind == kind) ke++;
                if (ImGui::TreeNode(kind.c_str())) {
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
    if (next && next->model == current) { shown = next; dirty = true; }
    bool moving = ImGui::IsAnyItemActive();           // the mouse on the view or a slider
    static bool plain;                                // the last frame was the quick kind
    if (plain && !moving) dirty = true;
    if (shown && dirty && InterlockedCompareExchange(&drawing, 1, 0) == 0) {
        dirty = false;
        plain = moving;
        Job *j = new Job{shown, picture, s, w, h, view3d, !moving};
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
    if (ImGui::IsItemActive() && (io.MouseDelta.x || io.MouseDelta.y)) {
        view3d.yaw -= io.MouseDelta.x * .5f;
        view3d.pitch = fminf(85, fmaxf(-20, view3d.pitch + io.MouseDelta.y * .4f));
        dirty = true;
    }
    if (ImGui::IsItemHovered() && io.MouseWheel) {
        view3d.zoom = fminf(6, fmaxf(.5f, view3d.zoom * (1 + io.MouseWheel * .1f)));
        dirty = true;
    }
    ImGui::TextDisabled(TR("Левая кнопка: вращать. Колесо: приблизить. Гусеницы в просмотре не показаны.", "Left button: turn. Wheel: zoom. Tracks are not shown here."));
}
void vehicle_settings() {
    const bool disable_import = importing || presets.size() >= 256;
    if (disable_import) ImGui::BeginDisabled();
    if (ImGui::Button(TR("Загрузить PNG", "Import PNG"), ImVec2(-1,0))) {
        InterlockedExchange(&importing,1);
        HANDLE thread = CreateThread(nullptr, 4<<20, pick, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (thread) CloseHandle(thread); else { InterlockedExchange(&importing,0); status = "Cannot open file picker"; }
    }
    if (disable_import) ImGui::EndDisabled();
    if (current.empty()) { if (!status.empty()) ImGui::TextWrapped("%s", status.c_str()); return; }
    ImGui::TextWrapped("%s", current.substr(current.rfind('/') + 1).c_str());
    if (!shown) ImGui::BeginDisabled();
    const char *none = TR("Без камуфляжа", "No camouflage");
    if (ImGui::BeginCombo(TR("Камуфляж", "Camouflage"), selected >= 0 ? presets[selected].name.c_str() : none)) {
        if (ImGui::Selectable(none, selected < 0)) put_picture(-1);
        for (int i=0; i<(int)presets.size(); i++) {
            ImGui::PushID(i);
            if (ImGui::Selectable(presets[i].name.c_str(), i==selected)) put_picture(i);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (!shown) ImGui::EndDisabled();
    Assign *a = assign_of(current);
    if (a) {
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
        changed |= ImGui::Combo(TR("Наложение", "Blend"), &s.blend, "Normal\0Multiply\0Overlay\0");
        if (ImGui::Button(TR("Сбросить настройки", "Reset settings"), ImVec2(-1,0))) { s = CamoSettings(); changed = true; }
        if (changed) { dirty = true; unsaved = true; }
        ImGui::Spacing();
        ImGui::TextWrapped("%s", TR("В бою камуфляж появится на твоей технике сам.", "In a battle your own vehicles get it by themselves."));
    }
    if (unsaved && !ImGui::IsAnyItemActive()) { unsaved = false; status.clear(); save(); }
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
}
}
extern "C" void camo_draw(ID3D11Device *device, void *window) {
    owner = (HWND)window;
    static bool was;
    bool key = GetForegroundWindow() == owner && (GetAsyncKeyState(VK_SHIFT)&0x8000) && (GetAsyncKeyState(VK_F11)&0x8000);
    if (key && !was) floating = !floating; was = key;
    accept_import();
    if (imported) { imported = false; if (!current.empty() && shown) put_picture(selected); else { selected = -1; load_image(-1); } }
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
    if (!ImGui::Begin("CAMOUFLAGE", p.shown ? nullptr : &floating, flags)) { ImGui::End(); return; }
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
    const char *model = "/entity/-vehicle/ussr/tank_heavy/is2_44/is2_44.mdl";
    std::shared_ptr<Preview> p = load_preview(model);
    if (!p) { rc_log("camo selftest: no preview"); return; }
    load_image(0);
    for (int through = 0; through < 2; through++) {
        CamoSettings s;
        s.through = through;
        View3D v;
        Buffers buf;
        LARGE_INTEGER a, b, hz;
        QueryPerformanceFrequency(&hz);
        QueryPerformanceCounter(&a);
        render_view(buf, *p, picture.get(), s, 720, 460, v, false);
        QueryPerformanceCounter(&b);
        rc_log("camo selftest: quick view %.1f ms", (b.QuadPart - a.QuadPart) * 1000. / hz.QuadPart);
        QueryPerformanceCounter(&a);
        render_view(buf, *p, picture.get(), s, 720, 460, v, true);
        QueryPerformanceCounter(&b);
        rc_log("camo selftest: fine view %.1f ms", (b.QuadPart - a.QuadPart) * 1000. / hz.QuadPart);
        const std::vector<unsigned char> &f = buf.out;
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%s\\preview%d.ppm", folder, through);
        FILE *d = _wfopen(path, L"wb");
        if (!d) continue;
        fprintf(d, "P6 720 460 255\n");
        for (size_t i = 0; i < f.size(); i += 4) fwrite(&f[i], 1, 3, d);
        fclose(d);
    }
    rc_log("camo selftest: stems %d, main %d", (int)p->all.size(), (int)p->main.size());
    load_image(-1);
}
