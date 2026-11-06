// Competitive balance: while the switch is on, a few balance files the game reads come out as our version.
// Nothing on disk changes, so the lobby's resource check (CRCs taken at startup) is not touched. Our
// versions are the game's own files with a few same-length edits (tools/build.py), so a file is swapped
// in the stream the game just opened, after checking it holds exactly the game's file (size and CRC).
// The match roster is read when the host starts the game ("Game: Create game", mp_game.cpp d06a00),
// not in the lobby, so the switch counts for the next match or replay started (re/NOTES.md).
// A hosted match also runs in a server process the host's game starts (call_to_arms_server.exe, the same
// engine built apart): it reads the files itself and runs the match with the players, so the DLL in it
// follows the switch of the game on the same PC (2026-11-01: with only the players switched, the server
// spawned the mortar for the NbW they bought, its crew could not board, the match fell apart).
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "goh_replay.h"
#include "balance_data.h"

// Where things are in each exe (the server's found with re/twin.py: each function matched whole)
typedef struct {
    const char *exe;
    // eIORepository open(path object, mode) -> stream or NULL; every file the game reads comes through it
    // (the SDL reader with mode 1, the path as written in an include, e.g. "late/units_ger.set")
    uint32_t repo_open;
    uint32_t vt_zip_stream;     // a packed entry: the whole file unpacked into a buffer of its own
    uint32_t vt_buffer_stream;  // a stored entry: a view of the pak's memory, never written
    uint32_t vt_units;          // eUnits@Multiplayer, a roster
    uint32_t roster_global;     // the match's roster, read again at "Create game"
    uint32_t users_begin;       // users: eUserInfo at user+0x58, its roster at +0x30, read once at start
} Layout;
static const Layout LAYOUTS[] = {
    {"call_to_arms.exe", 0x663c30, 0x1060678, 0x10c6258, 0x10315d8, 0x148dd38, 0x156e250},
    {"call_to_arms_server.exe", 0x47a170, 0xd6d208, 0xdc8c98, 0xdd2160, 0x10f9538, 0x11b8730},
};
static const Layout *L = &LAYOUTS[0];
// stream: +8 buffer, +0x20 file offset of the buffer, +0x28 end of data, +0x30 read position; a buffer
// stream also keeps +0x10 end and +0x18 size (its constructor 6796c0)
static const uint8_t REPO_OPEN_INS[10] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08};

// the game's switch, for the server it starts: named memory both processes open
#define SHARE_NAME  "Local\\goh_balance_switch"
#define SHARE_MAGIC 0x31424f47u   // "GOB1"
typedef struct { uint32_t magic; char tag[12]; volatile long on; DWORD pid; } Share;
static Share *g_share;

#define NFILES (sizeof BALANCE_FILES / sizeof BALANCE_FILES[0])

volatile long g_balance_on, g_balance_ready, g_balance_locked, g_balance_reads, g_balance_last;
// players can only share a match with the same tag: tools/build.py makes it from this file and our copies
#ifndef BALANCE_TAG
#define BALANCE_TAG "?"
#endif
const char g_balance_tag[] = BALANCE_TAG;

typedef void *(*open_fn)(void *path, uint32_t mode);
static open_fn g_open;                       // the game's open, through our copy of its first 10 bytes
static uint8_t *g_base;
static char g_dir_copy[MAX_PATH];
static uint8_t *g_heap_copy[NFILES];         // our version for a buffer stream to point at

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

// a std::string as the game lays it out: 16 bytes of text or a pointer, size, capacity
static int str_view(const void *s, const char **text, size_t *len) {
    const uint8_t *p = s;
    uint64_t size = *(const uint64_t *)(p + 0x10), cap = *(const uint64_t *)(p + 0x18);
    if (cap < 15 || size > cap || size > 4096) return 0;
    *text = cap > 15 ? *(const char *const *)p : (const char *)p;
    *len = size;
    return *text != NULL;
}

