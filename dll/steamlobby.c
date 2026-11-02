// Which Steam lobby the player is in, for /invite in the chat (ui.cpp). Steam has no call for that, and does
// not tell a player their own lobby (FriendGameInfo_t comes back empty for oneself, tried 2026-11-02). The
// game's lobbies are Steam lobbies (SteamMatchMaking009): while in one it asks Steam about its members and
// sets its data. So a few methods of ISteamMatchmaking get a stub of ours that notes the lobby they are called
// with and goes on to Steam's own, untouched (the registers and the stack as they were).
//
// Slots in the interface's vtable, read from the flat API thunks of the game's steam_api64.dll (each is
// "mov rax,[rcx]; jmp [rax+slot*8]"): 14 JoinLobby, 15 LeaveLobby, 18 GetLobbyMemberByIndex (returns a CSteamID through a
// hidden pointer: the lobby is in r8, not rdx), 20 SetLobbyData, 24 GetLobbyMemberData, 25 SetLobbyMemberData,
// 26 SendLobbyChatMsg, 33 SetLobbyType, 34 SetLobbyJoinable. Steam answers members' calls only for lobbies the
// player is in; the lobby list (GetLobbyData of other lobbies) goes through slots not watched here.
// Seen in a lobby the player hosted (2026-11-02): SetLobbyType and SetLobbyData with it, a lobby SteamID
// 0186xxxxxxxxxxxx (universe 1, account type 8 "chat", the lobby instance flag). Steam's LobbyEnter /
// LobbyCreated callbacks, listened for through SteamAPI_RegisterCallback, never came.
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
STUB(14, 112)
STUB(15, 120)
STUB(18, 144)
STUB(20, 160)
STUB(24, 192)
STUB(25, 200)
STUB(26, 208)
STUB(33, 264)
STUB(34, 272)

static const struct { int slot; void (*stub)(void); } HOOKS[] = {
    {14, stub_14}, {15, stub_15}, {18, stub_18}, {20, stub_20}, {24, stub_24}, {25, stub_25}, {26, stub_26}, {33, stub_33}, {34, stub_34},
};

static int is_lobby(uint64_t id) { return id >> 52 == 0x18; }   // universe 1, account type 8 (k_EAccountTypeChat)

void steam_lobby_watch(void) {
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
    uint64_t best = 0, best_seq = 0;
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
        rc_log("lobby: none seen");
    }
    return best;
}
