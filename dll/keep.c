// Objects outside the game's memory that a restore must not leave dangling.
//
// A restore puts the game's memory back to an earlier moment, but Direct3D and the sound library keep
// their objects in their own memory. Whatever the game destroyed since that moment would come back as
// pointers to freed objects (the first try crashed in the NVIDIA driver 5 ms after a restore). So from
// the first snapshot until the replay ends, when the game's own code drops its last reference to such an
// object, the object is kept alive instead, and freed when the replay is over.
//   Direct3D 11: Release (vtable slot 2) of every D3D11 object class is replaced; the classes are found by
//     creating one object of each kind right after the game creates its device.
//   Miles Sound System: the exe's import slots for AIL_close_stream and AIL_mem_free_lock.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <string.h>
#include "goh_replay.h"

int snap_patch_iat(uint8_t *exe, const char *dll, const char *fn, void *hook, void **real);

enum { K_D3D = 1, K_STREAM, K_MSSMEM };
#define KEEP_CAP (1u << 20)                 // open addressing table of kept objects

typedef struct { void *p; uint32_t kind; } Kept;
static Kept *g_kept;
static volatile LONG g_keep_lock, g_keeping;
static LONG g_nkept[4];
static uint8_t *g_text_lo, *g_text_hi;      // the exe's code: only its releases are held back

typedef ULONG (STDMETHODCALLTYPE *RefFn)(void *);
static struct { void **vt; RefFn addref, release; } g_vt[48];
static int g_nvt;

static HRESULT (WINAPI *real_CreateDeviceAndSwapChain)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                                       const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
                                                       D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
static void (WINAPI *real_close_stream)(void *);
static void (WINAPI *real_mem_free_lock)(void *);
static void (WINAPI *real_pause_stream)(void *, int);
static void *(WINAPI *real_open_digital)(unsigned, int, int, unsigned);
static void (WINAPI *real_set_master)(void *, float);
static float (WINAPI *real_get_master)(void *);
static void *g_dig;                         // the game's sound output, for muting it while a replay is prepared

static inline void klock(void) { while (InterlockedExchange(&g_keep_lock, 1)) YieldProcessor(); }
static inline void kunlock(void) { InterlockedExchange(&g_keep_lock, 0); }

static int from_game(void *ret) { return (uint8_t *)ret >= g_text_lo && (uint8_t *)ret < g_text_hi; }

