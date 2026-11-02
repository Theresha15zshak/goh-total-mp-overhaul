// A sampling profiler for the prepare pass: where the game's logic thread (one frame after another) and its
// quant thread (the simulation) spend their time, so the pass can be made faster where it matters.
// A sample suspends the thread for a moment and reads its rip: in the game's code it counts for that
// function (.pdata), elsewhere for the module plus the game function that called into it (first return
// address into the game's code on the stack). Nothing that could take a lock runs while a thread is held.
// It also reads the engine's own stack of named steps ("scene:quant.anim", "scene:quant.particles", ...: the
// debug stack crash reports print, pushed by d1310 and popped by 5e37f0) and counts each step on it, so
// the report says which steps of a quant take the time.
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <string.h>
#include "goh_replay.h"

#define SLOTS 8192
#define ZONE_SLOTS 1024
#define MAX_MODS 256
#define ZONE_TLS 0x147cb08          // DWORD: TlsAlloc index of the per-thread step stack (5e4c60)
#define ZONE_SIZE 0x38              // one step: +0 kind, +8 const char *name
#define MAX_DEPTH 24

typedef struct { uint64_t key; uint32_t n; } Slot;
typedef struct {
    DWORD tid; HANDLE h; Slot *slot; uint32_t total; const char *name;
    uint8_t *teb; Slot *zone; uint32_t zone_total; uint32_t mod_n[MAX_MODS + 1];
} Target;
typedef struct { LONG status; void *teb; HANDLE pid, tid; ULONG_PTR affinity; LONG priority, base; } ThreadInfo;
typedef LONG (NTAPI *QueryThreadFn)(HANDLE, int, void *, ULONG, ULONG *);

static Target g_t[2];
static SRWLOCK g_lock = SRWLOCK_INIT;           // a sample and a stopped world never overlap
static volatile LONG g_run;
static HANDLE g_thread;
static uint8_t *g_exe, *g_exe_hi, *g_text_lo, *g_text_hi;
static RUNTIME_FUNCTION *g_rf;
static uint32_t g_nrf;
static struct { uint8_t *lo, *hi; char name[32]; } g_mod[MAX_MODS];
static int g_nmod;

static uint32_t func_of(uint8_t *ip) {
    uint32_t rva = (uint32_t)(ip - g_exe), lo = 0, hi = g_nrf;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (g_rf[m].EndAddress <= rva) lo = m + 1;
        else if (g_rf[m].BeginAddress > rva) hi = m;
        else return g_rf[m].BeginAddress;
    }
    return rva;
}

static int mod_of(uint8_t *ip) {
    for (int i = 0; i < g_nmod; i++) if (ip >= g_mod[i].lo && ip < g_mod[i].hi) return i;
    return -1;
}

static void count_in(Slot *table, uint32_t size, uint64_t key) {
    uint64_t h = (key * 0x9E3779B97F4A7C15ull) >> 40;
    for (uint32_t i = 0; i < size; i++) {
        Slot *s = &table[(h + i) & (size - 1)];
        if (s->key == key || !s->n) { s->key = key; s->n++; return; }
    }
}

static void count(Target *t, uint64_t key) { count_in(t->slot, SLOTS, key); t->total++; }

// the names on the thread's step stack, outermost first; the thread is held. Its stack vector lives in
// the heap: read through ReadProcessMemory, which fails instead of faulting on a bad pointer.
static int zones_of(Target *t, uint64_t *names) {
    if (!t->teb || g_exe_hi < g_exe + ZONE_TLS + 4) return 0;     // not the game (the tests)
    DWORD idx = *(DWORD *)(g_exe + ZONE_TLS);
    uint8_t **slot;
    if (idx < 64) slot = (uint8_t **)(t->teb + 0x1480) + idx;
    else if (idx < 1088) {
        uint8_t **more = *(uint8_t ***)(t->teb + 0x1780);
        if (!more) return 0;
        slot = more + (idx - 64);
    } else return 0;
    uint64_t vec[2];
    SIZE_T got;
    uint8_t *v = *slot;
    if (!v || !ReadProcessMemory(GetCurrentProcess(), v, vec, sizeof vec, &got)) return 0;
    if (vec[1] <= vec[0] || (vec[1] - vec[0]) % ZONE_SIZE) return 0;
    uint64_t n = (vec[1] - vec[0]) / ZONE_SIZE;
    if (n > MAX_DEPTH) n = MAX_DEPTH;
    uint8_t buf[MAX_DEPTH * ZONE_SIZE];
    if (!ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)vec[0], buf, n * ZONE_SIZE, &got)) return 0;
    int k = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint8_t *name = *(uint8_t **)(buf + i * ZONE_SIZE + 8);
        if (name < g_exe || name >= g_exe_hi || (name >= g_text_lo && name < g_text_hi)) continue;   // names live in .rdata
        int seen = 0;
        for (int j = 0; j < k; j++) seen |= names[j] == (uint64_t)(uintptr_t)name;
        if (!seen) names[k++] = (uint64_t)(uintptr_t)name;
    }
    return k;
}

