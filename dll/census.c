// Memory census, for finding where the game keeps a match's players and result (re/NOTES.md "Rated matches").
// Ctrl+F12 in the game, and by itself when the results page opens if goh_snapshots.ini [debug] census=1:
// writes goh_census.log next to the DLL with
//   - every SteamID in the game's memory (as a 64-bit number and as decimal text), and the game object
//     around it: the nearest vtable before it with the game's RTTI, its class and the SteamID's offset;
//   - the object graph of the match (the eGame the stats events come with) and of the results page: each
//     field pointing at an object of a known class, and vectors of them, two levels down.
// Only reads (ReadProcessMemory: a bad pointer costs nothing), on a thread of ours, the game runs on.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "goh_replay.h"

static uint8_t *g_base;
static size_t g_image;
static FILE *g_cf;
static char g_path[MAX_PATH];
static volatile long g_busy;
static char g_why[64];
static uintptr_t g_game, g_page;

static void clog(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(g_cf, fmt, a);
    va_end(a);
    fputc('\n', g_cf);
}

static int peek(uintptr_t p, void *out, size_t n) {
    SIZE_T got = 0;
    return p && ReadProcessMemory(GetCurrentProcess(), (void *)p, out, n, &got) && got == n;
}
static uint64_t pq(uintptr_t p) { uint64_t v = 0; peek(p, &v, 8); return v; }

static int in_exe(uint64_t p) { return p >= (uintptr_t)g_base && p < (uintptr_t)g_base + g_image; }

// the class of a vtable from its RTTI (complete object locator before it), "" if none
static const char *rtti(uint64_t vt, char *buf, size_t n) {
    buf[0] = 0;
    if (!in_exe(vt)) return buf;
    uint64_t col = pq(vt - 8);
    uint32_t sig = 1, td = 0;
    if (!in_exe(col) || !peek(col, &sig, 4) || sig != 1 || !peek(col + 12, &td, 4) || td >= g_image) return buf;
    char name[128] = "";
    if (!peek((uintptr_t)g_base + td + 16, name, sizeof name - 1)) return buf;
    name[sizeof name - 1] = 0;
    if (strncmp(name, ".?AV", 4) && strncmp(name, ".?AU", 4)) return buf;
    char *end = strstr(name, "@@");
    if (end) *end = 0;
    snprintf(buf, n, "%s", name + 4);
    return buf;
}

static const char *cls(uint64_t obj, char *buf, size_t n) { return rtti(pq(obj), buf, n); }

// the object a field sits in: the nearest qword before it that is a vtable with RTTI
static void owner(uintptr_t at, char *out, size_t n) {
    char c[128];
    for (int back = 0; back <= 0x1000; back += 8) {
        uint64_t q = pq(at - back);
        if (rtti(q, c, sizeof c)[0]) { snprintf(out, n, "%s @%llx +0x%x", c, (unsigned long long)(at - back), back); return; }
    }
    snprintf(out, n, "(no class within 0x1000)");
}

// text for the log; runs of 16+ hex digits become '#': the player's key sits in memory (the heartbeat) and
// must never reach a log someone may send around
static void printable(const uint8_t *p, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) out[i] = p[i] >= 32 && p[i] < 127 ? (char)p[i] : '.';
    out[n] = 0;
    for (size_t i = 0; i < n;) {
        size_t k = i;
        while (k < n && strchr("0123456789abcdefABCDEF", out[k]) && out[k]) k++;
        if (k - i >= 16 && !(k - i == 17 && !memcmp(out + i, "7656119", 7))) memset(out + i, '#', k - i);
        i = k > i ? k : i + 1;
    }
}

