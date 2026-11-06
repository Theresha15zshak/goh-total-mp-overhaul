// Snapshots of the game's own memory, for replay jumps.
//
// The game allocates everything through its statically linked CRT, which uses the process heap
// (call_to_arms.exe imports HeapAlloc/HeapFree/HeapReAlloc/HeapSize and VirtualAlloc/VirtualFree, nothing
// else). That heap is shared with Direct3D, the driver, the sound library, so it cannot be put back as a
// whole. Here the exe's import slots for those functions are pointed at an allocator of ours that lives in
// one reserved range with MEM_WRITE_WATCH: all of the game's objects, and the allocator's own bookkeeping,
// sit there; DLLs keep the real heap. A snapshot is that range (pages the game wrote since the last one)
// plus the exe's .data; a jump writes back the pages that differ.
//
// Locks live outside the range so a restore never overwrites them.
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <string.h>
#include "goh_replay.h"

#define REGION_SIZE     (48ull << 30)       // address space for the game's heap
#define COMMIT_STEP     (32ull << 20)
#define PAGE            4096ull
#define SPAN_PAGES      64                  // small blocks of one class are carved from runs this long
#define SMALL_MAX       32768u              // block sizes above this get page runs of their own
#define NCLASS          64
#define EXACT_RUNS      256                 // free runs shorter than this are kept by exact length,
#define RUN_BUCKETS     (EXACT_RUNS + 24)   // longer ones by power of two
#define HDR             16u
#define LARGE           0xffffu
#define MAGIC           0x6f68u             // marks our block headers
#define TAG_USED        (0x5553ull << 48)   // run tags: length in pages in the low bits
#define TAG_FREE        (0x4652ull << 48)
#define TAG_PAGES(t)    ((t) & 0xffffffffffull)
// Freed memory is not handed out again at once. The engine has threads that read an object a little after
// another thread freed it; under the Windows heap that goes unnoticed (freed blocks keep their bytes for a
// while and come back in no set order), but a heap that reused the newest free block at once, with its
// free-list link written over the first 8 bytes (the vtable), crashed the render job and the resource
// threads within a minute of preparing a replay. So a freed block keeps its bytes (the link lives in its
// header) and waits behind the newer ones: a class reuses its oldest free block only while more than
// QUARANTINE_BYTES (at least QUARANTINE_MIN blocks) are waiting. Large runs wait the same way.
// (Those crashes turned out to be half-drawn frames; the wait stays short: 1 MB per class doubled the
// pages each snapshot had to store.)
#define QUARANTINE_BYTES (64ull << 10)
#define QUARANTINE_MIN  16
#define LARGE_WAIT      32                  // freed large runs held back (and at most LARGE_WAIT_BYTES)
#define LARGE_WAIT_BYTES (16ull << 20)

typedef struct { uint16_t cls, magic; uint32_t pages; uint64_t size; } Hdr;   // pages: run length of a large block;
                                                                                  // size: the next free block once freed
// Page runs: everything past the bookkeeping is a row of runs, each tagged at its first and last
// 8 bytes with its length and state, so a freed run merges with free neighbours.
typedef struct Run { uint64_t tag; struct Run *prev, *next; } Run;            // the start of a free run

// bookkeeping at the start of the range: it is part of every snapshot
typedef struct {
    uint64_t top;                           // offset of the first never-used byte
    Hdr *free_small[NCLASS], *free_tail[NCLASS];   // freed blocks, oldest first
    uint64_t free_n[NCLASS];
    uint8_t *large_wait[LARGE_WAIT];        // freed large runs, oldest at large_first
    uint32_t large_first, large_n;
    uint64_t large_bytes;
    uint8_t *span_cur[NCLASS], *span_end[NCLASS];
    Run *free_runs[RUN_BUCKETS];
    int64_t live_bytes, live_blocks;
} State;

static uint8_t *g_r;                        // the range
static uint8_t *g_first;                    // first run
static State *g_st;
static uint32_t g_class_size[NCLASS];
static int g_nclass;
static uint8_t g_class_of[SMALL_MAX / 16 + 1];   // (block size / 16) -> smallest class that fits
static volatile uint64_t g_committed;
static volatile LONG g_lock_class[NCLASS], g_lock_runs;
static volatile LONG g_in_alloc;            // threads inside our heap functions right now
static HANDLE g_heap;                       // the process heap: what the exe passes to HeapAlloc
static int g_on;
static uint8_t *g_exe;

static LPVOID (WINAPI *real_HeapAlloc)(HANDLE, DWORD, SIZE_T);
static BOOL (WINAPI *real_HeapFree)(HANDLE, DWORD, LPVOID);
static LPVOID (WINAPI *real_HeapReAlloc)(HANDLE, DWORD, LPVOID, SIZE_T);
static SIZE_T (WINAPI *real_HeapSize)(HANDLE, DWORD, LPCVOID);
static LPVOID (WINAPI *real_VirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD);
static BOOL (WINAPI *real_VirtualFree)(LPVOID, SIZE_T, DWORD);

static inline void lock(volatile LONG *l) {
    for (int n = 0; InterlockedExchange(l, 1); n++) {
        if (n < 64) YieldProcessor();
        else SwitchToThread();
    }
}
static inline void unlock(volatile LONG *l) { InterlockedExchange(l, 0); }

int snap_owns(const void *p) { return g_on && (const uint8_t *)p >= g_r && (const uint8_t *)p < g_r + REGION_SIZE; }

// ---- page runs (all under g_lock_runs) ------------------------------------------------------------------
static int bucket_of(uint64_t pages) {
    if (pages < EXACT_RUNS) return (int)pages;
    int b = EXACT_RUNS;
    for (uint64_t p = pages >> 8; p > 1 && b < RUN_BUCKETS - 1; p >>= 1) b++;
    return b;
}

static void set_tags(uint8_t *run, uint64_t pages, uint64_t state) {
    *(uint64_t *)run = state | pages;
    *(uint64_t *)(run + pages * PAGE - 8) = state | pages;
}

static void list_add(uint8_t *p, uint64_t pages) {
    Run *r = (Run *)p;
    int b = bucket_of(pages);
    set_tags(p, pages, TAG_FREE);
    r->prev = NULL;
    r->next = g_st->free_runs[b];
    if (r->next) r->next->prev = r;
    g_st->free_runs[b] = r;
}

static void list_remove(Run *r) {
    if (r->prev) r->prev->next = r->next;
    else g_st->free_runs[bucket_of(TAG_PAGES(r->tag))] = r->next;
    if (r->next) r->next->prev = r->prev;
}

// n pages, tagged in use: a free run of that length, else the first longer one (split), else fresh pages
static uint8_t *take_run(uint64_t n) {
    lock(&g_lock_runs);
    Run *r = NULL;
    for (int b = bucket_of(n); b < RUN_BUCKETS && !r; b++)
        for (Run *x = g_st->free_runs[b]; x; x = x->next)
            if (TAG_PAGES(x->tag) >= n) { r = x; break; }
    uint8_t *p;
    if (r) {
        uint64_t have = TAG_PAGES(r->tag);
        list_remove(r);
        p = (uint8_t *)r;
        if (have > n) list_add(p + n * PAGE, have - n);
    } else {
        uint64_t end = g_st->top + n * PAGE;
        if (end > REGION_SIZE) { unlock(&g_lock_runs); return NULL; }
        if (end > g_committed) {
            uint64_t want = (end + COMMIT_STEP - 1) & ~(COMMIT_STEP - 1);
            if (want > REGION_SIZE) want = REGION_SIZE;
            if (!real_VirtualAlloc(g_r + g_committed, want - g_committed, MEM_COMMIT, PAGE_READWRITE)) {
                unlock(&g_lock_runs);
                return NULL;
            }
            g_committed = want;
        }
        p = g_r + g_st->top;
        g_st->top = end;
    }
    set_tags(p, n, TAG_USED);
    unlock(&g_lock_runs);
    return p;
}

