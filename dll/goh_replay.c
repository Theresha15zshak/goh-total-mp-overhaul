// Replay control for Gates of Hell 1.067 (call_to_arms.exe).
// Installed as binaries\x64\iphlpapi.dll: the game loads it from its own folder and every
// iphlpapi export is passed on to the real System32 DLL (proxy_iphlpapi.c).
// Here: speed, pause, game clock, replay length and seek; ui.cpp draws the in-game panel.
// Addresses and how they were found: re/NOTES.md. Every patch site is checked byte for byte
// first; on another game build nothing is touched.
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "goh_replay.h"
#include "camo.h"

#define STBI_NO_STDIO
#define STBI_ONLY_PNG          // keeps the zlib decoder, which is all we use
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"

BOOL proxy_init(void);

// ---- game addresses (RVA, 1.067) --------------------------------------------------------------
#define SITE_RATIO        0xc6c14c   // movss xmm8, [rip+disp] ; pacing floor 1.0
#define GAME_TIME_MS      0x1556e48
#define UNPAUSE_DELAY     0x1556db8
#define MP_SINGLETON      0x15a4eb0
#define SESSION_CLIENT    0x48
#define REPLAY_SERVER     0xe0
#define SERVER_CORE       0xb0
#define VT_REPLAY_CLIENT  0x113e3f8
#define VT_REPLAY_SERVER  0x1038b40
#define VT_CORE           0x113d488
#define CORE_PAUSED       0x21
#define CORE_NEXT_QUANT   0x28
#define CORE_SENT_TIME    0x40       // int: game ms where the last batch the server sent begins
#define CORE_SENT_DELTA   0x44       // int: game ms per quant
#define CORE_SENT_AMOUNT  0x48       // int: quants in that batch
#define REPLAY_MANAGER    0x153e2b0
#define GAME_MODE_OBJ     0x159c760   // [..]+0x908 byte: 1 from "Enter game mode" (a69130) to "Exit game mode" (a69000)
#define GAME_MODE_ON      0x908
#define IAT_QPC           0xfed168
#define IMAGE_END         0x1707000
// The client runs one quant per frame (mp.thread_game.exec 53aca0), so frames set the seek pace, not the
// server: it is let loose and held once it has sent the target, so the client stops right there.
#define SEEK_SPEED        50.0f
#define SEEK_SPEED_SAFE   8.0f       // when the server cannot be held: the old pace, more would overshoot

static const uint8_t RATIO_INS[9] = {0xf3, 0x44, 0x0f, 0x10, 0x05, 0x7b, 0x43, 0x39, 0x00};

// functions we watch: (rcx, rdx) go to watch_hit before the game's code runs.
// The stolen prologue (len bytes, whole instructions) must be position independent.
typedef struct { const char *name; uint32_t rva; uint8_t len, prologue[8]; } Watch;
static const Watch WATCH[] = {
    {"menu_command", 0xbd4990, 5, {0x48, 0x89, 0x5c, 0x24, 0x10}},  // in-game menu buttons
    {"replay_load",  0x2637e0, 5, {0x48, 0x89, 0x5c, 0x24, 0x18}},  // Replays list "Load"
    {"server_tick",  0xc6b2e0, 5, {0x48, 0x89, 0x5c, 0x24, 0x10}},  // quant server
    {"exit_game",    0xa69000, 5, {0x48, 0x89, 0x5c, 0x24, 0x08}},  // "Exit game mode"
    {"spawn",        0xd2e2a0, 5, {0x4c, 0x89, 0x44, 0x24, 0x18}},  // (combat stats, &{unit, player}): purchase bookkeeping
    {"page_show",    0x343930, 5, {0x44, 0x89, 0x44, 0x24, 0x18}},  // PageManager::Show(mgr, page, mods)
    {"page_close",   0x344180, 5, {0x48, 0x89, 0x5c, 0x24, 0x08}},  // PageManager::Close(mgr, &page)
    {"stats_event",  0xd24380, 5, {0x48, 0x89, 0x5c, 0x24, 0x08}},  // eCompetitiveGame/eGuerrillaGame::HandleStatsEvent(game, ev)
    {"stats_event2", 0xd13ba0, 7, {0x40, 0x55, 0x53, 0x41, 0x56, 0x41, 0x57}},  // the same for frontline, battle zones and the rest
    {"present",      0x4fc010, 6, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20}},  // eRenderDevice present: swapchain [+0xa8]->Present
};
enum { W_LOAD = 1, W_TICK = 2, W_SPAWN = 4, W_PAGE_SHOW = 5, W_PAGE_CLOSE = 6, W_STATS_EVENT = 7, W_STATS_EVENT2 = 8, W_PRESENT = 9 };
#define NWATCH (sizeof(WATCH) / sizeof(WATCH[0]))

Ctl g_ctl;
static uint8_t *g_base;
static uint8_t *g_page;         // RWX page near the image: ratio cell + hook stubs
static size_t g_page_used;
static float *g_cell;           // pacing ratio the patched instruction reads
static int g_ratio_patched;
static float g_speed = 1.0f;
static int g_seek_to = -1;
static FILE *g_log;
static char g_dir[MAX_PATH];    // folder of this DLL (the game's binaries\x64), ends with a backslash
static CRITICAL_SECTION g_log_cs;

void rc_log(const char *fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_log_cs);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_log_cs);
}

void rc_send(int cmd, double arg) {
    g_ctl.cmd = cmd;
    g_ctl.arg = arg;
    MemoryBarrier();
    InterlockedIncrement(&g_ctl.cmd_seq);
}

static void ctl_msg(const char *m) { lstrcpynA(g_ctl.msg, m, sizeof g_ctl.msg); }

// ---- safe access (the game frees objects while we look at them; no SEH on this toolchain) -------
static int rd(const void *p, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}
static int wr(uintptr_t a, const void *src, size_t n) {
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), (void *)a, src, n, &put) && put == n;
}
static uintptr_t rdq(uintptr_t a) { uintptr_t v = 0; return a && rd((void *)a, &v, 8) ? v : 0; }

static uintptr_t replay_core(void) {
    uintptr_t s = (uintptr_t)g_base + MP_SINGLETON;
    uintptr_t sc = rdq(s + SESSION_CLIENT);
    if (!sc || rdq(sc) != (uintptr_t)g_base + VT_REPLAY_CLIENT) return 0;
    uintptr_t srv = rdq(s + REPLAY_SERVER);
    if (!srv || rdq(srv) != (uintptr_t)g_base + VT_REPLAY_SERVER) return 0;
    uintptr_t core = rdq(srv + SERVER_CORE);
    return core && rdq(core) == (uintptr_t)g_base + VT_CORE ? core : 0;
}

static int game_ms(void) { int v = 0; rd(g_base + GAME_TIME_MS, &v, 4); return v; }

static void replay_path(char *out, size_t n) {
    out[0] = 0;
    uintptr_t sess = rdq((uintptr_t)g_base + REPLAY_MANAGER + 8);
    if (!sess) return;
    uint64_t size = 0, cap = 0;
    if (!rd((void *)(sess + 0x18), &size, 8) || !rd((void *)(sess + 0x20), &cap, 8) || size >= n) return;
    uintptr_t data = cap > 15 ? rdq(sess + 8) : sess + 8;
    if (data && rd((void *)data, out, size)) out[size] = 0;
}

// ---- replay length: gameEndTime - gameStartTime from replay.ss inside the .rep (a zip) ---------
static int zip_entry(const uint8_t *z, size_t n, const char *name, char **out, int *out_len) {
    // walk the central directory from the end-of-central-directory record
    for (size_t e = n >= 22 ? n - 22 : 0; e > 0 && e + 22 <= n; e--) {
        if (memcmp(z + e, "PK\5\6", 4)) continue;
        uint16_t count = *(uint16_t *)(z + e + 10);
        size_t c = *(uint32_t *)(z + e + 16);
        for (int i = 0; i < count && c + 46 <= n; i++) {
            uint16_t method = *(uint16_t *)(z + c + 10), nl = *(uint16_t *)(z + c + 28);
            uint16_t xl = *(uint16_t *)(z + c + 30), cl = *(uint16_t *)(z + c + 32);
            uint32_t csize = *(uint32_t *)(z + c + 20), usize = *(uint32_t *)(z + c + 24);
            uint32_t lh = *(uint32_t *)(z + c + 42);
            if (nl == strlen(name) && !memcmp(z + c + 46, name, nl) && lh + 30 <= n) {
                size_t data = lh + 30 + *(uint16_t *)(z + lh + 26) + *(uint16_t *)(z + lh + 28);
                if (data + csize > n || usize > (64u << 20)) return 0;
                *out = malloc(usize + 1);
                if (method == 0) { memcpy(*out, z + data, usize); *out_len = (int)usize; }
                else *out_len = stbi_zlib_decode_noheader_buffer(*out, (int)usize, (const char *)z + data, (int)csize);
                if (*out_len < 0) { free(*out); return 0; }
                (*out)[*out_len] = 0;
                return 1;
            }
            c += 46 + nl + xl + cl;
        }
        return 0;
    }
    return 0;
}

static long long ss_field(const char *ss, const char *key) {
    const char *p = strstr(ss, key);
    return p ? _atoi64(p + strlen(key)) : 0;
}

// Fills length and game start (unix time) of a replay; 0 when unknown.
static void replay_meta(const char *path, int *duration_s, long long *start) {
    *duration_s = 0; *start = 0;
    // the engine keeps it profile-relative ("//replays/x.rep"); look in every profile
    while (*path == '/' || *path == '\\') path++;
    wchar_t docs[MAX_PATH], rel[260], pattern[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docs))) return;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, rel, 260);
    swprintf(pattern, MAX_PATH, L"%ls\\my games\\gates of hell\\profiles\\*", docs);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    int found = 0;
    while (f != INVALID_HANDLE_VALUE && !found) {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
            wchar_t file[MAX_PATH];
            swprintf(file, MAX_PATH, L"%ls\\my games\\gates of hell\\profiles\\%ls\\%ls", docs, fd.cFileName, rel);
            HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD size = GetFileSize(h, NULL), got = 0;
                uint8_t *z = size < (256u << 20) ? malloc(size) : NULL;
                char *ss = NULL; int len = 0;
                if (z && ReadFile(h, z, size, &got, NULL) && got == size && zip_entry(z, size, "replay.ss", &ss, &len)) {
                    long long a = ss_field(ss, "{gameStartTime "), b = ss_field(ss, "{gameEndTime ");
                    if (b > a && a > 0) *duration_s = (int)(b - a);
                    *start = a;
                    found = 1;
                    free(ss);
                }
                free(z);
                CloseHandle(h);
            }
        }
        if (!FindNextFileW(f, &fd)) break;
    }
    if (f != INVALID_HANDLE_VALUE) FindClose(f);
}

// ---- code patching ----------------------------------------------------------------------------
static void patch(void *dst, const void *src, size_t n) {
    DWORD old;
    VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
}

static uint8_t *page_alloc(size_t n) {
    if (g_page_used + n > 0x1000) return NULL;
    uint8_t *p = g_page + g_page_used;
    g_page_used = (g_page_used + n + 15) & ~(size_t)15;
    return p;
}