// the path as the game asked for it (relative to an including file, or whole) is the tail of ours
static int names_file(const char *s, size_t n, const char *ours) {
    while (n && (*s == '/' || *s == '\\')) s++, n--;
    size_t k = strlen(ours);
    if (!n || n > k || _strnicmp(ours + k - n, s, n)) return 0;
    for (size_t i = 0; i < n; i++) if (s[i] == '\\') return 0;
    return n == k || ours[k - n - 1] == '/';
}

// the units' answers (pathing.c) in the server's files: in a player's game pvp.c does this. A file as the game is about to read it
static void tuned_file(const char *s, size_t n, uint8_t *st) {
    static const char *const FILES[][2] = {{"properties/human.ext", "human.ext"}, {"properties/mobility.inc", "mobility.inc"},
                                           {"properties/human_fsm/request.inc", "request.inc"}, {"properties/human_fsm/switch.inc", "switch.inc"}};
    for (int i = 0; i < 4; i++) {
        if (!names_file(s, n, FILES[i][0])) continue;
        uintptr_t vt = *(uintptr_t *)st - (uintptr_t)g_base;
        uint8_t *b = *(uint8_t **)(st + 8), *e = *(uint8_t **)(st + 0x28);
        if (!b || e <= b || *(uint64_t *)(st + 0x20) || *(uint8_t **)(st + 0x30) != b) { rc_log("pathing: %s: the stream is not as expected", FILES[i][1]); return; }
        size_t size = (size_t)(e - b);
        size_t cap = size;
        if (vt == L->vt_buffer_stream) {
            cap = size + pathing_more();
            uint8_t *c = malloc(cap);                     // lives as long as the game may read it
            if (!c) return;
            memcpy(c, b, size);
            b = c;
        } else if (vt != L->vt_zip_stream) { rc_log("pathing: %s: a stream of kind %llx", FILES[i][1], (unsigned long long)vt); return; }
        size = pathing_edit(FILES[i][1], b, size, cap);
        if (vt == L->vt_buffer_stream) {
            *(uint8_t **)(st + 8) = b; *(uint8_t **)(st + 0x10) = b + size; *(uint64_t *)(st + 0x18) = size;
            *(uint8_t **)(st + 0x30) = b; *(uint8_t **)(st + 0x28) = b + size;
        }
        return;
    }
}

static void *open_hook(void *path, uint32_t mode) {
    void *r = g_open(path, mode);
    const char *s;
    size_t n;
    if (!r || !str_view(path, &s, &n)) return r;
    if (L == &LAYOUTS[1] && pathing_wants()) tuned_file(s, n, r);
    for (size_t i = 0; i < NFILES; i++) {
        if (!names_file(s, n, BALANCE_FILES[i].path)) continue;
        const BalanceFile *f = &BALANCE_FILES[i];
        uint8_t *st = r;
        uintptr_t vt = *(uintptr_t *)st - (uintptr_t)g_base;
        uint8_t *b = *(uint8_t **)(st + 8), *e = *(uint8_t **)(st + 0x28);
        uint64_t at = *(uint64_t *)(st + 0x20);
        int whole = b && e - b == (ptrdiff_t)f->size && at == 0 && *(uint8_t **)(st + 0x30) == b;
        int game = whole && crc32(b, f->size) == f->crc;
        int on = g_balance_on && g_balance_ready;
        const char *what = "its own";
        if (on && game && vt == L->vt_zip_stream) {
            memcpy(b, f->data, f->size);
            what = "ours";
        } else if (on && game && vt == L->vt_buffer_stream && g_heap_copy[i]) {
            uint8_t *c = g_heap_copy[i];
            *(uint8_t **)(st + 8) = c; *(uint8_t **)(st + 0x10) = c + f->size; *(uint64_t *)(st + 0x18) = f->size;
            *(uint8_t **)(st + 0x30) = c; *(uint8_t **)(st + 0x28) = c + f->size;
            what = "ours";
        } else if (on) {
            what = !whole ? "its own (stream not as expected)" : !game ? "its own (not the game's file)" : "its own (unknown stream)";
        }
        InterlockedIncrement(&g_balance_reads);
        g_balance_last = what[0] == 'o';
        rc_log("balance: the game opens %.*s (mode %x, stream %llx): %s", (int)n, s, mode, (unsigned long long)vt, what);
        break;
    }
    return r;
}