// merge with free neighbours; a run that ends at the top gives its pages back to the top
static void give_run(uint8_t *p) {
    lock(&g_lock_runs);
    uint64_t pages = TAG_PAGES(*(uint64_t *)p);
    uint8_t *next = p + pages * PAGE;
    if (next < g_r + g_st->top && (*(uint64_t *)next & ~0xffffffffffull) == TAG_FREE) {
        uint64_t np = TAG_PAGES(*(uint64_t *)next);
        list_remove((Run *)next);
        pages += np;
    }
    if (p > g_first && (*(uint64_t *)(p - 8) & ~0xffffffffffull) == TAG_FREE) {
        uint64_t pp = TAG_PAGES(*(uint64_t *)(p - 8));
        p -= pp * PAGE;
        list_remove((Run *)p);
        pages += pp;
    }
    if (p + pages * PAGE == g_r + g_st->top) g_st->top = (uint64_t)(p - g_r);
    else list_add(p, pages);
    unlock(&g_lock_runs);
}

static void *alloc_small(int c, uint64_t size) {
    uint32_t bs = g_class_size[c];
    lock(&g_lock_class[c]);
    uint8_t *b = NULL;
    uint64_t wait = QUARANTINE_BYTES / bs > QUARANTINE_MIN ? QUARANTINE_BYTES / bs : QUARANTINE_MIN;
    if (g_st->free_n[c] > wait) {
        Hdr *f = g_st->free_small[c];
        g_st->free_small[c] = (Hdr *)(uintptr_t)f->size;
        if (!g_st->free_small[c]) g_st->free_tail[c] = NULL;
        g_st->free_n[c]--;
        b = (uint8_t *)f;
    } else {
        if (!g_st->span_cur[c] || g_st->span_cur[c] + bs > g_st->span_end[c]) {
            // block headers stay 16-byte aligned: carve from run+16 to the end tag
            uint64_t pages = bs * 4ull > SPAN_PAGES * PAGE ? (bs * 4ull + 32 + PAGE - 1) / PAGE : SPAN_PAGES;
            uint8_t *s = take_run(pages);
            if (!s) { unlock(&g_lock_class[c]); return NULL; }
            g_st->span_cur[c] = s + 16;
            g_st->span_end[c] = s + pages * PAGE - 16;
        }
        b = g_st->span_cur[c];
        g_st->span_cur[c] += bs;
    }
    unlock(&g_lock_class[c]);
    Hdr *h = (Hdr *)b;
    h->cls = (uint16_t)c; h->magic = MAGIC; h->pages = 0; h->size = size;
    InterlockedAdd64(&g_st->live_bytes, (int64_t)size);
    InterlockedIncrement64(&g_st->live_blocks);
    return b + HDR;
}

// a page run with the data at run+32 or the first `align` boundary after it; before the data sit the
// offset back to the run start (8 bytes) and the header (16), after it the run's end tag
static void *alloc_large(uint64_t size, uint64_t align) {
    uint64_t slack = (align <= 16 ? 32 : 32 + align) + 8;
    uint64_t pages = (size + slack + PAGE - 1) / PAGE;
    if (pages > 0xffffffffull) return NULL;
    uint8_t *run = take_run(pages);
    if (!run) return NULL;
    uint8_t *data = (uint8_t *)(((uintptr_t)run + 32 + align - 1) & ~(uintptr_t)(align - 1));
    Hdr *h = (Hdr *)(data - HDR);
    h->cls = LARGE; h->magic = MAGIC; h->pages = (uint32_t)pages; h->size = size;
    ((uint64_t *)h)[-1] = (uint64_t)(data - run);
    InterlockedAdd64(&g_st->live_bytes, (int64_t)size);
    InterlockedIncrement64(&g_st->live_blocks);
    return data;
}

static void *my_alloc(uint64_t size, uint64_t align) {
    if (align <= 16 && size + HDR <= SMALL_MAX) return alloc_small(g_class_of[(size + HDR + 15) / 16], size);
    return alloc_large(size, align < 16 ? 16 : align);
}

static Hdr *hdr_of(const void *p) {
    Hdr *h = (Hdr *)((const uint8_t *)p - HDR);
    return h->magic == MAGIC && (h->cls < g_nclass || h->cls == LARGE) ? h : NULL;
}

static uint64_t capacity(const Hdr *h) {
    if (h->cls != LARGE) return g_class_size[h->cls] - HDR;
    return h->pages * PAGE - 8 - ((const uint64_t *)h)[-1];
}

// a freed large run waits; the oldest waiting one goes back to the free runs once too many wait
static void free_large(uint8_t *run) {
    uint64_t bytes = TAG_PAGES(*(uint64_t *)run) * PAGE;
    uint8_t *old[LARGE_WAIT];
    int nold = 0;
    lock(&g_lock_runs);
    while (g_st->large_n && (g_st->large_n == LARGE_WAIT || g_st->large_bytes + bytes > LARGE_WAIT_BYTES)) {
        uint8_t *o = g_st->large_wait[g_st->large_first];
        g_st->large_first = (g_st->large_first + 1) % LARGE_WAIT;
        g_st->large_n--;
        g_st->large_bytes -= TAG_PAGES(*(uint64_t *)o) * PAGE;
        old[nold++] = o;
    }
    g_st->large_wait[(g_st->large_first + g_st->large_n) % LARGE_WAIT] = run;
    g_st->large_n++;
    g_st->large_bytes += bytes;
    unlock(&g_lock_runs);
    for (int i = 0; i < nold; i++) give_run(old[i]);
}

static void my_free(void *p) {
    Hdr *h = hdr_of(p);
    if (!h) return;
    InterlockedAdd64(&g_st->live_bytes, -(int64_t)h->size);
    InterlockedDecrement64(&g_st->live_blocks);
    if (h->cls == LARGE) {
        uint8_t *run = (uint8_t *)p - ((uint64_t *)h)[-1];
        h->magic = 0;
        free_large(run);
        return;
    }
    int c = h->cls;
    lock(&g_lock_class[c]);
    h->magic = 0;
    h->size = 0;                            // the link: the block's own bytes stay as they were
    if (g_st->free_tail[c]) g_st->free_tail[c]->size = (uint64_t)(uintptr_t)h;
    else g_st->free_small[c] = h;
    g_st->free_tail[c] = h;
    g_st->free_n[c]++;
    unlock(&g_lock_class[c]);
}

// ---- the exe's heap functions ------------------------------------------------------------------------
// g_in_alloc counts callers inside, so a snapshot can wait until nobody is halfway through the heap
static LPVOID WINAPI hook_HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
    if (heap != g_heap) return real_HeapAlloc(heap, flags, size);
    InterlockedIncrement(&g_in_alloc);
    void *p = my_alloc(size, 16);
    if (p && (flags & HEAP_ZERO_MEMORY)) memset(p, 0, size);
    InterlockedDecrement(&g_in_alloc);
    if (!p) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}

static BOOL WINAPI hook_HeapFree(HANDLE heap, DWORD flags, LPVOID p) {
    if (!snap_owns(p)) return real_HeapFree(heap, flags, p);
    InterlockedIncrement(&g_in_alloc);
    my_free(p);
    InterlockedDecrement(&g_in_alloc);
    return TRUE;
}

static SIZE_T WINAPI hook_HeapSize(HANDLE heap, DWORD flags, LPCVOID p) {
    if (!snap_owns(p)) return real_HeapSize(heap, flags, p);
    Hdr *h = hdr_of(p);
    return h ? h->size : (SIZE_T)-1;
}

static LPVOID realloc_ours(DWORD flags, LPVOID p, SIZE_T size) {
    Hdr *h = hdr_of(p);
    if (!h) return NULL;
    uint64_t old = h->size;
    if (size <= capacity(h)) {
        InterlockedAdd64(&g_st->live_bytes, (int64_t)size - (int64_t)old);
        h->size = size;
        if ((flags & HEAP_ZERO_MEMORY) && size > old) memset((uint8_t *)p + old, 0, size - old);
        return p;
    }
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    void *q = my_alloc(size, 16);
    if (!q) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    memcpy(q, p, old < size ? old : size);
    if ((flags & HEAP_ZERO_MEMORY) && size > old) memset((uint8_t *)q + old, 0, size - old);
    my_free(p);
    return q;
}

