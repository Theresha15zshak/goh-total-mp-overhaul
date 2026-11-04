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
#include <math.h>
#include "goh_replay.h"
#include "camo.h"
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
static void *volatile g_page, *volatile g_btn, *volatile g_list, *volatile g_head, *volatile g_chat_frame;
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

#define G_DESKTOP 0x159ca68     // the screen's root, eDesktop (see the lobby below)
static uint64_t pq(uint64_t a) { uint64_t v = 0; peek((void *)a, &v, 8); return v; }

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

// The Extras page's Camouflage tab has no text of the game's to go by: its title in the language of the mod. The file keeps its
// length: what comes after moves into the spaces at the end.
static void localize_layout(uint8_t *b, size_t n) {
    static const char key[] = "titleValue=\"CAMOUFLAGE\"", ru[] = "titleValue=\"КАМУФЛЯЖ\"";
    const size_t kn = sizeof key - 1, rn = sizeof ru - 1, grow = rn - kn;
    if (lang_en() || n < rn) return;
    for (size_t i = 0; i + kn <= n; i++) {
        if (memcmp(b + i, key, kn)) continue;
        for (size_t k = n - grow; k < n; k++) if (b[k] != ' ') return;
        memmove(b + i + rn, b + i + kn, n - i - rn);
        memcpy(b + i, ru, rn);
        return;
    }
}

static void *open_hook(void *path, uint32_t mode) {
    void *r;
    if (camo_open(path, mode, g_prev_open, &r)) return r;
    r = g_prev_open(path, mode);
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
            localize_layout(b, f->size);
            what = "ours";
        } else if (game && vt == VT_BUF_STREAM && g_ui_copy[i]) {
            uint8_t *c = g_ui_copy[i];
            localize_layout(c, f->size);
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
    g_chat_frame = find(layout, "gchat_frame");
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
    // The page's own commands leave the block on: the page sends some by itself (seen 2026-11-02: the block went
    // off with nobody at the PVP button, the list refreshing on its own), and HOST / JOIN take the player to
    // another page anyway. Only the PVP button puts the list back.
    if (id >= 0x15 && id <= 0x21 && g_pvp_on) {
        static uint32_t logged;
        if (!(logged & 1u << (id - 0x15))) { logged |= 1u << (id - 0x15); rc_log("pvp: page command %#x with the block on", id); }
    }
    return 0;
}

// after the page's own handling: whatever it did to its list, ours stays over it
static void after_command(void) { if (g_pvp_on) list_shown(0); }

static void cmd_online(void *self, void *msg) { if (!command(msg)) { g_cmd[0](self, msg); after_command(); } }
static void cmd_lan(void *self, void *msg) { if (!command(msg)) { g_cmd[1](self, msg); after_command(); } }