static void set_ratio_patch(int on) {
    if (on == g_ratio_patched) return;
    uint8_t *site = g_base + SITE_RATIO;
    int32_t disp = on ? (int32_t)((uint8_t *)g_cell - (site + 9)) : *(int32_t *)(RATIO_INS + 5);
    patch(site + 5, &disp, 4);
    g_ratio_patched = on;
}

// While seeking, the ratio the server writes into each quant request is forced to ours. Left alone it is
// recomputed from the client's reported speed, kept at least 0.5 above the floor and lowered by 0.02 per
// quant (c6c0a0), so seeks ran x4-5 with the frame loop idle 9 frames out of 10. The ratio only spaces
// quants in real time; their game time (delta) is untouched.
#define SITE_RATIO_STORE1 0xc6c387   // movss [rdi+0Ch], xmm8 (end of c6c0a0) -> mov al,1 at c6c38d
#define SITE_RATIO_STORE2 0xc6b5db   // movss [r14+0Ch], xmm0 (adaptive branch of c6b2e0) -> c6b5e1
static const uint8_t RATIO_STORE1_INS[6] = {0xf3, 0x44, 0x0f, 0x11, 0x47, 0x0c};
static const uint8_t RATIO_STORE2_INS[6] = {0xf3, 0x41, 0x0f, 0x11, 0x46, 0x0c};
static volatile uint8_t *g_force_ratio;      // in the page next to the game: read by the stubs

static void put_rel32(uint8_t *at, const void *target) { int32_t d = (int32_t)((const uint8_t *)target - (at + 4)); memcpy(at, &d, 4); }

// stub: if forcing, load our ratio into the register first; then the original store; then back
static int force_ratio_stub(uint32_t site, const uint8_t ins[6], int xmm8) {
    uint8_t *target = g_base + site;
    if (memcmp(target, ins, 6)) return 0;
    uint8_t *s = page_alloc(64), *p = s;
    if (!s) return 0;
    *p++ = 0x80; *p++ = 0x3d; uint8_t *d1 = p; p += 4; *p++ = 0x00;     // cmp byte [rip+force], 0
    uint8_t load_len = xmm8 ? 9 : 8;
    *p++ = 0x74; *p++ = load_len;                                       // je over the load
    put_rel32(d1, (const void *)g_force_ratio);                         // (relative to the end of the cmp: patched below)
    int32_t fix; memcpy(&fix, d1, 4); fix -= 1; memcpy(d1, &fix, 4);    // the cmp ends one byte after its disp32
    if (xmm8) { *p++ = 0xf3; *p++ = 0x44; *p++ = 0x0f; *p++ = 0x10; *p++ = 0x05; }   // movss xmm8, [rip+cell]
    else { *p++ = 0xf3; *p++ = 0x0f; *p++ = 0x10; *p++ = 0x05; }                     // movss xmm0, [rip+cell]
    put_rel32(p, g_cell); p += 4;
    memcpy(p, ins, 6); p += 6;                                          // the original store
    *p++ = 0xff; *p++ = 0x25; memset(p, 0, 4); p += 4;                  // jmp [rip+0] back
    uint64_t back = (uint64_t)(uintptr_t)(target + 6); memcpy(p, &back, 8);
    uint8_t jmp[6] = {0xe9, 0, 0, 0, 0, 0x90};
    int32_t rel = (int32_t)(s - (target + 5)); memcpy(jmp + 1, &rel, 4);   // from the site, not from this buffer
    patch(target, jmp, 6);
    return 1;
}

static void force_ratio(int on) { if (g_force_ratio) *g_force_ratio = (uint8_t)on; }

static void apply_speed(float s) {
    if (s < 0.1f) s = 0.1f;
    if (s > 50.f) s = 50.f;
    *(volatile float *)g_cell = 1.0f / s;
    set_ratio_patch(s != 1.0f);
}

static void set_paused(uintptr_t core, int on) {
    uint8_t flag = (uint8_t)on;
    if (!on) {
        // the engine's own unpause: next quant due shortly, no catch-up burst
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        int64_t next = now.QuadPart + *(int64_t *)(g_base + UNPAUSE_DELAY);
        wr(core + CORE_NEXT_QUANT, &next, 8);
    }
    wr(core + CORE_PAUSED, &flag, 1);
}

// ---- diagnostics: which threads run what --------------------------------------------------------
typedef struct { volatile LONG tid, count; } TidCount;
static TidCount g_qpc[64];
static volatile LONG g_watch_count[NWATCH], g_watch_tid[NWATCH];
static BOOL (WINAPI *g_real_qpc)(LARGE_INTEGER *);
static int g_qpc_hooked;            // the game's timer calls are counted

static void count_tid(TidCount *t, size_t n, LONG tid) {
    for (size_t i = 0; i < n; i++) {
        if (t[i].tid == 0) InterlockedCompareExchange(&t[i].tid, tid, 0);   // claim a free slot
        if (t[i].tid == tid) { InterlockedIncrement(&t[i].count); return; }
    }
}

static BOOL WINAPI qpc_hook(LARGE_INTEGER *out) {
    count_tid(g_qpc, 64, (LONG)GetCurrentThreadId());
    return g_real_qpc(out);
}

static void *g_load_dialog, *g_load_item;   // last Replays "Load": (eGUI_ReplaysDialog*, eReplayInfo*)
static volatile LONG g_load_fresh;          // a Load just happened; its path is not known yet

// ---- match stats sources (re/NOTES.md "Match charts") -------------------------------------------
// Stats object = rcx of the spawn function: std::vector of per-player records.
#define REC_SIZE        0x1b0
#define REC_ARMY        0x118       // float: army size in limit points (+ on purchase, - on losses)
#define REC_ECON        0x70        // std::vector of economies, one per resource (money, doctrine points)
#define ECON_SIZE       0x508
#define ECON_BUDGET     0x40c       // float: points in hand
#define ECON_INCOME     0x430       // float: everything earned ("totalAdd")
#define ECON_SPENT      0x434       // float: everything spent ("totalSpent")
#define ACTOR_PLAYER    0xda8       // byte: owner of a unit
#define EV_ACTOR        0x10        // stats event: the unit it is about (the victim of a kill)
#define EV_KILLER_PLAYER 0x38       // byte: player of the killer descriptor at ev+0x28 (0xff = none)
#define EV_KILLED       3
#define MAX_PLAYER_BYTE 0x12        // the game's own bound for player bytes
static const uint32_t VT_PAGE_GAME_RESULT[] = {0x10403c0, 0x1040490, 0x10404a0, 0x10404b8};   // eGUI_PageGameResult and its bases

MatchStats g_ms;
volatile long g_ms_results;
volatile uintptr_t g_census_game, g_census_page;
int g_census_auto;
volatile long g_live_match;
static volatile uintptr_t g_stats;          // stats object of the current match
static volatile uintptr_t g_result_page;    // eGUI_PageGameResult while it is on screen
static volatile LONG g_kills[2][MAX_PLAYER_BYTE], g_deaths[2][MAX_PLAYER_BYTE];   // [0 infantry, 1 vehicles] by actor player byte
static volatile LONG g_player_rec[MAX_PLAYER_BYTE];   // actor player byte -> record player number + 1, learned at spawns
static volatile LONG g_match_seq;           // bumped by the first purchase of a match
static volatile DWORD g_battle_tick;        // GetTickCount of the last purchase or stats event: the simulation runs

// snapshot test: what the battle did in the first run past the snapshot and in the run after the restore -
// every stats event (dozens a second: kills, hits, captures...) and every purchase, in order
typedef struct { int ms, type; uint8_t a, b; } TestEv;
#define TEST_EVENTS 65536
#define EV_PURCHASE 1000
static TestEv g_test_ev[2][TEST_EVENTS];
static volatile LONG g_test_nev[2];
static volatile LONG g_test_run = -1;       // which run records now

static void test_record(int type, uint8_t a, uint8_t b) {
    LONG run = g_test_run;
    if (run < 0) return;
    LONG i = InterlockedIncrement(&g_test_nev[run]) - 1;
    if (i < TEST_EVENTS) { TestEv *e = &g_test_ev[run][i]; e->ms = game_ms(); e->type = type; e->a = a; e->b = b; }
}

static void spawn_seen(uintptr_t stats, uintptr_t req) {
    static int logged, last_ms;
    int now = game_ms();
    // a new match: another stats object, or the same one again with the clock back at the start (replay reloaded)
    if (stats != g_stats || now < last_ms - 5000) {
        rc_log("match stats %p", (void *)stats);
        for (int i = 0; i < MAX_PLAYER_BYTE; i++) g_kills[0][i] = g_kills[1][i] = g_deaths[0][i] = g_deaths[1][i] = g_player_rec[i] = 0;
        g_stats = stats;
        InterlockedIncrement(&g_match_seq);
        logged = 0;
    }
    last_ms = now;
    uint8_t rec = 0, pb = 0xff;
    uintptr_t unit = rdq(req);
    if (!rd((void *)(req + 8), &rec, 1) || !unit || !rd((void *)(unit + ACTOR_PLAYER), &pb, 1)) return;
    test_record(EV_PURCHASE, rec, pb);
    if (pb < MAX_PLAYER_BYTE) g_player_rec[pb] = rec + 1;
    if (logged++ < 6) rc_log("spawn: player#%d, unit owner byte %d", rec, pb);
}

// infantry or not: the victim's class from its RTTI (eHuman), remembered per vtable
static int is_human(uintptr_t actor) {
    static uintptr_t vts[32];
    static int kinds[32], n;
    uintptr_t vt = rdq(actor);
    for (int i = 0; i < n; i++) if (vts[i] == vt) return kinds[i];
    uintptr_t col = rdq(vt - 8);
    uint32_t td = 0;
    char name[24] = "";
    if (col && rd((void *)(col + 12), &td, 4) && td < 0x2000000) rd(g_base + td + 16, name, sizeof name - 1);
    int human = !strncmp(name, ".?AVeHuman@", 11);
    if (n < 32) { vts[n] = vt; kinds[n++] = human; }
    return human;
}

static void stats_event(uintptr_t ev) {
    static int logged;
    int32_t type = 0;
    uintptr_t victim = rdq(ev + EV_ACTOR);
    if (!rd((void *)ev, &type, 4)) return;
    uint8_t vp = 0xff, kp = 0xff;
    if (victim) rd((void *)(victim + ACTOR_PLAYER), &vp, 1);
    rd((void *)(ev + EV_KILLER_PLAYER), &kp, 1);
    test_record(type, vp, type == EV_KILLED ? kp : 0);   // other events may leave that byte unset
    if (type != EV_KILLED || !victim) return;
    music_kill();
    int veh = !is_human(victim);
    if (vp < MAX_PLAYER_BYTE) InterlockedIncrement(&g_deaths[veh][vp]);
    if (kp < MAX_PLAYER_BYTE && kp != vp) InterlockedIncrement(&g_kills[veh][kp]);   // no killer: the game puts the victim's owner there
    if (logged++ < 6) rc_log("kill: owner byte %d killed by %d", vp, kp);
}