static LPVOID WINAPI hook_HeapReAlloc(HANDLE heap, DWORD flags, LPVOID p, SIZE_T size) {
    if (!snap_owns(p)) return real_HeapReAlloc(heap, flags, p, size);
    InterlockedIncrement(&g_in_alloc);
    LPVOID q = realloc_ours(flags, p, size);
    InterlockedDecrement(&g_in_alloc);
    return q;
}

// the engine's page allocator asks for fresh committed pages; give them from the range, 64 KB aligned
static LPVOID WINAPI hook_VirtualAlloc(LPVOID at, SIZE_T size, DWORD type, DWORD protect) {
    if (snap_owns(at)) {                    // committing again what we handed out: already committed
        if (type & MEM_COMMIT) memset(at, 0, size);
        return at;
    }
    if (at || protect != PAGE_READWRITE || !(type & MEM_COMMIT) || (type & ~(MEM_COMMIT | MEM_RESERVE)))
        return real_VirtualAlloc(at, size, type, protect);
    InterlockedIncrement(&g_in_alloc);
    void *p = alloc_large(size, 65536);
    if (p) memset(p, 0, size);
    InterlockedDecrement(&g_in_alloc);
    return p;
}

static BOOL WINAPI hook_VirtualFree(LPVOID p, SIZE_T size, DWORD type) {
    if (!snap_owns(p)) return real_VirtualFree(p, size, type);
    InterlockedIncrement(&g_in_alloc);
    if (type & MEM_RELEASE) my_free(p);
    else if (type & MEM_DECOMMIT) memset(p, 0, size);   // contents gone, as with a real decommit
    InterlockedDecrement(&g_in_alloc);
    return TRUE;
}

// ---- setup -----------------------------------------------------------------------------------------------
static DWORD WINAPI hook_TlsAlloc(void);    // per-thread data, below
static DWORD (WINAPI *real_TlsAlloc)(void);

int snap_patch_iat(uint8_t *base, const char *dll, const char *fn, void *hook, void **real) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)base + d->Name, dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp(((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, fn)) continue;
            DWORD old;
            *real = (void *)slots->u1.Function;
            VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old);
            slots->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&slots->u1.Function, 8, old, &old);
            return 1;
        }
    }
    return 0;
}

// DllMain, before the exe's CRT starts: nothing of the game is allocated yet
int snap_install(uint8_t *exe) {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    g_exe = exe;
    real_VirtualAlloc = (void *)GetProcAddress(k32, "VirtualAlloc");
    g_r = real_VirtualAlloc(NULL, REGION_SIZE, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!g_r) return 0;
    if (!real_VirtualAlloc(g_r, COMMIT_STEP, MEM_COMMIT, PAGE_READWRITE)) return 0;
    g_committed = COMMIT_STEP;
    g_st = (State *)g_r;
    g_st->top = (sizeof(State) + PAGE - 1) & ~(PAGE - 1);
    g_first = g_r + g_st->top;
    // block sizes: 16-byte steps to 256, then four per doubling
    int n = 0;
    for (uint32_t s = 32; s <= 256; s += 16) g_class_size[n++] = s;
    for (uint32_t p = 256; p < SMALL_MAX && n < NCLASS; p *= 2)
        for (int q = 1; q <= 4 && n < NCLASS; q++) g_class_size[n++] = p + p / 4 * q;
    g_nclass = n;
    for (uint32_t i = 0, c = 0; i <= SMALL_MAX / 16; i++) {
        while (c < (uint32_t)n - 1 && g_class_size[c] < i * 16) c++;
        g_class_of[i] = (uint8_t)c;
    }
    g_heap = GetProcessHeap();
    real_HeapAlloc = (void *)GetProcAddress(k32, "HeapAlloc");
    g_on = 1;
    if (!keep_install(exe)) rc_log("snapshots: Direct3D/sound keeper incomplete, restores may crash");
    void *dummy;
    int ok = snap_patch_iat(exe, "KERNEL32.dll", "HeapAlloc", hook_HeapAlloc, (void **)&real_HeapAlloc) &
             snap_patch_iat(exe, "KERNEL32.dll", "HeapFree", hook_HeapFree, (void **)&real_HeapFree) &
             snap_patch_iat(exe, "KERNEL32.dll", "HeapReAlloc", hook_HeapReAlloc, (void **)&real_HeapReAlloc) &
             snap_patch_iat(exe, "KERNEL32.dll", "HeapSize", hook_HeapSize, (void **)&real_HeapSize) &
             snap_patch_iat(exe, "KERNEL32.dll", "VirtualAlloc", hook_VirtualAlloc, &dummy) &
             snap_patch_iat(exe, "KERNEL32.dll", "VirtualFree", hook_VirtualFree, (void **)&real_VirtualFree) &
             snap_patch_iat(exe, "KERNEL32.dll", "TlsAlloc", hook_TlsAlloc, (void **)&real_TlsAlloc);
    return ok;
}

// ---- stopping the world ---------------------------------------------------------------------------------
// A snapshot or a restore runs on the game's logic thread at the start of a frame, with every other thread
// suspended at a moment when none of them is running the game's code or is inside our heap functions.
// While they are suspended nothing here may allocate, log or take a lock another thread could hold.
static uint8_t *g_text_lo, *g_text_hi, *g_data;
static uint64_t g_data_size;
static struct { HANDLE h; DWORD tid; } g_th[1024];
static int g_nth;
static char g_busy[256];                    // why the world would not stop, for the log

static void find_sections(void) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (!memcmp(s->Name, ".text", 6)) { g_text_lo = g_exe + s->VirtualAddress; g_text_hi = g_text_lo + s->Misc.VirtualSize; }
        if (!memcmp(s->Name, ".data", 6)) { g_data = g_exe + s->VirtualAddress; g_data_size = s->Misc.VirtualSize; }
    }
}

static void resume_world(void) {
    for (int i = 0; i < g_nth; i++) { ResumeThread(g_th[i].h); CloseHandle(g_th[i].h); }
    g_nth = 0;
    prof_unlock();
}

static int stop_world(void) {
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    g_busy[0] = 0;
    HANDLE ts = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (ts == INVALID_HANDLE_VALUE) { lstrcpynA(g_busy, "no thread list", sizeof g_busy); return 0; }
    prof_lock();                            // released by resume_world
    THREADENTRY32 te = {sizeof te};
    g_nth = 0;
    for (BOOL ok = Thread32First(ts, &te); ok && g_nth < 1024; ok = Thread32Next(ts, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                              te.th32ThreadID);
        if (h) { g_th[g_nth].h = h; g_th[g_nth].tid = te.th32ThreadID; g_nth++; }
    }
    CloseHandle(ts);
    for (int i = 0; i < g_nth; i++) SuspendThread(g_th[i].h);
    DWORD busy_tid[4];
    uint32_t busy_rva[4];
    int nbusy = 0;
    for (int i = 0; i < g_nth; i++) {
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(g_th[i].h, &c)) continue;        // also waits until the suspension took hold
        uint8_t *ip = (uint8_t *)c.Rip;
        if (ip >= g_text_lo && ip < g_text_hi && nbusy < 4) { busy_tid[nbusy] = g_th[i].tid; busy_rva[nbusy++] = (uint32_t)(ip - g_exe); }
    }
    LONG in_heap = g_in_alloc;
    if (!nbusy && !in_heap) return 1;
    resume_world();
    int len = wsprintfA(g_busy, "%ld in the heap;", in_heap);
    for (int i = 0; i < nbusy; i++) len += wsprintfA(g_busy + len, " thread %lu at %x", busy_tid[i], busy_rva[i]);
    return 0;
}