static void scan_steam_ids(void) {
    static uint8_t buf[1 << 20];
    MEMORY_BASIC_INFORMATION mi;
    int bin = 0, txt = 0;
    for (uintptr_t a = 0x10000; VirtualQuery((void *)a, &mi, sizeof mi) == sizeof mi; a = (uintptr_t)mi.BaseAddress + mi.RegionSize) {
        if (mi.State != MEM_COMMIT || !(mi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) || (mi.Protect & PAGE_GUARD)) continue;
        for (uintptr_t p = (uintptr_t)mi.BaseAddress; p < (uintptr_t)mi.BaseAddress + mi.RegionSize; p += sizeof buf) {
            size_t n = (uintptr_t)mi.BaseAddress + mi.RegionSize - p;
            if (n > sizeof buf) n = sizeof buf;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (void *)p, buf, n, &got)) continue;
            for (size_t i = 0; i + 8 <= got && bin < 400; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if ((v >> 32) != 0x01100001u || (uint32_t)v == 0) continue;
                char o[200];
                owner(p + i, o, sizeof o);
                if (strstr(o, "no class")) continue;            // Steam's own memory, not the game's
                clog("id  %llu at %llx in %s", (unsigned long long)v, (unsigned long long)(p + i), o);
                bin++;
            }
            for (size_t i = 0; i + 17 <= got && txt < 200; i++) {
                if (memcmp(buf + i, "7656119", 7)) continue;
                int digits = 0;
                while (i + digits < got && buf[i + digits] >= '0' && buf[i + digits] <= '9') digits++;
                if (digits != 17) continue;
                char o[200], ctx[97];
                size_t from = i >= 40 ? i - 40 : 0;
                printable(buf + from, got - from < 96 ? got - from : 96, ctx);
                if (i >= 8 && !memcmp(buf + i - 8, "STEAMID=", 8)) { i += 16; continue; }   // environment blocks
                owner(p + i, o, sizeof o);
                clog("txt at %llx [%s] in %s", (unsigned long long)(p + i), ctx, o);
                txt++;
                i += 16;
            }
        }
    }
    clog("steam ids: %d as numbers in game objects, %d as text", bin, txt);
}

// fields of an object that point at objects of known classes, or are vectors of them
static void graph(uintptr_t obj, size_t size, int depth, const char *indent) {
    char c[128], c2[128];
    for (size_t off = 0; off < size; off += 8) {
        uint64_t q = pq(obj + off);
        if (!q || in_exe(q)) continue;
        if (cls(q, c, sizeof c)[0]) {
            clog("%s+0x%zx -> %s @%llx", indent, off, c, (unsigned long long)q);
            if (depth > 1) {
                char deeper[16];
                snprintf(deeper, sizeof deeper, "%s    ", indent);
                graph(q, 0x300, depth - 1, deeper);
            }
            continue;
        }
        uint64_t e = pq(obj + off + 8), cap = pq(obj + off + 16);
        if (q < e && e <= cap && e - q < 0x100000 && !((e - q) & 7)) {
            uint64_t first = pq(q);
            if (cls(first, c2, sizeof c2)[0]) clog("%s+0x%zx -> vector of %llu x %s*", indent, off, (unsigned long long)((e - q) / 8), c2);
            else {
                uint8_t head[48];
                char pr[49];
                if (peek(q, head, sizeof head)) { printable(head, sizeof head, pr); clog("%s+0x%zx -> vector %llu bytes [%s]", indent, off, (unsigned long long)(e - q), pr); }
            }
            off += 16;
        }
    }
}

static DWORD WINAPI census_thread(LPVOID unused) {
    g_cf = fopen(g_path, "a");
    if (!g_cf) { g_busy = 0; return 0; }
    SYSTEMTIME t;
    GetLocalTime(&t);
    clog("==== census %02d:%02d:%02d (%s), my SteamID %s", t.wHour, t.wMinute, t.wSecond, g_why, online_my_id());
    char c[128];
    if (g_game) { clog("-- match object %llx: %s", (unsigned long long)g_game, cls(g_game, c, sizeof c)); graph(g_game, 0x1000, 2, "  "); }
    if (g_page) { clog("-- results page %llx: %s", (unsigned long long)g_page, cls(g_page, c, sizeof c)); graph(g_page, 0x800, 2, "  "); }
    scan_steam_ids();
    clog("==== done");
    fclose(g_cf);
    rc_log("census written (%s)", g_why);
    g_busy = 0;
    return 0;
}

void census_run(const char *why, uintptr_t game, uintptr_t page) {
    if (!g_base || InterlockedExchange(&g_busy, 1)) return;
    snprintf(g_why, sizeof g_why, "%s", why);
    g_game = game;
    g_page = page;
    HANDLE h = CreateThread(NULL, 0, census_thread, NULL, 0, NULL);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); }
    else g_busy = 0;
}

static DWORD WINAPI census_timer(LPVOID secs) {
    Sleep((DWORD)(uintptr_t)secs * 1000);
    census_run("timer", g_census_game, g_census_page);
    return 0;
}

// after_s > 0 ([debug] census_after): one census that many seconds after the start, for tests without a keyboard
void census_init(uint8_t *exe, const char *dir, int after_s) {
    g_base = exe;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    g_image = nt->OptionalHeader.SizeOfImage;
    snprintf(g_path, sizeof g_path, "%sgoh_census.log", dir);
    if (after_s > 0) CloseHandle(CreateThread(NULL, 0, census_timer, (LPVOID)(uintptr_t)after_s, 0, NULL));
}