// 1 if newly kept, 0 if it was kept already (released again after a restore), -1 if the table is full
static int keep(void *p, uint32_t kind) {
    if (!g_kept) return -1;
    uint64_t h = ((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull;
    klock();
    for (uint32_t i = 0; i < KEEP_CAP; i++) {
        Kept *k = &g_kept[(h + i) & (KEEP_CAP - 1)];
        if (k->p == p) { kunlock(); return 0; }
        if (!k->p) {
            if (g_nkept[0] >= (LONG)(KEEP_CAP / 2)) break;
            k->p = p; k->kind = kind;
            g_nkept[0]++; g_nkept[kind]++;
            kunlock();
            return 1;
        }
    }
    kunlock();
    return -1;
}

static int vt_index(void **vt) {
    for (int i = 0; i < g_nvt; i++) if (g_vt[i].vt == vt) return i;
    return -1;
}

// ---- when each Direct3D object was born ------------------------------------------------------------------
// The game creates and drops hundreds of objects a second (test 3: 56 000 kept in 2.5 minutes). Only
// objects that were alive when some snapshot was taken can be pointed at after a restore, so every object
// created through the device gets the number of snapshots taken so far; one that dies with that number
// unchanged lived between two snapshots and is let go. Objects made any other way count as old.
#define BORN_CAP (1u << 21)
typedef struct { void *p; LONG epoch; } Born;
static Born *g_born;
static volatile LONG g_born_lock, g_epoch, g_let_go;

static Born *born_slot(void *p, int insert) {
    uint64_t h = ((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0; i < 64; i++) {
        Born *b = &g_born[(h + i) & (BORN_CAP - 1)];
        if (b->p == p || (insert && !b->p)) return b;
        if (!b->p) return NULL;
    }
    return NULL;
}

static void born(void *p) {
    if (!g_born || !p) return;
    while (InterlockedExchange(&g_born_lock, 1)) YieldProcessor();
    Born *b = born_slot(p, 1);
    if (b) { b->p = p; b->epoch = g_epoch; }
    InterlockedExchange(&g_born_lock, 0);
}

// born after the last snapshot, so no snapshot can point at it
static int newer_than_snapshots(void *p) {
    if (!g_born) return 0;
    while (InterlockedExchange(&g_born_lock, 1)) YieldProcessor();
    Born *b = born_slot(p, 0);
    int newer = b && b->epoch == g_epoch;
    InterlockedExchange(&g_born_lock, 0);
    return newer;
}

static ULONG STDMETHODCALLTYPE keep_release(void *o) {
    int i = vt_index(*(void ***)o);
    if (i < 0) return 0;                    // cannot happen: only patched vtables lead here
    if (!g_keeping || !from_game(__builtin_return_address(0))) return g_vt[i].release(o);
    ULONG n = g_vt[i].addref(o);
    if (n > 2) { g_vt[i].release(o); return g_vt[i].release(o); }
    if (newer_than_snapshots(o)) { InterlockedIncrement(&g_let_go); g_vt[i].release(o); return g_vt[i].release(o); }
    // the game's release would destroy it: our reference keeps it
    if (keep(o, K_D3D) < 0) return g_vt[i].release(o);
    g_vt[i].release(o);
    return 0;
}

// the device's Create* methods (slots 3..27) and FinishCommandList (deferred context slot 114): one
// wrapper per slot, all with ten arguments (extra ones are ignored by the real method), `out` = index of
// the result pointer
typedef HRESULT (STDMETHODCALLTYPE *Create10)(void *, void *, void *, void *, void *, void *, void *, void *, void *, void *);
static Create10 g_create_real[128];
#define WRAP(slot, out)                                                                                          \
    static HRESULT STDMETHODCALLTYPE create_##slot(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5,   \
                                                   void *a6, void *a7, void *a8, void *a9) {                     \
        void *a[10] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9};                                                  \
        HRESULT hr = g_create_real[slot](a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);                                \
        if (SUCCEEDED(hr) && a[out]) born(*(void **)a[out]);                                                     \
        return hr;                                                                                               \
    }
WRAP(3, 3) WRAP(4, 3) WRAP(5, 3) WRAP(6, 3) WRAP(7, 3) WRAP(8, 3) WRAP(9, 3) WRAP(10, 3)
WRAP(11, 5) WRAP(12, 4) WRAP(13, 4) WRAP(14, 9) WRAP(15, 4) WRAP(16, 4) WRAP(17, 4) WRAP(18, 4)
WRAP(19, 1) WRAP(20, 2) WRAP(21, 2) WRAP(22, 2) WRAP(23, 2) WRAP(24, 2) WRAP(25, 2) WRAP(26, 2) WRAP(27, 2)
WRAP(114, 2)
static void *const g_create_hook[28] = {0, 0, 0, create_3, create_4, create_5, create_6, create_7, create_8, create_9,
                                        create_10, create_11, create_12, create_13, create_14, create_15, create_16,
                                        create_17, create_18, create_19, create_20, create_21, create_22, create_23,
                                        create_24, create_25, create_26, create_27};

static void patch_slot(void **vt, int slot, void *hook) {
    DWORD old;
    if (g_create_real[slot]) return;
    g_create_real[slot] = (Create10)vt[slot];
    VirtualProtect(&vt[slot], 8, PAGE_READWRITE, &old);
    vt[slot] = hook;
    VirtualProtect(&vt[slot], 8, old, &old);
}

static void add_vt(void *obj) {
    if (!obj) return;
    void **vt = *(void ***)obj;
    if (vt_index(vt) >= 0 || g_nvt >= 48) return;
    g_vt[g_nvt].vt = vt;
    g_vt[g_nvt].addref = (RefFn)vt[1];
    g_vt[g_nvt].release = (RefFn)vt[2];
    g_nvt++;
}

static ID3DBlob *compile(const char *src, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    if (FAILED(D3DCompile(src, strlen(src), NULL, NULL, NULL, "main", target, 0, 0, &code, &err))) code = NULL;
    if (err) ID3D10Blob_Release(err);
    return code;
}

#define REL(x) do { if (x) IUnknown_Release((IUnknown *)(x)); } while (0)

// one object of every kind the game can create, to learn the classes' vtables
static void find_vtables(ID3D11Device *dev) {
    void *o[40] = {0};
    int n = 0;
    D3D11_BUFFER_DESC bd = {256, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    ID3D11Device_CreateBuffer(dev, &bd, NULL, (ID3D11Buffer **)&o[n++]);
    D3D11_BUFFER_DESC bdyn = {256, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Device_CreateBuffer(dev, &bdyn, NULL, (ID3D11Buffer **)&o[n++]);
    D3D11_TEXTURE1D_DESC t1 = {16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Device_CreateTexture1D(dev, &t1, NULL, (ID3D11Texture1D **)&o[n++]);
    D3D11_TEXTURE2D_DESC t2 = {16, 16, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS, 0, 0};
    ID3D11Texture2D *tex = NULL;
    ID3D11Device_CreateTexture2D(dev, &t2, NULL, &tex);
    o[n++] = tex;
    D3D11_TEXTURE2D_DESC ts = t2;
    ts.Usage = D3D11_USAGE_STAGING; ts.BindFlags = 0; ts.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(dev, &ts, NULL, (ID3D11Texture2D **)&o[n++]);
    D3D11_TEXTURE2D_DESC td = t2;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D *depth = NULL;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &depth);
    o[n++] = depth;
    D3D11_TEXTURE3D_DESC t3 = {4, 4, 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Device_CreateTexture3D(dev, &t3, NULL, (ID3D11Texture3D **)&o[n++]);
    if (tex) {
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, (ID3D11ShaderResourceView **)&o[n++]);
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, (ID3D11RenderTargetView **)&o[n++]);
        ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)tex, NULL, (ID3D11UnorderedAccessView **)&o[n++]);
    }
    if (depth) ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)depth, NULL, (ID3D11DepthStencilView **)&o[n++]);
    ID3DBlob *vs = compile("float4 main(float4 p : POSITION) : SV_Position { return p; }", "vs_5_0");
    ID3DBlob *ps = compile("float4 main() : SV_Target { return 1; }", "ps_5_0");
    ID3DBlob *gs = compile("struct V { float4 p : SV_Position; }; [maxvertexcount(1)] void main(point V i[1], inout PointStream<V> s) { s.Append(i[0]); }", "gs_5_0");
    ID3DBlob *cs = compile("[numthreads(1, 1, 1)] void main() {}", "cs_5_0");
    ID3DBlob *hs = compile("struct P { float4 p : POSITION; }; struct C { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; };"
                           "C pc(InputPatch<P, 3> ip) { C c; c.e[0] = c.e[1] = c.e[2] = 1; c.i = 1; return c; }"
                           "[domain(\"tri\")] [partitioning(\"integer\")] [outputtopology(\"triangle_cw\")] [outputcontrolpoints(3)]"
                           "[patchconstantfunc(\"pc\")] P main(InputPatch<P, 3> ip, uint i : SV_OutputControlPointID) { return ip[i]; }", "hs_5_0");
    ID3DBlob *ds = compile("struct P { float4 p : POSITION; }; struct C { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; };"
                           "[domain(\"tri\")] float4 main(C c, float3 w : SV_DomainLocation, const OutputPatch<P, 3> p) : SV_Position"
                           "{ return p[0].p * w.x + p[1].p * w.y + p[2].p * w.z; }", "ds_5_0");
