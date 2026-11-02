// Shared between the replay logic (goh_replay.c) and the in-game panel (ui.cpp).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { CMD_NONE, CMD_SPEED, CMD_PAUSE, CMD_SEEK, CMD_POV, CMD_PREPARE };   // CMD_PREPARE: 1 start, 0 stop
enum { ST_OFF, ST_IDLE, ST_REPLAY, ST_SEEKING, ST_ERROR, ST_REWIND };

typedef struct {
    // panel -> logic
    volatile long cmd_seq;      // bumped after filling cmd/arg
    int32_t cmd;
    double arg;
    // logic -> panel
    volatile long ack_seq;      // = cmd_seq once handled
    int32_t state, game_ms, paused, seek_target_ms, duration_s;
    int32_t has_pov, pov;       // a camera recording exists for this replay / it drives the camera
    float speed;                // what the user picked
    int32_t seek_from_ms;       // where the running seek started
    float rate;                 // game seconds per real second while seeking
    int32_t snap_from_ms, snap_to_ms, snap_mb, snap_state;   // instant jumps possible in [from, to]
    int32_t preparing;          // the whole replay is being run through for snapshots
    char path[260];
    char msg[160];              // UTF-8
} Ctl;

extern Ctl g_ctl;

// ---- match charts: per-player history, one sample per game second ----
// MS_ARMY_MP: army on the field in manpower, estimated: limit points on the field x the player's MP spent per
// limit point bought. Kills and losses apart for infantry (eHuman) and vehicles (everything else).
enum { MS_SPENT, MS_ARMY_MP, MS_BUDGET, MS_ARMY, MS_LOST, MS_KILLS_INF, MS_KILLS_VEH, MS_DEATHS_INF, MS_DEATHS_VEH, MS_SERIES };
#define MS_PLAYERS 8
#define MS_SAMPLES 7200         // two hours

typedef struct {
    volatile long n;            // samples filled (written last)
    volatile long epoch;        // bumped when a new match starts
    int32_t players;
    int32_t id[MS_PLAYERS];     // player number as the game's log writes it (player#N)
    char name[MS_PLAYERS][64];  // UTF-8, from the replay's card once known ("" until then)
    int32_t t_ms[MS_SAMPLES];
    float v[MS_SERIES][MS_PLAYERS][MS_SAMPLES];
} MatchStats;

extern MatchStats g_ms;
extern volatile long g_ms_results;  // the post-match results page is on screen
extern volatile long g_live_match;  // a battle is being played (not a replay): no charts, they show the enemy's numbers

// ---- music director (music.cpp): our own battle music in place of the maps' music triggers ----
enum { MUS_MENU, MUS_CALM, MUS_TENSION, MUS_BATTLE };

typedef struct {
    volatile long enabled;      // play our music during battles (game music muted meanwhile)
    volatile float volume;      // 0..1
    // status for the panel, written by the audio thread
    volatile long active;       // a battle is on and we play it
    int level, target;          // level of the track playing (0 = none) / what the battle calls for
    int tracks[4];              // files found per level (MUS_MENU = main menu)
    float heat;                 // recent fighting
    char now[200];              // UTF-8: what plays right now
} MusicCtl;

extern MusicCtl g_music;
extern volatile long g_music_stop_vanilla;   // set by the director, done on the logic thread

void music_start(const char *dir);           // dir: the DLL folder, ends with a backslash
void music_tick(int in_match, int game_ms, int players);   // worker thread, every 50 ms; in_match -1 = quiet
void music_kill(void);                       // a unit died, any side
int music_block_game_music(void);           // the game wants a /music/ track: 1 = mute it, ours plays
void music_save(void);                       // the panel changed enabled/volume
void music_open_folder(void);

// ---- snapshots (snap.c): the game's heap in a range of ours, so it can be put back ----
int snap_install(uint8_t *exe);              // DllMain, before the game's CRT starts
int snap_owns(const void *p);
void snap_stats(uint64_t *committed, uint64_t *used, int64_t *live_bytes, int64_t *live_blocks);
int snap_session_start(int game_ms);         // logic thread, a replay is on: base snapshot starts (1 = started)
void snap_session_end(void);                 // logic thread, the replay is over
void snap_tick(int game_ms);                 // logic thread, every frame: base done, a snapshot every 15 s
int snap_restore_to(int target_ms, int *pages);  // logic thread: back at the latest snapshot <= target; its time,
                                             // -1 = not now (next frame), -2 = no snapshot there
