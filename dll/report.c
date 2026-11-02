// Match reports for the rating server (server/src/matches.js), from the replays the game writes itself.
// After every multiplayer match the game writes autoreplayN.rep (a zip) into the profile's replays folder.
// Its replay.ss is the session card in the game's text format: game type, army selection mode, map, start and
// end time, every member (type, name, team a/b, army, SteamID), which team is Axis or Allies, and
// battleInfoTotal, whose "info" block (base64 of zlib) holds the winning team's letter (re/NOTES.md).
// A match that counts (Axis vs Allies, Battle Zones or Domination, equal human sides of 1..4, a winner, us in
// it) becomes a report; online.c sends it. Only replay.ss is read, not the whole replay.
#include <windows.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wctype.h>
#include "goh_replay.h"

// stb_image's zlib decoder, built in goh_replay.c
int stbi_zlib_decode_noheader_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);
char *stbi_zlib_decode_malloc(const char *buffer, int len, int *outlen);

#define BACKLOG_S (14 * 86400)      // replays older than this are not reported (the server takes 14 days)
#define SETTLE_S 20                 // a replay untouched this long is finished

static char g_ini[MAX_PATH];

// replay.ss out of a .rep: the zip's central directory from the end of the file, then that one entry
static char *read_ss(const wchar_t *file) {
    HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    char *ss = NULL;
    LARGE_INTEGER size;
    static uint8_t tail[65557];
    if (GetFileSizeEx(h, &size) && size.QuadPart > 22) {
        LONGLONG from = size.QuadPart > (LONGLONG)sizeof tail ? size.QuadPart - (LONGLONG)sizeof tail : 0;
        LARGE_INTEGER at = {.QuadPart = from};
        DWORD got = 0;
        if (SetFilePointerEx(h, at, NULL, FILE_BEGIN) && ReadFile(h, tail, (DWORD)(size.QuadPart - from), &got, NULL)) {
            for (long e = (long)got - 22; e >= 0 && !ss; e--) {
                if (memcmp(tail + e, "PK\5\6", 4)) continue;
                uint32_t cd_size, cd_off;
                memcpy(&cd_size, tail + e + 12, 4);
                memcpy(&cd_off, tail + e + 16, 4);
                uint8_t *cd = cd_size < (1u << 20) ? malloc(cd_size) : NULL;
                at.QuadPart = cd_off;
                if (cd && SetFilePointerEx(h, at, NULL, FILE_BEGIN) && ReadFile(h, cd, cd_size, &got, NULL) && got == cd_size) {
                    for (uint32_t c = 0; c + 46 <= cd_size && !memcmp(cd + c, "PK\1\2", 4);) {
                        uint16_t method, nl, xl, cl;
                        uint32_t csize, usize, lh;
                        memcpy(&method, cd + c + 10, 2); memcpy(&csize, cd + c + 20, 4); memcpy(&usize, cd + c + 24, 4);
                        memcpy(&nl, cd + c + 28, 2); memcpy(&xl, cd + c + 30, 2); memcpy(&cl, cd + c + 32, 2); memcpy(&lh, cd + c + 42, 4);
                        if (nl == 9 && !memcmp(cd + c + 46, "replay.ss", 9)) {
                            uint8_t local[30];
                            at.QuadPart = lh;
                            if (usize < (16u << 20) && csize < (16u << 20) && SetFilePointerEx(h, at, NULL, FILE_BEGIN) &&
                                ReadFile(h, local, 30, &got, NULL) && got == 30 && !memcmp(local, "PK\3\4", 4)) {
                                uint16_t lnl, lxl;
                                memcpy(&lnl, local + 26, 2); memcpy(&lxl, local + 28, 2);
                                at.QuadPart = (LONGLONG)lh + 30 + lnl + lxl;
                                char *packed = malloc(csize ? csize : 1), *plain = malloc(usize + 1);
                                int n = -1;
                                if (packed && plain && SetFilePointerEx(h, at, NULL, FILE_BEGIN) && ReadFile(h, packed, csize, &got, NULL) && got == csize) {
                                    if (method == 0 && csize == usize) { memcpy(plain, packed, usize); n = (int)usize; }
                                    else if (method == 8) n = stbi_zlib_decode_noheader_buffer(plain, (int)usize, packed, (int)csize);
                                }
                                free(packed);
                                if (n >= 0) { plain[n] = 0; ss = plain; } else free(plain);
                            }
                            break;
                        }
                        c += 46 + nl + xl + cl;
                    }
                }
                free(cd);
                break;
            }
        }
    }
    CloseHandle(h);
    return ss;
}

