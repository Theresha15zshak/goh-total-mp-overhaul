// PVP button in the game's own Internet / LAN game list (eGUI_PageSessionList, eGUI_PageSessionList.cpp):
// our layout of the page (tools/build.py, the game's with a btn_pvp after JOIN) is swapped in when the game
// reads it, the button is wired like the page's own ones, and its click shows our block (ui.cpp) over the
// list of games. Everything is checked against GoH 1.067 first; anything else leaves the page alone.
//
// How the page works (re/NOTES.md): its bind (vtable slot 20, 2e0260) finds each button in its layout
// (page+0x58, vtable +0x1e0 by id), casts it to eButton and registers (aa4240) a click (event 7) sending a
// command number to page+0x60; the commands arrive at the handler of the page's sub-object at +0x28 (slot 1,
// 2e0eb0) as msg+8: 0x15 host, 0x16 refresh, 0x18 join... Ours is PVP_CMD, which no page uses.
// A dialog (eDialog): +0x48 flags (bit 1 shown), +0x98 x,y within its parent, +0xa0 w,h, +0xf0 parent;
// vtable +0x108 "shown and every parent shown", true only up to the screen's root, which overrides it.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "goh_replay.h"
#include "ui_files.h"

#define NUI (sizeof UI_FILES / sizeof UI_FILES[0])
#define PVP_CMD 0x4f50

#define FN_REPO_OPEN   0x663c30
#define VT_ZIP_STREAM  0x1060678
#define VT_BUF_STREAM  0x10c6258
#define FN_BIND        0x2e0260
#define FN_COMMAND     0x2e0eb0
#define FN_ON_EVENT    0xaa4240     // (dialog, target, event, command)
#define FN_DYNCAST     0xf1c498     // __RTDynamicCast
#define TD_DIALOG      0x14b1c58
#define TD_BUTTON      0x14d4fc0
#define FN_IS_SHOWN    0xaa4ba0     // eDialog's own vtable +0x108: the screen's root has another

static const uint8_t REPO_OPEN_INS[10] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08};

// the two pages using the layout: main vtable (slot 0 destructor, slot 20 bind) and the +0x28 one (slot 1 commands)
typedef struct { const char *name; uint32_t vt, vt_cmd, dtor; } PageClass;
static const PageClass PAGES[2] = {
    {"online", 0x1049f30, 0x104a020, 0x2e72d0},
    {"lan", 0x104a050, 0x104a140, 0x2e7350},
};

volatile long g_pvp_on;
static uint8_t *g_base;
static char g_pak[MAX_PATH];        // the game's interface.pak: its fonts, for the block to look like the page
static void *(*g_prev_open)(void *path, uint32_t mode);
static uint8_t *g_ui_copy[NUI];
static void (*g_bind[2])(void *page);
static void (*g_cmd[2])(void *self, void *msg);
static void *(*g_dtor[2])(void *page, unsigned flags);
static void *volatile g_page, *volatile g_btn, *volatile g_list, *volatile g_head;
static volatile long g_cmds_logged;