// ---- the game's per-thread data ------------------------------------------------------------------------
// The exe's static TLS block (thread_local variables) of every thread and the values of the TLS slots the
// exe allocated live outside the range but point into it (a thread caches its open pak archives there:
// test 2 crashed in minizip on such a handle, opened after the snapshot). They are saved and put back
// with the heap; a thread born after the snapshot is left as it is and counted.
#define MAX_SAVED_THREADS 1024
typedef LONG (NTAPI *QueryThreadFn)(HANDLE, int, void *, ULONG, ULONG *);
static QueryThreadFn g_query_thread;
static ULONG *g_tls_index;                  // where the loader wrote the exe's TLS index
static uint8_t *g_tls_init;                 // initial values of the block
static uint64_t g_tls_raw, g_tls_size;
#define MAX_SLOTS 1088                      // what Windows allows: 64 slots in the TEB + 1024 expansion slots
static DWORD g_tls_slot[MAX_SLOTS];         // TlsAlloc indices the exe got (one per pak archive: 151 in a replay)
static volatile LONG g_ntls_slot;
typedef struct { DWORD tid; uint8_t *block; void *slot[MAX_SLOTS]; } ThreadSave;
static ThreadSave *g_tsave;
static uint8_t *g_tls_store;
static int g_ntsave, g_tls_new, g_tls_put;

static DWORD WINAPI hook_TlsAlloc(void) {
    DWORD i = real_TlsAlloc();
    if (i != TLS_OUT_OF_INDEXES) {
        LONG n = InterlockedIncrement(&g_ntls_slot) - 1;
        if (n < MAX_SLOTS) g_tls_slot[n] = i;
    }
    return i;
}

static uint8_t *teb_of(HANDLE h) {
    struct { LONG status; PVOID teb; HANDLE pid, tid; KAFFINITY affinity; LONG priority, base; } tbi;   // THREAD_BASIC_INFORMATION
    if (!g_query_thread || g_query_thread(h, 0, &tbi, sizeof tbi, NULL) < 0) return NULL;   // ThreadBasicInformation
    return tbi.teb;
}

static uint8_t *tls_block(uint8_t *teb) {
    void **arr = teb ? *(void ***)(teb + 0x58) : NULL;    // TEB.ThreadLocalStoragePointer
    return arr && g_tls_index ? arr[*g_tls_index] : NULL;
}

static void **tls_slot_ptr(uint8_t *teb, DWORD i) {
    if (i < 64) return (void **)(teb + 0x1480) + i;      // TEB.TlsSlots
    void **exp = *(void ***)(teb + 0x1780);               // TEB.TlsExpansionSlots
    return exp && i < 64 + 1024 ? exp + (i - 64) : NULL;
}

static void save_thread(DWORD tid, uint8_t *teb) {
    if (!teb || g_ntsave >= MAX_SAVED_THREADS) return;
    ThreadSave *s = &g_tsave[g_ntsave];
    s->tid = tid;
    s->block = tls_block(teb);
    if (s->block) memcpy(g_tls_store + g_ntsave * g_tls_size, s->block, g_tls_size);
    int n = g_ntls_slot < MAX_SLOTS ? g_ntls_slot : MAX_SLOTS;
    for (int i = 0; i < n; i++) { void **p = tls_slot_ptr(teb, g_tls_slot[i]); s->slot[i] = p ? *p : NULL; }
    g_ntsave++;
}

static void put_thread(DWORD tid, uint8_t *teb) {
    if (!teb) return;
    for (int k = 0; k < g_ntsave; k++) {
        ThreadSave *s = &g_tsave[k];
        if (s->tid != tid) continue;
        uint8_t *block = tls_block(teb);
        if (block && block == s->block) memcpy(block, g_tls_store + k * g_tls_size, g_tls_size);
        int n = g_ntls_slot < MAX_SLOTS ? g_ntls_slot : MAX_SLOTS;
        for (int i = 0; i < n; i++) { void **p = tls_slot_ptr(teb, g_tls_slot[i]); if (p) *p = s->slot[i]; }
        g_tls_put++;
        return;
    }
    g_tls_new++;
}

// all threads, the world stopped: this one and the suspended ones
static void save_threads(void) {
    g_ntsave = 0;
    if (!g_tsave) return;
    save_thread(GetCurrentThreadId(), (uint8_t *)NtCurrentTeb());
    for (int i = 0; i < g_nth; i++) save_thread(g_th[i].tid, teb_of(g_th[i].h));
}

static void put_threads(void) {
    g_tls_new = g_tls_put = 0;
    if (!g_tsave) return;
    put_thread(GetCurrentThreadId(), (uint8_t *)NtCurrentTeb());
    for (int i = 0; i < g_nth; i++) put_thread(g_th[i].tid, teb_of(g_th[i].h));
}

static void find_tls(void) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dir.VirtualAddress) return;
    IMAGE_TLS_DIRECTORY64 *t = (IMAGE_TLS_DIRECTORY64 *)(g_exe + dir.VirtualAddress);
    g_tls_index = (ULONG *)t->AddressOfIndex;
    g_tls_init = (uint8_t *)t->StartAddressOfRawData;
    g_tls_raw = t->EndAddressOfRawData - t->StartAddressOfRawData;
    g_tls_size = g_tls_raw + t->SizeOfZeroFill;
    g_query_thread = (QueryThreadFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
}

// ---- the game's threads themselves ------------------------------------------------------------------------
// A parked worker keeps heap pointers in its registers and on its stack: the async pather swaps its own task
// list with the shared queue every round (8be020: r15 <-> [manager+0xc0]), so after a restore both pointed at
// one list, whose tasks then ran and were freed twice (test 5: crashes in async_pather_manager and the quant
// thread 1.4 s after a jump). So a snapshot also keeps, for every thread that runs the game's code, its
// registers and the used part of its stack, and a restore puts them back into a thread parked at the very
// same spot (same rip and rsp) - there that is exactly right: it is in the same wait, which returns as before.
// Threads of this DLL and threads that never run the game's code are left alone.
typedef struct { DWORD tid; uint32_t stack_len; uint64_t rip, rsp; CONTEXT ctx; } ThreadState;   // stack bytes follow
#define MAX_STACK_SAVE  (4u << 20)
#define MAX_HOME        512
static uint8_t *g_ts_buf;                   // ThreadState records and their stacks, back to back
static uint64_t g_ts_len, g_ts_cap;
static uint8_t *g_self_lo, *g_self_hi;      // this DLL
static struct { DWORD tid; uint64_t rip, rsp; } g_home[MAX_HOME];   // where each game thread sat at the last snapshot
static int g_nhome;

static int ensure(void *base, uint64_t *committed, uint64_t need, uint64_t step);

static void find_self(void) {
    HMODULE m = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)find_self, &m);
    if (!m) return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    g_self_lo = (uint8_t *)m;
    g_self_hi = g_self_lo + nt->OptionalHeader.SizeOfImage;
}

static uintptr_t start_address(HANDLE h) {
    uintptr_t a = 0;
    if (g_query_thread) g_query_thread(h, 9, &a, sizeof a, NULL);   // ThreadQuerySetWin32StartAddress
    return a;
}

// a return address into the game's code on the stack: the thread runs game code
static int runs_game(const uint8_t *stack, uint64_t len) {
    for (uint64_t o = 0; o + 8 <= len; o += 8) {
        const uint8_t *q = *(const uint8_t *const *)(stack + o);
        if (q < g_text_lo + 6 || q >= g_text_hi) continue;
        if (q[-5] == 0xe8 || (q[-6] == 0xff && (q[-5] & 0x38) == 0x10) || (q[-3] == 0xff && (q[-2] & 0x38) == 0x10) ||
            (q[-2] == 0xff && (q[-1] & 0x38) == 0x10))
            return 1;
    }
    return 0;
}

static ThreadState *ts_next(ThreadState *t) {
    return (ThreadState *)((uint8_t *)t + ((sizeof(ThreadState) + t->stack_len + 15) & ~15ull));
}

