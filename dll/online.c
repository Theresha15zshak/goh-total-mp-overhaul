// Who plays with the mod right now: our server goh-online (server/ in this repo, Cloudflare Workers).
// Only with the player's yes (asked once, kept in goh_online.ini): every 2 minutes the SteamID, the Steam
// name and the balance version go to the server. The list is fetched while the F6 window is open.
// The SteamID and name come from the game's own steam_api64.dll; the server cannot verify them.
// Everything here runs on a thread of ours with short timeouts: no server, no effect on the game.
// The player's key (Documents\My Games\gates of hell\goh_mod_key.txt, made at the first start): the server
// gives the SteamID to the first key it sees and takes nothing for it without that key afterwards. It is
// kept outside the game folder so a reinstall keeps it; it opens nothing but our server.
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "goh_replay.h"

#define HOST L"goh-online.goh-mod.workers.dev"
#define BEAT_MS 120000          // heartbeat (the server counts 5 minutes as online)
#define LIST_MS 20000           // list refresh while the window is open
#define MAX_BODY 32768

volatile long g_online_consent = -1;
volatile long g_online_want;
volatile long g_online_held;
static char g_ini[MAX_PATH];
static char g_key[65];          // 64 hex characters, "" if it could be neither read nor made
static char g_id[18];           // our SteamID, "" until Steam is up
static char g_name[ONLINE_NAME];
static SRWLOCK g_lock = SRWLOCK_INIT;
static OnlinePlayer g_list[ONLINE_MAX];
static int g_count, g_state;
static volatile long g_beat_now;   // consent just given: send at once

const char *online_my_id(void) { return g_id; }

int online_copy(OnlinePlayer *out, int max, int *state) {
    AcquireSRWLockShared(&g_lock);
    int n = g_count < max ? g_count : max;
    memcpy(out, g_list, n * sizeof *out);
    *state = g_state;
    ReleaseSRWLockShared(&g_lock);
    return n;
}

void online_set_consent(int yes) {
    g_online_consent = yes;
    WritePrivateProfileStringA("online", "consent", yes ? "1" : "0", g_ini);
    if (yes) g_beat_now = 1;
    rc_log("online: player said %s", yes ? "yes" : "no");
}

// the game's Steam: its flat API (steam_api64.dll of GoH 1.067 exports these versions)
static int steam_me(void) {
    HMODULE h = GetModuleHandleA("steam_api64.dll");
    if (!h) return 0;
    void *(*user_if)(void) = (void *(*)(void))GetProcAddress(h, "SteamAPI_SteamUser_v021");
    uint64_t (*get_id)(void *) = (uint64_t (*)(void *))GetProcAddress(h, "SteamAPI_ISteamUser_GetSteamID");
    void *(*friends_if)(void) = (void *(*)(void))GetProcAddress(h, "SteamAPI_SteamFriends_v017");
    const char *(*get_name)(void *) = (const char *(*)(void *))GetProcAddress(h, "SteamAPI_ISteamFriends_GetPersonaName");
    if (!user_if || !get_id || !friends_if || !get_name) return 0;
    void *u = user_if(), *f = friends_if();     // null until the game has started Steam
    if (!u || !f) return 0;
    uint64_t id = get_id(u);
    if (id < 76561190000000000ull || id > 76561199999999999ull) return 0;
    const char *name = get_name(f);
    snprintf(g_name, sizeof g_name, "%s", name && *name ? name : "?");
    for (char *p = g_name; *p; p++) if ((unsigned char)*p < 32) *p = ' ';   // the body is newline separated
    snprintf(g_id, sizeof g_id, "%llu", (unsigned long long)id);
    return 1;
}

static int read_key(const char *path) {
    char k[65] = "";
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(k, 1, 64, f);
    fclose(f);
    k[n] = 0;
    if (n != 64 || strspn(k, "0123456789abcdef") != 64) return 0;
    memcpy(g_key, k, 65);
    return 1;
}

