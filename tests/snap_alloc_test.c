// Stress test for the snapshot allocator (dll/snap.c): threads allocate, fill, check, resize and free
// blocks of every size class and page runs, through the same entry points the game's imports reach.
//   python -m ziglang cc -target x86_64-windows-gnu -O2 -Idll tests/snap_alloc_test.c dll/keep.c -ld3d11 -ld3dcompiler_47 -o build/snap_alloc_test.exe
#include "../dll/snap.c"
#include <stdio.h>
#include <stdlib.h>

void rc_log(const char *fmt, ...) { (void)fmt; }

#define SLOTS 4096
#define THREADS 8
#define ROUNDS 400000

static volatile LONG g_fail;

static uint32_t rnd(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

static size_t pick_size(uint32_t *s) {
    uint32_t r = rnd(s) % 100;
    if (r < 60) return rnd(s) % 256;
    if (r < 90) return rnd(s) % 32768;
    if (r < 99) return 32768 + rnd(s) % (1 << 20);
    return (1 << 20) + rnd(s) % (8 << 20);
}

static void fill(uint8_t *p, size_t n, uint8_t v) { for (size_t i = 0; i < n; i += 61) p[i] = v; if (n) p[n - 1] = v; }
static int check(const uint8_t *p, size_t n, uint8_t v) {
    for (size_t i = 0; i < n; i += 61) if (p[i] != v) return 0;
    return !n || p[n - 1] == v;
}

static DWORD WINAPI worker(LPVOID arg) {
    uint32_t s = (uint32_t)(uintptr_t)arg * 2654435761u + 1;
    uint8_t *ptr[SLOTS] = {0}; size_t len[SLOTS] = {0}; uint8_t tag[SLOTS] = {0};
    for (int i = 0; i < ROUNDS && !g_fail; i++) {
        int k = rnd(&s) % SLOTS;
        if (ptr[k] && !check(ptr[k], len[k], tag[k])) { printf("corrupt block %p size %zu\n", ptr[k], len[k]); g_fail = 1; break; }
        uint32_t op = rnd(&s) % 10;
        if (!ptr[k] || op < 2) {
            if (ptr[k]) hook_HeapFree(g_heap, 0, ptr[k]);
            len[k] = pick_size(&s);
            int zero = rnd(&s) & 1;
            ptr[k] = hook_HeapAlloc(g_heap, zero ? HEAP_ZERO_MEMORY : 0, len[k]);
            if (!ptr[k]) { printf("alloc %zu failed\n", len[k]); g_fail = 1; break; }
            if (((uintptr_t)ptr[k] & 15) || !snap_owns(ptr[k])) { printf("bad pointer %p\n", ptr[k]); g_fail = 1; break; }
            if (zero) for (size_t j = 0; j < len[k]; j += 97) if (ptr[k][j]) { printf("not zeroed\n"); g_fail = 1; break; }
            if (hook_HeapSize(g_heap, 0, ptr[k]) != len[k]) { printf("size mismatch\n"); g_fail = 1; break; }
            tag[k] = (uint8_t)rnd(&s);
            fill(ptr[k], len[k], tag[k]);
        } else if (op < 4) {
            size_t n = pick_size(&s);
            uint8_t *q = hook_HeapReAlloc(g_heap, HEAP_ZERO_MEMORY, ptr[k], n);
            if (!q) { printf("realloc failed\n"); g_fail = 1; break; }
            size_t keep = n < len[k] ? n : len[k];
            int lost = 0;
            for (size_t j = 0; j < keep; j += 61) lost |= q[j] != tag[k];
            if (keep == len[k] && keep) lost |= q[keep - 1] != tag[k];
            if (lost) { printf("realloc lost data (%zu -> %zu)\n", len[k], n); g_fail = 1; break; }
            for (size_t j = len[k]; j < n; j += 97) if (q[j]) { printf("realloc tail not zeroed\n"); g_fail = 1; break; }
            ptr[k] = q; len[k] = n;
            tag[k] = (uint8_t)rnd(&s);
            fill(ptr[k], len[k], tag[k]);
        } else if (op == 4) {
            // the engine's page allocator path
            size_t n = 4096 + rnd(&s) % (2 << 20);
            uint8_t *v = hook_VirtualAlloc(NULL, n, MEM_COMMIT, PAGE_READWRITE);
            if (!v || ((uintptr_t)v & 0xffff)) { printf("virtual alloc %p\n", v); g_fail = 1; break; }
            for (size_t j = 0; j < n; j += 4096) if (v[j]) { printf("virtual not zeroed\n"); g_fail = 1; break; }
            memset(v, 0x5a, n);
            hook_VirtualFree(v, 0, MEM_RELEASE);
        }
    }
    for (int k = 0; k < SLOTS; k++) if (ptr[k]) hook_HeapFree(g_heap, 0, ptr[k]);
    return 0;
}

// the Direct3D keeper: a texture the "game" (this exe) releases for the last time stays alive while
// keeping, a second last release after a "restore" is not counted twice, and keep_end really frees it
#define COBJMACROS
#include <d3d11.h>
void keep_begin(void);
void keep_end(void);
void keep_stats(long *d3d, long *streams, long *mem, long *let_go);

static int keep_test(void) {
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, NULL, NULL, &dev, NULL, &ctx);
    if (FAILED(hr)) { printf("no device: %lx\n", hr); return 1; }
    D3D11_TEXTURE2D_DESC t2 = {64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D *tex = NULL;
    ID3D11Device_CreateTexture2D(dev, &t2, NULL, &tex);
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv);
    keep_begin();
    ULONG a = ID3D11ShaderResourceView_Release(srv);     // last release by the game: kept
    ULONG b = ID3D11Texture2D_Release(tex);               // views hold textures by an internal count: kept too
    ULONG c = ID3D11ShaderResourceView_AddRef(srv);       // still a live object: ours + this one
    ULONG d = ID3D11ShaderResourceView_Release(srv);      // back to ours
    ULONG e = ID3D11ShaderResourceView_Release(srv);      // a restored game releasing it again: still kept
    ID3D11Texture2D *fresh = NULL;                        // born after the snapshot: no snapshot points at it
    ID3D11Device_CreateTexture2D(dev, &t2, NULL, &fresh);
    ULONG f = ID3D11Texture2D_Release(fresh);
    long kd, ks, km, kg;
    keep_stats(&kd, &ks, &km, &kg);
    keep_end();
    printf("keep: releases returned %lu %lu, alive check %lu -> %lu, again %lu, kept %ld (want 0 0 2 1 0 2); newer: released to %lu, let go %ld (want 0 1)\n",
           a, b, c, d, e, kd, f, kg);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    return !(a == 0 && b == 0 && c == 2 && d == 1 && e == 0 && kd == 2 && f == 0 && kg == 1);
}