// world stopped: registers and stacks of the game's threads into g_ts_buf; how many sit away from home
static int save_stacks(char *moved, int cap) {
    g_ts_len = 0;
    int away = 0, len = 0;
    moved[0] = 0;
    for (int i = 0; i < g_nth; i++) {
        uint8_t *sa = (uint8_t *)start_address(g_th[i].h);
        if (sa >= g_self_lo && sa < g_self_hi) continue;
        CONTEXT c;
        c.ContextFlags = CONTEXT_FULL | CONTEXT_FLOATING_POINT;
        uint8_t *teb = teb_of(g_th[i].h);
        if (!teb || !GetThreadContext(g_th[i].h, &c)) continue;
        uint64_t base = *(uint64_t *)(teb + 8), rsp = c.Rsp;                 // TEB.NtTib.StackBase
        if (rsp >= base || base - rsp > MAX_STACK_SAVE || !runs_game((uint8_t *)rsp, base - rsp)) continue;
        uint64_t rec = (sizeof(ThreadState) + (base - rsp) + 15) & ~15ull;
        if (!ensure(g_ts_buf, &g_ts_cap, g_ts_len + rec, 1 << 20)) break;
        ThreadState *t = (ThreadState *)(g_ts_buf + g_ts_len);
        t->tid = g_th[i].tid; t->stack_len = (uint32_t)(base - rsp); t->rip = c.Rip; t->rsp = rsp;
        memcpy(&t->ctx, &c, sizeof c);
        memcpy(t + 1, (void *)rsp, t->stack_len);
        g_ts_len += rec;
        for (int h = 0; h < g_nhome; h++)
            if (g_home[h].tid == t->tid && (g_home[h].rip != t->rip || g_home[h].rsp != t->rsp)) {
                away++;
                if (len < cap - 40) len += wsprintfA(moved + len, " %lu@%llx", t->tid, (unsigned long long)(t->rip - (uintptr_t)g_exe));
            }
    }
    return away;
}

static void set_home(void) {
    g_nhome = 0;
    for (ThreadState *t = (ThreadState *)g_ts_buf; (uint8_t *)t < g_ts_buf + g_ts_len && g_nhome < MAX_HOME; t = ts_next(t)) {
        g_home[g_nhome].tid = t->tid; g_home[g_nhome].rip = t->rip; g_home[g_nhome].rsp = t->rsp;
        g_nhome++;
    }
}

static HANDLE thread_handle(DWORD tid) {
    for (int i = 0; i < g_nth; i++) if (g_th[i].tid == tid) return g_th[i].h;
    return NULL;
}

// world stopped, g_ts_buf = the target snapshot's threads: how many are parked elsewhere now (or gone)
static int threads_away(char *moved, int cap) {
    int away = 0, len = 0;
    moved[0] = 0;
    for (ThreadState *t = (ThreadState *)g_ts_buf; (uint8_t *)t < g_ts_buf + g_ts_len; t = ts_next(t)) {
        HANDLE h = thread_handle(t->tid);
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        if (h && GetThreadContext(h, &c) && c.Rip == t->rip && c.Rsp == t->rsp) continue;
        away++;
        if (len < cap - 40) len += wsprintfA(moved + len, " %lu@%llx", t->tid, (unsigned long long)(t->rip - (uintptr_t)g_exe));
    }
    return away;
}

// world stopped: registers and stacks back into the threads parked where they were; how many
static int put_stacks(void) {
    int put = 0;
    for (ThreadState *t = (ThreadState *)g_ts_buf; (uint8_t *)t < g_ts_buf + g_ts_len; t = ts_next(t)) {
        HANDLE h = thread_handle(t->tid);
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        if (!h || !GetThreadContext(h, &c) || c.Rip != t->rip || c.Rsp != t->rsp) continue;
        memcpy((void *)t->rsp, t + 1, t->stack_len);
        memcpy(&c, &t->ctx, sizeof c);
        c.ContextFlags = CONTEXT_FULL | CONTEXT_FLOATING_POINT;
        put += SetThreadContext(h, &c) != 0;
    }
    return put;
}

// ---- compact snapshots ------------------------------------------------------------------------------------
// A session (one replay) keeps a chain of snapshots. Snapshot 0 is the base: every page of the range,
// compressed by helper threads while the game goes on. It cannot be restored (pages written meanwhile are
// torn), but snapshot 1 holds every page written since the base began, so from 1 on all are exact.
// Snapshot k >= 1 holds the pages that may differ from snapshot k-1: copied with the world stopped for a
// moment, compressed by the helpers afterwards.
// Every page has a list of versions, newest first. A version is the page compressed, or the XOR with an
// older full version of the same page (its key) compressed: a page of game state changes in a few places,
// so that is mostly zeros (the codec takes such a page to ~2%).
// The game's memory is always "snapshot g_cur plus the pages written since" (write watch). A restore to j
// writes back those pages plus every page that changed between g_cur and j.
#define MAX_SNAPS       4096
#define REGION_PAGES    (REGION_SIZE / PAGE)
#define DATA_PAGES_MAX  1024                // .data is 0x13d930 bytes: 318 pages
#define ALL_PAGES       (REGION_PAGES + DATA_PAGES_MAX)
#define ARENA_CHUNK     (256ull << 20)
#define MAX_CHUNKS      256
#define XOR_KEEP        1024                // a XOR version bigger than this starts a new full version
#define HELPERS         2                   // two, under the game's own threads: a replay on a 6 core CPU stuttered with four
#define JOB_CLOSED      0x40000000          // far below the top: helpers keep adding to it, and past 2^31 it would turn into a page number
#define MAX_VERSIONS    (64ull << 20)

uint32_t lz_pack(const uint8_t *src, uint32_t n, uint8_t *dst);
int lz_unpack(const uint8_t *src, uint32_t sn, uint8_t *dst, uint32_t dn);

enum { V_ZERO, V_RAW, V_LZ, V_XOR };
typedef struct { uint32_t snap, prev, key; uint16_t size; uint8_t kind, pad; uint64_t at; } Ver;
typedef struct {
    int ms; uint64_t top, list; uint32_t npages; uint64_t tls_at; uint32_t tls_size, tls_raw; int nthreads;
    uint64_t ts_at; uint32_t ts_size, ts_raw;   // the game's threads: registers and stacks
} SnapRec;

// compressed pages live in a temporary file mapped in chunks: a whole replay is ~10 GB, and Windows keeps
// in memory only what it has room for (file pages are cheap to drop); the file goes when the game closes
static uint8_t *g_chunk[MAX_CHUNKS];
static HANDLE g_chunk_map[MAX_CHUNKS], g_arena_file = INVALID_HANDLE_VALUE;
static uint64_t g_arena_used;
static volatile LONG g_arena_lock, g_ver_lock;
static Ver *g_ver;                          // version 0 means none
static volatile LONG g_nver;
static uint64_t g_ver_committed;
static uint32_t *g_head;                    // newest version of every page (region pages, then .data pages)
static uint32_t *g_lists;                   // page numbers of every snapshot, back to back
static uint64_t g_nlist, g_lists_committed;
static SnapRec g_snaps[MAX_SNAPS];
static volatile LONG g_nsnap;               // complete snapshots, the base included
static int g_cur = -1;                      // what the game's memory derives from
static int g_sess;                          // 0 none, 1 base being made, 2 snapshots usable
static uint64_t *g_bits;                    // a set of pages
static ULONG_PTR *g_dirty;                  // GetWriteWatch output
static uint32_t *g_stage_pages;             // pages copied at the last snapshot
static uint8_t *g_stage, *g_stage_data, *g_data_shadow, *g_tls_blob, *g_pack;
static uint64_t g_pack_cap;
static uint64_t g_stage_committed, g_data_bytes;
static uint32_t g_data_pages;
static DWORD g_base_t0;
static double g_stall_ms;                   // how long the last snapshot stopped the game

// helpers: a job is a range of items (pages) taken in batches by whoever is free
static struct { volatile LONG next, left, working; LONG n; uint32_t snap; int base; } g_job;
static HANDLE g_job_sem;
static int g_job_pending;                   // a delta job whose snapshot still has to be finished

static void *big(uint64_t bytes) { return real_VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
static void *reserve(uint64_t bytes) { return real_VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_READWRITE); }

static int ensure(void *base, uint64_t *committed, uint64_t need, uint64_t step) {
    if (need <= *committed) return 1;
    uint64_t want = (need + step - 1) / step * step;
    if (!real_VirtualAlloc((uint8_t *)base + *committed, want - *committed, MEM_COMMIT, PAGE_READWRITE)) return 0;
    *committed = want;
    return 1;
}