// the first return address into the game's code on the stack: the game function that called out
static uint32_t caller_in_game(uint64_t rsp) {
    for (int i = 0; i < 512; i++) {
        uint8_t *q = ((uint8_t **)rsp)[i];
        if (q < g_text_lo + 6 || q >= g_text_hi) continue;
        if (q[-5] == 0xe8 || (q[-6] == 0xff && (q[-5] & 0x38) == 0x10) || (q[-3] == 0xff && (q[-2] & 0x38) == 0x10) ||
            (q[-2] == 0xff && (q[-1] & 0x38) == 0x10))
            return func_of(q);
    }
    return 0;
}

// the world is stopped from the logic thread, which suspends this one too: a sample may not have the logic
// thread held at that moment, or both would wait for each other forever
void prof_lock(void) { AcquireSRWLockExclusive(&g_lock); }
void prof_unlock(void) { ReleaseSRWLockExclusive(&g_lock); }

static void sample(Target *t) {
    if (!t->h) return;
    CONTEXT c;
    c.ContextFlags = CONTEXT_CONTROL;
    prof_lock();
    if (SuspendThread(t->h) == (DWORD)-1) { prof_unlock(); return; }
    int ok = GetThreadContext(t->h, &c);
    uint8_t *ip = (uint8_t *)c.Rip;
    uint64_t key = 0, names[MAX_DEPTH];
    int mod = -1;
    if (ok && ip >= g_text_lo && ip < g_text_hi) key = func_of(ip);
    else if (ok) { mod = mod_of(ip); key = (uint64_t)(mod + 2) << 32 | caller_in_game(c.Rsp); }   // the stack is still: the thread is held
    int nz = ok ? zones_of(t, names) : 0;
    ResumeThread(t->h);
    prof_unlock();
    if (!ok) return;
    count(t, key);
    t->mod_n[mod + 1]++;
    for (int i = 0; i < nz; i++) count_in(t->zone, ZONE_SLOTS, names[i]);
    t->zone_total++;
}

// the biggest entries of a table, as " pct label |"; empties it
static int top(char *line, int len, Slot *table, uint32_t size, uint32_t total, int many, int zones) {
    for (int k = 0; k < many && len < 900; k++) {
        Slot *best = NULL;
        for (uint32_t i = 0; i < size; i++) if (table[i].n && (!best || table[i].n > best->n)) best = &table[i];
        if (!best) break;
        int pct = (int)(best->n * 100ull / total);
        if (zones) {
            char name[48];
            const char *z = (const char *)(uintptr_t)best->key;
            int i = 0;
            while (i < 47 && z[i] >= 32 && z[i] < 127) { name[i] = z[i]; i++; }
            name[i] = 0;
            len += wsprintfA(line + len, " %d%% %s |", pct, name);
        } else {
            int m = (int)(best->key >> 32) - 2;
            uint32_t rva = (uint32_t)best->key;
            if (m < -1) len += wsprintfA(line + len, " %d%% %x |", pct, rva);
            else len += wsprintfA(line + len, " %d%% %s<%x |", pct, m >= 0 ? g_mod[m].name : "?", rva);
        }
        best->n = 0;
    }
    memset(table, 0, size * sizeof(Slot));
    return len;
}