// crc and size of a file inside a pak (a zip), from its central directory: no unpacking needed
static int pak_entry(const char *pak, const char *name, unsigned *crc, unsigned *size) {
    FILE *f = fopen(pak, "rb");
    if (!f) return 0;
    int ok = 0;
    static uint8_t tail[65557];
    _fseeki64(f, 0, SEEK_END);
    long long len = _ftelli64(f), from = len > (long long)sizeof tail ? len - (long long)sizeof tail : 0;
    _fseeki64(f, from, SEEK_SET);
    size_t got = fread(tail, 1, sizeof tail, f);
    for (long long i = (long long)got - 22; i >= 0 && !ok; i--) {
        if (memcmp(tail + i, "PK\x05\x06", 4)) continue;
        uint32_t cd_size, cd_off;
        memcpy(&cd_size, tail + i + 12, 4);
        memcpy(&cd_off, tail + i + 16, 4);
        uint8_t *cd = malloc(cd_size);
        if (!cd) break;
        _fseeki64(f, cd_off, SEEK_SET);
        if (fread(cd, 1, cd_size, f) == cd_size) {
            size_t want = strlen(name);
            for (uint32_t p = 0; p + 46 <= cd_size && !memcmp(cd + p, "PK\x01\x02", 4);) {
                uint16_t nl, xl, cl;
                memcpy(&nl, cd + p + 28, 2); memcpy(&xl, cd + p + 30, 2); memcpy(&cl, cd + p + 32, 2);
                if (nl == want && p + 46 + nl <= cd_size && !memcmp(cd + p + 46, name, nl)) {
                    memcpy(crc, cd + p + 16, 4);
                    memcpy(size, cd + p + 24, 4);
                    ok = 1;
                    break;
                }
                p += 46 + nl + xl + cl;
            }
        }
        free(cd);
        break;
    }
    fclose(f);
    return ok;
}

static DWORD WINAPI server_follow(void *arg);