static int map_chunk(uint64_t c) {
    if (g_arena_file == INVALID_HANDLE_VALUE) {
        char dir[MAX_PATH], path[MAX_PATH];
        GetTempPathA(MAX_PATH, dir);
        wsprintfA(path, "%sgoh_snapshots_%lu.bin", dir, GetCurrentProcessId());
        g_arena_file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        rc_log("snapshots: compressed data in %s%s", path, g_arena_file == INVALID_HANDLE_VALUE ? " - could not create it, using memory" : "");
    }
    if (g_arena_file != INVALID_HANDLE_VALUE) {
        uint64_t end = (c + 1) * ARENA_CHUNK;
        g_chunk_map[c] = CreateFileMappingA(g_arena_file, NULL, PAGE_READWRITE, (DWORD)(end >> 32), (DWORD)end, NULL);
        if (g_chunk_map[c])
            g_chunk[c] = MapViewOfFile(g_chunk_map[c], FILE_MAP_ALL_ACCESS, (DWORD)((c * ARENA_CHUNK) >> 32), (DWORD)(c * ARENA_CHUNK), ARENA_CHUNK);
        if (g_chunk[c]) return 1;
        if (g_chunk_map[c]) { CloseHandle(g_chunk_map[c]); g_chunk_map[c] = NULL; }
    }
    g_chunk[c] = big(ARENA_CHUNK);          // no file: memory, as before
    return g_chunk[c] != NULL;
}

static uint64_t arena_put(const void *src, uint32_t n) {
    lock(&g_arena_lock);
    uint64_t at = g_arena_used;
    if (at / ARENA_CHUNK != (at + n - 1) / ARENA_CHUNK) at = (at / ARENA_CHUNK + 1) * ARENA_CHUNK;   // never across chunks
    uint64_t c = at / ARENA_CHUNK;
    if (c >= MAX_CHUNKS || (!g_chunk[c] && !map_chunk(c))) { unlock(&g_arena_lock); return ~0ull; }
    g_arena_used = at + n;
    unlock(&g_arena_lock);
    memcpy(g_chunk[c] + at % ARENA_CHUNK, src, n);
    return at;
}

static uint8_t *arena_at(uint64_t at) { return g_chunk[at / ARENA_CHUNK] + at % ARENA_CHUNK; }

static uint32_t ver_new(void) {
    LONG v = InterlockedIncrement(&g_nver);
    if ((uint64_t)v >= MAX_VERSIONS) return 0;
    if ((uint64_t)(v + 1) * sizeof(Ver) > g_ver_committed) {
        lock(&g_ver_lock);
        int ok = ensure(g_ver, &g_ver_committed, (uint64_t)(v + 1) * sizeof(Ver), 32ull << 20);
        unlock(&g_ver_lock);
        if (!ok) return 0;
    }
    return (uint32_t)v;
}

static int is_zero(const uint8_t *p) {
    const uint64_t *q = (const uint64_t *)p;
    for (int i = 0; i < (int)(PAGE / 8); i++) if (q[i]) return 0;
    return 1;
}

static int decode(uint32_t v, uint8_t *dst) {
    Ver *x = &g_ver[v];
    switch (x->kind) {
    case V_ZERO: memset(dst, 0, PAGE); return 1;
    case V_RAW: memcpy(dst, arena_at(x->at), PAGE); return 1;
    case V_LZ: return lz_unpack(arena_at(x->at), x->size, dst, PAGE);
    case V_XOR: {
        uint64_t tmp[PAGE / 8];
        if (!decode(x->key, dst) || !lz_unpack(arena_at(x->at), x->size, (uint8_t *)tmp, PAGE)) return 0;
        for (int i = 0; i < (int)(PAGE / 8); i++) ((uint64_t *)dst)[i] ^= tmp[i];
        return 1;
    }
    }
    return 0;
}

// a new version of page p, newest in its list; tmp and out are the caller's scratch (PAGE and PAGE + 64)
static int encode(uint32_t p, uint32_t snap, const uint8_t *bytes, uint8_t *tmp, uint8_t *out) {
    uint32_t v = ver_new();
    if (!v) return 0;
    Ver *n = &g_ver[v];
    n->snap = snap; n->prev = g_head[p]; n->key = v; n->size = 0; n->at = 0; n->kind = V_ZERO;
    if (!is_zero(bytes)) {
        uint32_t key = n->prev ? g_ver[n->prev].key : 0, c = 0;
        const uint8_t *data = out;
        if (key && g_ver[key].kind != V_ZERO && decode(key, tmp)) {
            for (int i = 0; i < (int)(PAGE / 8); i++) ((uint64_t *)tmp)[i] ^= ((const uint64_t *)bytes)[i];
            c = lz_pack(tmp, PAGE, out);
            if (c <= XOR_KEEP) { n->kind = V_XOR; n->key = key; }
        }
        if (n->kind != V_XOR) {
            c = lz_pack(bytes, PAGE, out);
            if (c >= PAGE) { n->kind = V_RAW; c = PAGE; data = bytes; } else n->kind = V_LZ;
        }
        n->size = (uint16_t)c;
        n->at = arena_put(data, c);
        if (n->at == ~0ull) return 0;
    }
    g_head[p] = v;
    return 1;
}

static uint32_t ver_at(uint32_t p, int j) {
    uint32_t v = g_head[p];
    while (v && (int)g_ver[v].snap > j) v = g_ver[v].prev;
    return v;
}

static DWORD WINAPI helper(LPVOID _) {
    (void)_;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    uint8_t tmp[PAGE], out[PAGE + 64];
    for (;;) {
        WaitForSingleObject(g_job_sem, INFINITE);
        InterlockedIncrement(&g_job.working);
        for (;;) {
            LONG i = InterlockedExchangeAdd(&g_job.next, 64);
            if (i < 0 || i >= g_job.n) break;
            LONG e = i + 64 < g_job.n ? i + 64 : g_job.n;
            for (LONG k = i; k < e; k++) {
                if (g_job.base) encode((uint32_t)k, 0, g_r + (uint64_t)k * PAGE, tmp, out);
                else encode(g_stage_pages[k], g_job.snap, g_stage + (uint64_t)k * PAGE, tmp, out);
            }
            InterlockedExchangeAdd(&g_job.left, -(e - i));
        }
        InterlockedDecrement(&g_job.working);
    }
    return 0;
}

static void post_job(int base, uint32_t snap, LONG n) {
    g_job.next = JOB_CLOSED;                // closed while it is filled in
    MemoryBarrier();
    g_job.n = n; g_job.snap = snap; g_job.base = base; g_job.left = n;
    MemoryBarrier();
    g_job.next = 0;
    ReleaseSemaphore(g_job_sem, HELPERS, NULL);
}

static int job_busy(void) { return g_job.left > 0 || g_job.working > 0; }

static void bits_clear(void) { memset(g_bits, 0, (ALL_PAGES + 63) / 64 * 8); }
static void bit_set(uint64_t p) { g_bits[p >> 6] |= 1ull << (p & 63); }

// the pages of the snapshots after `from` up to `to`
static void mark_between(int from, int to) {
    for (int k = from + 1; k <= to; k++)
        for (uint32_t i = 0; i < g_snaps[k].npages; i++) bit_set(g_lists[g_snaps[k].list + i]);
}

const char *snap_busy(void) { return g_busy; }

int snap_ready(void) { return g_sess == 2; }

// The player's switch; and a replay too big for the memory free gets no snapshots: the store is about three quarters of the game's
// memory, in a file the system caches. With 16 GB and a 3 vs 3 of 8 GB that left nothing, and the replay crawled (2026-11-06).
static int g_rewind = 1, g_refused, g_low_logged;
static uint64_t g_stage_warm;               // bytes of g_stage touched, ahead of the first snapshot
static DWORD g_first_at;                    // the first snapshot waits for the base's writes to settle
#define STAGE_WARM      (512ull << 20)
void snap_set_rewind(int on) { g_rewind = on; }
int snap_rewind(void) { return g_rewind; }
void snap_new_replay(void) { g_refused = 0; g_low_logged = 0; }
static uint64_t memory_free(void) {
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? m.ullAvailPhys : ~0ull;
}