static int is_result_page(uintptr_t page) {
    uintptr_t vt = rdq(page) - (uintptr_t)g_base;
    for (size_t i = 0; i < sizeof VT_PAGE_GAME_RESULT / sizeof VT_PAGE_GAME_RESULT[0]; i++)
        if (vt == VT_PAGE_GAME_RESULT[i]) return 1;
    return 0;
}

static int g_ui_on;              // our panel draws into the game's frames

static void __cdecl watch_hit(uint32_t id, void *a1, void *a2) {
    g_watch_tid[id] = (LONG)GetCurrentThreadId();
    LONG n = InterlockedIncrement(&g_watch_count[id]);
    if (id == W_LOAD) { g_load_dialog = a1; g_load_item = a2; g_load_fresh = 1; }
    if (id == W_PRESENT) { if (g_ui_on) ui_present(a1); return; }
    if (id == W_SPAWN) { g_battle_tick = GetTickCount(); spawn_seen((uintptr_t)a1, (uintptr_t)a2); return; }
    if (id == W_STATS_EVENT || id == W_STATS_EVENT2) {
        g_battle_tick = GetTickCount();
        g_census_game = (uintptr_t)a1;
        stats_event((uintptr_t)a2);
        return;
    }
    if (id == W_PAGE_SHOW) {
        uintptr_t page = (uintptr_t)a2;
        rc_log("page shown %p, vtable rva %llx", a2, (unsigned long long)(rdq(page) - (uintptr_t)g_base));
        pvp_page_shown(a2);
        camo_page(a2);
        if (is_result_page(page)) {
            g_result_page = page; g_ms_results = 1; g_census_page = page;
            rc_log("results page shown");
            if (g_census_auto) census_run("results page", g_census_game, page);
        }
        return;
    }
    if (id == W_PAGE_CLOSE) {
        uintptr_t p = (uintptr_t)a2;
        if (g_result_page && (p == g_result_page || rdq(p) == g_result_page)) {
            g_result_page = 0; g_ms_results = 0; g_census_page = 0;
            rc_log("results page closed");
        }
        return;
    }
    if (n > 2 && id == W_TICK) return;                              // the tick is too chatty to log every call
    rc_log("hit %s (%p, %p)", WATCH[id].name, a1, a2);
    if (id == 1)
        rc_log("  load: dialog vtable rva %llx, item vtable rva %llx",
               (unsigned long long)(rdq((uintptr_t)a1) - (uintptr_t)g_base), (unsigned long long)(rdq((uintptr_t)a2) - (uintptr_t)g_base));
    if (n <= 2) {
        // call chain as game RVAs, to find the loop above it. Our stubs have no unwind data, so
        // scan the raw stack for values that point right after a call instruction in the game code.
        uintptr_t lo = (uintptr_t)g_base + 0x1000, hi = (uintptr_t)g_base + 0xfec000;
        volatile uintptr_t marker = 0;
        const uintptr_t *sp = (const uintptr_t *)&marker;
        char line[1200]; int len = 0, found = 0;
        for (int i = 0; i < 0x1000 && found < 40; i++) {
            uintptr_t q = 0;
            if (!rd(sp + i, &q, 8)) break;
            if (q < lo + 6 || q >= hi) continue;
            const uint8_t *c = (const uint8_t *)q;
            int call = c[-5] == 0xe8 ||                                   // call rel32
                       (c[-6] == 0xff && (c[-5] & 0x38) == 0x10) ||       // call [rip/reg+disp32]
                       (c[-3] == 0xff && (c[-2] & 0x38) == 0x10) ||       // call [reg+disp8]
                       (c[-2] == 0xff && (c[-1] & 0x38) == 0x10);         // call reg / [reg]
            if (!call) continue;
            len += snprintf(line + len, sizeof line - len, " %llx", (unsigned long long)(q - (uintptr_t)g_base));
            found++;
        }
        line[len] = 0;
        rc_log("  stack%s", line);
    }
}

// stub: save args, call watch_hit(id), restore, run the stolen prologue, jump back
static int detour(uint32_t id) {
    uint8_t *target = g_base + WATCH[id].rva;
    int len = WATCH[id].len;
    if (memcmp(target, WATCH[id].prologue, len)) { rc_log("skip %s: unexpected code", WATCH[id].name); return 0; }
    uint8_t *s = page_alloc(128), *p = s;
    if (!s) return 0;
    static const uint8_t pre[] = {
        0x51, 0x52, 0x41, 0x50, 0x41, 0x51,                         // push rcx, rdx, r8, r9
        0x48, 0x83, 0xec, 0x68,                                     // sub rsp, 0x68 (keeps 16-byte alignment)
        0x0f, 0x11, 0x44, 0x24, 0x20, 0x0f, 0x11, 0x4c, 0x24, 0x30, // movups [rsp+20], xmm0 / [rsp+30], xmm1
        0x0f, 0x11, 0x54, 0x24, 0x40, 0x0f, 0x11, 0x5c, 0x24, 0x50, // movups [rsp+40], xmm2 / [rsp+50], xmm3
        0x49, 0x89, 0xd0,                                           // mov r8, rdx   (2nd arg of the game function)
        0x48, 0x89, 0xca,                                           // mov rdx, rcx  (1st arg)
        0xb9};                                                      // mov ecx, imm32
    memcpy(p, pre, sizeof pre); p += sizeof pre;
    memcpy(p, &id, 4); p += 4;
    *p++ = 0x48; *p++ = 0xb8;                                       // mov rax, imm64
    uint64_t fn = (uint64_t)(uintptr_t)watch_hit; memcpy(p, &fn, 8); p += 8;
    static const uint8_t post[] = {
        0xff, 0xd0,                                                 // call rax
        0x0f, 0x10, 0x44, 0x24, 0x20, 0x0f, 0x10, 0x4c, 0x24, 0x30,
        0x0f, 0x10, 0x54, 0x24, 0x40, 0x0f, 0x10, 0x5c, 0x24, 0x50,
        0x48, 0x83, 0xc4, 0x68,                                     // add rsp, 0x68
        0x41, 0x59, 0x41, 0x58, 0x5a, 0x59};                        // pop r9, r8, rdx, rcx
    memcpy(p, post, sizeof post); p += sizeof post;
    memcpy(p, target, len); p += len;                               // stolen prologue (position independent)
    *p++ = 0xff; *p++ = 0x25; memset(p, 0, 4); p += 4;              // jmp [rip+0]
    uint64_t back = (uint64_t)(uintptr_t)(target + len); memcpy(p, &back, 8); p += 8;
    uint8_t jmp[8] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90};         // nops pad the rest of a longer prologue
    int32_t rel = (int32_t)(s - (target + 5)); memcpy(jmp + 1, &rel, 4);
    patch(target, jmp, len);
    return 1;
}

// ---- the game's music: muted while our director plays a battle ------------------------------------
// MediaManager play(path, channel, fade, loop) -> channel or -1; stop(path, fade), empty path = all.
// path is the engine's path object: std::string + cached wide copy at +0x20.
#define FN_MEDIA_PLAY   0x4caa10
#define FN_MEDIA_STOP   0x4cabf0
#define FN_PATH_FREE    0x674210
static const uint8_t MEDIA_PLAY_INS[5] = {0x89, 0x54, 0x24, 0x10, 0x53};     // mov [rsp+10h], edx ; push rbx
static const uint8_t MEDIA_STOP_INS[5] = {0x4c, 0x8b, 0xdc, 0x41, 0x54};     // mov r11, rsp ; push r12

static int __cdecl media_play_filter(uintptr_t path) {
    static int logged;
    uint64_t size = 0, cap = 0;
    char head[8] = {0};
    if (!rd((void *)(path + 0x10), &size, 8) || !rd((void *)(path + 0x18), &cap, 8) || size < 7) return 0;
    if (!rd((void *)(cap > 15 ? rdq(path) : path), head, 7) || _strnicmp(head, "/music/", 7)) return 0;
    if (!music_block_game_music()) return 0;
    if (logged++ < 8) rc_log("game music muted (%llu chars)", (unsigned long long)size);
    return 1;
}

// function entry: ask filter(rcx); nonzero -> return -1 at once, else run the stolen 5 bytes and go on
static int hook_filter(uint32_t rva, const uint8_t ins[5], int (__cdecl *filter)(uintptr_t)) {
    uint8_t *target = g_base + rva;
    if (memcmp(target, ins, 5)) return 0;
    uint8_t *s = page_alloc(128), *p = s;
    if (!s) return 0;
    static const uint8_t pre[] = {
        0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x68,
        0x0f, 0x11, 0x44, 0x24, 0x20, 0x0f, 0x11, 0x4c, 0x24, 0x30,
        0x0f, 0x11, 0x54, 0x24, 0x40, 0x0f, 0x11, 0x5c, 0x24, 0x50,
        0x48, 0xb8};                                                // mov rax, imm64 (rcx = 1st arg already)
    memcpy(p, pre, sizeof pre); p += sizeof pre;
    uint64_t fn = (uint64_t)(uintptr_t)filter; memcpy(p, &fn, 8); p += 8;
    static const uint8_t post[] = {
        0xff, 0xd0,
        0x0f, 0x10, 0x44, 0x24, 0x20, 0x0f, 0x10, 0x4c, 0x24, 0x30,
        0x0f, 0x10, 0x54, 0x24, 0x40, 0x0f, 0x10, 0x5c, 0x24, 0x50,
        0x48, 0x83, 0xc4, 0x68, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59,
        0x85, 0xc0, 0x74, 0x06,                                     // test eax, eax ; jz original
        0xb8, 0xff, 0xff, 0xff, 0xff, 0xc3};                        // mov eax, -1 ; ret
    memcpy(p, post, sizeof post); p += sizeof post;
    memcpy(p, target, 5); p += 5;
    *p++ = 0xff; *p++ = 0x25; memset(p, 0, 4); p += 4;
    uint64_t back = (uint64_t)(uintptr_t)(target + 5); memcpy(p, &back, 8); p += 8;
    uint8_t jmp[5] = {0xe9};
    int32_t rel = (int32_t)(s - (target + 5)); memcpy(jmp + 1, &rel, 4);
    patch(target, jmp, 5);
    return 1;
}

// play() returning -1 means nothing played
static int hook_media_play(void) {
    if (memcmp(g_base + FN_MEDIA_STOP, MEDIA_STOP_INS, 5)) return 0;
    return hook_filter(FN_MEDIA_PLAY, MEDIA_PLAY_INS, media_play_filter);
}

static int g_media_ok;

// ---- drawing during a seek ------------------------------------------------------------------------
// Each frame the main loop runs a quant and, in parallel, the render job (581ca0 -> 694960 "render.scr":
// scene, the game's GUI, Present) and waits for both. The simulation never depends on drawing (lockstep),
// so while seeking only a few frames a second are drawn and the rest end sooner. Nothing may be drawn in
// place of a skipped frame: presenting from outside the game's own frame broke its render state.
// A frame is two render jobs (581ca0): mode 8, the scene (render.tgt), and mode 2, the GUI (render.gui).
// They are drawn or skipped together: letting through whichever came first after the pause drew a scene
// without its GUI pass now and then, and an object that pass sets up crashed render.tgt with a null
// vertex declaration (51ffd0 -> 6e0ec0) at the same moment of a replay twice.
#define FN_RENDER_SCREEN  0x694960
static const uint8_t RENDER_SCREEN_INS[5] = {0x48, 0x89, 0x5c, 0x24, 0x18};   // mov [rsp+18h], rbx
static volatile LONG g_turbo;               // a seek runs
static volatile LONG g_frames, g_frames_drawn;   // main loop iterations / frames drawn
static volatile LONG g_draw_frame = -1;     // the frame whose jobs are drawn while seeking

