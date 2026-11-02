// Which Steam lobby the player is in, for /invite in the chat (ui.cpp). Steam has no call for that, and does
// not tell a player their own lobby (FriendGameInfo_t comes back empty for oneself, tried 2026-11-02). The
// game's lobbies are Steam lobbies (SteamMatchMaking009): while in one it asks Steam about its members and
// sets its data. So a few methods of ISteamMatchmaking get a stub of ours that notes the lobby they are called
// with and goes on to Steam's own, untouched (the registers and the stack as they were).
//
// Slots in the interface's vtable, read from the flat API thunks of the game's steam_api64.dll (each is
// "mov rax,[rcx]; jmp [rax+slot*8]"): 15 LeaveLobby, 18 GetLobbyMemberByIndex (returns a CSteamID through a
// hidden pointer: the lobby is in r8, not rdx), 20 SetLobbyData, 24 GetLobbyMemberData, 25 SetLobbyMemberData,
// 26 SendLobbyChatMsg, 33 SetLobbyType, 34 SetLobbyJoinable. Steam answers members' calls only for lobbies the
// player is in; the lobby list (GetLobbyData of other lobbies) goes through slots not watched here.
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "goh_replay.h"

#define SLOTS 40
uint64_t goh_mm_orig[SLOTS], goh_mm_rdx[SLOTS], goh_mm_r8[SLOTS], goh_mm_seq[SLOTS];
volatile uint64_t goh_mm_tick;

// rdx and r8 as they came, the order of the call, then on to Steam's method; rax is free (no argument in it)
#define STUB(n, off)                                                          \
    __attribute__((naked)) static void stub_##n(void) {                         \
        __asm__ volatile("movq %rdx, goh_mm_rdx+" #off "(%rip)\n"               \
                         "movq %r8, goh_mm_r8+" #off "(%rip)\n"                 \
                         "lock incq goh_mm_tick(%rip)\n"                        \
                         "movq goh_mm_tick(%rip), %rax\n"                       \
                         "movq %rax, goh_mm_seq+" #off "(%rip)\n"               \
                         "jmpq *goh_mm_orig+" #off "(%rip)\n");                 \
    }
STUB(15, 120)
STUB(18, 144)
STUB(20, 160)
STUB(24, 192)
STUB(25, 200)
STUB(26, 208)
STUB(33, 264)
STUB(34, 272)

static const struct { int slot; void (*stub)(void); } HOOKS[] = {
    {15, stub_15}, {18, stub_18}, {20, stub_20}, {24, stub_24}, {25, stub_25}, {26, stub_26}, {33, stub_33}, {34, stub_34},
};

static int is_lobby(uint64_t id) { return id >> 52 == 0x17; }   // universe 1, account type 7 ("chat")

// ---- Steam's own word: the callbacks it sends every listener when the player enters or creates a lobby ----
// The stubs above saw nothing in a lobby (2026-11-02): the game asked Steam about none of it through them. The
// game dispatches Steam callbacks the old way (it imports SteamAPI_RegisterCallback / SteamAPI_RunCallbacks),
// so a listener of ours gets them too, on the thread the game runs them on.
// CCallbackBase as steam_api_common.h has it: vtable, uint8 flags, int callback id; MSVC puts the two Run
// overloads in its vtable in reverse order, and both only read the parameter here, so one function serves both.
#define CB_LOBBY_ENTER 504               // LobbyEnter_t: uint64 lobby, uint32 permissions, bool locked, uint32 response
#define CB_LOBBY_CREATED 513             // LobbyCreated_t: EResult (padded to 8), uint64 lobby
typedef struct Listener { void **vt; uint8_t flags; int callback; } Listener;
static volatile uint64_t g_cb_lobby, g_cb_seq;