int balance_install(uint8_t *exe, const char *dir, uint8_t *(*alloc)(size_t), void (*patch)(void *, const void *, size_t),
                    int server) {
    L = &LAYOUTS[server ? 1 : 0];
    if (memcmp(exe + L->repo_open, REPO_OPEN_INS, 10)) {
        rc_log("balance: unexpected code in %s, the switch is off", L->exe);
        return 0;
    }
    // the game's files must be the ones our versions were made from
    for (size_t i = 0; i < NFILES; i++) {
        char pak[MAX_PATH];
        unsigned crc = 0, size = 0;
        snprintf(pak, sizeof pak, "%s..\\..\\%s", dir, BALANCE_FILES[i].pak);
        if (!pak_entry(pak, BALANCE_FILES[i].path, &crc, &size) || crc != BALANCE_FILES[i].crc || size != BALANCE_FILES[i].size) {
            rc_log("balance: %s is not the file our version was made from (crc %08x), the switch is off", BALANCE_FILES[i].path, crc);
            return 0;
        }
        g_heap_copy[i] = malloc(BALANCE_FILES[i].size);
        if (g_heap_copy[i]) memcpy(g_heap_copy[i], BALANCE_FILES[i].data, BALANCE_FILES[i].size);
    }
    g_base = exe;
    snprintf(g_dir_copy, sizeof g_dir_copy, "%s", dir);
    // the game's first 10 bytes, then on into it; the entry jumps to our hook
    uint8_t *t = alloc(32), *s = alloc(16);
    if (!t || !s) return 0;
    memcpy(t, REPO_OPEN_INS, 10);
    t[10] = 0xff; t[11] = 0x25; memset(t + 12, 0, 4);                  // jmp [rip+0]
    uint64_t back = (uint64_t)(uintptr_t)(exe + L->repo_open + 10); memcpy(t + 16, &back, 8);
    g_open = (open_fn)t;
    s[0] = 0x48; s[1] = 0xb8;                                           // mov rax, open_hook ; jmp rax
    uint64_t fn = (uint64_t)(uintptr_t)open_hook; memcpy(s + 2, &fn, 8);
    s[10] = 0xff; s[11] = 0xe0;
    uint8_t jmp[10] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    int32_t rel = (int32_t)(s - (exe + L->repo_open + 5)); memcpy(jmp + 1, &rel, 4);
    patch(exe + L->repo_open, jmp, 10);
    g_balance_ready = 1;
    rc_log("balance: ready in %s, %d file(s), version %s", L->exe, (int)NFILES, g_balance_tag);
    if (server) {
        CloseHandle(CreateThread(NULL, 0, server_follow, NULL, 0, NULL));
    } else {
        // the game publishes its switch (balance_tick) for the server it may start
        HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(Share), SHARE_NAME);
        Share *sh = h ? MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Share)) : NULL;
        if (sh) {
            snprintf(sh->tag, sizeof sh->tag, "%s", g_balance_tag);
            sh->pid = GetCurrentProcessId();
            sh->on = g_balance_on;
            sh->magic = SHARE_MAGIC;
            g_share = sh;
        } else {
            rc_log("balance: cannot share the switch (error %lu): a match hosted here would desync with it on", GetLastError());
        }
    }
    return 1;
}

// ---- what the game holds: the two purchases in each roster copy, for the log ---------------------------
// A roster (eUnits@Multiplayer) keeps at +0x10 a pointer to a list: +8 begin, +0x10 end of unit
// descriptor pointers. Copies: the global one (Layout roster_global, read again at "Create game") and one
// in every profile (Layout users_begin), read once at start.
static int peek(const void *p, void *out, size_t n) {
    SIZE_T got = 0;
    return p && ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}
static uint64_t peekq(uint64_t a) { uint64_t v = 0; peek((void *)a, &v, 8); return v; }

// a std::string at p holding text exactly
static int str_is(uint64_t p, const char *text) {
    uint64_t size = peekq(p + 0x10), cap = peekq(p + 0x18);
    size_t n = strlen(text);
    if (size != n || cap < 15 || cap > 4096) return 0;
    char buf[80];
    if (n >= sizeof buf || !peek((void *)(cap > 15 ? peekq(p) : p), buf, n)) return 0;
    return !memcmp(buf, text, n);
}