static int __cdecl render_filter(uintptr_t flags) {
    static LARGE_INTEGER last, freq;
    (void)flags;
    if (g_turbo) {
        LARGE_INTEGER now;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        LONG f = g_frames;                  // counted at the start of the frame, before its jobs
        if (f != g_draw_frame) {
            if (now.QuadPart - last.QuadPart < freq.QuadPart / 4) return 1;   // 4 frames a second show the progress
            last = now;
            g_draw_frame = f;
        }
    }
    InterlockedIncrement(&g_frames_drawn);
    return 0;
}

// logic thread: fade out whatever the game plays on its media channels
static void stop_game_music(void) {
    struct { uint64_t q[5]; } path = {{0, 0, 0, 15, 0}};             // empty path object = all channels
    ((void (*)(void *, float))(g_base + FN_MEDIA_STOP))(&path, 2.0f);
    ((void (*)(void *))(g_base + FN_PATH_FREE))(&path);
    rc_log("game music stopped");
}

// the game stopped calling the timer for a whole report: every thread's stack into goh_hang.dmp, once
static void hang_dump(void) {
    static int done;
    if (done++) return;
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    BOOL (WINAPI *write)(HANDLE, DWORD, HANDLE, int, void *, void *, void *) =
        dbg ? (void *)GetProcAddress(dbg, "MiniDumpWriteDump") : NULL;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sgoh_hang.dmp", g_dir);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    // MiniDumpNormal | WithThreadInfo | WithUnloadedModules | WithIndirectlyReferencedMemory
    BOOL ok = write && f != INVALID_HANDLE_VALUE &&
              write(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1000 | 0x20 | 0x40, NULL, NULL, NULL);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    rc_log("the game has not called the timer for 10 s: %s %s", ok ? "stacks written to" : "could not write", path);
}

static void dump_threads(void) {
    static HRESULT (WINAPI *get_desc)(HANDLE, PWSTR *);
    static int reports;
    long total = 0;
    if (!get_desc) get_desc = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    for (size_t i = 0; i < 64 && g_qpc[i].tid; i++) {
        LONG c = InterlockedExchange(&g_qpc[i].count, 0);
        char name[64] = "";
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)g_qpc[i].tid);
        PWSTR w = NULL;
        if (h && get_desc && SUCCEEDED(get_desc(h, &w)) && w) { WideCharToMultiByte(CP_UTF8, 0, w, -1, name, sizeof name, 0, 0); LocalFree(w); }
        if (h) CloseHandle(h);
        if (c) rc_log("  qpc thread %5ld %-24s %6ld calls/10s", g_qpc[i].tid, name, c);
        total += c;
    }
    if (++reports >= 2 && !total && g_qpc_hooked) hang_dump();
    for (size_t i = 0; i < NWATCH; i++)
        rc_log("  %-12s total %ld, last thread %ld", WATCH[i].name, g_watch_count[i], g_watch_tid[i]);
    uint64_t committed, used; int64_t live, blocks;
    snap_stats(&committed, &used, &live, &blocks);
    if (committed)
        rc_log("  game heap: %llu MB committed, %llu MB used, %lld MB in %lld blocks", (unsigned long long)(committed >> 20),
               (unsigned long long)(used >> 20), (long long)(live >> 20), (long long)blocks);
    long kd, ks, km, kg;
    keep_stats(&kd, &ks, &km, &kg);
    if (keep_active())
        rc_log("  kept alive: %ld Direct3D objects, %ld sound streams, %ld sound buffers; %ld newer objects let go", kd, ks, km, kg);
}

// ---- rewind: leave the replay, load it again the way the Replays list does, seek forward -------
// The engine cannot step back, it only replays orders from the start. All of this runs on the
// game's logic thread, right before its per-iteration GUI update (MAIN_ITER), where the menu buttons run.
// (The process main thread draws frames; sending GUI events from there only works after ~15 s.)
#define MAIN_ITER         0x33a2b0   // GUI update, once per logic-loop iteration (57b8b5): sub rsp,28h; call 2bec60
#define FN_SEND_EVENT     0x6532b0   // (event*, 0): synchronous GUI event dispatch
#define EV_VTABLE         0x102ebb0  // plain GUI event, slot 0 = 224700
#define EV_EXIT           0xfc2      // what the in-game menu's "Exit" sends
#define FN_LOAD           0x2637e0   // Replays list "Load"(eGUI_ReplaysDialog*, eReplayInfo*)
static const uint32_t VT_REPLAYS_DIALOG[] = {0x1038d58, 0x1038fa0, 0x1038fb0, 0x1038fc8, 0x1038ff0};
static const uint32_t VT_REPLAY_INFO[] = {0x1038d00, 0x1038d38};
static const uint8_t MAIN_ITER_INS[9] = {0x48, 0x83, 0xec, 0x28, 0xe8, 0xa7, 0x49, 0xf8, 0xff};
static const uint8_t SEND_EVENT_INS[9] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9};

static void apply_pov(void);         // player view, below
static volatile LONG g_pov_on;

enum { RW_NONE, RW_EXIT, RW_WAIT_MENU, RW_WAIT_REPLAY };
static volatile LONG g_rw;
static int g_rw_target, g_rewind_ok;
static DWORD g_rw_t;
static volatile LONG g_pending_seek = -1;
static char g_load_path[260];       // replay that g_load_item opens

// object's vtable is one of the class's (pointers may be to a base subobject)
static int is_a(void *obj, const uint32_t *vts, int n) {
    uintptr_t vt = rdq((uintptr_t)obj);
    for (int i = 0; i < n; i++) if (vt == (uintptr_t)g_base + vts[i]) return 1;
    rc_log("rewind: object %p has vtable rva %llx", obj, (unsigned long long)(vt - (uintptr_t)g_base));
    return 0;
}

static void rw_fail(const char *m) {
    g_rw = RW_NONE;
    ctl_msg(m);
    rc_log("rewind failed: %s", m);
}

// ---- snapshots in a replay (goh_snapshots.ini enabled=1) ---------------------------------------------------
// A session runs while a replay is open: snapshots every 15 s of game time (snap.c). A click on the timeline
// before the present, or past it where a snapshot lies ahead, jumps to the nearest snapshot at or before the
// target on the logic thread, and the worker fast-forwards the rest.
// test=1: at 70 s of game time, jump back to 20 s, play to 70 s again; every stats event and purchase of both
// runs must match, or the restored game is not the game that was saved.
#define NO_RESULT       (-0x7fffffff)
#define TEST_AT_MS      70000
#define TEST_BACK_MS    20000
static int g_snapshots, g_snap_test;
enum { T_WAIT, T_JUMPED, T_SECOND, T_DONE };
static int g_test_phase, g_test_from, g_test_end;
static volatile LONG g_restore_req = -1;            // worker -> logic thread: jump to this game time
static volatile LONG g_restore_done = NO_RESULT;    // logic thread -> worker: where it landed (-2 no snapshot)
static int g_seek_after_restore = -1;               // worker: fast-forward to here after the jump

static double ms_since(LARGE_INTEGER t0) {
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
    return (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}

// while the test runs: where the game faults, if a restore broke something
static volatile LONG g_fault_logs;
static LONG CALLBACK fault_log(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != 0xc0000374) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_fault_logs) > 20) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t ip = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ip, &m);
    if (m) GetModuleFileNameA(m, name, MAX_PATH);
    const char *base = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    ULONG_PTR *info = e->ExceptionRecord->ExceptionInformation;
    rc_log("fault %08lx at %s+%llx (%s %p), thread %lu, test phase %d, clock %d ms", code, base,
           (unsigned long long)(ip - (uintptr_t)m), info[0] == 1 ? "writing" : info[0] == 8 ? "executing" : "reading",
           (void *)info[1], GetCurrentThreadId(), g_test_phase, game_ms());
    return EXCEPTION_CONTINUE_SEARCH;
}

static void test_compare(void) {
    int n[2] = {0, 0}, kills[2] = {0, 0}, buys[2] = {0, 0};
    TestEv *k[2] = {g_test_ev[0], g_test_ev[1]};
    for (int r = 0; r < 2; r++) {
        int total = g_test_nev[r] < TEST_EVENTS ? g_test_nev[r] : TEST_EVENTS;
        for (int i = 0; i < total; i++)
            if (k[r][i].ms >= g_test_from && k[r][i].ms < g_test_end) {
                kills[r] += k[r][i].type == EV_KILLED;
                buys[r] += k[r][i].type == EV_PURCHASE;
                k[r][n[r]++] = k[r][i];
            }
    }
    // events of one quant may come from several threads in any order: compare quant by quant as sets
    int same = 0, i = 0, j = 0;
    while (i < n[0] && j < n[1] && k[0][i].ms == k[1][j].ms) {
        int ms = k[0][i].ms, i2 = i, j2 = j;
        while (i2 < n[0] && k[0][i2].ms == ms) i2++;
        while (j2 < n[1] && k[1][j2].ms == ms) j2++;
        if (i2 - i != j2 - j) break;
        int ok = 1;
        for (int x = i; x < i2 && ok; x++) {
            int found = 0;
            for (int y = j; y < j2 && !found; y++) found = !memcmp(&k[0][x], &k[1][y], sizeof(TestEv));
            ok = found;
        }
        if (!ok) break;
        same += i2 - i;
        i = i2; j = j2;
    }
    rc_log("snapshot test: %d events (%d kills, %d purchases) before the restore, %d (%d, %d) after; %d identical%s", n[0],
           kills[0], buys[0], n[1], kills[1], buys[1], same, same == n[0] && same == n[1] && n[0] ? " - THE RESTORED GAME PLAYS THE SAME" : "");
    if (i < n[0] || j < n[1]) {
        TestEv *a = i < n[0] ? &k[0][i] : NULL, *b = j < n[1] ? &k[1][j] : NULL;
        rc_log("snapshot test: first difference: before %d ms type %d %d/%d, after %d ms type %d %d/%d", a ? a->ms : -1, a ? a->type : -1,
               a ? a->a : -1, a ? a->b : -1, b ? b->ms : -1, b ? b->type : -1, b ? b->a : -1, b ? b->b : -1);
    }
}

static int game_mode(void);

