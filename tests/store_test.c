// Test of the snapshot store (dll/snap.c) set up the way the game uses it: the store lives in a DLL
// (build/store_test.dll), this exe plays the game - its heap calls go through import slots the DLL
// redirects, and its .data and thread-local data are part of every snapshot.
//   python -m ziglang cc -target x86_64-windows-gnu -O2 -shared -Idll dll/snap.c dll/keep.c dll/lz.c tests/store_test_log.c -ld3d11 -ld3dcompiler_47 -o build/store_test.dll
//   python -m ziglang cc -target x86_64-windows-gnu -O2 tests/store_test.c -o build/store_test.exe
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int (*InstallFn)(uint8_t *);
typedef int (*StartFn)(int);
typedef void (*TickFn)(int);
typedef int (*RestoreFn)(int, int *);
typedef void (*InfoFn)(int *, int *, int *, int *);
typedef int (*OwnsFn)(const void *);
static StartFn session_start;
static TickFn tick;
static RestoreFn restore_to;
static InfoFn info;
static OwnsFn owns;
typedef void (*ProfStartFn)(DWORD, DWORD);
typedef void (*ProfStopFn)(void);

#define N 6000
#define SNAPS 6
static uint8_t *blk[N];
static size_t len[N];
// what every block holds at each snapshot: kept outside the "game" (VirtualAlloc, not the heap, not .data),
// or a restore would put the expectations back too
typedef struct { uint8_t tag[SNAPS][N]; int global_at[SNAPS], tls_at[SNAPS], slot_at[SNAPS]; volatile int worker_bad; } Expect;
static Expect *x;

// a worker like the game's async pather: it keeps its own list in a register and swaps it with the shared
// queue's every round; after a restore both must still be different lists
typedef struct { void *head; int rounds; } Queue;
static Queue *queue;
static HANDLE poke, poked;

static DWORD WINAPI worker(LPVOID _) {
    (void)_;
    void *mine = HeapAlloc(GetProcessHeap(), 0, 64);
    for (;;) {
        WaitForSingleObject(poke, INFINITE);
        if (mine == queue->head) x->worker_bad++;
        void *t = queue->head;
        queue->head = mine;
        mine = t;
        queue->rounds++;
        SetEvent(poked);
    }
    return 0;
}

static void nudge(int times) {
    for (int i = 0; i < times; i++) { SetEvent(poke); WaitForSingleObject(poked, INFINITE); }
    Sleep(5);                                // back at its wait
}
static volatile int g_global;               // the game's .data
static _Thread_local int t_value;           // the game's thread_local
static DWORD slot;                          // a TlsAlloc slot past the TEB's 64

static uint32_t s = 99;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

static void fill(int i, uint8_t v) { memset(blk[i], v, len[i]); }
static int check(int i, uint8_t v) {
    for (size_t k = 0; k < len[i]; k += 97) if (blk[i][k] != v) return 0;
    return !len[i] || blk[i][len[i] - 1] == v;
}

// change some blocks, churn the heap (free and allocate), set the globals: the state of a new phase
static void play(int phase, int changes) {
    for (int c = 0; c < changes; c++) { int i = rnd() % N; fill(i, (uint8_t)(phase * 37 + c)); }
    for (int c = 0; c < 2000; c++) {
        void *q = HeapAlloc(GetProcessHeap(), 0, 16 + rnd() % 50000);
        memset(q, 0x77, 16);
        HeapFree(GetProcessHeap(), 0, q);
    }
    g_global = 1000 + phase;
    t_value = 2000 + phase;
    TlsSetValue(slot, (void *)(uintptr_t)(3000 + phase));
    nudge(1 + phase % 3);
}

static void remember(int k) {
    for (int i = 0; i < N; i++) x->tag[k][i] = blk[i][0];
    x->global_at[k] = g_global;
    x->tls_at[k] = t_value;
    x->slot_at[k] = (int)(uintptr_t)TlsGetValue(slot);
}

// snapshot at game time ms: tick until it is complete
static int snapshot(int ms) {
    int from, to, mb, state, before;
    info(&from, &to, &mb, &state);
    before = to;
    for (int t = 0; t < 2000; t++) {
        tick(ms);
        info(&from, &to, &mb, &state);
        if (state == 2 && to == ms && to != before) return 1;
        Sleep(1);
    }
    printf("snapshot at %d ms never completed\n", ms);
    return 0;
}