static void report(Target *t) {
    if (!t->total) return;
    char line[1024];
    int len = wsprintfA(line, "prof %s (%lu samples):", t->name, t->total);
    top(line, len, t->slot, SLOTS, t->total, 10, 0);
    rc_log("%s", line);
    len = wsprintfA(line, "prof %s by module:", t->name);
    for (int k = 0; k < 6; k++) {
        int best = -1;
        for (int i = 0; i <= g_nmod; i++) if (t->mod_n[i] && (best < 0 || t->mod_n[i] > t->mod_n[best])) best = i;
        if (best < 0) break;
        len += wsprintfA(line + len, " %d%% %s |", (int)(t->mod_n[best] * 100ull / t->total), best ? g_mod[best - 1].name : "game");
        t->mod_n[best] = 0;
    }
    rc_log("%s", line);
    memset(t->mod_n, 0, sizeof t->mod_n);
    if (t->zone_total) {
        len = wsprintfA(line, "prof %s steps:", t->name);
        top(line, len, t->zone, ZONE_SLOTS, t->zone_total, 24, 1);
        rc_log("%s", line);
    }
    t->total = t->zone_total = 0;
}

static DWORD WINAPI prof_thread(LPVOID _) {
    (void)_;
    DWORD last = GetTickCount();
    while (g_run) {
        sample(&g_t[0]);
        sample(&g_t[1]);
        Sleep(2);
        if (GetTickCount() - last >= 10000) { report(&g_t[0]); report(&g_t[1]); last = GetTickCount(); }
    }
    report(&g_t[0]);
    report(&g_t[1]);
    return 0;
}

void prof_start(DWORD logic_tid, DWORD quant_tid) {
    if (g_run) return;
    if (!g_exe) {
        g_exe = (uint8_t *)GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
        IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
            if (!memcmp(s->Name, ".text", 6)) { g_text_lo = g_exe + s->VirtualAddress; g_text_hi = g_text_lo + s->Misc.VirtualSize; }
        IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        g_exe_hi = g_exe + nt->OptionalHeader.SizeOfImage;
        g_rf = (RUNTIME_FUNCTION *)(g_exe + d.VirtualAddress);
        g_nrf = d.Size / sizeof(RUNTIME_FUNCTION);
        g_t[0].slot = VirtualAlloc(NULL, SLOTS * sizeof(Slot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_t[1].slot = VirtualAlloc(NULL, SLOTS * sizeof(Slot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_t[0].zone = VirtualAlloc(NULL, ZONE_SLOTS * sizeof(Slot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_t[1].zone = VirtualAlloc(NULL, ZONE_SLOTS * sizeof(Slot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    QueryThreadFn query = (QueryThreadFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    // the module list now, not while a thread is held (the loader's lock)
    HMODULE m[MAX_MODS];
    DWORD need = 0;
    g_nmod = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), m, sizeof m, &need))
        for (DWORD i = 0; i < need / sizeof(HMODULE) && g_nmod < MAX_MODS; i++) {
            MODULEINFO mi;
            if (!K32GetModuleInformation(GetCurrentProcess(), m[i], &mi, sizeof mi)) continue;
            g_mod[g_nmod].lo = mi.lpBaseOfDll;
            g_mod[g_nmod].hi = (uint8_t *)mi.lpBaseOfDll + mi.SizeOfImage;
            K32GetModuleBaseNameA(GetCurrentProcess(), m[i], g_mod[g_nmod].name, sizeof g_mod[g_nmod].name);
            g_nmod++;
        }
    for (int i = 0; i < 2; i++) {
        DWORD tid = i ? quant_tid : logic_tid;
        g_t[i].tid = tid;
        g_t[i].name = i ? "quant" : "logic";
        g_t[i].total = g_t[i].zone_total = 0;
        memset(g_t[i].mod_n, 0, sizeof g_t[i].mod_n);
        if (g_t[i].slot) memset(g_t[i].slot, 0, SLOTS * sizeof(Slot));
        if (g_t[i].zone) memset(g_t[i].zone, 0, ZONE_SLOTS * sizeof(Slot));
        g_t[i].h = tid && g_t[i].slot && g_t[i].zone ?
                   OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid) : NULL;
        g_t[i].teb = NULL;
        ThreadInfo ti;
        if (g_t[i].h && query && !query(g_t[i].h, 0, &ti, sizeof ti, NULL)) g_t[i].teb = ti.teb;
    }
    g_run = 1;
    g_thread = CreateThread(NULL, 0, prof_thread, NULL, 0, NULL);
    rc_log("prof: sampling the logic thread %lu and the quant thread %lu", logic_tid, quant_tid);
}

void prof_stop(void) {
    if (!g_run) return;
    g_run = 0;
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    for (int i = 0; i < 2; i++) if (g_t[i].h) { CloseHandle(g_t[i].h); g_t[i].h = NULL; }
}