// logic thread, every frame
static void snapshots_tick(void) {
    static int active;
    static DWORD busy_log;
    uintptr_t core = replay_core();
    if (!core) {
        if (active) { snap_session_end(); active = 0; }
        return;
    }
    if (!active) {
        if (!game_mode() || !snap_session_start(game_ms())) return;
        active = 1;
        if (g_snap_test && g_test_phase == T_WAIT) { g_test_nev[0] = g_test_nev[1] = 0; g_test_run = 0; }
    }
    int now = game_ms();
    if (g_snap_test && g_test_phase == T_WAIT && now >= TEST_AT_MS && snap_best(TEST_BACK_MS) >= 0 && g_restore_req < 0) {
        g_test_end = now;
        g_test_run = -1;
        g_test_phase = T_JUMPED;
        g_restore_req = TEST_BACK_MS;
        rc_log("snapshot test: at %d ms, jumping back to %d ms", now, TEST_BACK_MS);
    }
    if (g_test_phase == T_SECOND && now >= g_test_end) {
        g_test_run = -1;
        test_compare();
        g_test_phase = T_DONE;
    }
    LONG want = g_restore_req;
    if (want < 0) { snap_tick(now); return; }
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    int pages = 0, got = snap_restore_to(want, &pages);
    if (got == -1) {                        // the helpers or the world are busy: next frame
        if (GetTickCount() - busy_log > 1000) { busy_log = GetTickCount(); rc_log("snapshots: jump waits: %s", snap_busy()); }
        return;
    }
    g_restore_req = -1;
    if (got >= 0) {
        set_paused(core, 0);                // the restored server's next quant is due in the past: no catch-up burst
        rc_log("snapshots: jumped %d -> %d ms (asked %d) in %.0f ms, %d pages", now, got, (int)want, ms_since(t0), pages);
        if (g_test_phase == T_JUMPED) { g_test_from = got; g_test_run = 1; g_test_phase = T_SECOND; }
    }
    g_restore_done = got;
}

static volatile DWORD g_logic_tid;

static void __cdecl main_tick(void) {
    static int logged;
    if (!logged) { logged = 1; rc_log("main_tick runs here"); g_logic_tid = GetCurrentThreadId(); }
    InterlockedIncrement(&g_frames);
    if (g_snapshots) snapshots_tick();
    if (g_pov_on && g_rw == RW_NONE) apply_pov();
    if (g_media_ok && InterlockedExchange(&g_music_stop_vanilla, 0)) stop_game_music();
    pvp_gui_tick();
    lobby_list_tick();
    camo_tick();
    switch (g_rw) {
    case RW_EXIT: {
        struct { void *vt; uint32_t id, pad; uint64_t rest[5]; } ev;
        memset(&ev, 0, sizeof ev);
        ev.vt = g_base + EV_VTABLE;
        ev.id = EV_EXIT;
        rc_log("rewind: leaving the replay (target %d ms)", g_rw_target);
        g_rw_t = GetTickCount();
        g_rw = RW_WAIT_MENU;
        ((void (*)(void *, void *))(g_base + FN_SEND_EVENT))(&ev, NULL);
        break;
    }
    case RW_WAIT_MENU:
        if (replay_core()) {
            if (GetTickCount() - g_rw_t > 60000) rw_fail(TR("Не удалось выйти из реплея.", "Could not leave the replay."));
            break;
        }
        if (GetTickCount() - g_rw_t < 1500) break;      // let the exit and the menu page settle
        if (!is_a(g_load_dialog, VT_REPLAYS_DIALOG, 5) || !is_a(g_load_item, VT_REPLAY_INFO, 2)) {
            rw_fail(TR("Список реплеев изменился, загрузите реплей вручную.", "The replay list changed, load the replay by hand."));
            break;
        }
        rc_log("rewind: loading the replay again");
        g_rw_t = GetTickCount();
        g_rw = RW_WAIT_REPLAY;
        ((void (*)(void *, void *))(g_base + FN_LOAD))(g_load_dialog, g_load_item);
        break;
    case RW_WAIT_REPLAY:
        if (replay_core()) {
            int now = game_ms();
            if (now <= g_rw_target) {                   // clock of the new run (the old value can linger a moment)
                g_pending_seek = g_rw_target;
                g_rw = RW_NONE;
                rc_log("rewind: replay is back at %d ms, seeking to %d", now, g_rw_target);
            }
        } else if (GetTickCount() - g_rw_t > 120000) {
            rw_fail(TR("Реплей не загрузился.", "The replay did not load."));
        }
        break;
    }
}

// main_loop iteration: call main_tick, then the 9 original bytes (the call re-targeted), jump back
static int hook_main_iter(void) {
    uint8_t *target = g_base + MAIN_ITER;
    if (memcmp(target, MAIN_ITER_INS, 9)) return 0;
    uint8_t *s = page_alloc(160), *p = s;
    if (!s) return 0;
    static const uint8_t pre[] = {
        0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x68,
        0x0f, 0x11, 0x44, 0x24, 0x20, 0x0f, 0x11, 0x4c, 0x24, 0x30,
        0x0f, 0x11, 0x54, 0x24, 0x40, 0x0f, 0x11, 0x5c, 0x24, 0x50,
        0x48, 0xb8};                                                // mov rax, imm64
    memcpy(p, pre, sizeof pre); p += sizeof pre;
    uint64_t fn = (uint64_t)(uintptr_t)main_tick; memcpy(p, &fn, 8); p += 8;
    static const uint8_t post[] = {
        0xff, 0xd0,
        0x0f, 0x10, 0x44, 0x24, 0x20, 0x0f, 0x10, 0x4c, 0x24, 0x30,
        0x0f, 0x10, 0x54, 0x24, 0x40, 0x0f, 0x10, 0x5c, 0x24, 0x50,
        0x48, 0x83, 0xc4, 0x68, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59,
        0x48, 0x83, 0xec, 0x28,                                     // sub rsp, 28h (stolen)
        0xff, 0x15, 0x02, 0x00, 0x00, 0x00, 0xeb, 0x08};            // call [rip+2]; jmp over the address
    memcpy(p, post, sizeof post); p += sizeof post;
    uint64_t callee = (uint64_t)(uintptr_t)(target + 9 + *(int32_t *)(target + 5)); memcpy(p, &callee, 8); p += 8;
    *p++ = 0xff; *p++ = 0x25; memset(p, 0, 4); p += 4;              // jmp [rip+0] back
    uint64_t back = (uint64_t)(uintptr_t)(target + 9); memcpy(p, &back, 8); p += 8;
    uint8_t jmp[9] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    int32_t rel = (int32_t)(s - (target + 5)); memcpy(jmp + 1, &rel, 4);
    patch(target, jmp, 9);
    return 1;
}

// ---- camera speed ceilings ------------------------------------------------------------------------
// Options keep {min, max} pairs for the camera speeds; 38bf77 resets a value outside them and the
// options sliders stop at max. We only lift the maxima, before the game reads options.set.
#define CAM_RANGES 0x14768a8   // scroll {0.03, 0.3}, rotate {0.1, 2.0}, third person {0.03, 0.3}
static const float CAM_RANGES_ORIG[6] = {0.03f, 0.3f, 0.1f, 2.0f, 0.03f, 0.3f};
static const float CAM_MAX[3] = {3.0f, 10.0f, 3.0f};

static void lift_camera_limits(void) {
    float *r = (float *)(g_base + CAM_RANGES);
    if (memcmp(r, CAM_RANGES_ORIG, sizeof CAM_RANGES_ORIG)) { rc_log("camera limits: unexpected values, left alone"); return; }
    for (int i = 0; i < 3; i++) r[2 * i + 1] = CAM_MAX[i];
    rc_log("camera limits: scroll %.1f, rotate %.1f, third person %.1f", CAM_MAX[0], CAM_MAX[1], CAM_MAX[2]);
}

// ---- autoreplay names -----------------------------------------------------------------------------
// Every game is recorded as replays/autoreplayN.rep. 52ebb0 takes the lowest N of 1..5 with no file and,
// once all five exist, overwrites the oldest: only the last five games were ever kept. With 9999 numbers
// they just go on (a deleted replay's number is used again); the oldest is overwritten only past 9999.
#define SITE_AUTOREPLAYS 0x52dfd6   // mov r9d, 5 in 52df60 (a new recording session)
#define AUTOREPLAYS      9999
static const uint8_t AUTOREPLAYS_INS[6] = {0x41, 0xb9, 0x05, 0x00, 0x00, 0x00};

static void widen_autoreplays(void) {
    uint8_t *site = g_base + SITE_AUTOREPLAYS;
    if (memcmp(site, AUTOREPLAYS_INS, sizeof AUTOREPLAYS_INS)) { rc_log("autoreplays: unexpected code, left at 5"); return; }
    int32_t n = AUTOREPLAYS;
    patch(site + 2, &n, 4);
    rc_log("autoreplays: %d names, none overwritten before that", n);
}

// ---- player view: own camera recorded during live games, put back in replays ------------------
// Replays only hold orders; the camera never leaves the player's PC. So during a live game we
// sample our camera 20 times a second into Documents\my games\gates of hell\replay_camera, and in
// a replay whose game started at the same time we drive the camera from that file.
#define CAMERA          0x159ae88   // +0 pitch, +4 yaw, +8 fov, +0xc distance, +0x20 look-at x, y, z
#define FN_CAMERA_SET   0x6e0000    // (eCameraState*, camera*, bool instant): what loading a save does
#define VT_CAMERA_STATE 0x10fdbf0
#define VT_GAME_CLIENT  0x113d660
#define TPC_CAMERA      0x159e218   // third-person camera owner; active if [[it+0x3b0]+0x10] (af7b20)
static const uint8_t CAMERA_SET_INS[11] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};

typedef struct { int32_t ms; float pitch, yaw, fov, dist, x, y, z; } CamSample;
typedef struct { char magic[8]; int64_t start; } CamHeader;   // start: unix time the game began
static const char CAM_MAGIC[8] = "GOHCAM1";

static int g_pov_ok;                     // camera code matched
static FILE *g_rec;
static int g_rec_last_ms = -1;
static SRWLOCK g_pov_lock = SRWLOCK_INIT;
static CamSample *g_pov;                 // samples for the replay on screen
static int g_pov_n;

static int cam_dir(wchar_t *out) {
    wchar_t docs[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docs))) return 0;
    swprintf(out, MAX_PATH, L"%ls\\my games\\gates of hell\\replay_camera", docs);
    CreateDirectoryW(out, NULL);
    return 1;
}

static int read_camera(CamSample *c) {
    uint8_t b[0x2c];
    if (!rd(g_base + CAMERA, b, sizeof b)) return 0;
    memcpy(&c->pitch, b + 0x0, 4); memcpy(&c->yaw, b + 0x4, 4);
    memcpy(&c->fov, b + 0x8, 4);   memcpy(&c->dist, b + 0xc, 4);
    memcpy(&c->x, b + 0x20, 12);
    return 1;
}

// direct control of a unit (third-person view): not the overview camera we want to replay
static int third_person(void) {
    uintptr_t t = rdq((uintptr_t)g_base + TPC_CAMERA), c = t ? rdq(t + 0x3b0) : 0;
    uint8_t on = 0;
    return c && rd((void *)(c + 0x10), &on, 1) && on;
}

static int overview(const CamSample *c) { return c->dist >= 300.f && c->fov >= 0.5f; }

static int live_game(void) {
    uintptr_t s = (uintptr_t)g_base + MP_SINGLETON;
    uintptr_t gc = rdq(s + 0x50);
    if (!gc || rdq(gc) != (uintptr_t)g_base + VT_GAME_CLIENT) return 0;
    uintptr_t sc = rdq(s + SESSION_CLIENT);
    return !(sc && rdq(sc) == (uintptr_t)g_base + VT_REPLAY_CLIENT);
}