static void forget(void *page) {
    if (page != g_page) return;
    g_page = g_btn = g_list = g_head = g_chat_frame = NULL;
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
int dialog_covered(void *d) { return covered(d); }

// The chat's frame is drawn over only while its page is the one on the screen. Seen 2026-11-02: with the
// "host game" page up, the list page's frame still had all its parents shown up to a root of its own, and the
// chat filled the screen. So: its root must be the screen's (eDesktop), and the frame where the layout puts it,
// a column at the right.
static uint64_t root_of(uint64_t d) {
    for (int i = 0; d && i < 64; i++) {
        uint64_t up = pq(d + 0xf0);
        if (!up) return d;
        d = up;
    }
    return 0;
}

int chat_rect(float *x, float *y, float *w, float *h, float *root_w, float *root_h) {
    void *f = g_chat_frame;
    int32_t fx, fy, fw, fh, rw, rh;
    static int why_logged = -1;
    int why = !f ? 1 : covered(f) ? 2 : !place(f, 1, &fx, &fy, &fw, &fh, &rw, &rh) ? 3 :
              root_of((uintptr_t)f) != pq((uintptr_t)(g_base + G_DESKTOP)) ? 4 :
              fw <= 0 || fh < rh / 6 || rw <= 0 || rh <= 0 || fx < rw / 2 || fw > rw / 2 || fy < rh / 8 || fy + fh > rh ? 5 : 0;   // laid out small under another page: not it
    if (why != why_logged && why != 1) {
        rc_log("pvp: chat frame %s", why == 0 ? "on screen" : why == 2 ? "covered" : why == 3 ? "not shown" :
                                                 why == 4 ? "under another root (page put away)" : "not where the layout puts it");
        if (why == 5) rc_log("pvp: chat frame at %d,%d size %dx%d of %dx%d", fx, fy, fw, fh, rw, rh);
    }
    why_logged = why;
    if (why) return 0;
    *x = (float)fx; *y = (float)fy; *w = (float)fw; *h = (float)fh; *root_w = (float)rw; *root_h = (float)rh;
    return 1;
}

int pvp_rect(float *x, float *y, float *w, float *h, float *head_h, float *root_w, float *root_h) {
    void *btn = g_btn, *list = g_list, *head = g_head;
    if (btn && covered(btn)) return 0;
    int32_t bx, by, bw, bh, lx, ly, lw, lh, rw, rh;
    if (!btn || !list || !place(btn, 1, &bx, &by, &bw, &bh, &rw, &rh) || !place(list, 0, &lx, &ly, &lw, &lh, &rw, &rh)) return 0;
    if (root_of((uintptr_t)btn) != pq((uintptr_t)(g_base + G_DESKTOP))) return 0;     // its page put away (chat_rect)
    int32_t hx, hy, hw, hh, top = 0;
    if (head && place(head, 0, &hx, &hy, &hw, &hh, &rw, &rh) && hy < ly) { top = ly - hy; lh += top; ly = hy; }   // the column titles too
    if (lw <= 0 || lh <= 0 || rw <= 0 || rh <= 0) return 0;
    *x = (float)lx; *y = (float)ly; *w = (float)lw; *h = (float)lh; *head_h = (float)top;
    *root_w = (float)rw; *root_h = (float)rh;
    return 1;
}


// a file kept unpacked in the game's interface.pak (its fonts are), read from the zip's central directory;
// malloc'd, NULL if it is not there or packed
static char g_sound_pak[MAX_PATH];

static unsigned char *pak_file(const char *pak, const char *name, size_t *n) {
    FILE *f = pak[0] ? fopen(pak, "rb") : NULL;
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

unsigned char *pvp_pak_file(const char *name, size_t *n) { return pak_file(g_pak, name, n); }
unsigned char *pvp_sound_file(const char *name, size_t *n) { return pak_file(g_sound_pak, name, n); }

// ---- the lobby's player list: MMR under each name (ui.cpp) ------------------------------------------------
// Seen in a running game (2026-11-02): the session page's chat panel, eGUI_ChatChannelSessionView (vtable
// 1042fc0), lists its members in an eMenuListbox as plain eListboxItems (22 units high), each with an
// eUserNameView (vtable 105b538) whose +0x178 is the player's SteamID. They are found twice a second by
// walking the interface down from the screen's root, eDesktop (the game keeps it at 159ca68), through each
// dialog's children (+0xd8..+0xe0); their places are taken every frame.
#define VT_DESKTOP 0x1101800
#define VT_SESSION_VIEW 0x1042fc0
#define VT_NAME_VIEW 0x105b538
#define MAX_NAMES 16

static void *g_names[MAX_NAMES];
static int g_name_count;
static SRWLOCK g_names_lock = SRWLOCK_INIT;
static DWORD g_scan_at;

void pvp_page_shown(void *page) { (void)page; }


// The chat panel of the lobby (eGUI_ChatView, vtable 1044a10): its tabs on top (MAIN LOBBY | SESSION) and the
// channel of the tab that is on below them (an eGUI_ChatChannel...View). Our GLOBAL CHAT tab goes after the
// game's (ui.cpp), and our chat over the channel's place while it is on.
#define VT_CHAT_VIEW 0x1044a10
static const uint32_t VT_CHANNEL_VIEWS[] = {0x1042fc0, 0x1042c58, 0x1041bd0, 0x10435e0, 0x1043da0, 0x1042270};
static void *g_panel_walk;                   // found on this walk
static void *volatile g_panel;               // ... and the last one
#define VT_GAME_MENU 0x1013f48               // eDialogBlur: only the ESC menu (page_game_menu) and popups have one
static int g_menu_walk;                      // seen on this walk
static volatile int g_game_menu;             // ... and on the last one

static void walk(uint64_t d, int depth, int in_session, void **found, int *n, int *budget) {
    if (!d || depth > 72 || --*budget < 0 || *n >= MAX_NAMES) return;
    uint32_t flags = 0;
    if (!peek((void *)(d + 0x48), &flags, 4) || !(flags & 2)) return;     // not on screen: nothing under it is
    uint64_t vt = pq(d);
    if (vt == (uintptr_t)(g_base + VT_CHAT_VIEW) && !g_panel_walk) g_panel_walk = (void *)d;
    if (vt == (uintptr_t)(g_base + VT_GAME_MENU)) g_menu_walk = 1;
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
    g_panel_walk = NULL;
    g_menu_walk = 0;
    if (root && pq(root) == (uintptr_t)(g_base + VT_DESKTOP)) walk(root, 0, 0, found, &n, &budget);
    if (g_menu_walk != g_game_menu) rc_log("pvp: the ESC menu %s", g_menu_walk ? "on screen" : "gone");
    g_game_menu = g_menu_walk;
    if (g_panel_walk != g_panel) rc_log("pvp: the game's chat panel %s", g_panel_walk ? "on screen" : "gone");
    g_panel = g_panel_walk;
    AcquireSRWLockExclusive(&g_names_lock);
    memcpy(g_names, found, n * sizeof found[0]);
    g_name_count = n;
    ReleaseSRWLockExclusive(&g_names_lock);
    static int last_n = -1;
    if (n != last_n) { rc_log("pvp: lobby names on screen: %d (%d dialogs looked at)", n, 20000 - budget); last_n = n; }
}

int pvp_game_menu(void) { scan_names(); return g_game_menu; }

// ---- the lobby's lines taller: the listbox's own line height -------------------------------------------------
// What did not work (2026-11-02): resizing and moving the lines ourselves (the listbox put them back: they jumped),
// a MinSize on a line or its content (not asked), a line's size answer (+0xc0, never asked), the lines' SetSize
// (the session list makes its lines anew and sizes them all from one value). That value is the listbox's own:
// an eMenuListbox (vtable 1053370) keeps its lines' size at +0x1c0 (w, h), gives it to every line (ac77dd, each
// line's SetSize) and stacks them by its height (ac7919); when its width changes it keeps the height (ac5257,
// ac732f). So the lobby's session list gets a line height of LINE_H once, the way the game itself changes it
// (ac527a: the value, vtable +0x248, ac67b0, aa3f60 with 0xd), and it lays its lines out at that height from then
// on: names, avatars and icons with them. On the game's logic thread (main_tick, before its GUI update).
#define VT_LINE_ITEM 0x1019228          // eListboxItem
#define VT_MENU_LISTBOX 0x1053370       // eMenuListbox
#define LB_LINE_SIZE 0x1c0
#define FN_LB_LAYOUT 0xac67b0
#define FN_DLG_NOTIFY 0xaa3f60
#define LINE_H 50                       // the game's 22 and more: the name and the MMR under it
// The session view gives its list the game's 22 again whenever it updates (through ac7e97 / ac909b and the like,
// each followed by the listbox's "lines' size changed", vtable +0x248, ac52e0): the lines flickered between that
// and ours until the next tick. So that call is hooked for menu listboxes: in the session list, the height it is
// about to give the lines is ours before it does.
static void (*g_lb_apply)(void *self);

static int under_session_view(uint64_t d) {
    for (int up = 0; up < 12 && d; up++) {
        d = pq(d + 0xf0);
        if (d && pq(d) == (uintptr_t)(g_base + VT_SESSION_VIEW)) return 1;
    }
    return 0;
}

static void lb_apply(void *self) {
    int32_t *size = (int32_t *)((uint8_t *)self + LB_LINE_SIZE);
    if (size[1] > 0 && size[1] < LINE_H && under_session_view((uintptr_t)self)) {
        static int logged;
        if (logged++ < 20)
            rc_log("pvp: the session list was given lines %d high by %llx: %d", size[1],
                   (unsigned long long)((uint8_t *)__builtin_return_address(0) - g_base), LINE_H);
        size[1] = LINE_H;
    }
    g_lb_apply(self);
}

static void lb_apply_install(void (*patch)(void *, const void *, size_t)) {
    void **slot = (void **)(g_base + VT_MENU_LISTBOX + 0x248);
    if (*slot != (void *)(g_base + 0xac52e0)) { rc_log("pvp: the menu listbox is not as expected, its lines may flicker"); return; }
    g_lb_apply = (void (*)(void *))*slot;
    void *ours = (void *)lb_apply;
    patch(slot, &ours, sizeof ours);
}

static uint64_t g_tall_listbox;          // the last one made taller (a new one is made each time a lobby opens)

// the session list and the names in its lines
typedef struct { uint64_t listbox; void *names[MAX_NAMES]; int n; } SessionFind;

static void tall_walk(uint64_t d, int depth, int in_session, int *budget, SessionFind *f) {
    if (!d || depth > 72 || --*budget < 0) return;
    uint32_t flags = 0;
    if (!peek((void *)(d + 0x48), &flags, 4) || !(flags & 2)) return;
    uint64_t vt = pq(d);
    if (vt == (uintptr_t)(g_base + VT_SESSION_VIEW)) in_session = 1;
    if (in_session && vt == (uintptr_t)(g_base + VT_MENU_LISTBOX) && !f->listbox) f->listbox = d;
    if (in_session && vt == (uintptr_t)(g_base + VT_NAME_VIEW)) { if (f->n < MAX_NAMES) f->names[f->n++] = (void *)(uintptr_t)d; return; }
    uint64_t b = pq(d + 0xd8), e = pq(d + 0xe0);
    if (!b || e < b || e - b > 8 * 4096) return;
    for (uint64_t p = b; p < e; p += 8) tall_walk(pq(p), depth + 1, in_session, budget, f);
}

static int rows_now(void **names, int count, LobbyRow *out, int max);
static int names_alive(void **names, int count);
static void publish_rows(const LobbyRow *rows, int n, DWORD now);
// The MMR under the names is drawn on another thread than the one laying the list out: read there, the lines'
// places were sometimes caught in the middle of a layout and the MMR blinked to a wrong place (2026-11-02). So the
// places are read here, on the game's thread between its layouts, and the drawing takes the last copy.
static LobbyRow g_rows[16];
static int g_rows_n;
static DWORD g_rows_at;
static SRWLOCK g_rows_lock = SRWLOCK_INIT;

void pvp_gui_tick(void) {
    static DWORD at, rows_at;
    static SessionFind f;                // the session list and its names as last found; checked on every iteration
    DWORD now = GetTickCount();
    if (!g_base) return;
    if (now - at >= 250 || (now - at >= 100 && !names_alive(f.names, f.n))) {   // a new look: in time, or a line made anew
        at = now;
        uint64_t root = pq((uintptr_t)(g_base + G_DESKTOP));
        if (!root || pq(root) != (uintptr_t)(g_base + VT_DESKTOP)) return;
        int budget = 20000;
        memset(&f, 0, sizeof f);
        tall_walk(root, 0, 0, &budget, &f);
    }
    if (now - rows_at >= 30) {           // the places for the MMR, about thirty times a second
        rows_at = now;
        LobbyRow rows[16];
        int n = rows_now(f.names, f.n, rows, 16);
        publish_rows(rows, n, now);
    }
    uint64_t lb = f.listbox;
    if (!lb || pq(lb) != (uintptr_t)(g_base + VT_MENU_LISTBOX)) { f.listbox = 0; return; }
    int32_t *size = (int32_t *)(uintptr_t)(lb + LB_LINE_SIZE);
    if (size[1] == LINE_H) return;
    if (lb == g_tall_listbox) {          // ours taken back by something the hook did not see
        static int logged;
        if (logged++ < 20) rc_log("pvp: the session list's lines were %d high again (not through +0x248)", size[1]);
    }
    int32_t was = size[1];
    size[1] = LINE_H;
    void **vt = *(void ***)(uintptr_t)lb;
    ((void (*)(void *))vt[0x248 / 8])((void *)(uintptr_t)lb);
    ((void (*)(void *))(g_base + FN_LB_LAYOUT))((void *)(uintptr_t)lb);
    ((void (*)(void *, int))(g_base + FN_DLG_NOTIFY))((void *)(uintptr_t)lb, 0xd);
    if (lb != g_tall_listbox) rc_log("pvp: the lobby's session list: lines %d high instead of %d", LINE_H, was);
    g_tall_listbox = lb;
}

// The session list makes its lines anew when it updates: names found before are then gone (another object, or
// not on the screen), and the MMR under them blinked out until the next look. One of them gone: a new look then.
static int names_alive(void **names, int count) {
    for (int i = 0; i < count; i++) {
        uint32_t flags = 0;
        if (pq((uintptr_t)names[i]) != (uintptr_t)(g_base + VT_NAME_VIEW) || !peek((uint8_t *)names[i] + 0x48, &flags, 4) || !(flags & 2))
            return 0;
    }
    return 1;
}

// What is drawn is steady: a line's place changes only when it was read the same twice in a row (60 ms), and a
// line missing from one reading stays for 200 ms; lines are kept in the order of their places. Whatever the list
// does for a moment while it updates is not drawn. Lines whose spacing is not LINE_H are logged (a few times).
typedef struct { LobbyRow row, seen; int same; DWORD at; } SteadyRow;
static SteadyRow g_steady[16];
static int g_steady_n;

static int same_place(const LobbyRow *a, const LobbyRow *b) {
    return fabsf(a->x - b->x) < 1 && fabsf(a->y - b->y) < 1 && fabsf(a->line_bottom - b->line_bottom) < 1;
}

static void publish_rows(const LobbyRow *rows, int n, DWORD now) {
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (k < g_steady_n && strcmp(g_steady[k].row.id, rows[i].id)) k++;
        if (k == g_steady_n) {                                  // a new one: as it is
            if (g_steady_n == 16) continue;
            g_steady_n++;
            g_steady[k].row = g_steady[k].seen = rows[i];
            g_steady[k].same = 2;
        } else if (same_place(&g_steady[k].seen, &rows[i])) {
            if (++g_steady[k].same >= 2) g_steady[k].row = rows[i];
        } else {
            g_steady[k].seen = rows[i];
            g_steady[k].same = 1;
        }
        g_steady[k].at = now;
        if (i > 0) {
            static int logged;
            float gap = rows[i].line_bottom - rows[i - 1].line_bottom;
            if (fabsf(gap) > 1 && fabsf(fabsf(gap) - LINE_H) > 1 && logged++ < 20)
                rc_log("pvp: lobby lines %d high apart, not %d (line %d)", (int)gap, LINE_H, i);
        }
    }
    int keep = 0;
    for (int k = 0; k < g_steady_n; k++) if (now - g_steady[k].at < 200) g_steady[keep++] = g_steady[k];
    g_steady_n = keep;
    for (int a = 1; a < g_steady_n; a++)                       // by place, top down
        for (int b = a; b > 0 && g_steady[b].row.y < g_steady[b - 1].row.y; b--) {
            SteadyRow t = g_steady[b]; g_steady[b] = g_steady[b - 1]; g_steady[b - 1] = t;
        }
    AcquireSRWLockExclusive(&g_rows_lock);
    for (int k = 0; k < g_steady_n; k++) g_rows[k] = g_steady[k].row;
    g_rows_n = g_steady_n;
    g_rows_at = now;
    ReleaseSRWLockExclusive(&g_rows_lock);
}

// the lines' places as pvp_gui_tick last read them (none if it has not for half a second: not in a lobby)
int lobby_rows(LobbyRow *out, int max) {
    AcquireSRWLockShared(&g_rows_lock);
    int n = GetTickCount() - g_rows_at < 500 ? (g_rows_n < max ? g_rows_n : max) : 0;
    memcpy(out, g_rows, n * sizeof out[0]);
    ReleaseSRWLockShared(&g_rows_lock);
    return n;
}

// the game's thread: each name's place, whose it is, and its line's bottom
static int rows_now(void **names, int count, LobbyRow *out, int max) {
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        int32_t x, y, w, h, rw, rh;
        uint64_t id = 0;
        if (!place(names[i], 1, &x, &y, &w, &h, &rw, &rh) || w <= 0 || h <= 0 || covered(names[i])) continue;
        if (!peek((uint8_t *)names[i] + 0x178, &id, 8) || id < 76561190000000000ull || id > 76561199999999999ull) continue;
        LobbyRow *r = &out[n++];
        r->x = (float)x; r->y = (float)y; r->w = (float)w; r->h = (float)h; r->root_w = (float)rw; r->root_h = (float)rh;
        uint64_t item = (uintptr_t)names[i];
        for (int up = 0; up < 3 && item; up++) item = pq(item + 0xf0);
        int32_t lx, ly, lw, lh;
        r->line_bottom = item && place((void *)(uintptr_t)item, 1, &lx, &ly, &lw, &lh, &rw, &rh) ? (float)(ly + lh) : (float)(y + h);
        snprintf(r->id, sizeof r->id, "%llu", (unsigned long long)id);
    }
    return n;
}