static void dump_roster(const char *what, uint64_t roster) {
    static const char *ids[2] = {"doctrine_120mm_sgrw42_late", "doctrine_150mm_nebelwerfer41_late"};
    uint64_t list = peekq(roster + 0x10), b = peekq(list + 8), e = peekq(list + 0x10);
    if (!list || !b || e < b || (e - b) / 8 > 100000) { rc_log("balance dump: %s roster %llx has no list", what, (unsigned long long)roster); return; }
    int found = 0;
    for (uint64_t i = 0; i < (e - b) / 8 && found < 2; i++) {
        uint64_t d = peekq(b + 8 * i);
        uint8_t raw[0x520];
        if (!peek((void *)d, raw, sizeof raw)) continue;
        for (int k = 0; k < 2; k++) {
            int name_at = -1;
            for (int o = 0; o + 0x20 <= (int)sizeof raw && name_at < 0; o += 8) if (str_is(d + o, ids[k])) name_at = o;
            if (name_at < 0) continue;
            // what may tell the tier: group strings and the delay (300/900/1800) anywhere in the descriptor
            char info[400]; int len = 0;
            for (int o = 0; o + 0x20 <= (int)sizeof raw; o += 8) {
                if (str_is(d + o, "doctrine_t1") || str_is(d + o, "doctrine_t2") || str_is(d + o, "doctrine_t3")) {
                    char g[16] = {0}; peek((void *)(peekq(d + o + 0x18) > 15 ? peekq(d + o) : d + o), g, 11);
                    len += snprintf(info + len, sizeof info - len, " %s@%x", g, o);
                }
            }
            for (int o = 0; o + 4 <= (int)sizeof raw && len < 360; o += 4) {
                int32_t v; memcpy(&v, raw + o, 4);
                float f; memcpy(&f, raw + o, 4);
                if (v == 300 || v == 900 || v == 1800 || v == 480 || f == 300.f || f == 900.f || f == 1800.f || f == 480.f)
                    len += snprintf(info + len, sizeof info - len, " %s%g@%x", (v == 300 || v == 900 || v == 1800 || v == 480) ? "" : "f", (v == 300 || v == 900 || v == 1800 || v == 480) ? (double)v : (double)f, o);
            }
            info[len] = 0;
            rc_log("balance dump: %s #%llu of %llu %s at %llx:%s", what, (unsigned long long)i, (unsigned long long)((e - b) / 8), ids[k],
                   (unsigned long long)d, info);
            found++;
        }
    }
    if (!found) rc_log("balance dump: %s roster %llx (%llu units) has neither purchase", what, (unsigned long long)roster, (unsigned long long)((e - b) / 8));
}

void balance_dump(const char *why) {
    if (!g_base) return;
    rc_log("balance dump (%s), switch %s", why, g_balance_on ? "on" : "off");
    dump_roster("global", (uint64_t)(g_base + L->roster_global));
    uint64_t ub = peekq((uint64_t)(g_base + L->users_begin)), ue = peekq((uint64_t)(g_base + L->users_begin + 8));
    for (uint64_t u = ub; u && u < ue && u - ub < 64 * 8; u += 8) {
        uint64_t info = peekq(peekq(u) + 0x58);
        if (info) dump_roster("profile", info + 0x30);
    }
}

static void fix_all(int on);

// worker thread, every 50 ms: a second after the switch moves or the game reads a file, every roster in
// memory is put to the switch and the two purchases are logged
DWORD g_balance_on_after_ms;               // test ([debug] balance_on_after): the switch turns itself on then

void balance_tick(void) {
    static long seen_on = -1, seen_reads = -1;
    static DWORD due, start;
    if (!start) start = GetTickCount();
    if (g_balance_on_after_ms && GetTickCount() - start >= g_balance_on_after_ms) {
        g_balance_on_after_ms = 0;
        g_balance_on = 1;
        rc_log("balance: test, switch turned on");
    }
    if (g_share && L == &LAYOUTS[0]) g_share->on = g_balance_on;   // the game's switch, for its server
    if (g_balance_on != seen_on || g_balance_reads != seen_reads) {
        seen_on = g_balance_on; seen_reads = g_balance_reads;
        due = GetTickCount() + 1000;
    }
    if (due && (LONG)(GetTickCount() - due) >= 0) {
        due = 0;
        if (g_balance_ready) fix_all(g_balance_on != 0);
        balance_dump(g_balance_reads ? "after a read or the switch" : "start");
    }
}