// worker thread, every 50 ms
static void record_tick(void) {
    int live = g_pov_ok && live_game(), now = game_ms();
    if (g_rec && (!live || now < g_rec_last_ms - 1000)) {
        fclose(g_rec); g_rec = NULL;
        rc_log("camera: recording stopped at %d ms", g_rec_last_ms);
    }
    if (!live) { g_rec_last_ms = -1; return; }
    if (!g_rec) {
        // start only at the very beginning of a game, so the start time matches the replay's
        if (now <= 0 || now > 5000) return;
        wchar_t dir[MAX_PATH], file[MAX_PATH];
        if (!cam_dir(dir)) return;
        CamHeader h;
        memcpy(h.magic, CAM_MAGIC, 8);
        h.start = (int64_t)time(NULL) - now / 1000;
        swprintf(file, MAX_PATH, L"%ls\\cam_%lld.bin", dir, (long long)h.start);
        g_rec = _wfopen(file, L"wb");
        if (!g_rec) return;
        fwrite(&h, sizeof h, 1, g_rec);
        g_rec_last_ms = -1;
        rc_log("camera: recording, game started at %lld", (long long)h.start);
    }
    if (now - g_rec_last_ms < 50) return;
    CamSample c;
    if (third_person() || !read_camera(&c) || !overview(&c)) return;
    c.ms = now;
    fwrite(&c, sizeof c, 1, g_rec);
    if ((now / 50) % 40 == 0) fflush(g_rec);
    g_rec_last_ms = now;
}

// worker thread, when a replay comes up: find the recording whose game started at the same time
static void load_pov(long long start) {
    CamSample *buf = NULL;
    int n = 0;
    wchar_t dir[MAX_PATH], pattern[MAX_PATH], best[MAX_PATH] = L"";
    long long best_d = 61;
    if (start && g_pov_ok && cam_dir(dir)) {
        swprintf(pattern, MAX_PATH, L"%ls\\cam_*.bin", dir);
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(pattern, &fd);
        while (f != INVALID_HANDLE_VALUE) {
            long long t = _wtoi64(fd.cFileName + 4), d = t > start ? t - start : start - t;
            if (d < best_d) { best_d = d; swprintf(best, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName); }
            if (!FindNextFileW(f, &fd)) break;
        }
        if (f != INVALID_HANDLE_VALUE) FindClose(f);
    }
    if (best[0]) {
        FILE *in = _wfopen(best, L"rb");
        CamHeader h;
        if (in && fread(&h, sizeof h, 1, in) == 1 && !memcmp(h.magic, CAM_MAGIC, 8)) {
            fseek(in, 0, SEEK_END);
            long size = ftell(in) - (long)sizeof h;
            fseek(in, sizeof h, SEEK_SET);
            n = size > 0 ? (int)(size / sizeof(CamSample)) : 0;
            buf = n ? malloc(n * sizeof(CamSample)) : NULL;
            n = buf ? (int)fread(buf, sizeof(CamSample), n, in) : 0;
            int k = 0;                  // older files still hold third-person samples
            for (int i = 0; i < n; i++) if (overview(&buf[i])) buf[k++] = buf[i];
            n = k;
        }
        if (in) fclose(in);
    }
    AcquireSRWLockExclusive(&g_pov_lock);
    free(g_pov);
    g_pov = buf;
    g_pov_n = n;
    ReleaseSRWLockExclusive(&g_pov_lock);
    g_ctl.has_pov = n > 0;
    if (n) rc_log("camera: player view for this replay, %d samples (start off by %lld s)", n, best_d);
}

static float lerp_angle(float a, float b, float t) {
    float period = (fabsf(a) > 6.3f || fabsf(b) > 6.3f) ? 360.f : 6.2831853f;   // degrees or radians
    float d = b - a;
    if (d > period / 2) d -= period;
    if (d < -period / 2) d += period;
    return a + d * t;
}

// logic thread (main_tick): put the recorded camera on screen
static void apply_pov(void) {
    static LARGE_INTEGER last, freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (now.QuadPart - last.QuadPart < freq.QuadPart / 60) return;
    last = now;
    if (!replay_core()) return;
    int ms = game_ms();
    CamSample c;
    AcquireSRWLockShared(&g_pov_lock);
    int n = g_pov_n, lo = 0, hi = n - 1;
    if (n) {
        while (lo < hi) { int mid = (lo + hi + 1) / 2; if (g_pov[mid].ms <= ms) lo = mid; else hi = mid - 1; }
        CamSample a = g_pov[lo], b = g_pov[lo + 1 < n ? lo + 1 : lo];
        float t = b.ms > a.ms ? (float)(ms - a.ms) / (b.ms - a.ms) : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        c = a;
        c.x += (b.x - a.x) * t; c.y += (b.y - a.y) * t; c.z += (b.z - a.z) * t;
        c.dist += (b.dist - a.dist) * t;
        c.pitch = lerp_angle(a.pitch, b.pitch, t);
        c.yaw = lerp_angle(a.yaw, b.yaw, t);
    }
    ReleaseSRWLockShared(&g_pov_lock);
    if (!n) return;
    struct { void *vt; float x, y, z, dist, pitch, yaw, fov; uint8_t valid, pad[3]; } st;
    memset(&st, 0, sizeof st);
    st.vt = g_base + VT_CAMERA_STATE;
    st.x = c.x; st.y = c.y; st.z = c.z; st.dist = c.dist;
    st.pitch = c.pitch; st.yaw = c.yaw; st.fov = c.fov;
    st.valid = 1;
    // instant = 0: the setter batches its changes and only recomputes the camera when this is 0
    ((void (*)(void *, void *, int))(g_base + FN_CAMERA_SET))(&st, g_base + CAMERA, 0);
}

// ---- match charts: sample every player's numbers once per game second ---------------------------
// Runs every worker tick (50 ms) so that army losses between samples are not missed.
static void match_tick(void) {
    static LONG seq;
    static int last_ms;
    static float prev_army[MS_PLAYERS], lost[MS_PLAYERS], bought[MS_PLAYERS];   // limit points lost / bought so far
    uintptr_t st = g_stats;
    // once the match is over nothing may touch what was collected: not the results page, not the menus
    if (!st || g_ms_results) return;
    uintptr_t b = rdq(st), e = rdq(st + 8);
    // after the match the object is freed: keep what was collected, take nothing that does not look right
    if (!b || e <= b || (e - b) % REC_SIZE || (e - b) / REC_SIZE > MS_PLAYERS) return;
    int n = (int)((e - b) / REC_SIZE), now = game_ms();
    uint8_t ids[MS_PLAYERS];
    float army[MS_PLAYERS];
    for (int i = 0; i < n; i++) {
        uintptr_t r = b + (uintptr_t)i * REC_SIZE;
        if (!rd((void *)r, &ids[i], 1) || ids[i] >= MAX_PLAYER_BYTE || !rd((void *)(r + REC_ARMY), &army[i], 4)) return;
        if (!(army[i] >= 0 && army[i] < 1e6f)) return;
    }
    if (seq != g_match_seq) {                           // a new match (or the replay restarted)
        seq = g_match_seq;
        g_ms.n = 0;
        InterlockedIncrement(&g_ms.epoch);
        for (int i = 0; i < MS_PLAYERS; i++) { prev_army[i] = lost[i] = bought[i] = 0; g_ms.name[i][0] = 0; }
        last_ms = -1000000;
    }
    for (int i = 0; i < n; i++) {
        if (army[i] < prev_army[i]) lost[i] += prev_army[i] - army[i];
        else bought[i] += army[i] - prev_army[i];
        prev_army[i] = army[i];
    }
    long k = g_ms.n;
    if (now < last_ms || now - last_ms < 1000 || k >= MS_SAMPLES) return;   // a clock that went back belongs to no match
    int mapped = 0;
    for (int p = 0; p < MAX_PLAYER_BYTE; p++) mapped |= g_player_rec[p] != 0;
    for (int i = 0; i < n; i++) {
        uintptr_t r = b + (uintptr_t)i * REC_SIZE;
        // economies: the money one is the one that earns (doctrine points have no income)
        float budget = 0, income = 0, spent = 0;
        uintptr_t eb = rdq(r + REC_ECON), ee = rdq(r + REC_ECON + 8);
        for (int j = 0; eb && ee > eb && j < 4 && eb + (uintptr_t)j * ECON_SIZE < ee; j++) {
            float f[3];
            uintptr_t x = eb + (uintptr_t)j * ECON_SIZE;
            if (!rd((void *)(x + ECON_BUDGET), &f[0], 4) || !rd((void *)(x + ECON_INCOME), &f[1], 8)) break;
            if (j == 0 || f[1] > income) { budget = f[0]; income = f[1]; spent = f[2]; }
        }
        // kills and deaths are counted by the units' owner byte; spawns told us which player that is
        long kills[2] = {0}, deaths[2] = {0};
        for (int p = 0; p < MAX_PLAYER_BYTE; p++)
            if (mapped ? g_player_rec[p] == ids[i] + 1 : p == ids[i])
                for (int v = 0; v < 2; v++) { kills[v] += g_kills[v][p]; deaths[v] += g_deaths[v][p]; }
        g_ms.id[i] = ids[i];
        g_ms.v[MS_SPENT][i][k] = spent;
        g_ms.v[MS_ARMY_MP][i][k] = bought[i] > 0 ? army[i] * spent / bought[i] : 0;   // MP per limit point bought, on the field
        g_ms.v[MS_BUDGET][i][k] = budget;
        g_ms.v[MS_ARMY][i][k] = army[i];
        g_ms.v[MS_LOST][i][k] = lost[i];
        g_ms.v[MS_KILLS_INF][i][k] = (float)kills[0];
        g_ms.v[MS_KILLS_VEH][i][k] = (float)kills[1];
        g_ms.v[MS_DEATHS_INF][i][k] = (float)deaths[0];
        g_ms.v[MS_DEATHS_VEH][i][k] = (float)deaths[1];
    }
    g_ms.players = n;
    g_ms.t_ms[k] = now;
    MemoryBarrier();
    g_ms.n = k + 1;
    last_ms = now;
}

// the charts' players by name: from the replay on screen, or after a live match from the replay it wrote;
// tried every 3 s until found, once per match
static void names_tick(void) {
    static long done_epoch = -1;
    static DWORD tried;
    if (g_ms.n < 2 || done_epoch == g_ms.epoch || GetTickCount() - tried < 3000) return;
    int replay = g_ctl.state == ST_REPLAY || g_ctl.state == ST_SEEKING || g_ctl.state == ST_REWIND;
    if (!replay && !g_ms_results) return;
    tried = GetTickCount();
    static char names[MS_PLAYERS * 2][64];
    memset(names, 0, sizeof names);
    if (!replay_player_names(replay ? g_ctl.path : "", names, MS_PLAYERS * 2)) return;
    for (int i = 0; i < g_ms.players && i < MS_PLAYERS; i++) {
        int id = g_ms.id[i];
        if (id >= 0 && id < MS_PLAYERS * 2 && names[id][0]) snprintf(g_ms.name[i], sizeof g_ms.name[i], "%s", names[id]);
    }
    done_epoch = g_ms.epoch;
    rc_log("charts: players' names from the replay");
}