static int vt_slot(uint32_t vt, int slot, uint32_t want) {
    uint64_t v = 0;
    return peek(g_base + vt + 8 * slot, &v, 8) && v == (uintptr_t)(g_base + want);
}

// The game has a Ping column for its list of games, filled by its own eSteamLobbyPingService, behind a variable
// g_enableLobbyPings (a byte at 1471500, set to 0 by its registration at 1c2e0 when the game's statics start).
// The list page makes the ping service when it opens (2e01b6), its header gets "mp/lobby/header/ping" (2e438e)
// and each line its session's ping (2e5b9a, the session item's +0x850; 999 while not measured). Turned on once
// the game has started, after its statics, and before the player gets to the page.
#define G_LOBBY_PINGS 0x1471500
#define FN_LOBBY_PINGS_REG 0x1c32d             // mov byte ptr [1471500], 0
// The service (eSteamLobbyPingService, 43b780) pings every lobby on one repeating timer of 2500 ms, through
// Steam P2P; the first packets to a host go while Steam still sets up the P2P session and get no answer, and each
// try waits for the next tick: the column filled in slowly (2026-11-02). The timer's period, the "mov r8d, 2500"
// at 43b7ae, becomes PING_PERIOD_MS: the answers come about five times sooner, a small packet per lobby each time.
#define FN_PING_PERIOD 0x43b7ae
#define PING_PERIOD_MS 500
// Each answer (or a ping given up on, as -1) goes to 43bf70: a lost one counts a miss (+0x48, five in a row show
// the "no answer" icon, -2) and, as the game has it, went into the average too. With pings sent before Steam had
// the P2P session up, those -1s came back later among the answers and pulled the average under 0: the column
// went icon, number, icon (2026-11-02). Now a lost ping only counts its miss: at 43bfac, "mov eax,[rdx+48];
// inc eax" (falling into the store and the average) becomes "inc dword [rdx+48]; jmp 43bfdd" (the five check).
#define FN_PING_MISS 0x43bfac
// Off for now (2026-11-02): the column stayed unreliable to use; the game keeps its own (no column, no service).
#define LOBBY_PINGS 0