// replay over: everything goes
void snap_session_end(void) {
    if (!g_sess && !g_ver) return;
    g_job.next = JOB_CLOSED;
    while (g_job.working) Sleep(1);
    for (int c = 0; c < MAX_CHUNKS; c++) {
        if (g_chunk_map[c]) { UnmapViewOfFile(g_chunk[c]); CloseHandle(g_chunk_map[c]); }
        else if (g_chunk[c]) real_VirtualFree(g_chunk[c], 0, MEM_RELEASE);
        g_chunk[c] = NULL; g_chunk_map[c] = NULL;
    }
    if (g_arena_file != INVALID_HANDLE_VALUE) { CloseHandle(g_arena_file); g_arena_file = INVALID_HANDLE_VALUE; }
    if (g_ver) real_VirtualFree(g_ver, 0, MEM_RELEASE);
    if (g_head) real_VirtualFree(g_head, 0, MEM_RELEASE);
    if (g_lists) real_VirtualFree(g_lists, 0, MEM_RELEASE);
    if (g_stage) real_VirtualFree(g_stage, 0, MEM_RELEASE);
    g_ver = NULL; g_head = NULL; g_lists = NULL; g_stage = NULL;
    g_arena_used = 0; g_nver = 0; g_ver_committed = 0; g_nlist = 0; g_lists_committed = 0; g_stage_committed = 0; g_stage_warm = 0;
    g_nsnap = 0; g_cur = -1; g_sess = 0; g_job_pending = 0; g_job.left = 0; g_job.n = 0;
    g_nhome = 0;
    keep_end();
    rc_log("snapshots: session over, memory released");
}

// logic thread, a replay is on: snapshot 0 starts (1 = started)
int snap_session_start(int game_ms) {
    if (!g_on || g_sess) return g_sess != 0;
    if (!g_rewind || g_refused) return 0;
    uint64_t game = g_st->top, free_now = memory_free(), need = game / 4 * 3 + (1ull << 30);
    if (free_now < need) {
        g_refused = 1;
        lstrcpynA(g_busy, "not enough free memory for snapshots", sizeof g_busy);
        rc_log("snapshots: off for this replay: game %llu MB, free %llu MB, they would take %llu MB", (unsigned long long)(game >> 20),
               (unsigned long long)(free_now >> 20), (unsigned long long)(need >> 20));
        return 0;
    }
    if (!g_data) find_sections();
    if (!g_tls_index) find_tls();
    if (!g_job_sem) {
        g_job_sem = CreateSemaphoreA(NULL, 0, 0x7fffffff, NULL);
        for (int i = 0; i < HELPERS; i++) CloseHandle(CreateThread(NULL, 0, helper, NULL, 0, NULL));
        g_bits = big((ALL_PAGES + 63) / 64 * 8);
        g_dirty = big(REGION_PAGES * sizeof(ULONG_PTR));
        g_stage_pages = big(ALL_PAGES * sizeof(uint32_t));
        g_data_bytes = (g_data_size + PAGE - 1) & ~(PAGE - 1);
        g_data_pages = (uint32_t)(g_data_bytes / PAGE);
        g_stage_data = big(g_data_bytes);
        g_data_shadow = big(g_data_bytes);
        g_tsave = big(MAX_SAVED_THREADS * sizeof(ThreadSave));
        g_tls_store = g_tls_size ? big(MAX_SAVED_THREADS * g_tls_size) : NULL;
        g_tls_blob = big(2 * (MAX_SAVED_THREADS * (sizeof(ThreadSave) + g_tls_size)) + 4096);
        g_ts_buf = reserve(256ull << 20);
        g_pack = reserve(512ull << 20);
        find_self();
    }
    if (!g_ver) {                           // kept between tries while the world will not stop
        g_ver = reserve(MAX_VERSIONS * sizeof(Ver));
        g_head = big(ALL_PAGES * sizeof(uint32_t));
        g_lists = reserve(1ull << 32);
        g_stage = reserve(REGION_SIZE);
    }
    if (!g_bits || !g_dirty || !g_stage_pages || !g_stage_data || !g_data_shadow || !g_tsave || !g_tls_blob || !g_ts_buf || !g_pack ||
        !g_ver || !g_head ||
        !g_lists || !g_stage || g_data_pages > DATA_PAGES_MAX) {
        lstrcpynA(g_busy, "no memory for the snapshot store", sizeof g_busy);
        return 0;
    }
    if (!stop_world()) return 0;
    g_sess = 1;
    ResetWriteWatch(g_r, g_committed);
    uint64_t top = g_st->top;
    memcpy(g_data_shadow, g_data, g_data_bytes);
    resume_world();
    uint8_t tmp[PAGE], out[PAGE + 64];
    for (uint32_t d = 0; d < g_data_pages; d++) encode((uint32_t)REGION_PAGES + d, 0, g_data_shadow + (uint64_t)d * PAGE, tmp, out);
    g_snaps[0].ms = game_ms; g_snaps[0].top = top; g_snaps[0].list = 0; g_snaps[0].npages = 0;
    g_cur = 0;
    g_base_t0 = GetTickCount();
    post_job(1, 0, (LONG)(top / PAGE));
    rc_log("snapshots: session at %d ms, compressing %llu MB of game memory in the background", game_ms,
           (unsigned long long)(top >> 20));
    return 1;
}

// the helpers are done with snapshot k's pages: .data, per-thread data, page list
static void finish_snapshot(void) {
    int k = g_nsnap;
    SnapRec *s = &g_snaps[k];
    uint8_t tmp[PAGE], out[PAGE + 64];
    uint32_t n = s->npages;                 // pages the helpers did
    for (uint32_t d = 0; d < g_data_pages; d++) {
        uint8_t *now = g_stage_data + (uint64_t)d * PAGE, *was = g_data_shadow + (uint64_t)d * PAGE;
        if (!memcmp(now, was, PAGE)) continue;
        encode((uint32_t)REGION_PAGES + d, k, now, tmp, out);
        memcpy(was, now, PAGE);
        g_stage_pages[n++] = (uint32_t)REGION_PAGES + d;
    }
    if (ensure(g_lists, &g_lists_committed, (g_nlist + n) * sizeof(uint32_t), 16ull << 20)) {
        memcpy(g_lists + g_nlist, g_stage_pages, n * sizeof(uint32_t));
        s->list = g_nlist; s->npages = n;
        g_nlist += n;
    } else s->npages = 0;
    uint64_t raw = (uint64_t)s->nthreads * sizeof(ThreadSave), raw2 = (uint64_t)s->nthreads * g_tls_size;
    uint8_t *packed = g_tls_blob + raw + raw2;
    memcpy(g_tls_blob, g_tsave, raw);
    if (raw2) memcpy(g_tls_blob + raw, g_tls_store, raw2);
    uint32_t c = lz_pack(g_tls_blob, (uint32_t)(raw + raw2), packed);
    s->tls_raw = (uint32_t)(raw + raw2);
    s->tls_size = c;
    s->tls_at = arena_put(packed, c);
    s->ts_raw = (uint32_t)g_ts_len;
    s->ts_size = 0;
    if (g_ts_len && ensure(g_pack, &g_pack_cap, g_ts_len + g_ts_len / 255 + 64, 1 << 20)) {
        s->ts_size = lz_pack(g_ts_buf, (uint32_t)g_ts_len, g_pack);
        s->ts_at = arena_put(g_pack, s->ts_size);
    }
    g_cur = k;
    g_nsnap = k + 1;
    g_job_pending = 0;
}

static uint64_t store_mb(void) { return (g_arena_used + (uint64_t)g_nver * sizeof(Ver) + g_nlist * 4) >> 20; }