// a battle is loaded and on screen: live game of any kind, skirmish or replay, until "Exit game mode"
static int game_mode(void) {
    uintptr_t o = rdq((uintptr_t)g_base + GAME_MODE_OBJ);
    uint8_t on = 0;
    return o && rd((void *)(o + GAME_MODE_ON), &on, 1) && on == 1;
}

// worker thread: the battle music runs while the game is in game mode. Stats events flowing count too,
// in case the flag ever reads wrong; the results page is no end, it also opens in the middle of a match.
static int in_battle(void) {
    static int was_mode = -1, was_live = -1;
    int mode = game_mode(), live = g_battle_tick && GetTickCount() - g_battle_tick < 10000;
    if (mode != was_mode) {
        rc_log("game mode %s", mode ? "on" : "off");
        if (was_mode == 1) g_stats = 0;  // the match is gone: its stats object with it
        was_mode = mode;
    }
    if (live != was_live) { rc_log("battle events %s", live ? "flow" : "stopped"); was_live = live; }
    return mode || live;
}

// players in a running match (0 = no match): the stats records look alive
static int match_players(void) {
    uintptr_t st = g_stats, b = st ? rdq(st) : 0, e = st ? rdq(st + 8) : 0;
    if (!b || e <= b || (e - b) % REC_SIZE || (e - b) / REC_SIZE > MS_PLAYERS) return 0;
    return (int)((e - b) / REC_SIZE);
}

// ---- seek forward: the engine has no jump, every quant is played; this plays them as fast as it can ----
static int g_seek_from, g_seek_hold, g_hold_ms, g_hold_broken, g_seek_serial;
static DWORD g_seek_t0, g_hold_t;

// end of what the server has sent (game ms), -1 when the fields do not look like it
static int sent_ms(uintptr_t core) {
    int32_t v[3];   // CORE_SENT_TIME, CORE_SENT_DELTA, CORE_SENT_AMOUNT
    if (!rd((void *)(core + CORE_SENT_TIME), v, sizeof v) || v[0] < 0 || v[1] <= 0 || v[1] > 1000 || v[2] < 0 || v[2] > 64) return -1;
    return v[0] + v[1] * v[2];
}

static void seek_start(uintptr_t core, int target) {
    int now = game_ms();
    g_seek_to = target;
    g_seek_from = g_ctl.seek_from_ms = now;
    g_seek_t0 = GetTickCount();
    g_seek_hold = 0;
    g_seek_serial++;
    g_ctl.rate = 0;
    set_paused(core, 0);
    apply_speed(g_hold_broken ? SEEK_SPEED_SAFE : SEEK_SPEED);
    g_turbo = 1;
    force_ratio(1);
    rc_log("seek: %d -> %d ms, server has sent up to %d", now, target, sent_ms(core));
}

// the seek is over or given up: full drawing, the user's speed, the server free again
static void seek_stop(uintptr_t core) {
    if (g_seek_to < 0) return;
    DWORD ms = GetTickCount() - g_seek_t0;
    int now = game_ms();
    rc_log("seek: stopped at %d ms (target %d) after %.1f s, x%.1f", now, g_seek_to, ms / 1000.0,
           ms ? (now - g_seek_from) / (double)ms : 0.0);
    g_turbo = 0;
    force_ratio(0);
    g_seek_to = -1;
    apply_speed(g_speed);
    if (g_seek_hold) set_paused(core, 0);   // the engine's own unpause: no catch-up burst
    g_seek_hold = 0;
}

// ---- preparing a whole replay: run it to the end at full pace with snapshots on the way, then come back ----
static int g_prep, g_prep_back, g_prep_ms;   // preparing / where the user was / the clock when last seen moving
static DWORD g_prep_still;                   // since when the clock stands (the replay is over)

static void prep_finish(uintptr_t core, const char *why) {
    if (!g_prep) return;
    g_prep = 0;
    int first = -1, last = -1, mb = 0, state = 0;
    snap_info(&first, &last, &mb, &state);
    rc_log("prepare: %s at %d ms; snapshots %d..%d ms, %d MB; back to %d ms", why, game_ms(), first, last, mb, g_prep_back);
    seek_stop(core);
    if (first < 0) return;
    g_seek_after_restore = g_prep_back;     // the nearest snapshot, then the few seconds to the very spot
    g_restore_req = g_prep_back < first ? first : g_prep_back;
}

static int g_auto_prep;                     // goh_snapshots.ini prepare=1: every replay is prepared when it opens
static int g_profile;                       // goh_snapshots.ini profile=1: sample the game's threads while preparing

// 0 when the snapshots are not ready yet
static int prep_start(uintptr_t core) {
    int first, last, mb, state;
    snap_info(&first, &last, &mb, &state);
    if (!g_snapshots || state != 2) return 0;
    int now = game_ms(), target = g_ctl.duration_s > 2 ? g_ctl.duration_s * 1000 - 1000 : 0x7fffffff;
    g_prep = 1; g_prep_back = g_prep_ms = now; g_prep_still = GetTickCount();
    rc_log("prepare: from %d ms to %d ms", now, target);
    if (last > now + 1000) { g_seek_after_restore = target; g_restore_req = last; }   // what lies ahead is ready
    else seek_start(core, target);
    return 1;
}

static void seek_tick(uintptr_t core, int now) {
    static DWORD rate_t, log_t;
    static int rate_ms, serial;
    static LONG frames0, drawn0;
    DWORD t = GetTickCount();
    if (serial != g_seek_serial) { serial = g_seek_serial; rate_t = log_t = t; rate_ms = now; frames0 = g_frames; drawn0 = g_frames_drawn; }
    if (!g_seek_hold && !g_hold_broken) {
        int sent = sent_ms(core);
        if (sent >= g_seek_to && sent - g_seek_to < 120000) {
            uint8_t on = 1;
            wr(core + CORE_PAUSED, &on, 1);      // nothing more is sent; the client plays what it has
            g_seek_hold = 1; g_hold_ms = now; g_hold_t = t;
            rc_log("seek: server held, it has sent up to %d ms, client at %d", sent, now);
        }
    } else if (g_seek_hold && t - g_hold_t > 3000) {
        // the clock creeps a little even with nothing to play: barely moving means the client ran dry
        // short of the target, so those fields are not what we think
        if (now - g_hold_ms < 300) {
            uint8_t off = 0;
            wr(core + CORE_PAUSED, &off, 1);
            g_seek_hold = 0; g_hold_broken = 1;
            apply_speed(SEEK_SPEED_SAFE);
            rc_log("seek: client stuck at %d ms short of %d; server let go, old pace from now on", now, g_seek_to);
        }
        g_hold_ms = now; g_hold_t = t;
    }
    if (t - rate_t >= 500) {
        g_ctl.rate = (float)(now - rate_ms) / (t - rate_t);
        rate_t = t; rate_ms = now;
    }
    if (t - log_t >= 2000) {
        LONG f = g_frames, d = g_frames_drawn;
        rc_log("seek: %d ms, x%.1f, %.0f frames/s, %.0f drawn/s, server sent %d%s", now, g_ctl.rate,
               (f - frames0) * 1000.0 / (t - log_t), (d - drawn0) * 1000.0 / (t - log_t), sent_ms(core), g_seek_hold ? ", held" : "");
        log_t = t; frames0 = f; drawn0 = d;
    }
    if (g_prep) {
        if (now != g_prep_ms) { g_prep_ms = now; g_prep_still = t; }
        else if (t - g_prep_still > 4000) { prep_finish(core, "the replay ended"); return; }
    }
    if (now >= g_seek_to) {
        seek_stop(core);
        prep_finish(core, "done");
    }
}

// ---- worker: commands and status ----------------------------------------------------------------
static void handle_cmd(uintptr_t core) {
    LONG seq = g_ctl.cmd_seq;
    if (seq == g_ctl.ack_seq) return;
    MemoryBarrier();
    int cmd = g_ctl.cmd; double arg = g_ctl.arg;
    if (core) {
        g_ctl.msg[0] = 0;
        if (cmd != CMD_PREPARE && cmd != CMD_POV) g_prep = 0;   // the user takes over
        switch (cmd) {
        case CMD_SPEED: g_speed = (float)arg; seek_stop(core); apply_speed(g_speed); break;
        case CMD_PAUSE: seek_stop(core); set_paused(core, arg != 0); break;
        case CMD_POV: g_pov_on = arg != 0; break;
        case CMD_PREPARE:
            if (!arg) prep_finish(core, "stopped");
            else if (!prep_start(core)) ctl_msg(TR("Снимки ещё готовятся, подождите пару секунд.", "Snapshots are still being made, wait a couple of seconds."));
            break;
        case CMD_SEEK: {
            int target = arg < 0 ? 0 : (int)arg, now = game_ms(), snap = g_snapshots ? snap_best(target) : -1;
            int first = -1, last = -1, mb, state;
            if (g_snapshots) snap_info(&first, &last, &mb, &state);
            if (snap >= 0 && target < last + SNAP_EVERY_MS && !(snap <= now && now < target)) {
                // inside the prepared stretch: the snapshot at or before the spot, at once, no playing on to
                // the very second (a click a few seconds ahead in the same stretch just plays there, below)
                seek_stop(core);
                g_seek_after_restore = -1;
                g_restore_req = target;
                ctl_msg(TR("Перескакиваю…", "Jumping…"));
            } else if (snap >= 0 && (target < now || snap > now + 1000)) {
                // past the prepared stretch: its last snapshot, then play fast to the spot
                seek_stop(core);
                g_seek_after_restore = target;
                g_restore_req = target;
                ctl_msg(TR("Перескакиваю…", "Jumping…"));
            } else if (target > now) {
                seek_start(core, target);
            } else if (!g_rewind_ok) {
                ctl_msg(TR("Перемотка назад недоступна: код игры не совпал.", "Seeking back is unavailable: the game's code did not match."));
            } else if (!g_load_item || strcmp(g_load_path, g_ctl.path)) {
                ctl_msg(TR("Для перемотки назад откройте реплей через «Реплеи» в меню.", "To seek back, open the replay from \"Replays\" in the menu."));
            } else if (g_rw == RW_NONE) {
                seek_stop(core);
                g_rw_target = target;
                g_rw = RW_EXIT;
            }
            break;
        }
        }
    }
    g_ctl.ack_seq = seq;
}