void pvp_lobby_pings(void) {
    static const uint8_t REG[7] = {0xc6, 0x05, 0xcc, 0x51, 0x45, 0x01, 0x00};   // the registration, as checked
    uint8_t now[7], on = 0;
    if (!LOBBY_PINGS) { rc_log("pvp: the Ping column is off in this build"); return; }
    if (!g_base || !peek(g_base + FN_LOBBY_PINGS_REG, now, 7) || memcmp(now, REG, 7) || !peek(g_base + G_LOBBY_PINGS, &on, 1) || on > 1) {
        rc_log("pvp: the game's lobby pings are not where expected, Ping column off");
        return;
    }
    g_base[G_LOBBY_PINGS] = 1;
    rc_log("pvp: the game's lobby pings on (Ping column in the list of games)");
}

// The list of games' filter by game type (Battle Zones, Domination...). The game keeps it as a profile option,
// mp/lobby_search_game_type, read into a new eLobbyFilter (+0x20, std::string) each time the page is made
// (slot 10, 2dfd70; slot 13 destroys it) and into the filter window's options (eSessionSearchOptionsServer, at
// popup+0x140, +0x10 std::string). The other filter options come back from settings.set; this one does not. So
// what the player applies (options slot 2, 468730) is also kept in goh_online.ini [lobby] game_type, and put back
// into a new page's filter and a new filter window. "Reset" and "all" apply an empty text, which clears it.
#define VT_SEARCH_OPTS 0x104ea98
#define FN_SEARCH_APPLY 0x468730
#define VT_SEARCH_POPUP 0x104eac0
#define FN_POPUP_BUILD 0x303cc0
#define FN_PAGE_CREATE 0x2dfd70
#define FN_STR_ASSIGN 0xa50b0           // std::string::assign(this, const char *, size)
#define PAGE_FILTER 0x140
#define FILTER_GAME_TYPE 0x20
#define POPUP_OPTS 0x140
#define OPTS_GAME_TYPE 0x10
#define GAME_TYPE_MAX 128