#define BP(b) ID3D10Blob_GetBufferPointer(b)
#define BS(b) ID3D10Blob_GetBufferSize(b)
    if (vs) {
        ID3D11Device_CreateVertexShader(dev, BP(vs), BS(vs), NULL, (ID3D11VertexShader **)&o[n++]);
        D3D11_INPUT_ELEMENT_DESC ie = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        ID3D11Device_CreateInputLayout(dev, &ie, 1, BP(vs), BS(vs), (ID3D11InputLayout **)&o[n++]);
    }
    if (ps) ID3D11Device_CreatePixelShader(dev, BP(ps), BS(ps), NULL, (ID3D11PixelShader **)&o[n++]);
    if (gs) ID3D11Device_CreateGeometryShader(dev, BP(gs), BS(gs), NULL, (ID3D11GeometryShader **)&o[n++]);
    if (cs) ID3D11Device_CreateComputeShader(dev, BP(cs), BS(cs), NULL, (ID3D11ComputeShader **)&o[n++]);
    if (hs) ID3D11Device_CreateHullShader(dev, BP(hs), BS(hs), NULL, (ID3D11HullShader **)&o[n++]);
    if (ds) ID3D11Device_CreateDomainShader(dev, BP(ds), BS(ds), NULL, (ID3D11DomainShader **)&o[n++]);
    REL(vs); REL(ps); REL(gs); REL(cs); REL(hs); REL(ds);
    D3D11_BLEND_DESC blend = {0};
    blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(dev, &blend, (ID3D11BlendState **)&o[n++]);
    D3D11_DEPTH_STENCIL_DESC dsd = {TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS, FALSE, 0xff, 0xff,
                                    {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS},
                                    {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS}};
    ID3D11Device_CreateDepthStencilState(dev, &dsd, (ID3D11DepthStencilState **)&o[n++]);
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_BACK, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    ID3D11Device_CreateRasterizerState(dev, &rd, (ID3D11RasterizerState **)&o[n++]);
    D3D11_SAMPLER_DESC sd = {D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP,
                             D3D11_TEXTURE_ADDRESS_WRAP, 0, 1, D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0, D3D11_FLOAT32_MAX};
    ID3D11Device_CreateSamplerState(dev, &sd, (ID3D11SamplerState **)&o[n++]);
    D3D11_QUERY_DESC q1 = {D3D11_QUERY_EVENT, 0}, q2 = {D3D11_QUERY_OCCLUSION, 0}, q3 = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0},
                     q4 = {D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    ID3D11Device_CreateQuery(dev, &q1, (ID3D11Query **)&o[n++]);
    ID3D11Device_CreateQuery(dev, &q2, (ID3D11Query **)&o[n++]);
    ID3D11Device_CreateQuery(dev, &q3, (ID3D11Query **)&o[n++]);
    ID3D11Device_CreatePredicate(dev, &q4, (ID3D11Predicate **)&o[n++]);
    ID3D11DeviceContext *dc = NULL;
    if (SUCCEEDED(ID3D11Device_CreateDeferredContext(dev, 0, &dc))) {
        o[n++] = dc;
        ID3D11DeviceContext_FinishCommandList(dc, FALSE, (ID3D11CommandList **)&o[n++]);
        patch_slot(*(void ***)dc, 114, create_114);       // command lists are born here
    }
    for (int s = 3; s <= 27; s++) patch_slot(*(void ***)dev, s, g_create_hook[s]);
    for (int i = 0; i < n; i++) add_vt(o[i]);
    for (int i = n - 1; i >= 0; i--) REL(o[i]);
    // now take Release over in every class found
    for (int i = 0; i < g_nvt; i++) {
        DWORD old;
        VirtualProtect(&g_vt[i].vt[2], 8, PAGE_READWRITE, &old);
        g_vt[i].vt[2] = (void *)keep_release;
        VirtualProtect(&g_vt[i].vt[2], 8, old, &old);
    }
    rc_log("keep: Release taken over in %d Direct3D classes", g_nvt);
}