static int verify(const char *what, int k, int restored_ms, int want_ms) {
    int bad = restored_ms != want_ms;
    if (bad) printf("%s: landed at %d ms, wanted %d\n", what, restored_ms, want_ms);
    int blocks = 0;
    for (int i = 0; i < N; i++) blocks += !check(i, x->tag[k][i]);
    int sv = (int)(uintptr_t)TlsGetValue(slot);
    if (blocks) printf("%s: %d blocks differ\n", what, blocks);
    if (g_global != x->global_at[k]) printf("%s: .data global %d, want %d\n", what, g_global, x->global_at[k]);
    if (t_value != x->tls_at[k]) printf("%s: thread_local %d, want %d\n", what, t_value, x->tls_at[k]);
    if (sv != x->slot_at[k]) printf("%s: TLS slot %d, want %d\n", what, sv, x->slot_at[k]);
    bad += blocks + (g_global != x->global_at[k]) + (t_value != x->tls_at[k]) + (sv != x->slot_at[k]);
    int before = x->worker_bad;
    nudge(3);
    if (x->worker_bad != before) { printf("%s: the worker and the queue share one list\n", what); bad++; }
    // the restored heap must keep working: allocate and free a lot
    for (int c = 0; c < 20000; c++) { void *q = HeapAlloc(GetProcessHeap(), 0, rnd() % 100000); memset(q, 1, 8); HeapFree(GetProcessHeap(), 0, q); }
    printf("%-34s %s\n", what, bad ? "FAILED" : "ok");
    return bad;
}

static int jump(int target, int *got) {
    int pages = 0;
    for (int t = 0; t < 2000; t++) {
        int r = restore_to(target, &pages);
        if (r != -1) { *got = r; return r >= 0; }
        Sleep(1);
    }
    return 0;
}

int main(void) {
    HMODULE m = LoadLibraryA("store_test.dll");
    if (!m) { printf("no store_test.dll\n"); return 1; }
    InstallFn install = (InstallFn)GetProcAddress(m, "snap_install");
    session_start = (StartFn)GetProcAddress(m, "snap_session_start");
    tick = (TickFn)GetProcAddress(m, "snap_tick");
    restore_to = (RestoreFn)GetProcAddress(m, "snap_restore_to");
    info = (InfoFn)GetProcAddress(m, "snap_info");
    owns = (OwnsFn)GetProcAddress(m, "snap_owns");
    if (!install || !session_start || !tick || !restore_to || !info || !owns) { printf("exports missing\n"); return 1; }
    install((uint8_t *)GetModuleHandleA(NULL));
    // not through this exe's import slot: the store takes VirtualAlloc over too (the engine's page allocator)
    void *(WINAPI *va)(void *, SIZE_T, DWORD, DWORD) = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "VirtualAlloc");
    x = va(NULL, sizeof(Expect), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (owns(x)) { printf("expectations landed in the game's heap\n"); return 1; }
    for (int i = 0; i < 100; i++) slot = TlsAlloc();
    queue = HeapAlloc(GetProcessHeap(), 0, sizeof(Queue));
    queue->head = HeapAlloc(GetProcessHeap(), 0, 64);
    queue->rounds = 0;
    poke = CreateEventA(NULL, FALSE, FALSE, NULL);
    poked = CreateEventA(NULL, FALSE, FALSE, NULL);
    DWORD worker_tid = 0;
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, &worker_tid));
    nudge(1);
    for (int i = 0; i < N; i++) {
        len[i] = 64 + rnd() % 60000;
        blk[i] = HeapAlloc(GetProcessHeap(), 0, len[i]);
        fill(i, 1);
    }
    if (!owns(blk[0])) { printf("the heap is not redirected (import slot not patched)\n"); return 1; }
    int bad = 0, got;
    play(0, 0);
    // the profiler samples this thread - the one that stops the world - all along: they must not lock up
    ProfStartFn prof_start = (ProfStartFn)GetProcAddress(m, "prof_start");
    ProfStopFn prof_stop = (ProfStopFn)GetProcAddress(m, "prof_stop");
    if (prof_start) prof_start(GetCurrentThreadId(), worker_tid);
    if (!session_start(0)) { printf("session did not start\n"); return 1; }
    play(0, 200);                            // written while the base is made: snapshot 1 must catch it
    if (!snapshot(1000)) return 1;
    remember(1);
    play(2, 1500);
    if (!snapshot(20000)) return 1;
    remember(2);
    play(9, 1500);                           // not in any snapshot
    bad += !jump(5000, &got) || verify("back to snapshot 1", 1, got, 1000);
    bad += !jump(25000, &got) || verify("forward to snapshot 2", 2, got, 20000);
    play(3, 1500);
    if (!snapshot(40000)) return 1;
    remember(3);
    bad += !jump(1000, &got) || verify("back to 1 again", 1, got, 1000);
    play(4, 1500);                           // a new branch from snapshot 1
    if (!snapshot(60000)) return 1;
    remember(4);
    bad += !jump(45000, &got) || verify("to 3 from the branch", 3, got, 40000);
    bad += !jump(60000, &got) || verify("to the branch's snapshot 4", 4, got, 60000);
    bad += !jump(20000, &got) || verify("to 2", 2, got, 20000);
    int from, to, mb, state;
    info(&from, &to, &mb, &state);
    printf("store: snapshots %d..%d ms, %d MB for %d blocks (%llu MB of data)\n", from, to, mb, N, 0ull);
    if (prof_stop) prof_stop();             // prints what it sampled
    printf("%s\n", bad ? "FAILED" : "all ok");
    return bad;
}