int snap_best(int target_ms);                // time of the snapshot a jump to target_ms would land on, -1 = none
#define SNAP_EVERY_MS 15000                  // game time between snapshots: a jump lands at most this far before the click
void snap_info(int *from_ms, int *to_ms, int *mb, int *state);   // state 0 off, 1 base being made, 2 usable
const char *snap_busy(void);                 // why the world would not stop
// keep.c: Direct3D and sound objects the game destroys are held while snapshots may still point at them
int keep_install(uint8_t *exe);
void keep_begin(void);
void keep_end(void);                         // game's logic thread, replay over
int keep_active(void);
void keep_stats(long *d3d, long *streams, long *mem, long *let_go);
void game_sound_mute(int on);                // the game's own sound off while a replay is prepared
// prof.c: where the game's logic and quant threads spend their time (logged every 10 s while running)
void prof_start(DWORD logic_tid, DWORD quant_tid);
void prof_stop(void);
void prof_lock(void);                        // held by a stopped world (snap.c)
void prof_unlock(void);

// ---- competitive balance (balance.c): our copies of a few balance files while the switch is on ----
extern volatile long g_balance_on;          // the panel's switch (off at every start)
extern volatile long g_balance_ready;       // our copies are in place and the game's code and files matched
extern volatile long g_balance_locked;      // a match or replay is on: the switch waits for the next one
extern volatile long g_balance_reads;       // times the game opened one of the files
extern volatile long g_balance_last;        // ... and whether it got our copy the last time
extern const char g_balance_tag[];          // version of the switch: every player of a match needs the same one
// server: in the match server the game starts (call_to_arms_server.exe), which follows the game's switch
int balance_install(uint8_t *exe, const char *dir, uint8_t *(*alloc)(size_t), void (*patch)(void *, const void *, size_t),
                    int server);
void balance_tick(void);                    // worker: logs what each roster copy holds after a change
void balance_dump(const char *why);

// ---- who is online with the mod (online.c, our server goh-online) ----
#define ONLINE_MAX 200
#define ONLINE_NAME 132             // Steam names are up to 32 characters of UTF-8
typedef struct { char id[18]; char name[ONLINE_NAME]; char version[17]; int ago, verified; } OnlinePlayer;
extern volatile long g_online_consent;      // -1 not asked yet, 0 no, 1 we report this player as online
extern volatile long g_online_want;         // the window is open: the list is fetched every 20 s
extern volatile long g_online_held;         // the server refused us: this SteamID is held by another key
// Steam sign-in (server/src/verify.js): -1 not known yet, 0 not signed in (Anonymous in the chat, no reports),
// 1 signed in, 2 another key holds our SteamID
extern volatile long g_steam_verified;
void online_steam_login(void);              // the player asked: the sign-in address is fetched
int online_login_url(char *out, size_t n);  // 1 = the address to open (once fetched), the login is awaited
int online_remind(void);                    // the start-up window about the sign-in is wanted
void online_set_remind(int on);
// Is this DLL an official build? Its SHA-256 against the server's list (tools/release.py, server/src/versions.js)
enum { INTEGRITY_CHECKING, INTEGRITY_CURRENT, INTEGRITY_OLD, INTEGRITY_UNKNOWN, INTEGRITY_UNREACHABLE };
extern volatile long g_integrity;
extern char g_integrity_newest[32];         // INTEGRITY_OLD: the version to update to
extern const char g_mod_version[];
void online_start(const char *dir);
void online_set_consent(int yes);
int online_copy(OnlinePlayer *out, int max, int *state);   // state 0 nothing yet, 1 list, 2 server unreachable
const char *online_my_id(void);
int steam_id_ok(const char *s);             // 17 digits of a SteamID             // "" until Steam is up
// texts of the rating server (server/src/matches.js), fetched while the panel asks for them each frame
enum { DOC_PLAYER, DOC_BOARD_1V1, DOC_BOARD_TEAM, DOC_RATINGS, DOC_LOBBY, DOC_CHAT, DOC_N };   // DOC_RATINGS: the online tab,
                                                                                            // DOC_CHAT: the chat's online list