static void cb_enter(Listener *self, void *p) {
    uint64_t lobby;
    uint32_t response;
    memcpy(&lobby, p, 8);
    memcpy(&response, (uint8_t *)p + 16, 4);
    rc_log("lobby: entered %llu (response %u)", (unsigned long long)lobby, response);
    if (response == 1 && is_lobby(lobby)) { g_cb_lobby = lobby; g_cb_seq = InterlockedIncrement64((volatile LONG64 *)&goh_mm_tick); }
}
static void cb_created(Listener *self, void *p) {
    uint32_t result;
    uint64_t lobby;
    memcpy(&result, p, 4);
    memcpy(&lobby, (uint8_t *)p + 8, 8);
    rc_log("lobby: created %llu (result %u)", (unsigned long long)lobby, result);
    if (result == 1 && is_lobby(lobby)) { g_cb_lobby = lobby; g_cb_seq = InterlockedIncrement64((volatile LONG64 *)&goh_mm_tick); }
}
static int size_enter(Listener *self) { return 24; }
static int size_created(Listener *self) { return 16; }
static void *VT_ENTER[3] = {(void *)cb_enter, (void *)cb_enter, (void *)size_enter};
static void *VT_CREATED[3] = {(void *)cb_created, (void *)cb_created, (void *)size_created};
static Listener g_enter = {VT_ENTER}, g_created = {VT_CREATED};

static void listen_lobbies(void) {
    HMODULE h = GetModuleHandleA("steam_api64.dll");
    void (*reg)(void *, int) = h ? (void (*)(void *, int))GetProcAddress(h, "SteamAPI_RegisterCallback") : NULL;
    if (!reg) { rc_log("lobby: no SteamAPI_RegisterCallback"); return; }
    reg(&g_enter, CB_LOBBY_ENTER);
    reg(&g_created, CB_LOBBY_CREATED);
    rc_log("lobby: listening for Steam's lobby callbacks");
}

void steam_lobby_watch(void) {
    listen_lobbies();
    HMODULE h = GetModuleHandleA("steam_api64.dll");
    void *(*mm_if)(void) = h ? (void *(*)(void))GetProcAddress(h, "SteamAPI_SteamMatchmaking_v009") : NULL;
    void *mm = mm_if ? mm_if() : NULL;
    if (!mm) { rc_log("lobby: no ISteamMatchmaking, /invite off"); return; }
    void **vt = *(void ***)mm;
    DWORD old;
    if (!VirtualProtect(vt, SLOTS * sizeof *vt, PAGE_READWRITE, &old)) { rc_log("lobby: vtable not writable, /invite off"); return; }
    for (size_t i = 0; i < sizeof HOOKS / sizeof HOOKS[0]; i++) {
        int s = HOOKS[i].slot;
        if (vt[s] == (void *)HOOKS[i].stub) continue;
        goh_mm_orig[s] = (uint64_t)vt[s];
        vt[s] = (void *)HOOKS[i].stub;
    }
    VirtualProtect(vt, SLOTS * sizeof *vt, old, &old);
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    rc_log("lobby: watching ISteamMatchmaking %p", mm);
}

uint64_t steam_lobby_now(void) {
    uint64_t best = g_cb_lobby, best_seq = best ? g_cb_seq : 0;
    for (size_t i = 0; i < sizeof HOOKS / sizeof HOOKS[0]; i++) {
        int s = HOOKS[i].slot;
        if (s == 15) continue;
        uint64_t id = s == 18 ? goh_mm_r8[s] : goh_mm_rdx[s], seq = goh_mm_seq[s];
        if (is_lobby(id) && seq > best_seq) { best = id; best_seq = seq; }
    }
    if (best && goh_mm_seq[15] > best_seq && goh_mm_rdx[15] == best) return 0;    // left it since
    if (!best) {                // what was seen, for the log
        for (size_t i = 0; i < sizeof HOOKS / sizeof HOOKS[0]; i++) {
            int s = HOOKS[i].slot;
            if (goh_mm_seq[s]) rc_log("lobby: slot %d last called #%llu with %llx / %llx", s, (unsigned long long)goh_mm_seq[s],
                                      (unsigned long long)goh_mm_rdx[s], (unsigned long long)goh_mm_r8[s]);
        }
        rc_log("lobby: none seen (callbacks: %llu)", (unsigned long long)g_cb_lobby);
    }
    return best;
}