static HRESULT WINAPI hook_CreateDeviceAndSwapChain(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL *fl,
                                                   UINT nfl, UINT sdk, const DXGI_SWAP_CHAIN_DESC *scd, IDXGISwapChain **sc,
                                                   ID3D11Device **dev, D3D_FEATURE_LEVEL *got, ID3D11DeviceContext **ctx) {
    HRESULT hr = real_CreateDeviceAndSwapChain(a, t, sw, flags, fl, nfl, sdk, scd, sc, dev, got, ctx);
    if (SUCCEEDED(hr) && dev && *dev && !g_nvt) find_vtables(*dev);
    return hr;
}

static void WINAPI hook_close_stream(void *s) {
    if (g_keeping && s && from_game(__builtin_return_address(0)) && keep(s, K_STREAM) >= 0) {
        real_pause_stream(s, 1);            // silent, but the handle stays valid for a restored game
        return;
    }
    real_close_stream(s);
}

static void WINAPI hook_mem_free_lock(void *p) {
    if (g_keeping && p && from_game(__builtin_return_address(0)) && keep(p, K_MSSMEM) >= 0) return;
    real_mem_free_lock(p);
}

static void *WINAPI hook_open_digital(unsigned freq, int bits, int channels, unsigned flags) {
    void *d = real_open_digital(freq, bits, channels, flags);
    if (d) g_dig = d;
    return d;
}