static uint32_t crc32(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    uint32_t c = 0xffffffffu;
    while (n--) c = t[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static int peek(const void *p, void *out, size_t n) {
    SIZE_T got = 0;
    return p && ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}

// ---- our layout in the stream the game opened (as balance.c does with the balance files) ----------------
static int ends_with(const void *str, const char *ours) {
    const uint8_t *p = str;
    uint64_t size = *(const uint64_t *)(p + 0x10), cap = *(const uint64_t *)(p + 0x18);
    if (cap < 15 || size > cap || size > 4096) return 0;
    const char *s = cap > 15 ? *(const char *const *)p : (const char *)p;
    size_t k = strlen(ours);
    if (!s) return 0;
    if (size >= k) return !_strnicmp(s + size - k, ours, k) && (size == k || s[size - k - 1] == '/');
    // the path as written in an including layout ("views/GameResult.xml"): the tail of ours
    while (size && (*s == '/' || *s == '\\')) s++, size--;
    return size && !_strnicmp(ours + k - size, s, size) && ours[k - size - 1] == '/';
}

static void *open_hook(void *path, uint32_t mode) {
    void *r = g_prev_open(path, mode);
    if (!r) return r;
    for (size_t i = 0; i < NUI; i++) {
        if (!ends_with(path, UI_FILES[i].path)) continue;
        const UiFile *f = &UI_FILES[i];
        uint8_t *st = r;
        uintptr_t vt = *(uintptr_t *)st - (uintptr_t)g_base;
        uint8_t *b = *(uint8_t **)(st + 8), *e = *(uint8_t **)(st + 0x28);
        int whole = b && e - b == (ptrdiff_t)f->size && *(uint64_t *)(st + 0x20) == 0 && *(uint8_t **)(st + 0x30) == b;
        int game = whole && crc32(b, f->size) == f->crc;
        const char *what = !whole ? "its own (stream not as expected)" : !game ? "its own (not the game's file: a mod?)" : "its own (unknown stream)";
        if (game && vt == VT_ZIP_STREAM) {
            memcpy(b, f->data, f->size);
            what = "ours";
        } else if (game && vt == VT_BUF_STREAM && g_ui_copy[i]) {
            uint8_t *c = g_ui_copy[i];
            *(uint8_t **)(st + 8) = c; *(uint8_t **)(st + 0x10) = c + f->size; *(uint64_t *)(st + 0x18) = f->size;
            *(uint8_t **)(st + 0x30) = c; *(uint8_t **)(st + 0x28) = c + f->size;
            what = "ours";
        }
        rc_log("pvp: the game opens %s: %s", f->path, what);
        break;
    }
    return r;
}

// ---- the page --------------------------------------------------------------------------------------------
typedef struct { union { char buf[16]; char *ptr; }; uint64_t size, cap; } GameString;   // the game's std::string

static void *find(void *layout, const char *id) {
    GameString s;
    memset(&s, 0, sizeof s);
    s.size = strlen(id);
    if (s.size <= 15) { memcpy(s.buf, id, s.size); s.cap = 15; }
    else { s.ptr = (char *)id; s.cap = s.size; }   // longer text sits outside; only read here
    void *(*fn)(void *, GameString *) = (*(void *(***)(void *, GameString *))layout)[0x1e0 / 8];
    return fn(layout, &s);
}

static void log_chain(const char *what, void *d) {
    for (int i = 0; d && i < 24; i++) {
        uint64_t vt = 0, parent = 0;
        int32_t pos[2] = {0}, size[2] = {0};
        uint32_t flags = 0;
        peek(d, &vt, 8); peek((uint8_t *)d + 0x48, &flags, 4); peek((uint8_t *)d + 0x98, pos, 8);
        peek((uint8_t *)d + 0xa0, size, 8); peek((uint8_t *)d + 0xf0, &parent, 8);
        rc_log("pvp: %s %d: %p vt %llx flags %x at %d,%d size %dx%d", what, i, d, (unsigned long long)(vt - (uintptr_t)g_base),
               flags, pos[0], pos[1], size[0], size[1]);
        d = (void *)parent;
    }
}

// the page's own list of games and its column titles on or off the screen (bit 1 of the flags, then the
// layout is told, as the dialog's own vtable +0x60 does); the game's thread only
static void list_shown(int on) {
    void *ds[2] = {g_list, g_head};
    for (int i = 0; i < 2; i++) {
        uint8_t *d = ds[i];
        if (!d) continue;
        uint32_t *flags = (uint32_t *)(d + 0x48);
        *flags = on ? *flags | 2 : *flags & ~2u;
        (*(void (***)(void *))d)[0x60 / 8](d);
    }
}

static void bind_page(int k, void *page) {
    void *layout = *(void **)((uint8_t *)page + 0x58);
    void *target = *(void **)((uint8_t *)page + 0x60);
    if (!layout) return;
    void *btn = find(layout, "btn_pvp");
    void *list = find(layout, "session_list_container"), *head = find(layout, "session_list_header");
    void *hidden = find(layout, "btn_ip_list");     // <Visible value="false"/>: its flags show which bit that is
    uint32_t hidden_flags = 0;
    if (hidden) peek((uint8_t *)hidden + 0x48, &hidden_flags, 4);
    rc_log("pvp: %s page %p bound: button %p, list %p, header %p, target %p (page%+lld), hidden button flags %x", PAGES[k].name,
           page, btn, list, head, target, (long long)((uint8_t *)target - (uint8_t *)page), hidden_flags);
    if (!btn || !target) return;
    void *(*cast)(void *, long, void *, void *, int) = (void *(*)(void *, long, void *, void *, int))(g_base + FN_DYNCAST);
    void *button = cast(btn, 0, g_base + TD_DIALOG, g_base + TD_BUTTON, 0);
    if (!button) { rc_log("pvp: btn_pvp is not a button"); return; }
    ((void (*)(void *, void *, int, int))(g_base + FN_ON_EVENT))(button, target, 7, PVP_CMD);
    if (page != g_page && g_pvp_on) { list_shown(1); g_pvp_on = 0; }   // another page: the last one gets its list back
    g_head = head; g_list = list; g_btn = btn; g_page = page;
    if (g_pvp_on) list_shown(0);
}

static void bind_online(void *page) { g_bind[0](page); bind_page(0, page); }
static void bind_lan(void *page) { g_bind[1](page); bind_page(1, page); }

static int command(void *msg) {
    int id = *(int *)((uint8_t *)msg + 8);
    if (id == PVP_CMD) {
        g_pvp_on = !g_pvp_on;
        list_shown(!g_pvp_on);
        rc_log("pvp: button, block %s", g_pvp_on ? "on" : "off");
        if (InterlockedIncrement(&g_cmds_logged) == 1) { log_chain("button", g_btn); log_chain("list", g_list); }
        return 1;
    }
    if (id >= 0x15 && id <= 0x21 && g_pvp_on) {     // the page's own buttons bring its list back
        g_pvp_on = 0;
        list_shown(1);
    }
    return 0;
}

static void cmd_online(void *self, void *msg) { if (!command(msg)) g_cmd[0](self, msg); }
static void cmd_lan(void *self, void *msg) { if (!command(msg)) g_cmd[1](self, msg); }

static void forget(void *page) {
    if (page != g_page) return;
    g_page = g_btn = g_list = g_head = NULL;
    g_pvp_on = 0;
}
static void *dtor_online(void *page, unsigned flags) { forget(page); return g_dtor[0](page, flags); }
static void *dtor_lan(void *page, unsigned flags) { forget(page); return g_dtor[1](page, flags); }

// where a dialog is on the game's screen, up to the screen's root (a page put away has a root of its own);
// shown: it and every parent are on, else only where it would be (the list, while we keep it off)
static int place(void *d, int shown_too, int32_t *x, int32_t *y, int32_t *w, int32_t *h, int32_t *root_w, int32_t *root_h) {
    int32_t size[2];
    if (!peek((uint8_t *)d + 0xa0, size, 8)) return 0;
    *w = size[0]; *h = size[1]; *x = *y = 0;
    for (int i = 0; i < 64; i++) {
        uint32_t flags;
        int32_t pos[2];
        uint64_t parent, vt, shown;
        if (!peek((uint8_t *)d + 0x48, &flags, 4) || (shown_too && !(flags & 2)) || !peek((uint8_t *)d + 0x98, pos, 8) ||
            !peek((uint8_t *)d + 0xf0, &parent, 8))
            return 0;
        if (!parent) {      // the root: the screen's overrides "shown"; a page put away has its own root
            if (!peek(d, &vt, 8) || !peek((void *)(vt + 0x108), &shown, 8) || shown == (uintptr_t)(g_base + FN_IS_SHOWN) ||
                !peek((uint8_t *)d + 0xa0, size, 8))
                return 0;
            *root_w = size[0]; *root_h = size[1];
            return 1;
        }
        *x += pos[0]; *y += pos[1];
        d = (void *)parent;
    }
    return 0;
}

// A page put over ours (the ESC menu, a confirmation): the screen's root keeps each page's top panel as a
// child, the later ones drawn over the earlier. Covered = a later child of the same kind is shown.
static int covered(void *d) {
    uint64_t top = (uint64_t)(uintptr_t)d, up, root = 0;
    for (int i = 0; i < 64; i++) {
        if (!peek((void *)(top + 0xf0), &up, 8) || !up) return 0;
        uint64_t upup = 0;
        if (!peek((void *)(up + 0xf0), &upup, 8)) return 0;
        if (!upup) { root = up; break; }
        top = up;
    }
    if (!root) return 0;
    uint64_t b = 0, e = 0, kind = 0;
    peek((void *)(root + 0xd8), &b, 8); peek((void *)(root + 0xe0), &e, 8); peek((void *)top, &kind, 8);
    if (!b || e < b || e - b > 8 * 256) return 0;
    int after = 0;
    for (uint64_t p = b; p < e; p += 8) {
        uint64_t c = 0, vt = 0;
        uint32_t flags = 0;
        peek((void *)p, &c, 8);
        if (c == top) { after = 1; continue; }
        if (after && peek((void *)c, &vt, 8) && vt == kind && peek((void *)(c + 0x48), &flags, 4) && (flags & 2)) return 1;
    }
    return 0;
}

int pvp_rect(float *x, float *y, float *w, float *h, float *head_h, float *root_w, float *root_h) {
    void *btn = g_btn, *list = g_list, *head = g_head;
    if (btn && covered(btn)) return 0;
    int32_t bx, by, bw, bh, lx, ly, lw, lh, rw, rh;
    if (!btn || !list || !place(btn, 1, &bx, &by, &bw, &bh, &rw, &rh) || !place(list, 0, &lx, &ly, &lw, &lh, &rw, &rh)) return 0;
    int32_t hx, hy, hw, hh, top = 0;
    if (head && place(head, 0, &hx, &hy, &hw, &hh, &rw, &rh) && hy < ly) { top = ly - hy; lh += top; ly = hy; }   // the column titles too
    if (lw <= 0 || lh <= 0 || rw <= 0 || rh <= 0) return 0;
    *x = (float)lx; *y = (float)ly; *w = (float)lw; *h = (float)lh; *head_h = (float)top;
    *root_w = (float)rw; *root_h = (float)rh;
    return 1;
}

// a file kept unpacked in the game's interface.pak (its fonts are), read from the zip's central directory;
// malloc'd, NULL if it is not there or packed
unsigned char *pvp_pak_file(const char *name, size_t *n) {
    FILE *f = g_pak[0] ? fopen(g_pak, "rb") : NULL;
    if (!f) return NULL;
    unsigned char *out = NULL;
    static uint8_t tail[65557];
    _fseeki64(f, 0, SEEK_END);
    long long len = _ftelli64(f), from = len > (long long)sizeof tail ? len - (long long)sizeof tail : 0;
    _fseeki64(f, from, SEEK_SET);
    size_t got = fread(tail, 1, sizeof tail, f);
    for (long long i = (long long)got - 22; i >= 0; i--) {
        if (memcmp(tail + i, "PK\x05\x06", 4)) continue;
        uint32_t cd_size, cd_off;
        memcpy(&cd_size, tail + i + 12, 4);
        memcpy(&cd_off, tail + i + 16, 4);
        uint8_t *cd = malloc(cd_size);
        if (cd && !_fseeki64(f, cd_off, SEEK_SET) && fread(cd, 1, cd_size, f) == cd_size) {
            size_t want = strlen(name);
            for (uint32_t p = 0; p + 46 <= cd_size && !memcmp(cd + p, "PK\x01\x02", 4);) {
                uint16_t method, nl, xl, cl;
                uint32_t size, at;
                memcpy(&method, cd + p + 10, 2); memcpy(&size, cd + p + 24, 4);
                memcpy(&nl, cd + p + 28, 2); memcpy(&xl, cd + p + 30, 2); memcpy(&cl, cd + p + 32, 2); memcpy(&at, cd + p + 42, 4);
                if (nl == want && p + 46 + nl <= cd_size && !memcmp(cd + p + 46, name, nl)) {
                    uint8_t lh[30];
                    uint16_t lnl, lxl;
                    if (method == 0 && size < (64u << 20) && !_fseeki64(f, at, SEEK_SET) && fread(lh, 1, 30, f) == 30 &&
                        !memcmp(lh, "PK\x03\x04", 4)) {
                        memcpy(&lnl, lh + 26, 2); memcpy(&lxl, lh + 28, 2);
                        out = malloc(size ? size : 1);
                        if (out && (_fseeki64(f, (long long)at + 30 + lnl + lxl, SEEK_SET) || fread(out, 1, size, f) != size)) {
                            free(out);
                            out = NULL;
                        }
                        *n = size;
                    }
                    break;
                }
                p += 46 + nl + xl + cl;
            }
        }
        free(cd);
        break;
    }
    fclose(f);
    return out;
}

// ---- the lobby's player list: MMR under each name (ui.cpp) ------------------------------------------------
// Seen in a running game (2026-11-02): the session page's chat panel, eGUI_ChatChannelSessionView (vtable
// 1042fc0), lists its members in an eMenuListbox as plain eListboxItems (22 units high), each with an
// eUserNameView (vtable 105b538) whose +0x178 is the player's SteamID. They are found twice a second by
// walking the interface down from the screen's root, eDesktop (the game keeps it at 159ca68), through each
// dialog's children (+0xd8..+0xe0); their places are taken every frame.
#define G_DESKTOP 0x159ca68
#define VT_DESKTOP 0x1101800
#define VT_SESSION_VIEW 0x1042fc0
#define VT_NAME_VIEW 0x105b538
#define MAX_NAMES 16

static void *g_names[MAX_NAMES];
static int g_name_count;
static SRWLOCK g_names_lock = SRWLOCK_INIT;
static DWORD g_scan_at;

void pvp_page_shown(void *page) { (void)page; }

static uint64_t pq(uint64_t a) { uint64_t v = 0; peek((void *)a, &v, 8); return v; }

static void walk(uint64_t d, int depth, int in_session, void **found, int *n, int *budget) {
    if (!d || depth > 72 || --*budget < 0 || *n >= MAX_NAMES) return;
    uint32_t flags = 0;
    if (!peek((void *)(d + 0x48), &flags, 4) || !(flags & 2)) return;     // not on screen: nothing under it is
    uint64_t vt = pq(d);
    if (vt == (uintptr_t)(g_base + VT_SESSION_VIEW)) in_session = 1;
    if (in_session && vt == (uintptr_t)(g_base + VT_NAME_VIEW)) { found[(*n)++] = (void *)d; return; }
    uint64_t b = pq(d + 0xd8), e = pq(d + 0xe0);
    if (!b || e < b || e - b > 8 * 4096) return;
    for (uint64_t p = b; p < e; p += 8) walk(pq(p), depth + 1, in_session, found, n, budget);
}

static void scan_names(void) {
    DWORD now = GetTickCount();
    if (now - g_scan_at < 500) return;
    g_scan_at = now;
    uint64_t root = pq((uintptr_t)(g_base + G_DESKTOP));
    void *found[MAX_NAMES];
    int n = 0, budget = 20000;
    if (root && pq(root) == (uintptr_t)(g_base + VT_DESKTOP)) walk(root, 0, 0, found, &n, &budget);
    AcquireSRWLockExclusive(&g_names_lock);
    memcpy(g_names, found, n * sizeof found[0]);
    g_name_count = n;
    ReleaseSRWLockExclusive(&g_names_lock);
    static int last_n = -1;
    if (n != last_n) { rc_log("pvp: lobby names on screen: %d (%d dialogs looked at)", n, 20000 - budget); last_n = n; }
}

int lobby_rows(LobbyRow *out, int max) {
    scan_names();
    void *names[MAX_NAMES];
    AcquireSRWLockShared(&g_names_lock);
    int count = g_name_count;
    memcpy(names, g_names, count * sizeof names[0]);
    ReleaseSRWLockShared(&g_names_lock);
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        int32_t x, y, w, h, rw, rh;
        uint64_t id = 0;
        if (!place(names[i], 1, &x, &y, &w, &h, &rw, &rh) || w <= 0 || h <= 0 || covered(names[i])) continue;
        if (!peek((uint8_t *)names[i] + 0x178, &id, 8) || id < 76561190000000000ull || id > 76561199999999999ull) continue;
        LobbyRow *r = &out[n++];
        r->x = (float)x; r->y = (float)y; r->w = (float)w; r->h = (float)h; r->root_w = (float)rw; r->root_h = (float)rh;
        uint64_t parent = 0;
        int32_t px, py, pw, ph;
        r->line_r = peek((uint8_t *)names[i] + 0xf0, &parent, 8) && parent && place((void *)parent, 1, &px, &py, &pw, &ph, &rw, &rh)
                    ? (float)(px + pw) : r->x + r->w;
        snprintf(r->id, sizeof r->id, "%llu", (unsigned long long)id);
    }
    return n;
}

static int vt_slot(uint32_t vt, int slot, uint32_t want) {
    uint64_t v = 0;
    return peek(g_base + vt + 8 * slot, &v, 8) && v == (uintptr_t)(g_base + want);
}

int pvp_install(uint8_t *exe, const char *dir, uint8_t *(*alloc)(size_t), void (*patch)(void *, const void *, size_t)) {
    g_base = exe;
    snprintf(g_pak, sizeof g_pak, "%s..\\..\\resource\\interface\\interface.pak", dir);
    for (int k = 0; k < 2; k++)
        if (!vt_slot(PAGES[k].vt, 0, PAGES[k].dtor) || !vt_slot(PAGES[k].vt, 20, FN_BIND) || !vt_slot(PAGES[k].vt_cmd, 1, FN_COMMAND)) {
            rc_log("pvp: the game list page is not as expected, no PVP button");
            return 0;
        }
    for (size_t i = 0; i < NUI; i++) {
        g_ui_copy[i] = malloc(UI_FILES[i].size);
        if (g_ui_copy[i]) memcpy(g_ui_copy[i], UI_FILES[i].data, UI_FILES[i].size);
    }
    // the file open: after balance.c, whose jump is at its start; or the game's own first 10 bytes
    uint8_t *entry = exe + FN_REPO_OPEN, *s = alloc(16);
    if (!s) return 0;
    if (entry[0] == 0xe9) {
        int32_t rel;
        memcpy(&rel, entry + 1, 4);
        g_prev_open = (void *(*)(void *, uint32_t))(entry + 5 + rel);
    } else if (!memcmp(entry, REPO_OPEN_INS, 10)) {
        uint8_t *t = alloc(32);
        if (!t) return 0;
        memcpy(t, REPO_OPEN_INS, 10);
        t[10] = 0xff; t[11] = 0x25; memset(t + 12, 0, 4);
        uint64_t back = (uint64_t)(uintptr_t)(entry + 10); memcpy(t + 16, &back, 8);
        g_prev_open = (void *(*)(void *, uint32_t))t;
    } else {
        rc_log("pvp: the file open is not as expected, no PVP button");
        return 0;
    }
    s[0] = 0x48; s[1] = 0xb8;
    uint64_t fn = (uint64_t)(uintptr_t)open_hook; memcpy(s + 2, &fn, 8);
    s[10] = 0xff; s[11] = 0xe0;
    uint8_t jmp[10] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    int32_t rel = (int32_t)(s - (entry + 5)); memcpy(jmp + 1, &rel, 4);
    patch(entry, jmp, 10);
    // the pages: their vtable slots point at ours, which call the game's
    void *binds[2] = {(void *)bind_online, (void *)bind_lan}, *cmds[2] = {(void *)cmd_online, (void *)cmd_lan};
    void *dtors[2] = {(void *)dtor_online, (void *)dtor_lan};
    for (int k = 0; k < 2; k++) {
        g_bind[k] = (void (*)(void *))(exe + FN_BIND);
        g_cmd[k] = (void (*)(void *, void *))(exe + FN_COMMAND);
        g_dtor[k] = (void *(*)(void *, unsigned))(exe + PAGES[k].dtor);
        patch(exe + PAGES[k].vt + 8 * 20, &binds[k], 8);
        patch(exe + PAGES[k].vt_cmd + 8, &cmds[k], 8);
        patch(exe + PAGES[k].vt, &dtors[k], 8);
    }
    rc_log("pvp: ready");
    return 1;
}
