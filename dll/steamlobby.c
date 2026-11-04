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
#include <stdio.h>
#include <stdlib.h>
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

// ---- the list of lobbies, asked of Steam ourselves: the game's own is only on its Internet page ----
typedef struct {
    void *mm, *ut;
    uint64_t (*request)(void *);
    void (*distance)(void *, int);
    void (*count)(void *, int);
    void (*string)(void *, const char *, const char *, int);
    uint64_t (*by_index)(void *, int);
    const char *(*data)(void *, uint64_t, const char *);
    int (*data_count)(void *, uint64_t);
    int (*data_at)(void *, uint64_t, int, char *, int, char *, int);
    int (*members)(void *, uint64_t);
    int (*limit)(void *, uint64_t);
    int (*done)(void *, uint64_t, unsigned char *);
    int (*result)(void *, uint64_t, void *, int, int, unsigned char *);
} Api;
static Api api;
static int api_ready(void) {
    if (api.mm && api.ut) return 1;
    HMODULE h = GetModuleHandleA("steam_api64.dll");
    if (!h) return 0;
    void *(*mm)(void) = (void *(*)(void))GetProcAddress(h, "SteamAPI_SteamMatchmaking_v009");
    void *(*ut)(void) = (void *(*)(void))GetProcAddress(h, "SteamAPI_SteamUtils_v010");
#define F(field, name) *(FARPROC *)&api.field = GetProcAddress(h, name); if (!api.field) return 0;
    F(request, "SteamAPI_ISteamMatchmaking_RequestLobbyList")
    F(distance, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter")
    F(count, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter")
    F(string, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter")
    F(by_index, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex")
    F(data, "SteamAPI_ISteamMatchmaking_GetLobbyData")
    F(data_count, "SteamAPI_ISteamMatchmaking_GetLobbyDataCount")
    F(data_at, "SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex")
    F(members, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers")
    F(limit, "SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit")
    F(done, "SteamAPI_ISteamUtils_IsAPICallCompleted")
    F(result, "SteamAPI_ISteamUtils_GetAPICallResult")
#undef F
    if (!mm || !ut) return 0;
    api.mm = mm(); api.ut = ut();
    return api.mm && api.ut;
}
void lobby_list_want(void);
void lobby_list_filter(int no_password, const char *modes);
// [debug] lobby_dump=1: what the lobbies hold, in the log, once
void lobby_dump_tick(const char *ini) {
    static int state;
    static DWORD at;
    static uint64_t call;
    static int on;
    if (on) lobby_list_want();
    if (on == 2) lobby_list_filter(1, "battle_zones,ammunition");
    if (state == 3) return;
    if (!state) {
        if (!GetPrivateProfileIntA("debug", "lobby_dump", 0, ini)) { state = 3; return; }
        on = GetPrivateProfileIntA("debug", "lobby_dump", 0, ini);
        state = 1; at = GetTickCount() + 20000;
    }
    if (state == 1) {
        if ((LONG)(GetTickCount() - at) < 0 || !api_ready()) return;
        api.distance(api.mm, 3);
        api.count(api.mm, 50);
        call = api.request(api.mm);
        rc_log("lobby dump: asked, call %llx", (unsigned long long)call);
        state = 2; at = GetTickCount() + 15000;
        return;
    }
    unsigned char failed = 0;
    if (!api.done(api.ut, call, &failed)) {
        if ((LONG)(GetTickCount() - at) >= 0) { rc_log("lobby dump: no answer"); state = 3; }
        return;
    }
    uint32_t n = 0;
    api.result(api.ut, call, &n, 4, 510, &failed);
    rc_log("lobby dump: %u lobbies, failed %d", n, failed);
    for (uint32_t i = 0; i < n && i < 14; i++) {
        uint64_t id = api.by_index(api.mm, (int)i);
        int c = api.data_count(api.mm, id);
        rc_log("lobby dump: #%u %llx members %d of %d, %d keys", i, (unsigned long long)id, api.members(api.mm, id), api.limit(api.mm, id), c);
        for (int k = 0; k < c && k < 40; k++) {
            char key[256], value[8192];
            if (api.data_at(api.mm, id, k, key, sizeof key, value, sizeof value)) rc_log("lobby dump:   %s = %.160s", key, value);
        }
    }
    state = 3;
}

// The tab asks for the list; this, on the game's thread, keeps it fresh while the tab is open. Names as the game's own
// list shows them: GameType "battle_zones" is the text mp/game/battle_zones, mapname "multi/x:mode" is mission/multi/x/name.
#define FN_TEXT 0x4775e0                // (key): the text's object
#define FN_TEXT_STR 0x4f8460            // (object): its std::string
typedef struct { union { char buf[16]; const char *ptr; }; size_t size, cap; } Str;
static const char *text_of(const char *key) {
    static struct { char key[112], text[160]; } cache[512];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].key, key)) return cache[i].text;
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    void *o = ((void *(*)(const char *))(base + FN_TEXT))(key);
    const Str *s = o ? ((const Str *(*)(void *))(base + FN_TEXT_STR))(o) : NULL;
    int at = n < 512 ? n++ : 511;
    snprintf(cache[at].key, sizeof cache[at].key, "%s", key);
    cache[at].text[0] = 0;
    if (s && s->size && s->size < 159) {
        memcpy(cache[at].text, s->cap > 15 ? s->ptr : s->buf, s->size);
        cache[at].text[s->size] = 0;
    }
    if (!strcmp(cache[at].text, key)) cache[at].text[0] = 0;     // no such text: its key comes back
    return cache[at].text;
}
// "multi/1v1_balzerweg" without a text: "Balzerweg"
static void plain_name(const char *s, char *out, size_t n) {
    const char *slash = strrchr(s, '/');
    if (slash) s = slash + 1;
    if (s[0] >= '0' && s[0] <= '9' && s[1] == 'v' && s[3] == '_') s += 4;
    if (!strncmp(s, "fr-", 3)) s += 3;
    snprintf(out, n, "%s", s);
    for (char *c = out; *c; c++) if (*c == '_') *c = ' ';
    if (out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
}
static SRWLOCK list_lock = SRWLOCK_INIT;
static LobbyLine list[LOBBY_LINES];
static int list_n, list_state;                  // 0 not yet, 1 has an answer, 2 Steam did not answer
static volatile DWORD list_wanted;
void lobby_list_want(void) { list_wanted = GetTickCount() | 1; }
static int want_open;                           // only lobbies without a password
static char want_modes[400];                    // only these GameTypes, a comma between; "" all
static volatile LONG filter_changed;
void lobby_list_filter(int no_password, const char *modes) {
    AcquireSRWLockExclusive(&list_lock);
    if (want_open != no_password || strcmp(want_modes, modes)) {
        want_open = no_password;
        snprintf(want_modes, sizeof want_modes, "%s", modes);
        filter_changed = 1;
    }
    ReleaseSRWLockExclusive(&list_lock);
}
void lobby_list_refresh(void) { filter_changed = 1; }
static LobbyMode known[48];
static int known_n;
int lobby_modes_copy(LobbyMode *out, int max) {
    AcquireSRWLockShared(&list_lock);
    int n = known_n < max ? known_n : max;
    memcpy(out, known, sizeof *out * n);
    ReleaseSRWLockShared(&list_lock);
    return n;
}
int lobby_list_copy(LobbyLine *out, int max, int *state) {
    AcquireSRWLockShared(&list_lock);
    int n = list_n < max ? list_n : max;
    memcpy(out, list, sizeof *out * n);
    *state = list_state;
    ReleaseSRWLockShared(&list_lock);
    return n;
}
static int number(const char *s) { return s && *s ? (int)strtol(s, NULL, 0) : 0; }
static void plain_name(const char *s, char *out, size_t n);
static void know_mode(const char *key) {        // the game's thread: the name comes from the game
    if (!*key) return;
    for (int i = 0; i < known_n; i++) if (!strcmp(known[i].key, key)) return;
    if (known_n >= 48) return;
    LobbyMode m;
    char text_key[112];
    snprintf(m.key, sizeof m.key, "%s", key);
    snprintf(text_key, sizeof text_key, "mp/game/%s", key);
    const char *text = text_of(text_key);
    if (*text) snprintf(m.name, sizeof m.name, "%s", text); else plain_name(key, m.name, sizeof m.name);
    AcquireSRWLockExclusive(&list_lock);
    known[known_n++] = m;
    ReleaseSRWLockExclusive(&list_lock);
}
void lobby_list_tick(void) {
    static uint64_t call;
    static DWORD asked, next;
    static int open, at, count, good, in_batch; // a round: one request per mode
    static char modes[16][48];
    static LobbyLine fresh[LOBBY_LINES];
    static int n;
    DWORD now = GetTickCount();
    if (!list_wanted || now - list_wanted > 3000 || !api_ready()) { next = 0; if (!call) return; }
    if (InterlockedExchange(&filter_changed, 0)) { call = 0; next = 0; in_batch = 0; }  // what was asked is not wanted any more
    static int seeded;
    if (!seeded) {
        seeded = 1;
        static const char *const COMMON[] = {"ammunition", "battle_zones", "frontlines", "cooperative"};
        for (int i = 0; i < 4; i++) know_mode(COMMON[i]);
    }
    if (!call) {
        if (next && (LONG)(now - next) < 0) return;
        if (!in_batch) {
            char list_text[400];
            AcquireSRWLockShared(&list_lock);
            open = want_open;
            memcpy(list_text, want_modes, sizeof list_text);
            ReleaseSRWLockShared(&list_lock);
            count = 0;
            for (char *m = strtok(list_text, ","); m && count < 16; m = strtok(NULL, ",")) {
                snprintf(modes[count++], sizeof modes[0], "%s", m);
                know_mode(m);
            }
            at = 0; n = 0; good = 0; in_batch = 1;
        }
        api.distance(api.mm, 3);                 // worldwide
        api.count(api.mm, 50);
        api.string(api.mm, "state", "0", 0);     // not started yet (k_ELobbyComparisonEqual); 1 = the battle is on
        if (open) api.string(api.mm, "password", "0x00000000", 0);
        if (count) api.string(api.mm, "GameType", modes[at], 0);
        call = api.request(api.mm);
        asked = now;
        return;
    }
    unsigned char failed = 0;
    int ok = 0;
    uint32_t found = 0;
    if (api.done(api.ut, call, &failed)) ok = api.result(api.ut, call, &found, 4, 510, &failed) && !failed;
    else if (now - asked < 15000) return;
    call = 0;
    good |= ok;
    for (uint32_t i = 0; ok && i < found && n < LOBBY_LINES; i++) {
        uint64_t id = api.by_index(api.mm, (int)i);
        const char *host = api.data(api.mm, id, "hostname"), *map = api.data(api.mm, id, "mapname"), *mode = api.data(api.mm, id, "GameType");
        if (!host || !*host || !map || !*map) continue;     // parties and the like
        if (number(api.data(api.mm, id, "state"))) continue;
        LobbyLine *l = &fresh[n++];
        memset(l, 0, sizeof *l);
        snprintf(l->host, sizeof l->host, "%s", host);
        const char *owner = api.data(api.mm, id, "owner");
        l->id = id;
        snprintf(l->owner, sizeof l->owner, "%s", owner ? owner : "");
        char key[112], name[96];
        snprintf(name, sizeof name, "%s", map);
        char *colon = strchr(name, ':');
        if (colon) *colon = 0;
        snprintf(key, sizeof key, "mission/%s/name", name);
        const char *text = text_of(key);
        if (*text) snprintf(l->map, sizeof l->map, "%s", text); else plain_name(name, l->map, sizeof l->map);
        snprintf(key, sizeof key, "mp/game/%s", mode ? mode : "");
        text = text_of(key);
        if (*text) snprintf(l->mode, sizeof l->mode, "%s", text); else plain_name(mode ? mode : "", l->mode, sizeof l->mode);
        know_mode(mode ? mode : "");
        l->players = number(api.data(api.mm, id, "numplayers"));
        l->max = number(api.data(api.mm, id, "MaxPlayers"));
        l->spectators = number(api.data(api.mm, id, "numspectators"));
        l->max_spectators = number(api.data(api.mm, id, "maxpectators"));   // so spelled in the game
        l->locked = number(api.data(api.mm, id, "password")) != 0;
        snprintf(l->mode_key, sizeof l->mode_key, "%s", mode ? mode : "");
        const char *mods = api.data(api.mm, id, "mods");    // mod_<workshop id>:0|mod_...
        if (mods && *mods) { l->mods = 1; for (const char *c = mods; *c; c++) l->mods += *c == '|'; }
    }
    if (++at < count) return;                   // the next mode, at once
    in_batch = 0;
    next = now + 15000;
    static int logged;
    if (logged++ < 3) rc_log("lobby list: %d shown after %d request(s)", n, count ? count : 1);
    for (int i = 0; i < n && i < 40 && logged == 1; i += i < 3 ? 1 : 12)
        rc_log("lobby list: %s | %s | %s | %d/%d [%d/%d] locked %d mods %d", fresh[i].host, fresh[i].mode, fresh[i].map, fresh[i].players, fresh[i].max, fresh[i].spectators, fresh[i].max_spectators, fresh[i].locked, fresh[i].mods);
    AcquireSRWLockExclusive(&list_lock);
    if (good) { memcpy(list, fresh, sizeof *list * n); list_n = n; list_state = 1; }
    else if (!list_n) list_state = 2;
    ReleaseSRWLockExclusive(&list_lock);
}