// {key value} or {key "value"} between from and to
static int field(const char *from, const char *to, const char *key, char *out, size_t n) {
    char pat[48];
    snprintf(pat, sizeof pat, "{%s ", key);
    size_t k = strlen(pat);
    for (const char *p = from; p && p + k < to; p++) {
        if (memcmp(p, pat, k)) continue;
        p += k;
        const char *end;
        if (*p == '"') { p++; end = memchr(p, '"', to - p); }
        else end = memchr(p, '}', to - p);
        if (!end || (size_t)(end - p) >= n) return 0;
        memcpy(out, p, end - p);
        out[end - p] = 0;
        return 1;
    }
    return 0;
}

static int b64(const char *s, size_t n, uint8_t *out, size_t cap) {
    static const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n && s[i] != '='; i++) {
        const char *c = strchr(A, s[i]);
        if (!c || !s[i]) return -1;
        acc = acc << 6 | (uint32_t)(c - A);
        if ((bits += 6) >= 8) { bits -= 8; if (o >= cap) return -1; out[o++] = (uint8_t)(acc >> bits); }
    }
    return (int)o;
}

// the winning team's letter, from battleInfoTotal's "info" block: zlib of "\1" + letter
static char winner_team(const char *ss) {
    const char *t = strstr(ss, "{battleInfoTotal");
    const char *p = t ? strstr(t, "{\"info\" \"B64:") : NULL;
    if (!p) return 0;
    p += 13;
    const char *end = strchr(p, '"');
    uint8_t raw[256];
    int n = end && end - p < 340 ? b64(p, end - p, raw, sizeof raw) : -1, len = 0;
    char *data = n > 0 ? stbi_zlib_decode_malloc((const char *)raw, n, &len) : NULL;
    char team = data && len >= 2 && data[0] == 1 && data[1] >= 'a' && data[1] <= 'h' ? data[1] : 0;
    free(data);
    return team;
}

typedef struct { char type[16], name[100], team[4], army[8], id[24]; } Member;

// the report body (server/src/matches.js "v2") for a match that counts; 0 with why when it does not
static int make_report(const char *ss, const char *me, const char *key, char *body, size_t n, long long *end_time, const char **why) {
    const char *stop = ss + strlen(ss);
    char type[32], sel[32], map[160], start[24], end[24], bots[8] = "0";
    *end_time = 0;
    if (!field(ss, stop, "gameStartTime", start, sizeof start) || !field(ss, stop, "gameEndTime", end, sizeof end)) { *why = "no times"; return 0; }
    *end_time = _atoi64(end);
    if (!field(ss, stop, "gameType", type, sizeof type) || (strcmp(type, "battle_zones") && strcmp(type, "ammunition"))) { *why = "mode not rated"; return 0; }
    if (!field(ss, stop, "armySelectionMode", sel, sizeof sel) || strcmp(sel, "alliance")) { *why = "not Axis vs Allies"; return 0; }
    field(ss, stop, "bots", bots, sizeof bots);
    if (strcmp(bots, "0")) { *why = "bots"; return 0; }
    if (!field(ss, stop, "map", map, sizeof map)) map[0] = 0;
    // which team is which side
    const char *teams = strstr(ss, "{teams {"), *alliances = teams ? strstr(teams, "{alliances") : NULL;
    if (!teams || !alliances) { *why = "no teams"; return 0; }
    char side_of[8][8] = {{0}};
    for (const char *p = teams; (p = strstr(p, "{name ")) && p < alliances; p++) {
        char tn[8], al[16];
        const char *next = strstr(p + 1, "{name ");
        if (!next || next > alliances) next = alliances;
        if (field(p, next, "name", tn, sizeof tn) && field(p, next, "alliance", al, sizeof al) && tn[0] >= 'a' && tn[0] <= 'h' && !tn[1])
            snprintf(side_of[tn[0] - 'a'], 8, "%s", al);
    }
    // the members
    const char *mem = strstr(ss, "{members {");
    if (!mem) { *why = "no members"; return 0; }
    static Member m[16];
    int count = 0;
    for (const char *p = mem; (p = strstr(p, "{type ")) && p < teams && count < 16; p++) {
        const char *next = strstr(p + 1, "{type ");
        if (!next || next > teams) next = teams;
        Member *x = &m[count];
        memset(x, 0, sizeof *x);
        field(p, next, "type", x->type, sizeof x->type);
        if (!strcmp(x->type, "spectator")) continue;
        if (strcmp(x->type, "player")) { *why = "not only players"; return 0; }
        if (!field(p, next, "name", x->name, sizeof x->name) || !field(p, next, "team", x->team, sizeof x->team) ||
            !field(p, next, "army", x->army, sizeof x->army) || !field(p, next, "steamId", x->id, sizeof x->id)) { *why = "member incomplete"; return 0; }
        for (char *c = x->name; *c; c++) if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
        count++;
    }
    int axis = 0, allies = 0, mine = 0;
    for (int i = 0; i < count; i++) {
        const char *s = m[i].team[0] >= 'a' && m[i].team[0] <= 'h' ? side_of[m[i].team[0] - 'a'] : "";
        if (!strcmp(s, "axis")) axis++;
        else if (!strcmp(s, "allies")) allies++;
        else { *why = "a team with no side"; return 0; }
        if (!strcmp(m[i].id, me)) mine = 1;
    }
    if (!mine) { *why = "we were not in it"; return 0; }
    if (!axis || axis != allies || axis > 4) { *why = "sides not equal"; return 0; }
    char w = winner_team(ss);
    const char *won = w ? side_of[w - 'a'] : "";
    if (strcmp(won, "axis") && strcmp(won, "allies")) { *why = "no winner"; return 0; }
    int o = snprintf(body, n, "%s\n%s\nv2\n%s\naxis_allies\n%s\n%s\n%s\n%s\n", me, key, type, map, start, end, won);
    for (int i = 0; i < count && o > 0 && (size_t)o < n; i++)
        o += snprintf(body + o, n - o, "%s\t%s\t%s\n", m[i].id, m[i].army, m[i].name);
    if (o <= 0 || (size_t)o >= n) { *why = "too long"; return 0; }
    return 1;
}