void online_want(int doc);                  // on screen this frame
void online_doc_ids(int doc, const char *ids);   // DOC_RATINGS / DOC_LOBBY / DOC_CHAT: whose (comma separated SteamIDs, at most 16)
int online_doc(int doc, char *out, size_t n, int *state);   // the last text; state 0 none yet, 1 ok, 2 unreachable

// ---- global chat (online.c, server/src/chat.js): channels RU and EN, wiped by the server every two hours ----
#define CHAT_MAX 200
#define CHAT_TEXT 904               // 300 characters of UTF-8
typedef struct { long long id, at_ms; char sid[18]; char name[ONLINE_NAME]; char text[CHAT_TEXT]; int verified; } ChatMsg;
typedef struct { char sid[18]; char name[ONLINE_NAME]; int ban; long long until; } ChatBan;
typedef struct {
    int state;                      // 0 nothing yet, 1 ok, 2 server unreachable
    int banned, moderator;
    long long muted_until;          // unix seconds, 0 = may write
    char sent[64];                  // how the last message went: "" ok, else why not (UTF-8)
    int bans;                       // moderators: punishments in force
    ChatBan ban[50];
} ChatStatus;
void chat_want(int ch);                      // on screen this frame: 0 RU, 1 EN
int chat_copy(int ch, ChatMsg *out, int max, ChatStatus *st);
void chat_send(int ch, const char *text);    // queued, the online thread sends it
void chat_mod(const char *action, const char *target, int days);   // mute (days) / ban / unban / del

// ---- match reports out of the game's own replays (report.c), sent by online.c ----
void report_init(const char *dir);
int report_next(const char *me, const char *key, char *body, size_t n, long long *end_time);   // 1 = body to send
void report_done(long long end_time);       // that match is dealt with (taken, or never will be)
// player number (player#N) -> name, from a replay's card: rel is the game's profile-relative path
// ("//replays/x.rep"), "" for the newest replay written in the last 10 minutes; 1 when found
int replay_player_names(const char *rel, char names[][64], int max);
void replays_rename(const char *me);        // finished autoreplays get names of their own: mode_3v3_map_army_date

// ---- PVP button in the game's Internet / LAN game list (pvp.c): our block over its list of games ----
extern volatile long g_pvp_on;              // the button was clicked: the block is wanted while the page is up
int pvp_install(uint8_t *exe, const char *dir, uint8_t *(*alloc)(size_t), void (*patch)(void *, const void *, size_t));
// the list's place on the game's screen with its column titles head_h high on top (game units, the screen
// being root_w x root_h); 0 = the page is not up
int pvp_rect(float *x, float *y, float *w, float *h, float *head_h, float *root_w, float *root_h);
// the empty frame our layout puts over the friends for the global chat, as pvp_rect; 0 = not on screen
int chat_rect(float *x, float *y, float *w, float *h, float *root_w, float *root_h);
// the game's chat panel (the lobby's, with its tabs MAIN LOBBY | SESSION) where it is on screen: its place, where
// its channel starts below the tabs, the right end of the tabs and each tab's place (game units, as pvp_rect)
typedef struct { float x, y, w, h, body_y, tabs_r, root_w, root_h; int ntabs; float tab[8][4]; } ChatPanel;
int chat_panel(ChatPanel *p);
unsigned char *pvp_pak_file(const char *name, size_t *n);   // an unpacked file of interface.pak, malloc'd
// the lobby's player list lines on screen (game units, as pvp_rect) and whose they are, with room under each name
typedef struct { float x, y, w, h, root_w, root_h; char id[18]; } LobbyRow;
int lobby_rows(LobbyRow *out, int max);
void pvp_page_shown(void *page);             // the game shows a page: the way to the screen's root

// ---- memory census (census.c): where the game keeps a match's players and result; Ctrl+F12 ----
void census_init(uint8_t *exe, const char *dir, int after_s);
void census_run(const char *why, uintptr_t game, uintptr_t page);
extern volatile uintptr_t g_census_game;    // the eGame the last stats event came with
extern volatile uintptr_t g_census_page;    // the results page while it is up
extern int g_census_auto;                   // [debug] census=1: also when the results page opens

void rc_send(int cmd, double arg);
void rc_log(const char *fmt, ...);
extern int g_ui_draw;                // ui.cpp, [debug] ui_draw=0: the panel draws nothing
void ui_present(void *device);   // ui.cpp: the game is about to present its frame (eRenderDevice*)

#ifdef __cplusplus
}
#endif
