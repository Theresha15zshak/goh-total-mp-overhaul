// Who plays with the mod right now: our server goh-online (server/ in this repo, Cloudflare Workers).
// Only with the player's yes (asked once, kept in goh_online.ini): every 2 minutes the SteamID, the Steam
// name and the balance version go to the server. The list is fetched while the Shift+F6 window is open.
// The SteamID and name come from the game's own steam_api64.dll; the server cannot verify them.
// Steam sign-in: the server knows a SteamID is ours once the player signed in on steamcommunity.com (the
// game's Steam overlay or a browser, server/src/verify.js); until then the player is Anonymous in the chat and
// sends no match reports. Whether we are signed in comes with each heartbeat.
// The global chat (server/src/chat.js) goes through here too: read while it is on screen, written with the key.
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
#define REPORT_MS 15000         // a look for finished matches in the replays folder
#define MAX_BODY 32768

volatile long g_online_consent = -1;
volatile long g_online_want;
volatile long g_online_held;
volatile long g_steam_verified = -1;
static volatile long g_login_want;          // the player asked to sign in
static char g_login_url[1024];              // where to sign in, once the server gave a code for it
static DWORD g_login_at;                    // when; the status is asked every few seconds for 15 minutes
static char g_ini[MAX_PATH];
static char g_key[65];          // 64 hex characters, "" if it could be neither read nor made
static char g_id[18];           // our SteamID, "" until Steam is up
static char g_name[ONLINE_NAME];
static SRWLOCK g_lock = SRWLOCK_INIT;
static OnlinePlayer g_list[ONLINE_MAX];
static int g_count, g_state;
static volatile long g_beat_now;   // consent just given: send at once

const char *online_my_id(void) { return g_id; }

// ---- the language: the ini's if set, else the game's in Steam, else Windows' ---------------------------------
static volatile long g_lang = -1;              // 0 Russian, 1 English, -1 not decided yet

static int russian_lang(const char *steam) {
    return !strcmp(steam, "russian") || !strcmp(steam, "ukrainian") || !strcmp(steam, "belarusian");
}

int lang_en(void) {
    long v = g_lang;
    if (v >= 0) return (int)v;
    LANGID l = PRIMARYLANGID(GetUserDefaultUILanguage());
    return !(l == LANG_RUSSIAN || l == LANG_UKRAINIAN || l == LANG_BELARUSIAN);
}

static void lang_decide(void) {
    char set[8] = "";
    GetPrivateProfileStringA("ui", "language", "", set, sizeof set, g_ini);
    if (!_stricmp(set, "ru") || !_stricmp(set, "en")) { g_lang = !_stricmp(set, "en"); rc_log("ui: language %s (goh_online.ini)", set); return; }
    HMODULE h = GetModuleHandleA("steam_api64.dll");
    void *(*apps_if)(void) = h ? (void *(*)(void))GetProcAddress(h, "SteamAPI_SteamApps_v008") : NULL;
    const char *(*cur)(void *) = h ? (const char *(*)(void *))GetProcAddress(h, "SteamAPI_ISteamApps_GetCurrentGameLanguage") : NULL;
    void *apps = apps_if ? apps_if() : NULL;
    const char *steam = apps && cur ? cur(apps) : NULL;
    if (steam && *steam) g_lang = !russian_lang(steam);
    rc_log("ui: language %s (the game's in Steam: %s)", lang_en() ? "en" : "ru", steam && *steam ? steam : "unknown, Windows' then");
}

// a SteamID as the server sends it: 17 digits, nothing else (it ends up in steam:// addresses)
int steam_id_ok(const char *s) { return s && strlen(s) == 17 && strspn(s, "0123456789") == 17 && !strncmp(s, "7656119", 7); }

int online_copy(OnlinePlayer *out, int max, int *state) {
    AcquireSRWLockShared(&g_lock);
    int n = g_count < max ? g_count : max;
    memcpy(out, g_list, n * sizeof *out);
    *state = g_state;
    ReleaseSRWLockShared(&g_lock);
    return n;
}