static void load_key(void) {
    char path[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, path) != S_OK) { rc_log("online: no Documents folder, no key"); return; }
    strncat(path, "\\My Games", sizeof path - strlen(path) - 1);
    CreateDirectoryA(path, NULL);
    strncat(path, "\\gates of hell", sizeof path - strlen(path) - 1);
    CreateDirectoryA(path, NULL);
    strncat(path, "\\goh_mod_key.txt", sizeof path - strlen(path) - 1);
    if (read_key(path)) { rc_log("online: key found"); return; }
    unsigned char r[32];
    if (BCryptGenRandom(NULL, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) { rc_log("online: no random numbers, no key"); return; }
    char k[65];
    for (int i = 0; i < 32; i++) snprintf(k + 2 * i, 3, "%02x", r[i]);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {        // there already, unreadable as a key: leave it to the player
        if (!read_key(path)) rc_log("online: %s is not a key (error %lu), none sent", path, GetLastError());
        return;
    }
    DWORD put = 0;
    int ok = WriteFile(h, k, 64, &put, NULL) && put == 64;
    CloseHandle(h);
    if (ok) { memcpy(g_key, k, 65); rc_log("online: new key made in Documents\\My Games\\gates of hell"); }
    else { DeleteFileA(path); rc_log("online: the key could not be written"); }
}

// one request; returns the HTTP status (0 = no answer), body in out (NUL-terminated, at most MAX_BODY-1 bytes)
static int http(HINTERNET s, const wchar_t *verb, const wchar_t *path, const char *body, char *out) {
    int status = 0;
    out[0] = 0;
    HINTERNET c = WinHttpConnect(s, HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, verb, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE) : NULL;
    DWORD len = body ? (DWORD)strlen(body) : 0;
    if (r && WinHttpSendRequest(r, L"Content-Type: text/plain; charset=utf-8\r\n", (DWORD)-1L, (void *)body, len, len, 0) &&
        WinHttpReceiveResponse(r, NULL)) {
        DWORD st = 0, sz = sizeof st;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &st, &sz, NULL);
        status = (int)st;
        DWORD got = 0, n;
        while (got < MAX_BODY - 1 && WinHttpReadData(r, out + got, MAX_BODY - 1 - got, &n) && n) got += n;
        out[got] = 0;
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    return status;
}

// "count\n" then "steam_id\tname\tversion\tseconds_ago\n": anything not exactly that is skipped
static void parse_list(char *t) {
    static OnlinePlayer tmp[ONLINE_MAX];
    int n = 0;
    char *line = strchr(t, '\n');
    while (line && n < ONLINE_MAX) {
        char *p = line + 1;
        line = strchr(p, '\n');
        if (!line) break;
        *line = 0;
        char *f[4];
        int k = 0;
        f[k++] = p;
        for (char *q = p; *q && k < 4; q++) if (*q == '\t') { *q = 0; f[k++] = q + 1; }
        if (k != 4 || strlen(f[0]) != 17 || strspn(f[0], "0123456789") != 17) continue;
        if (strlen(f[1]) >= ONLINE_NAME || strlen(f[2]) >= sizeof tmp->version || strchr(f[3], '\t')) continue;
        OnlinePlayer *o = &tmp[n++];
        strcpy(o->id, f[0]);
        strcpy(o->name, f[1]);
        strcpy(o->version, f[2]);
        o->ago = atoi(f[3]);
    }
    AcquireSRWLockExclusive(&g_lock);
    memcpy(g_list, tmp, n * sizeof *tmp);
    g_count = n;
    g_state = 1;
    ReleaseSRWLockExclusive(&g_lock);
}

// ---- texts the panel shows (stats, leaders, ratings): fetched while it asks for them, every 30 s ----------
#define DOC_MS 30000
volatile DWORD g_doc_want[DOC_N];
static char *g_doc[DOC_N];
static int g_doc_state[DOC_N];
static DWORD g_doc_at[DOC_N];
static char g_doc_ids[16 * 18 + 1];     // DOC_RATINGS: comma separated SteamIDs
static SRWLOCK g_doc_lock = SRWLOCK_INIT;

void online_want(int doc) { g_doc_want[doc] = GetTickCount() | 1; }

void online_ratings_ids(const char *ids) {
    AcquireSRWLockExclusive(&g_doc_lock);
    if (strcmp(ids, g_doc_ids)) { snprintf(g_doc_ids, sizeof g_doc_ids, "%s", ids); g_doc_at[DOC_RATINGS] = 0; }
    ReleaseSRWLockExclusive(&g_doc_lock);
}