static DWORD WINAPI worker(LPVOID _) {
    DWORD last_dump = GetTickCount();
    char path[260];
    for (;;) {
        Sleep(50);
        uintptr_t core = replay_core();
        handle_cmd(core);
        record_tick();
        match_tick();
        names_tick();
        int battle = in_battle();
        g_balance_locked = battle || core;      // the roster of a running match or replay is already read
        g_live_match = battle && !core;
        balance_tick();
#ifndef COMPETITIVE_ONLY
        music_tick(g_prep ? -1 : battle, game_ms(), match_players());
#endif   // nothing plays while a replay is run through
        if (!core) {
            // outside a replay the pacing patch must never stay on: it would touch online games
            set_ratio_patch(0);
            g_seek_to = -1;
            g_turbo = 0;
            force_ratio(0);
            g_seek_hold = 0;
            g_prep = 0;
            if (g_rw == RW_NONE) {
                g_speed = 1.0f;
                g_ctl.state = ST_IDLE; g_ctl.path[0] = 0; g_ctl.duration_s = 0; g_ctl.msg[0] = 0;
            }
        } else {
            int now = game_ms();
            LONG ps = InterlockedExchange(&g_pending_seek, -1);
            if (ps >= 0) {                      // rewind: the replay is loaded again, now catch up
                g_ctl.msg[0] = 0;
                seek_start(core, ps);
            }
            LONG landed = InterlockedExchange(&g_restore_done, NO_RESULT);
            if (landed != NO_RESULT) {          // the logic thread jumped (or could not): play the rest fast
                g_ctl.msg[0] = 0;
                now = game_ms();
                if (landed >= 0 && g_seek_after_restore > landed + 300) seek_start(core, g_seek_after_restore);
                if (landed == -2) ctl_msg(TR("Снимка для этого места нет.", "No snapshot for this point."));
                g_seek_after_restore = -1;
            }
            if (g_seek_to >= 0) seek_tick(core, now);
            uint8_t pz = 0; rd((void *)(core + CORE_PAUSED), &pz, 1);
            g_ctl.game_ms = now;
            g_ctl.paused = pz && !g_seek_hold;  // our hold at the seek target is no pause of the user's
            g_ctl.state = g_seek_to >= 0 ? ST_SEEKING : ST_REPLAY;
            replay_path(path, sizeof path);
            if (strcmp(path, g_ctl.path)) {
                long long start;
                replay_meta(path, &g_ctl.duration_s, &start);
                load_pov(start);
                strcpy(g_ctl.path, path);
                rc_log("replay %s, %d s", path, g_ctl.duration_s);
            }
            if (path[0] && InterlockedExchange(&g_load_fresh, 0)) strcpy(g_load_path, path);
        }
        if (g_rw != RW_NONE) {
            g_ctl.state = ST_REWIND;
            ctl_msg(TR("Перематываю назад: перезапуск реплея…", "Seeking back: restarting the replay…"));
        }
        g_ctl.speed = g_speed;
        g_ctl.pov = g_pov_on && g_ctl.has_pov;
        g_ctl.preparing = g_prep;
        static int was_prep;
        if (g_prep != was_prep) {               // the pass begins or ends: the game's sound off/on, profiler
            was_prep = g_prep;
            game_sound_mute(g_prep);
            if (g_prep && g_profile) prof_start(g_logic_tid, (DWORD)g_watch_tid[W_STATS_EVENT]);
            else prof_stop();
        }
        static char prepared[260];              // each replay is prepared once, right after it opens
        if (core && g_auto_prep && !g_prep && g_seek_to < 0 && g_restore_req < 0 && g_ctl.path[0] && strcmp(prepared, g_ctl.path) &&
            prep_start(core))
            strcpy(prepared, g_ctl.path);
        if (g_snapshots) snap_info(&g_ctl.snap_from_ms, &g_ctl.snap_to_ms, &g_ctl.snap_mb, &g_ctl.snap_state);
        g_ctl.seek_target_ms = g_seek_to;
        if (GetTickCount() - last_dump > 10000) { last_dump = GetTickCount(); dump_threads(); }
    }
}

// rip-relative targets: the page has to sit within 2 GB of the game code
static int near_page(void) {
    for (uintptr_t a = (uintptr_t)g_base + IMAGE_END; !g_page && a < (uintptr_t)g_base + 0x70000000; a += 0x10000)
        g_page = VirtualAlloc((void *)a, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    return g_page != NULL;
}

static void startup(HMODULE self) {
    char path[MAX_PATH];
    GetModuleFileNameA(self, path, MAX_PATH);
    strrchr(path, '\\')[1] = 0;
    strcpy(g_dir, path);
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    // the server a hosting game starts for the match: only the balance switch, the game's
    int server = !_stricmp(strrchr(exe, '\\') + 1, "call_to_arms_server.exe");
    if (!server && _stricmp(strrchr(exe, '\\') + 1, "call_to_arms.exe")) return;   // editor, workshop: a plain proxy
    InitializeCriticalSection(&g_log_cs);
    // a log per process (the server runs beside the game); the last run's survives one restart (crashes are in it)
    const char *log = server ? "goh_replay_server" : "goh_replay";
    char prev[MAX_PATH];
    snprintf(path, sizeof path, "%s%s.log", g_dir, log);
    snprintf(prev, sizeof prev, "%s%s.prev.log", g_dir, log);
    MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
    g_log = fopen(path, "w");
    g_base = (uint8_t *)GetModuleHandleA(NULL);
    g_ctl.speed = 1.0f;

    if (server) {
        AddVectoredExceptionHandler(1, fault_log);
        if (!near_page()) { rc_log("server: no memory near its code, nothing patched"); return; }
        balance_install(g_base, g_dir, page_alloc, patch, 1);
        day_install(g_base, patch, 1);
        rc_log("server: loaded at %p", self);
        return;
    }
    if (memcmp(g_base + SITE_RATIO, RATIO_INS, 9)) {
        g_ctl.state = ST_ERROR; ctl_msg(TR("Другая версия игры (нужна 1.067), панель выключена.", "Another game version (1.067 needed), the panel is off."));
        rc_log("unsupported build, nothing patched");
        return;
    }
    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%sgoh_snapshots.ini", g_dir);
    census_init(g_base, g_dir, GetPrivateProfileIntA("debug", "census_after", 0, ini));
    camo_init(g_dir);
    g_census_auto = GetPrivateProfileIntA("debug", "census", 0, ini);
#ifdef COMPETITIVE_ONLY
    if (0) {
#else
    if (GetPrivateProfileIntA("snapshots", "enabled", 0, ini)) {
#endif
        int ok = snap_install(g_base);
        rc_log("snapshots: game heap %s", ok ? "in our range" : "NOT moved (import slots not found)");
        g_snapshots = ok;
        g_snap_test = ok && GetPrivateProfileIntA("snapshots", "test", 0, ini);
        g_auto_prep = ok && GetPrivateProfileIntA("snapshots", "prepare_v2", 0, ini);   // off for now (2026-11-02)
        g_profile = ok && GetPrivateProfileIntA("snapshots", "profile", 0, ini);
        if (g_snap_test) rc_log("snapshots: test armed (at %d s of a replay, jump back to %d s)", TEST_AT_MS / 1000, TEST_BACK_MS / 1000);
    }
    AddVectoredExceptionHandler(1, fault_log);   // the first faults land in our log, whatever is on
    if (!near_page()) { g_ctl.state = ST_ERROR; ctl_msg(TR("Нет памяти рядом с игрой.", "No memory near the game.")); return; }
    g_cell = (float *)page_alloc(16);
    g_force_ratio = page_alloc(16);
    *g_cell = 1.0f;

#ifdef COMPETITIVE_ONLY
    // the build for testing the competitive balance with other players: its switch and the Shift+F7 window, nothing else
    widen_autoreplays();             // without it the game's 5 names come back and overwrite the oldest replays
    balance_install(g_base, g_dir, page_alloc, patch, 0);
    if (GetPrivateProfileIntA("debug", "balance_on_at_start", 0, ini)) g_balance_on = 1;   // test: on before anything is read
    { extern DWORD g_balance_on_after_ms; g_balance_on_after_ms = 1000 * GetPrivateProfileIntA("debug", "balance_on_after", 0, ini); }
    g_ui_on = detour(W_PRESENT);
    g_ctl.state = ST_IDLE;
    rc_log("competitive test build loaded at %p (panel %s)", self, g_ui_on ? "on" : "off: unexpected code");
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    online_start(g_dir);
    pvp_install(g_base, g_dir, page_alloc, patch);
    return;
#endif
    // [debug] in goh_snapshots.ini: switch parts off one by one to find one that breaks a game
#define PART(name) GetPrivateProfileIntA("debug", name, 1, ini)
    lift_camera_limits();
    widen_autoreplays();
    if (PART("balance")) balance_install(g_base, g_dir, page_alloc, patch, 0);
    int forced = force_ratio_stub(SITE_RATIO_STORE1, RATIO_STORE1_INS, 1) + force_ratio_stub(SITE_RATIO_STORE2, RATIO_STORE2_INS, 0);
    rc_log("seek pacing %s (%d of 2 sites)", forced == 2 ? "forced while seeking" : "partly or not forced", forced);
    g_pov_ok = !memcmp(g_base + FN_CAMERA_SET, CAMERA_SET_INS, sizeof CAMERA_SET_INS) &&
               rdq((uintptr_t)g_base + VT_CAMERA_STATE) != 0;
    if (PART("watch")) for (uint32_t i = 0; i < NWATCH; i++) detour(i);
    g_rewind_ok = !memcmp(g_base + FN_SEND_EVENT, SEND_EVENT_INS, 9) &&
                  rdq((uintptr_t)g_base + EV_VTABLE) == (uintptr_t)g_base + 0x224700 &&
                  g_watch_count[1] == 0 && !memcmp(g_base + WATCH[1].rva, "\xe9", 1) &&   // Load is watched
                  PART("main_iter") && hook_main_iter();
    rc_log("rewind %s", g_rewind_ok ? "ready" : "unavailable (code mismatch)");
    g_media_ok = PART("media") && hook_media_play();
    rc_log("music hook %s", g_media_ok ? "ready" : "unavailable (code mismatch)");
    rc_log("seek without drawing %s", PART("filter") && hook_filter(FN_RENDER_SCREEN, RENDER_SCREEN_INS, render_filter) ? "ready" : "unavailable (code mismatch)");
    if (PART("music")) music_start(g_dir);
    if (PART("online")) online_start(g_dir);
    if (PART("pvp")) pvp_install(g_base, g_dir, page_alloc, patch);
    void *qpc = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "QueryPerformanceCounter");
    g_real_qpc = *(void **)(g_base + IAT_QPC);
    if (!PART("qpc")) {
        g_real_qpc = (BOOL (WINAPI *)(LARGE_INTEGER *))qpc;
    } else if (g_real_qpc == qpc || g_real_qpc == (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlQueryPerformanceCounter")) {
        void *hook = (void *)qpc_hook;
        patch(g_base + IAT_QPC, &hook, 8);
        g_qpc_hooked = 1;
    } else {
        rc_log("timer slot holds %p, not QueryPerformanceCounter: left alone", g_real_qpc);
        g_real_qpc = (BOOL (WINAPI *)(LARGE_INTEGER *))qpc;
    }

    g_ctl.state = ST_IDLE;
    rc_log("loaded at %p, game %p, page %p", self, g_base, g_page);
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    g_ui_draw = PART("ui_draw");
    g_ui_on = PART("ui");
    rc_log("parts: balance %d watch %d main_iter %d media %d filter %d music %d qpc %d ui %d", PART("balance"), PART("watch"),
           PART("main_iter"), PART("media"), PART("filter"), PART("music"), PART("qpc"), PART("ui"));
#undef PART
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        if (!proxy_init()) return FALSE;    // without the real iphlpapi the game could not run
        startup(h);
    }
    return TRUE;
}