// consent_v2: the question covers match reports too (the first one, "consent", only the online list)
void online_set_consent(int yes) {
    g_online_consent = yes;
    WritePrivateProfileStringA("online", "consent_v2", yes ? "1" : "0", g_ini);
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
        char *f[5] = {0};
        int k = 0;
        f[k++] = p;
        for (char *q = p; *q && k < 4; q++) if (*q == '\t') { *q = 0; f[k++] = q + 1; }
        if (k == 4 && f[3] && strchr(f[3], '\t')) {         // ?v=2: "\tverified" after the seconds
            char *t = strchr(f[3], '\t');
            *t = 0;
            f[k++] = t + 1;
        }
        if (k < 4 || !steam_id_ok(f[0])) continue;
        if (strlen(f[1]) >= ONLINE_NAME || strlen(f[2]) >= sizeof tmp->version) continue;
        OnlinePlayer *o = &tmp[n++];
        strcpy(o->id, f[0]);
        strcpy(o->name, f[1]);
        strcpy(o->version, f[2]);
        o->ago = atoi(f[3]);
        o->verified = k > 4 && atoi(f[4]) == 1;
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
static char g_doc_ids[DOC_N][16 * 18 + 1];     // DOC_RATINGS, DOC_LOBBY, DOC_CHAT: comma separated SteamIDs
static SRWLOCK g_doc_lock = SRWLOCK_INIT;

void online_want(int doc) { g_doc_want[doc] = GetTickCount() | 1; }

void online_doc_ids(int doc, const char *ids) {
    AcquireSRWLockExclusive(&g_doc_lock);
    if (strcmp(ids, g_doc_ids[doc])) { snprintf(g_doc_ids[doc], sizeof g_doc_ids[doc], "%s", ids); g_doc_at[doc] = 0; }
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
        char ids[sizeof g_doc_ids[0]];
        memcpy(ids, g_doc_ids[d], sizeof ids);
        ReleaseSRWLockShared(&g_doc_lock);
        if (fresh || ((d == DOC_RATINGS || d == DOC_LOBBY || d == DOC_CHAT) && !ids[0])) continue;
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

// ---- global chat: the channel on screen is fetched every CHAT_MS, only what is new unless the server took
// messages away (its epoch changed); what the player writes goes out from this thread too -------------------
#define CHAT_MS 3000
static const char *const CHAT_CH[2] = {"ru", "en"};
static volatile DWORD g_chat_want[2];
static ChatMsg g_chat[2][CHAT_MAX];
static int g_chat_n[2];
static long long g_chat_epoch[2] = {-1, -1};
static DWORD g_chat_at[2];
static ChatStatus g_chat_st;
static char g_chat_out[2][CHAT_TEXT], g_mod_out[128];      // waiting to be sent
static SRWLOCK g_chat_lock = SRWLOCK_INIT;

void chat_want(int ch) { g_chat_want[ch & 1] = GetTickCount() | 1; }

int chat_copy(int ch, ChatMsg *out, int max, ChatStatus *st) {
    AcquireSRWLockShared(&g_chat_lock);
    int n = g_chat_n[ch & 1] < max ? g_chat_n[ch & 1] : max;
    memcpy(out, g_chat[ch & 1] + g_chat_n[ch & 1] - n, n * sizeof *out);
    *st = g_chat_st;
    ReleaseSRWLockShared(&g_chat_lock);
    return n;
}

void chat_send(int ch, const char *text) {
    AcquireSRWLockExclusive(&g_chat_lock);
    snprintf(g_chat_out[ch & 1], sizeof g_chat_out[0], "%s", text);
    for (char *p = g_chat_out[ch & 1]; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    g_chat_st.sent[0] = 0;
    ReleaseSRWLockExclusive(&g_chat_lock);
}

void chat_mod(const char *action, const char *target, int days) {
    AcquireSRWLockExclusive(&g_chat_lock);
    snprintf(g_mod_out, sizeof g_mod_out, "%s\n%s\n%d", action, target, days);
    ReleaseSRWLockExclusive(&g_chat_lock);
}

// the server's lines (server/src/chat.js) into the channel and the status
static void chat_parse(int ch, char *t) {
    static ChatMsg add[CHAT_MAX];
    static ChatStatus st;
    int n = 0;
    long long epoch = -1;
    memset(&st, 0, sizeof st);
    st.state = 1;
    for (char *line = t, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *f[7];
        int k = 0;
        f[k++] = line;
        for (char *q = line; *q && k < 7; q++) if (*q == '\t') { *q = 0; f[k++] = q + 1; }
        if (!strcmp(f[0], "E") && k >= 2) epoch = _atoi64(f[1]);
        else if (!strcmp(f[0], "S") && k >= 2) { if (!strcmp(f[1], "ban")) st.banned = 1; else if (k >= 3) st.muted_until = _atoi64(f[2]); }
        else if (!strcmp(f[0], "A")) st.moderator = 1;
        else if (!strcmp(f[0], "B") && k >= 5 && st.bans < 50 && steam_id_ok(f[1])) {
            ChatBan *b = &st.ban[st.bans++];
            strcpy(b->sid, f[1]);
            snprintf(b->name, sizeof b->name, "%s", f[2]);
            b->ban = !strcmp(f[3], "ban");
            b->until = _atoi64(f[4]);
        } else if (!strcmp(f[0], "M") && k >= 6 && n < CHAT_MAX && steam_id_ok(f[3])) {
            ChatMsg *m = &add[n++];
            m->id = _atoi64(f[1]);
            m->at_ms = _atoi64(f[2]);
            strcpy(m->sid, f[3]);
            snprintf(m->name, sizeof m->name, "%s", f[4]);
            snprintf(m->text, sizeof m->text, "%s", f[5]);
            m->verified = k > 6 && atoi(f[6]) == 1;
        }
    }
    if (epoch < 0) return;
    AcquireSRWLockExclusive(&g_chat_lock);
    if (epoch != g_chat_epoch[ch]) g_chat_n[ch] = 0;      // the whole channel came: start over
    g_chat_epoch[ch] = epoch;
    int have = g_chat_n[ch], drop = have + n - CHAT_MAX;
    if (drop > 0) { memmove(g_chat[ch], g_chat[ch] + drop, (have - drop) * sizeof add[0]); have -= drop; }
    memcpy(g_chat[ch] + have, add, n * sizeof add[0]);
    g_chat_n[ch] = have + n;
    memcpy(st.sent, g_chat_st.sent, sizeof st.sent);
    g_chat_st = st;
    ReleaseSRWLockExclusive(&g_chat_lock);
}

static int chat_busy(void) { return g_chat_out[0][0] || g_chat_out[1][0] || g_mod_out[0]; }

static void chat_tick(HINTERNET s, char *buf) {
    DWORD now = GetTickCount();
    char out[CHAT_TEXT] = "", mod[128] = "";
    int out_ch = 0;
    AcquireSRWLockExclusive(&g_chat_lock);
    for (int c = 0; c < 2 && !out[0]; c++)
        if (g_chat_out[c][0]) { memcpy(out, g_chat_out[c], sizeof out); g_chat_out[c][0] = 0; out_ch = c; }
    memcpy(mod, g_mod_out, sizeof mod);
    g_mod_out[0] = 0;
    ReleaseSRWLockExclusive(&g_chat_lock);
    if ((out[0] || mod[0]) && (g_online_consent != 1 || !g_key[0])) {
        AcquireSRWLockExclusive(&g_chat_lock);
        snprintf(g_chat_st.sent, sizeof g_chat_st.sent, "%s", TR("нужно согласие на онлайн (Shift+F6)", "online consent needed (Shift+F6)"));
        ReleaseSRWLockExclusive(&g_chat_lock);
        out[0] = mod[0] = 0;
    }
    static char body[CHAT_TEXT + 256];
    if (out[0]) {
        snprintf(body, sizeof body, "%s\n%s\n%s\n%s", g_id, g_key, CHAT_CH[out_ch], out);
        int st = http(s, L"POST", L"/chat", body, buf);
        const char *why = st == 200 ? "" : !strcmp(buf, "flood") ? TR("слишком много сообщений подряд, подождите", "too many messages in a row, wait") :
                          !strcmp(buf, "repeat") ? TR("это сообщение уже было", "you already sent that") : st == 429 ? TR("не так часто", "not so often") :
                          !strncmp(buf, "muted", 5) ? TR("вам запрещено писать", "you may not write") :
                          !strcmp(buf, "banned") ? TR("вы забанены в чате", "you are banned from the chat") :
                          st == 403 ? TR("сервер не знает ваш ключ", "the server does not know your key") : TR("не отправлено", "not sent");
        AcquireSRWLockExclusive(&g_chat_lock);
        snprintf(g_chat_st.sent, sizeof g_chat_st.sent, "%s", why);
        ReleaseSRWLockExclusive(&g_chat_lock);
        g_chat_at[out_ch] = 0;                                  // and the channel at once
    }
    if (mod[0]) {
        snprintf(body, sizeof body, "%s\n%s\n%s", g_id, g_key, mod);
        int st = http(s, L"POST", L"/chatmod", body, buf);
        rc_log("chat: moderation %.10s: %d %.40s", mod, st, buf);
        g_chat_at[0] = g_chat_at[1] = 0;
    }
    for (int c = 0; c < 2; c++) {
        DWORD want = g_chat_want[c];
        if (!want || now - want > 2000 || (g_chat_at[c] && now - g_chat_at[c] < CHAT_MS)) continue;
        AcquireSRWLockShared(&g_chat_lock);
        long long after = g_chat_n[c] ? g_chat[c][g_chat_n[c] - 1].id : 0, epoch = g_chat_epoch[c];
        ReleaseSRWLockShared(&g_chat_lock);
        wchar_t path[200];
        // our SteamID only with the player's yes: it tells the server whose mute / moderator rights to show
        swprintf(path, 200, L"/chat?ch=%hs&after=%lld&epoch=%lld&id=%hs", CHAT_CH[c], after, epoch,
                 g_online_consent == 1 ? g_id : "");
        int st = http(s, L"GET", path, NULL, buf);
        g_chat_at[c] = now | 1;
        if (st == 200) chat_parse(c, buf);
        else { AcquireSRWLockExclusive(&g_chat_lock); g_chat_st.state = 2; ReleaseSRWLockExclusive(&g_chat_lock); }
    }
}

// ---- Steam sign-in: a one-time code from the server, Steam's page with it, then the answer awaited ---------
// Steam OpenID 2.0: Steam sends the player back to /verify/back on our server with the code, signed by Steam
#define OPENID_SELECT "http%3A%2F%2Fspecs.openid.net%2Fauth%2F2.0%2Fidentifier_select"
#define STEAM_LOGIN_A "https://steamcommunity.com/openid/login?openid.ns=http%3A%2F%2Fspecs.openid.net%2Fauth%2F2.0" \
                      "&openid.mode=checkid_setup&openid.return_to=https%3A%2F%2Fgoh-online.goh-mod.workers.dev%2Fverify%2Fback%3Ft%3D"
#define STEAM_LOGIN_B "&openid.realm=https%3A%2F%2Fgoh-online.goh-mod.workers.dev%2F&openid.identity=" OPENID_SELECT \
                      "&openid.claimed_id=" OPENID_SELECT
void online_steam_login(void) {
    if (g_online_consent != 1) online_set_consent(1);   // signing in is asking to take part
    g_login_want = 1;
}

int online_login_url(char *out, size_t n) {
    AcquireSRWLockShared(&g_lock);
    int ok = g_login_url[0] != 0;
    snprintf(out, n, "%s", g_login_url);
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

int online_remind(void) { return GetPrivateProfileIntA("online", "steam_remind", 1, g_ini); }
void online_set_remind(int on) { WritePrivateProfileStringA("online", "steam_remind", on ? "1" : "0", g_ini); }

static void login_tick(HINTERNET s, char *buf) {
    static DWORD asked;
    DWORD now = GetTickCount();
    char body[128];
    if (!g_key[0]) return;
    snprintf(body, sizeof body, "%s\n%s", g_id, g_key);
    if (InterlockedExchange(&g_login_want, 0)) {
        int st = http(s, L"POST", L"/verify/start", body, buf);
        rc_log("online: steam sign-in asked: %d", st);
        // Only the server's one-time code is taken (48 hex digits after "?t="): the address opened is Steam's
        // own sign-in page, put together here. A server that is not ours, or ours broken into, cannot send the
        // player to a page of its own asking for their Steam password.
        const char *t = st == 200 ? strstr(buf, "?t=") : NULL;
        char token[49] = "";
        if (t && strspn(t + 3, "0123456789abcdef") == 48 && !t[51]) memcpy(token, t + 3, 48);
        AcquireSRWLockExclusive(&g_lock);
        if (token[0])
            snprintf(g_login_url, sizeof g_login_url, "%s%s%s", STEAM_LOGIN_A, token, STEAM_LOGIN_B);
        ReleaseSRWLockExclusive(&g_lock);
        g_login_at = now | 1;
        asked = 0;
    }
    if (!g_login_at || g_steam_verified == 1) return;
    if (now - g_login_at > 15 * 60000) { g_login_at = 0; return; }
    if (asked && now - asked < 4000) return;
    asked = now;
    if (http(s, L"POST", L"/verify/status", body, buf) != 200) return;
    long v = !strcmp(buf, "verified") ? 1 : !strcmp(buf, "held") ? 2 : 0;
    if (v == 1 && g_steam_verified != 1) {
        rc_log("online: signed in with Steam");
        g_login_at = 0;
        AcquireSRWLockExclusive(&g_doc_lock);
        g_doc_at[DOC_PLAYER] = 0;
        ReleaseSRWLockExclusive(&g_doc_lock);
        g_beat_now = 1;                         // the online list gets our name
    }
    g_steam_verified = v;
}

// ---- is this DLL an official build: the SHA-256 of the file loaded against the server's list ---------------
// A changed DLL can of course claim anything about itself: this catches an old build or a copy someone changed
// without bothering, not a determined forger (that check is the server's page /versions.html, by hand).
volatile long g_integrity;
char g_integrity_newest[32];
char g_integrity_newest_sha[65];
char g_integrity_kind[16];
const char g_mod_version[] = MOD_VERSION;

static int own_sha256(char out[65]) {
    HMODULE me = NULL;
    char path[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(void *)own_sha256, &me) ||
        !GetModuleFileNameA(me, path, sizeof path))
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = n > 0 && n < (64 << 20) ? malloc(n) : NULL;
    int ok = b && fread(b, 1, n, f) == (size_t)n;
    fclose(f);
    unsigned char h[32];
    ok = ok && !BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, b, (ULONG)n, h, 32);
    free(b);
    for (int i = 0; ok && i < 32; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
    return ok;
}

// "sha256\tversion\tkind\treleased_at\tcurrent" per official build, newest first
static void integrity_check(HINTERNET s, char *buf) {
    char mine[65];
    if (!own_sha256(mine)) { g_integrity = INTEGRITY_UNKNOWN; rc_log("integrity: own file not readable"); return; }
    if (http(s, L"GET", L"/versions", NULL, buf) != 200) { g_integrity = INTEGRITY_UNREACHABLE; return; }
    char kind[16] = "", newest[32] = "", newest_sha[65] = "";
    int found = 0, current = 0;
    for (char *line = buf, *next; line && *line; line = next) {       // first: which build this is
        next = strchr(line, '\n');
        char *f[5];
        int k = 0;
        f[k++] = line;
        for (char *q = line; *q && *q != '\n' && k < 5; q++) if (*q == '\t') f[k++] = q + 1;
        if (k == 5 && !strncmp(f[0], mine, 64)) {
            found = 1;
            current = f[4][0] == '1';
            snprintf(kind, sizeof kind, "%.*s", (int)(f[3] - f[2] - 1), f[2]);
        }
        if (next) next++;
    }
    for (char *line = buf, *next; found && !current && line && *line; line = next) {   // then: the build to have
        next = strchr(line, '\n');
        char *f[5];
        int k = 0;
        f[k++] = line;
        for (char *q = line; *q && *q != '\n' && k < 5; q++) if (*q == '\t') f[k++] = q + 1;
        if (k == 5 && f[4][0] == '1' && (size_t)(f[3] - f[2] - 1) == strlen(kind) && !strncmp(f[2], kind, strlen(kind)) && !newest[0]) {
            snprintf(newest, sizeof newest, "%.*s", (int)(f[2] - f[1] - 1), f[1]);
            if (f[1] - f[0] - 1 == 64) snprintf(newest_sha, sizeof newest_sha, "%.64s", f[0]);
        }
        if (next) next++;
    }
    memcpy(g_integrity_newest, newest, sizeof newest);
    memcpy(g_integrity_newest_sha, newest_sha, sizeof newest_sha);
    memcpy(g_integrity_kind, kind, sizeof kind);
    g_integrity = !found ? INTEGRITY_UNKNOWN : current ? INTEGRITY_CURRENT : INTEGRITY_OLD;
    rc_log("integrity: %s, version %s, sha256 %.16s...", !found ? "not an official build" : current ? "official, current" : "official, old",
           g_mod_version, mine);
}

static DWORD WINAPI online_thread(LPVOID unused) {
    load_key();
    while (!steam_me()) Sleep(2000);
    lang_decide();
    steam_lobby_watch();
    pvp_lobby_pings();
    rc_log("online: steam user %s found", g_id);
    HINTERNET s = WinHttpOpen(L"goh-mod", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"goh-mod", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { rc_log("online: WinHttpOpen failed %lu, off", GetLastError()); return 0; }
    WinHttpSetTimeouts(s, 5000, 5000, 5000, 5000);
    static char buf[MAX_BODY], body_r[4096];
    integrity_check(s, buf);
    DWORD integrity_at = GetTickCount();
    DWORD last_beat = 0, last_list = 0, last_report = 0;
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
            if (st == 200) g_steam_verified = strstr(buf, "verified") ? 1 : 0;
            else if (st == 403) g_steam_verified = 2;
        }
        long want = g_online_want;
        if (want && (!wanted || now - last_list >= LIST_MS)) {
            int st = http(s, L"GET", L"/online?v=2", NULL, buf);
            last_list = now;
            if (st == 200) parse_list(buf);
            else { AcquireSRWLockExclusive(&g_lock); g_state = 2; ReleaseSRWLockExclusive(&g_lock); }
        }
        wanted = want;
        // finished matches out of the game's replays, one at a time (report.c); and their names
        if (!last_report || now - last_report >= REPORT_MS) replays_rename(g_id);
        if (g_online_consent == 1 && g_key[0] && g_steam_verified == 1 && (!last_report || now - last_report >= REPORT_MS)) {
            last_report = now | 1;
            long long end = 0;
            if (report_next(g_id, g_key, body_r, sizeof body_r, &end)) {
                int st = http(s, L"POST", L"/report", body_r, buf);
                rc_log("report: match ending %lld sent: %d %.80s", end, st, buf);
                if (st == 200 || (st >= 400 && st < 500 && st != 429)) {     // taken, or never will be
                    report_done(end);
                    AcquireSRWLockExclusive(&g_doc_lock);
                    g_doc_at[DOC_PLAYER] = 0;                               // the stats tab fetches again
                    ReleaseSRWLockExclusive(&g_doc_lock);
                }
            }
        }
        if (g_integrity == INTEGRITY_UNREACHABLE && now - integrity_at > 60000) { integrity_at = now; integrity_check(s, buf); }
        fetch_docs(s, buf);
        chat_tick(s, buf);
        login_tick(s, buf);
        Sleep(chat_busy() ? 100 : 500);
    }
}

void online_start(const char *dir) {
    snprintf(g_ini, sizeof g_ini, "%sgoh_online.ini", dir);
    g_online_consent = GetPrivateProfileIntA("online", "consent_v2", -1, g_ini);
    report_init(dir);
    rc_log("online: consent %ld", g_online_consent);
    CloseHandle(CreateThread(NULL, 0, online_thread, NULL, 0, NULL));
}