// logic thread, every frame of a replay: the base finishing, snapshots every SNAP_EVERY_MS of game time
void snap_tick(int game_ms) {
    // the first snapshot copies a lot with the game stopped (4.3 s in a big match, 2026-11-06): its pages are made real ahead of it,
    // a little each frame, while the base is at work
    if (g_sess == 1 && g_stage_warm < STAGE_WARM && ensure(g_stage, &g_stage_committed, g_stage_warm + (16ull << 20), 64ull << 20)) {
        for (uint64_t at = g_stage_warm; at < g_stage_warm + (16ull << 20); at += PAGE) g_stage[at] = 0;
        g_stage_warm += 16ull << 20;
    }
    if (!g_sess || job_busy()) return;
    if (g_sess == 1) {
        g_nsnap = 1;
        g_sess = 2;
        g_first_at = GetTickCount() + 5000;
        rc_log("snapshots: base of %llu MB done in %.1f s, stored in %llu MB", (unsigned long long)(g_snaps[0].top >> 20),
               (GetTickCount() - g_base_t0) / 1000.0, (unsigned long long)store_mb());
    }
    if (g_job_pending) {
        finish_snapshot();
        SnapRec *s = &g_snaps[g_nsnap - 1];
        rc_log("snapshots: #%ld at %d ms, %u pages, game stopped %.0f ms, store %llu MB", g_nsnap - 1, s->ms, s->npages, g_stall_ms,
               (unsigned long long)store_mb());
        return;
    }
    // the first one right after the base: it makes everything the base caught half-written exact
    if (g_nsnap >= MAX_SNAPS || (g_nsnap > 1 && game_ms < g_snaps[g_nsnap - 1].ms + SNAP_EVERY_MS)) return;
    if (g_nsnap == 1 && (LONG)(GetTickCount() - g_first_at) < 0) return;
    if (memory_free() < (768ull << 20)) {   // what is kept stays; no more of it
        if (!g_low_logged++) rc_log("snapshots: memory is short, no more of them for now");
        return;
    }
    int k = g_nsnap;
    bits_clear();
    if (g_cur != k - 1) mark_between(g_cur, k - 1);   // after a restore: whatever differs from the last snapshot
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    if (!stop_world()) return;
    static int tries;
    char moved[160];
    if (save_stacks(moved, sizeof moved) && tries++ < 60) {  // a game thread is busy somewhere else: a calmer frame
        resume_world();
        wsprintfA(g_busy, "threads away from home:%s", moved);
        return;
    }
    tries = 0;
    ULONG_PTR nd = REGION_PAGES;
    DWORD gran;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_r, g_committed, (PVOID *)g_dirty, &nd, &gran)) nd = 0;
    for (ULONG_PTR i = 0; i < nd; i++) bit_set((uint64_t)((uint8_t *)g_dirty[i] - g_r) / PAGE);
    uint64_t top = g_st->top, tp = top / PAGE;
    LONG n = 0;
    int ok = 1;
    for (uint64_t w = 0; w < (tp + 63) / 64 && ok; w++) {
        for (uint64_t b = g_bits[w]; b; b &= b - 1) {
            uint64_t p = w * 64 + __builtin_ctzll(b);
            if (p >= tp) break;
            if (!ensure(g_stage, &g_stage_committed, (uint64_t)(n + 1) * PAGE, 64ull << 20)) { ok = 0; break; }
            memcpy(g_stage + (uint64_t)n * PAGE, g_r + p * PAGE, PAGE);
            g_stage_pages[n++] = (uint32_t)p;
        }
    }
    memcpy(g_stage_data, g_data, g_data_bytes);
    save_threads();
    if (ok) keep_begin();                   // objects alive now may be pointed at by this snapshot
    if (ok) set_home();
    resume_world();
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
    g_stall_ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    if (!ok) { lstrcpynA(g_busy, "no memory to stage a snapshot", sizeof g_busy); return; }
    SnapRec *s = &g_snaps[k];
    s->ms = game_ms; s->top = top; s->npages = (uint32_t)n; s->nthreads = g_ntsave;
    g_job_pending = 1;
    post_job(0, (uint32_t)k, n);
}

// the latest usable snapshot at or before target_ms, -1 if none
static int snap_find(int target_ms) {
    if (g_sess != 2) return -1;
    for (int k = g_nsnap - 1; k >= 1; k--) if (g_snaps[k].ms <= target_ms) return k;
    return -1;
}

int snap_best(int target_ms) { int k = snap_find(target_ms); return k < 0 ? -1 : g_snaps[k].ms; }

void snap_info(int *from_ms, int *to_ms, int *mb, int *state) {
    *state = g_sess ? g_sess : !g_rewind ? 4 : g_refused ? 3 : 0;      // 3 not enough memory, 4 turned off
    *from_ms = g_sess == 2 && g_nsnap > 1 ? g_snaps[1].ms : -1;
    *to_ms = g_sess == 2 && g_nsnap > 1 ? g_snaps[g_nsnap - 1].ms : -1;
    *mb = g_sess ? (int)store_mb() : 0;
}

// logic thread: the game back at the latest snapshot at or before target_ms. Its time, -1 = not now (try
// again next frame), -2 = no such snapshot
int snap_restore_to(int target_ms, int *pages_out) {
    int j = snap_find(target_ms);
    if (j < 0) return -2;
    if (job_busy() || g_job_pending) return -1;
    static int waiting_for = -1;
    static DWORD since;
    if (waiting_for != j) { waiting_for = j; since = GetTickCount(); }
    SnapRec *s = &g_snaps[j];
    g_ts_len = 0;
    if (s->ts_size && ensure(g_ts_buf, &g_ts_cap, s->ts_raw, 1 << 20) && lz_unpack(arena_at(s->ts_at), s->ts_size, g_ts_buf, s->ts_raw))
        g_ts_len = s->ts_raw;
    bits_clear();
    mark_between(g_cur < j ? g_cur : j, g_cur < j ? j : g_cur);
    if (!stop_world()) return -1;
    char moved[160];
    int away = threads_away(moved, sizeof moved);
    if (away && GetTickCount() - since < 3000) {   // a game thread is busy elsewhere: wait for it, up to 3 s
        resume_world();
        wsprintfA(g_busy, "%d threads away from where they were:%s", away, moved);
        return -1;
    }
    ULONG_PTR nd = REGION_PAGES;
    DWORD gran;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_r, g_committed, (PVOID *)g_dirty, &nd, &gran)) nd = 0;
    for (ULONG_PTR i = 0; i < nd; i++) bit_set((uint64_t)((uint8_t *)g_dirty[i] - g_r) / PAGE);
    uint64_t tp = g_snaps[j].top / PAGE;
    int put = 0, bad = 0;
    for (uint64_t w = 0; w < (tp + 63) / 64; w++) {
        for (uint64_t b = g_bits[w]; b; b &= b - 1) {
            uint64_t p = w * 64 + __builtin_ctzll(b);
            if (p >= tp) break;
            uint32_t v = ver_at((uint32_t)p, j);
            if (v) bad += !decode(v, g_r + p * PAGE);
            else memset(g_r + p * PAGE, 0, PAGE);
            put++;
        }
    }
    for (uint32_t d = 0; d < g_data_pages; d++) {
        uint32_t v = ver_at((uint32_t)REGION_PAGES + d, j);
        if (v) bad += !decode(v, g_data + (uint64_t)d * PAGE);
    }
    uint64_t raw = (uint64_t)s->nthreads * sizeof(ThreadSave);
    if (lz_unpack(arena_at(s->tls_at), s->tls_size, g_tls_blob, s->tls_raw)) {
        memcpy(g_tsave, g_tls_blob, raw);
        if (s->tls_raw > raw) memcpy(g_tls_store, g_tls_blob + raw, s->tls_raw - raw);
        g_ntsave = s->nthreads;
        put_threads();
    } else bad++;
    int threads = put_stacks();
    ResetWriteWatch(g_r, g_committed);
    g_cur = j;
    resume_world();
    waiting_for = -1;
    if (pages_out) *pages_out = put;
    if (bad) rc_log("snapshots: %d pages did not decode", bad);
    rc_log("snapshots: %d game threads put back as they were%s%s", threads, away ? "; NOT put back (busy elsewhere):" : "",
           away ? moved : "");
    return s->ms;
}

void snap_stats(uint64_t *committed, uint64_t *used, int64_t *live_bytes, int64_t *live_blocks) {
    *committed = g_on ? g_committed : 0;
    *used = g_on ? g_st->top : 0;
    *live_bytes = g_on ? g_st->live_bytes : 0;
    *live_blocks = g_on ? g_st->live_blocks : 0;
}