static char g_filter_ini[MAX_PATH], g_game_type[GAME_TYPE_MAX];
static uint64_t (*g_search_apply)(void *opts, void *a, void *b);
static uint64_t (*g_popup_build)(void *popup);
static uint64_t (*g_page_create)(void *page);

// a std::string of the game: the text in place up to 15 characters, else behind a pointer
static int game_str(uint64_t s, char *out, size_t n) {
    uint64_t size = pq(s + 0x10), cap = pq(s + 0x18);
    if (size >= n) return 0;
    if (!peek(cap > 15 ? (void *)pq(s) : (void *)s, out, size)) return 0;
    out[size] = 0;
    return 1;
}

static void put_game_type(uint64_t s, const char *what) {
    char now[GAME_TYPE_MAX];
    if (!g_game_type[0] || !game_str(s, now, sizeof now) || !strcmp(now, g_game_type)) return;
    ((void *(*)(void *, const char *, size_t))(g_base + FN_STR_ASSIGN))((void *)s, g_game_type, strlen(g_game_type));
    rc_log("pvp: lobby filter: %s had game type \"%s\", \"%s\" put back", what, now, g_game_type);
}

static uint64_t search_apply(void *opts, void *a, void *b) {
    uint64_t r = g_search_apply(opts, a, b);
    char now[GAME_TYPE_MAX];
    if (!game_str((uintptr_t)opts + OPTS_GAME_TYPE, now, sizeof now)) return r;
    rc_log("pvp: lobby filter applied: game type \"%s\"", now);
    snprintf(g_game_type, sizeof g_game_type, "%s", now);
    WritePrivateProfileStringA("lobby", "game_type", now, g_filter_ini);
    return r;
}