// the server's thread: its switch is the one of the game on this PC (the host that started it)
static DWORD WINAPI server_follow(void *arg) {
    (void)arg;
    for (int tick = 0;; tick++, Sleep(50)) {
        if (!g_share && tick % 20 == 0) {
            HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, SHARE_NAME);
            Share *sh = h ? MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(Share)) : NULL;
            if (h) CloseHandle(h);                  // the view keeps it
            static int told;
            if (sh && sh->magic == SHARE_MAGIC && !strcmp(sh->tag, g_balance_tag)) {
                g_share = sh;
                rc_log("balance: following the switch of the game on this PC (pid %lu)", sh->pid);
            } else if (sh) {
                if (!told++) rc_log("balance: the game on this PC has balance version %s, this one %s: vanilla", sh->tag, g_balance_tag);
                UnmapViewOfFile(sh);
            } else if (!told++) {
                rc_log("balance: no game on this PC shares its switch (yet): vanilla");
            }
        }
        if (g_share && g_share->on != g_balance_on) {
            g_balance_on = g_share->on;
            rc_log("balance: the game turned the switch %s", g_balance_on ? "on" : "off");
        }
        balance_tick();
    }
    return 0;
}

// ---- the switch reaches the copies already in memory -----------------------------------------------------
// A roster read before the switch moved keeps the old values. Every copy is put right in place: the game's
// copies are found by their vtable (eUnits@Multiplayer) in its heap, plus the global one. In a unit
// descriptor only three things follow the tier (vanilla vs on, compared 2026-11-01): +0x3e0 charge delay and
// interval (int32 each), +0x3e8 hash of the group name, +0x3f0 the name ("doctrine_tN", N at +0x3fa).
// +0x50c (16 bits) is the purchase's number, its place in the list + 1: purchases are found by it and by name,
// both by walking the list (68b9f0, d34428), so two purchases trade places when their pointers and numbers do.
#define D_DELAY         0x3e0
#define D_HASH          0x3e8
#define D_TIER_CHAR     0x3fa
static const uint32_t GROUP_HASH[4] = {0, 0, 0xc5dd4ecfu, 0x71d5da44u};   // doctrine_t2, doctrine_t3
typedef struct { const char *id; int tier_off, delay_off, cool_off, tier_on, delay_on, cool_on; } UnitTier;
static const UnitTier TIERS[] = {
    {"doctrine_120mm_sgrw42_late", 2, 900, 300, 3, 1800, 480},
    {"doctrine_150mm_nebelwerfer41_late", 3, 1800, 480, 2, 900, 300},
};
#define NTIERS (sizeof TIERS / sizeof TIERS[0])
#define D_NUMBER        0x50c
// pairs that trade places with the switch on (TIERS indices, first is first in vanilla)
static const int SWAPS[][2] = {{0, 1}};
#define NSWAPS (sizeof SWAPS / sizeof SWAPS[0])

// a descriptor names one of TIERS: its id string is in its first bytes
static int unit_of(uint64_t d) {
    for (int o = 0; o < 0x80; o += 8)
        for (size_t k = 0; k < NTIERS; k++) if (str_is(d + o, TIERS[k].id)) return (int)k;
    return -1;
}

static void poke(void *p, const void *v, size_t n) { SIZE_T put = 0; WriteProcessMemory(GetCurrentProcess(), p, v, n, &put); }

// 1 changed, 0 already right, -1 neither state (left alone)
static int set_tier(uint64_t d, const UnitTier *t, int on) {
    int32_t dc[2];
    uint32_t hash;
    char tc;
    if (!peek((void *)(d + D_DELAY), dc, 8) || !peek((void *)(d + D_HASH), &hash, 4) || !peek((void *)(d + D_TIER_CHAR), &tc, 1)) return -1;
    int want_t = on ? t->tier_on : t->tier_off, other_t = on ? t->tier_off : t->tier_on;
    int32_t want[2] = {on ? t->delay_on : t->delay_off, on ? t->cool_on : t->cool_off};
    int32_t other[2] = {on ? t->delay_off : t->delay_on, on ? t->cool_off : t->cool_on};
    if (dc[0] == want[0] && dc[1] == want[1] && hash == GROUP_HASH[want_t] && tc == '0' + want_t) return 0;
    if (dc[0] != other[0] || dc[1] != other[1] || hash != GROUP_HASH[other_t] || tc != '0' + other_t) return -1;
    char c = (char)('0' + want_t);
    poke((void *)(d + D_DELAY), want, 8);
    poke((void *)(d + D_HASH), &GROUP_HASH[want_t], 4);
    poke((void *)(d + D_TIER_CHAR), &c, 1);
    return 1;
}