// the game's sound off (1) or back as it was (0)
void game_sound_mute(int on) {
    static float saved = -1;
    if (!g_dig || !real_set_master || !real_get_master) return;
    if (on && saved < 0) { saved = real_get_master(g_dig); real_set_master(g_dig, 0); }
    else if (!on && saved >= 0) { real_set_master(g_dig, saved); saved = -1; }
}

// ---- interface -------------------------------------------------------------------------------------------
int keep_install(uint8_t *exe) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (!memcmp(s->Name, ".text", 6)) { g_text_lo = exe + s->VirtualAddress; g_text_hi = g_text_lo + s->Misc.VirtualSize; }
    g_kept = VirtualAlloc(NULL, KEEP_CAP * sizeof(Kept), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_born = VirtualAlloc(NULL, BORN_CAP * sizeof(Born), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HMODULE mss = GetModuleHandleA("mss64.dll");
    real_pause_stream = mss ? (void *)GetProcAddress(mss, "AIL_pause_stream") : NULL;
    real_set_master = mss ? (void *)GetProcAddress(mss, "AIL_set_digital_master_volume_level") : NULL;
    real_get_master = mss ? (void *)GetProcAddress(mss, "AIL_digital_master_volume_level") : NULL;
    if (mss) snap_patch_iat(exe, "mss64.dll", "AIL_open_digital_driver", hook_open_digital, (void **)&real_open_digital);
    int ok = snap_patch_iat(exe, "d3d11.dll", "D3D11CreateDeviceAndSwapChain", hook_CreateDeviceAndSwapChain, (void **)&real_CreateDeviceAndSwapChain);
    if (real_pause_stream) {
        ok &= snap_patch_iat(exe, "mss64.dll", "AIL_close_stream", hook_close_stream, (void **)&real_close_stream);
        ok &= snap_patch_iat(exe, "mss64.dll", "AIL_mem_free_lock", hook_mem_free_lock, (void **)&real_mem_free_lock);
    } else ok = 0;
    return ok && g_kept;
}

// a snapshot was taken: whatever lives now may be pointed at by it
void keep_begin(void) { InterlockedIncrement(&g_epoch); g_keeping = 1; }

// game's logic thread, once the replay is over: free what was held back
void keep_end(void) {
    if (!g_keeping) return;
    g_keeping = 0;
    LONG n[4];
    memcpy(n, g_nkept, sizeof n);
    for (uint32_t i = 0; g_kept && i < KEEP_CAP; i++) {
        Kept *k = &g_kept[i];
        if (!k->p) continue;
        if (k->kind == K_D3D) { int v = vt_index(*(void ***)k->p); if (v >= 0) g_vt[v].release(k->p); }
        else if (k->kind == K_STREAM) real_close_stream(k->p);
        else if (k->kind == K_MSSMEM) real_mem_free_lock(k->p);
        k->p = NULL;
    }
    memset(g_nkept, 0, sizeof g_nkept);
    rc_log("keep: replay over, freed %ld Direct3D objects, %ld sound streams, %ld sound buffers", n[K_D3D], n[K_STREAM], n[K_MSSMEM]);
}

int keep_active(void) { return g_keeping; }

void keep_stats(long *d3d, long *streams, long *mem, long *let_go) {
    *d3d = g_nkept[K_D3D]; *streams = g_nkept[K_STREAM]; *mem = g_nkept[K_MSSMEM]; *let_go = g_let_go;
}