// a freed block keeps its bytes and is not handed out again while newer frees of its class are few
// (the game reads some objects a moment after another thread freed them); large runs likewise
static int quarantine_test(void) {
    int bad = 0;
    uint8_t *first = hook_HeapAlloc(g_heap, 0, 100);
    memset(first, 0xab, 100);
    hook_HeapFree(g_heap, 0, first);
    for (int i = 0; i < 300; i++) {
        uint8_t *q = hook_HeapAlloc(g_heap, 0, 100);
        if (q == first) { printf("quarantine: freed block reused after %d allocations\n", i); bad++; break; }
        hook_HeapFree(g_heap, 0, q);
    }
    for (int i = 0; i < 100; i++) if (first[i] != 0xab) { printf("quarantine: freed block's byte %d changed\n", i); bad++; break; }
    uint8_t *big = hook_HeapAlloc(g_heap, 0, 200000);
    memset(big, 0xcd, 200000);
    hook_HeapFree(g_heap, 0, big);
    for (int i = 0; i < 20; i++) {
        uint8_t *q = hook_HeapAlloc(g_heap, 0, 200000);
        if (q == big) { printf("quarantine: freed run reused after %d allocations\n", i); bad++; break; }
        hook_HeapFree(g_heap, 0, q);
    }
    for (int i = 0; i < 200000; i += 997) if (big[i] != 0xcd) { printf("quarantine: freed run's bytes changed\n"); bad++; break; }
    // and memory is reused in the end: a long churn of one class does not grow without bound
    uint64_t c0, u0, c1, u1; int64_t lb, bl;
    snap_stats(&c0, &u0, &lb, &bl);
    for (int i = 0; i < 2000000; i++) hook_HeapFree(g_heap, 0, hook_HeapAlloc(g_heap, 0, 100));
    snap_stats(&c1, &u1, &lb, &bl);
    if (u1 - u0 > (8ull << 20)) { printf("quarantine: churn grew the heap by %llu MB\n", (unsigned long long)((u1 - u0) >> 20)); bad++; }
    printf("quarantine %s (churn grew the heap by %llu KB)\n", bad ? "FAILED" : "ok", (unsigned long long)((u1 - u0) >> 10));
    return bad;
}

int main(int argc, char **argv) {
    // the test exe's own imports stand in for the game's
    if (!snap_install((uint8_t *)GetModuleHandleA(NULL))) printf("note: some import slots not patched (fine here)\n");
    // snapshots and restores: tests/store_test.c
    if (argc > 1 && !strcmp(argv[1], "keep")) return keep_test();
    if (quarantine_test()) return 1;
    DWORD t0 = GetTickCount();
    HANDLE h[THREADS];
    for (int i = 0; i < THREADS; i++) h[i] = CreateThread(NULL, 0, worker, (LPVOID)(uintptr_t)(i + 1), 0, NULL);
    WaitForMultipleObjects(THREADS, h, TRUE, INFINITE);
    uint64_t c, u; int64_t lb, bl;
    snap_stats(&c, &u, &lb, &bl);
    printf("%s in %lu ms: committed %llu MB, used %llu MB, live %lld bytes in %lld blocks\n", g_fail ? "FAILED" : "ok",
           GetTickCount() - t0, (unsigned long long)(c >> 20), (unsigned long long)(u >> 20), (long long)lb, (long long)bl);
    return g_fail || lb || bl;
}