static uint64_t popup_build(void *popup) {
    put_game_type((uintptr_t)popup + POPUP_OPTS + OPTS_GAME_TYPE, "the filter window");
    return g_popup_build(popup);
}

static uint64_t page_create(void *page) {
    uint64_t r = g_page_create(page);
    uint64_t filter = pq((uintptr_t)page + PAGE_FILTER);
    if (filter) put_game_type(filter + FILTER_GAME_TYPE, "the list's filter");
    return r;
}

static void filter_install(const char *dir, void (*patch)(void *, const void *, size_t)) {
    if (!vt_slot(VT_SEARCH_OPTS, 2, FN_SEARCH_APPLY) || !vt_slot(VT_SEARCH_POPUP, 26, FN_POPUP_BUILD) ||
        !vt_slot(PAGES[0].vt, 10, FN_PAGE_CREATE)) {
        rc_log("pvp: the lobby filter is not as expected, its game type is not kept");
        return;
    }
    snprintf(g_filter_ini, sizeof g_filter_ini, "%sgoh_online.ini", dir);
    GetPrivateProfileStringA("lobby", "game_type", "", g_game_type, sizeof g_game_type, g_filter_ini);
    g_search_apply = (uint64_t (*)(void *, void *, void *))(g_base + FN_SEARCH_APPLY);
    g_popup_build = (uint64_t (*)(void *))(g_base + FN_POPUP_BUILD);
    g_page_create = (uint64_t (*)(void *))(g_base + FN_PAGE_CREATE);
    void *apply = (void *)search_apply, *build = (void *)popup_build, *create = (void *)page_create;
    patch(g_base + VT_SEARCH_OPTS + 8 * 2, &apply, 8);
    patch(g_base + VT_SEARCH_POPUP + 8 * 26, &build, 8);
    patch(g_base + PAGES[0].vt + 8 * 10, &create, 8);
    rc_log("pvp: lobby filter: kept game type \"%s\"", g_game_type);
}