static int g_moved;

static void fix_roster(uint64_t roster, int on, int *changed, int *right, int *odd) {
    uint64_t list = peekq(roster + 0x10), b = peekq(list + 8), e = peekq(list + 0x10);
    if (!list || !b || e < b || (e - b) / 8 > 100000) return;
    int64_t at[NTIERS];
    for (size_t k = 0; k < NTIERS; k++) at[k] = -1;
    for (uint64_t i = 0; i < (e - b) / 8; i++) {
        uint64_t d = peekq(b + 8 * i);
        int k = d ? unit_of(d) : -1;
        if (k < 0) continue;
        at[k] = (int64_t)i;
        int r = set_tier(d, &TIERS[k], on);
        if (r > 0) (*changed)++; else if (r == 0) (*right)++; else (*odd)++;
    }
    // places: with the switch on the second of a pair comes first; pointers and numbers trade together
    for (size_t s = 0; s < NSWAPS; s++) {
        int64_t p = at[SWAPS[s][0]], q = at[SWAPS[s][1]];
        if (p < 0 || q < 0 || (on ? q < p : p < q)) continue;
        uint64_t lo = p < q ? (uint64_t)p : (uint64_t)q, hi = p < q ? (uint64_t)q : (uint64_t)p;
        uint64_t dl = peekq(b + 8 * lo), dh = peekq(b + 8 * hi);
        uint16_t nl = 0, nh = 0;
        peek((void *)(dl + D_NUMBER), &nl, 2);
        peek((void *)(dh + D_NUMBER), &nh, 2);
        if (nl != lo + 1 || nh != hi + 1) { (*odd)++; continue; }   // numbers not as the game gives them: leave it
        poke((void *)(b + 8 * lo), &dh, 8);
        poke((void *)(b + 8 * hi), &dl, 8);
        poke((void *)(dh + D_NUMBER), &nl, 2);
        poke((void *)(dl + D_NUMBER), &nh, 2);
        g_moved++;
    }
}

// every roster the game holds: the global one and each eUnits object in its heap
static void fix_all(int on) {
    int rosters = 0, changed = 0, right = 0, odd = 0;
    g_moved = 0;
    uint64_t vt = (uint64_t)(g_base + L->vt_units);
    fix_roster((uint64_t)(g_base + L->roster_global), on, &changed, &right, &odd);
    rosters++;
    MEMORY_BASIC_INFORMATION mi;
    static uint64_t chunk[1 << 16];
    for (uint8_t *a = (uint8_t *)0x10000; VirtualQuery(a, &mi, sizeof mi); a = (uint8_t *)mi.BaseAddress + mi.RegionSize) {
        if (mi.State != MEM_COMMIT || mi.Type != MEM_PRIVATE || mi.Protect != PAGE_READWRITE) continue;
        for (size_t off = 0; off < mi.RegionSize; off += sizeof chunk) {
            size_t n = mi.RegionSize - off < sizeof chunk ? mi.RegionSize - off : sizeof chunk;
            uint8_t *p = (uint8_t *)mi.BaseAddress + off;
            if (!peek(p, chunk, n)) continue;
            for (size_t i = 0; i < n / 8; i++)
                if (chunk[i] == vt && (uint8_t *)&chunk[i] != (uint8_t *)chunk) {
                    fix_roster((uint64_t)(p + 8 * i), on, &changed, &right, &odd);
                    rosters++;
                }
        }
    }
    rc_log("balance: %s in memory: %d rosters, %d purchases changed, %d already right, %d left alone (unexpected values), "
           "%d pairs moved", on ? "competitive" : "vanilla", rosters, changed, right, odd, g_moved);
}