static long long filetime_unix(FILETIME ft) {
    ULARGE_INTEGER u = {.LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime};
    return (long long)(u.QuadPart / 10000000ULL) - 11644473600LL;
}

static int is_autoreplay(const wchar_t *n) {
    if (_wcsnicmp(n, L"autoreplay", 10)) return 0;
    const wchar_t *p = n + 10;
    if (!iswdigit(*p)) return 0;
    while (iswdigit(*p)) p++;
    return !_wcsicmp(p, L".rep");
}

static long long last_end(void) {
    char v[32];
    GetPrivateProfileStringA("reports", "last_end", "", v, sizeof v, g_ini);
    if (v[0]) return _atoi64(v);
    long long now = (long long)time(NULL);
    return now - BACKLOG_S;      // the first start: the last two weeks' matches go in too
}

void report_done(long long end_time) {
    char v[32];
    snprintf(v, sizeof v, "%lld", end_time);
    WritePrivateProfileStringA("reports", "last_end", v, g_ini);
}

// The next finished match not reported yet, oldest first: 1 with a body to send, 0 none. A replay that does
// not count is passed over for good (report_done) right here.
int report_next(const char *me, const char *key, char *body, size_t n, long long *end_time) {
    if (!me[0] || !key[0]) return 0;
    wchar_t docs[MAX_PATH], pattern[MAX_PATH], best[MAX_PATH] = L"";
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docs))) return 0;
    unsigned long long account = _strtoui64(me, NULL, 10) - 76561197960265728ULL;
    swprintf(pattern, MAX_PATH, L"%ls\\My Games\\gates of hell\\profiles\\%llu\\replays\\autoreplay*.rep", docs, account);
    long long after = last_end(), now = (long long)time(NULL), best_end = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    for (int tries = 0;; ) {
        long long written = filetime_unix(fd.ftLastWriteTime);
        // written after the last reported match ended (it is written at the end), and finished
        if (is_autoreplay(fd.cFileName) && written > after && now - written > SETTLE_S && tries < 64) {
            wchar_t file[MAX_PATH];
            swprintf(file, MAX_PATH, L"%ls\\My Games\\gates of hell\\profiles\\%llu\\replays\\%ls", docs, account, fd.cFileName);
            char *ss = read_ss(file);
            tries++;
            if (ss) {
                long long e = 0;
                char t[24];
                if (field(ss, ss + strlen(ss), "gameEndTime", t, sizeof t)) e = _atoi64(t);
                if (e > after && (!best_end || e < best_end)) { best_end = e; wcscpy(best, file); }
                free(ss);
            }
        }
        if (!FindNextFileW(f, &fd)) break;
    }
    FindClose(f);
    if (!best[0]) return 0;
    char *ss = read_ss(best);
    if (!ss) return 0;
    const char *why = "";
    int ok = make_report(ss, me, key, body, n, end_time, &why);
    free(ss);
    if (!ok) {
        rc_log("report: match ending %lld not rated here (%s)", best_end, why);
        report_done(best_end);
        return 0;
    }
    *end_time = best_end;
    return 1;
}

void report_init(const char *dir) {
    snprintf(g_ini, sizeof g_ini, "%sgoh_online.ini", dir);
}