int pvp_install(uint8_t *exe, const char *dir, uint8_t *(*alloc)(size_t), void (*patch)(void *, const void *, size_t)) {
    g_base = exe;
    snprintf(g_pak, sizeof g_pak, "%s..\\..\\resource\\interface\\interface.pak", dir);
    snprintf(g_sound_pak, sizeof g_sound_pak, "%s..\\..\\resource\\sound\\sound.pak", dir);
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
    static const uint8_t PERIOD[6] = {0x41, 0xb8, 0xc4, 0x09, 0x00, 0x00};       // mov r8d, 2500
    if (!LOBBY_PINGS)
        ;
    else if (!memcmp(exe + FN_PING_PERIOD, PERIOD, 6)) {
        uint32_t ms = PING_PERIOD_MS;
        patch(exe + FN_PING_PERIOD + 2, &ms, 4);
    } else
        rc_log("pvp: the lobby ping timer is not as expected, left as it is");
    static const uint8_t MISS[8] = {0x8b, 0x42, 0x48, 0xff, 0xc0, 0x89, 0x42, 0x48}, MISS_NEW[5] = {0xff, 0x42, 0x48, 0xeb, 0x2c};
    if (!LOBBY_PINGS)
        ;
    else if (!memcmp(exe + FN_PING_MISS, MISS, 8) && exe[FN_PING_MISS + 0x31] == 0x83 && exe[FN_PING_MISS + 0x34] == 0x05)
        patch(exe + FN_PING_MISS, MISS_NEW, 5);
    else
        rc_log("pvp: the lobby ping average is not as expected, left as it is");
    lb_apply_install(patch);
    filter_install(dir, patch);
    camo_install(exe, alloc, patch);
    day_install(exe, patch, 0);
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

// The chat panel's parts, each frame: the shown channel view under it (its top is where the tabs end) and the
// shown dialogs in the strip above that (the tabs and the "+" button at the right end).
typedef struct { uint64_t d[64]; int n; } Dialogs;

static void panel_parts(uint64_t d, int depth, uint64_t *channel, Dialogs *all) {
    if (!d || depth > 24) return;
    uint32_t flags = 0;
    if (!peek((void *)(d + 0x48), &flags, 4) || !(flags & 2)) return;
    uint64_t vt = pq(d);
    for (size_t i = 0; i < sizeof VT_CHANNEL_VIEWS / sizeof VT_CHANNEL_VIEWS[0]; i++)
        if (vt == (uintptr_t)(g_base + VT_CHANNEL_VIEWS[i])) { if (!*channel) *channel = d; return; }
    if (depth && all->n < 64) all->d[all->n++] = d;
    uint64_t b = pq(d + 0xd8), e = pq(d + 0xe0);
    if (!b || e < b || e - b > 8 * 512) return;
    for (uint64_t p = b; p < e; p += 8) panel_parts(pq(p), depth + 1, channel, all);
}

int chat_panel(ChatPanel *out) {
    scan_names();
    void *v = g_panel;
    uint64_t vt = 0, channel = 0;
    int32_t px, py, pw, ph, rw, rh, cx, cy, cw, ch;
    if (!v || !peek(v, &vt, 8) || vt != (uintptr_t)(g_base + VT_CHAT_VIEW) || covered(v) ||
        !place(v, 1, &px, &py, &pw, &ph, &rw, &rh) || root_of((uintptr_t)v) != pq((uintptr_t)(g_base + G_DESKTOP)) ||
        pw <= 0 || ph <= 0 || rw <= 0 || rh <= 0 || pw > rw / 2)
        return 0;
    static Dialogs all;
    all.n = 0;
    panel_parts((uintptr_t)v, 0, &channel, &all);
    if (!channel || !place((void *)channel, 1, &cx, &cy, &cw, &ch, &rw, &rh) || cy <= py || cy - py > ph / 3) return 0;
    out->x = (float)px; out->y = (float)py; out->w = (float)pw; out->h = (float)ph;
    out->body_y = (float)cy; out->root_w = (float)rw; out->root_h = (float)rh;
    out->tabs_r = (float)px;
    out->ntabs = 0;
    for (int i = 0; i < all.n; i++) {       // the tabs: in the strip, narrow, not the "+" at the right end
        int32_t x, y, w, h;
        if (!place((void *)all.d[i], 1, &x, &y, &w, &h, &rw, &rh) || w <= 0 || h <= 0 || y < py || y + h > cy + 1 ||
            w > pw / 2 || x + w > px + pw - 40)
            continue;
        if (x + w > out->tabs_r) out->tabs_r = (float)(x + w);
        if (out->ntabs < 8) {
            float *t = out->tab[out->ntabs++];
            t[0] = (float)x; t[1] = (float)y; t[2] = (float)w; t[3] = (float)h;
        }
    }
    static int logged;
    if (!logged++) {
        rc_log("pvp: chat panel at %d,%d %dx%d, channel from y %d, tabs end at x %.0f (%d parts in the strip)", px, py, pw, ph, cy,
               out->tabs_r, out->ntabs);
        for (int i = 0; i < out->ntabs; i++) rc_log("pvp:   tab part %.0f,%.0f %.0fx%.0f", out->tab[i][0], out->tab[i][1], out->tab[i][2], out->tab[i][3]);
    }
    return out->tabs_r > px;
}