int online_doc(int doc, char *out, size_t n, int *state) {
    AcquireSRWLockShared(&g_doc_lock);
    int ok = g_doc[doc] != NULL;
    snprintf(out, n, "%s", ok ? g_doc[doc] : "");
    *state = g_doc_state[doc];
    ReleaseSRWLockShared(&g_doc_lock);
    return ok;
}

static void fetch_docs(HINTERNET s, char *buf) {
    DWORD now = GetTickCount();
    for (int d = 0; d < DOC_N; d++) {
        DWORD want = g_doc_want[d];
        if (!want || now - want > 2000) continue;               // not on screen
        AcquireSRWLockShared(&g_doc_lock);
        int fresh = g_doc_at[d] && now - g_doc_at[d] < DOC_MS;
        char ids[sizeof g_doc_ids];
        memcpy(ids, g_doc_ids, sizeof ids);
        ReleaseSRWLockShared(&g_doc_lock);
        if (fresh || (d == DOC_RATINGS && !ids[0])) continue;
        wchar_t path[400];
        if (d == DOC_PLAYER) swprintf(path, 400, L"/player?id=%hs", g_id);
        else if (d == DOC_BOARD_1V1) swprintf(path, 400, L"/leaderboard?ladder=1v1");
        else if (d == DOC_BOARD_TEAM) swprintf(path, 400, L"/leaderboard?ladder=team");
        else swprintf(path, 400, L"/ratings?ids=%hs", ids);
        int st = http(s, L"GET", path, NULL, buf);
        char *copy = st == 200 ? _strdup(buf) : NULL;
        AcquireSRWLockExclusive(&g_doc_lock);
        if (copy) { free(g_doc[d]); g_doc[d] = copy; }
        g_doc_state[d] = copy ? 1 : 2;
        g_doc_at[d] = now | 1;
        ReleaseSRWLockExclusive(&g_doc_lock);
    }
}

static DWORD WINAPI online_thread(LPVOID unused) {
    load_key();
    while (!steam_me()) Sleep(2000);
    rc_log("online: steam user %s found", g_id);
    HINTERNET s = WinHttpOpen(L"goh-mod", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"goh-mod", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { rc_log("online: WinHttpOpen failed %lu, off", GetLastError()); return 0; }
    WinHttpSetTimeouts(s, 5000, 5000, 5000, 5000);
    static char buf[MAX_BODY];
    DWORD last_beat = 0, last_list = 0;
    int beat_fails = 0;
    long wanted = 0;
    for (;;) {
        DWORD now = GetTickCount();
        if (g_online_consent == 1 && (InterlockedExchange(&g_beat_now, 0) || !last_beat || now - last_beat >= BEAT_MS)) {
            char body[256];
            snprintf(body, sizeof body, "%s\n%s\n%s\n%s", g_id, g_name, g_balance_tag, g_key);
            int st = http(s, L"POST", L"/hb", body, buf);
            last_beat = now;
            if (st != 200 && beat_fails++ < 3) rc_log("online: heartbeat got %d%s", st, st == 403 ? " (the SteamID is held by another key)" : "");
            if (st == 200) beat_fails = 0;
            if (st == 200 || st == 403) g_online_held = st == 403;
        }
        long want = g_online_want;
        if (want && (!wanted || now - last_list >= LIST_MS)) {
            int st = http(s, L"GET", L"/online", NULL, buf);
            last_list = now;
            if (st == 200) parse_list(buf);
            else { AcquireSRWLockExclusive(&g_lock); g_state = 2; ReleaseSRWLockExclusive(&g_lock); }
        }
        wanted = want;
        fetch_docs(s, buf);
        Sleep(500);
    }
}

void online_start(const char *dir) {
    snprintf(g_ini, sizeof g_ini, "%sgoh_online.ini", dir);
    g_online_consent = GetPrivateProfileIntA("online", "consent", -1, g_ini);
    rc_log("online: consent %ld", g_online_consent);
    CloseHandle(CreateThread(NULL, 0, online_thread, NULL, 0, NULL));
}
