// Units' answers to orders under the competitive balance. In a hosted match the simulation is the server's: a player's game has
// eHumanBrainClient, whose orders go nowhere (its add order is a stub), and follows what the server sends. So everything about a
// soldier's brain and paths works in call_to_arms_server.exe, started by the host, and the switch reaches it as the balance's does.
// A player's game keeps what is its own: the click (ui.cpp), the same data files, the same order of a quant's steps.
// A skirmish is all in the game. Addresses of the server found with re/twin.py, each function matched whole, offsets included.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "goh_replay.h"

typedef struct {
    uint32_t stat, smooth, human, tank, idle;       // dbg_pathStat, dbg_pathSmooth (ints), g_linearPathThresholdHuman 1.05, ..Vehicle 1.3, dbg_no_idle_animations
    uint32_t async;                                 // paths on the pather's thread; set at a scene's start
    uint32_t now;                                   // game time, ms
    uint32_t site_entity, debug_stack, ec_get, ec_exec;
    uint32_t vt_human, vt_brain, add_order, set_beo, message, rule;
} Where;
static const Where WHERE[2] = {
    {0x1482260, 0x1482290, 0x1482580, 0x1482598, 0x1483008, 0x14822b0, 0x1556e48, 0xa1c497, 0x5e4190, 0xb90d0, 0x14d080, 0x10ebdd8, 0x10ec7c8, 0x9053e0, 0x9052f0, 0x9424f0, 0x9121c0},
    {0x10ee430, 0x10ee460, 0x10ee750, 0x10ee768, 0x10ef1d8, 0x10ee480, 0x11a1328, 0x8310fb, 0x3fa1e0, 0x8d620, 0xee990, 0xdee9b8, 0xdef378, 0x71a480, 0x71a390, 0x7574d0, 0x7273b0},
};
static const Where *W = &WHERE[0];
static int g_server;
// the player's game only
#define V_DQ     0x148da28      // mp_serverDQ, int: a quant's game ms when not 0, the server's own (c6c16c), 50 by default
#define V_LAG_MIN 0x1478c48     // mp_minClientSpeedupLag, float: 80
#define V_LAG_MAX 0x1478c60     // mp_maxClientSpeedupLag, float: 300

static uint8_t *g_exe;
static int g_test, g_stat, g_smooth, g_no_idle, g_dq, g_press, g_sync, g_early, g_free_ms, g_wait_ms, g_hurry_ms, g_brain2, g_hot_ms;
static float g_human, g_tank, g_lag_min, g_lag_max;

void pathing_init(uint8_t *exe, const char *ini, int server) {
    char v[32];
    g_exe = exe;
    g_server = server;
    W = &WHERE[server ? 1 : 0];
    // what was tried and kept; [debug] path_... in goh_snapshots.ini changes it, on every machine of a match or not at all
    g_test = GetPrivateProfileIntA("debug", "path_test", 1, ini);
    g_stat = GetPrivateProfileIntA("debug", "path_stat", 1, ini);
    g_smooth = GetPrivateProfileIntA("debug", "path_smooth", 2, ini);
    g_no_idle = GetPrivateProfileIntA("debug", "path_no_idle", 1, ini);
    g_dq = GetPrivateProfileIntA("debug", "path_dq", 0, ini);
    g_press = GetPrivateProfileIntA("debug", "path_press", 1, ini);
    g_sync = GetPrivateProfileIntA("debug", "path_sync", 1, ini);
    g_early = GetPrivateProfileIntA("debug", "path_early", 1, ini);
    g_free_ms = GetPrivateProfileIntA("debug", "path_free_ms", 0, ini);
    g_wait_ms = GetPrivateProfileIntA("debug", "path_wait_ms", 300, ini);
    g_hurry_ms = GetPrivateProfileIntA("debug", "path_hurry_ms", 600, ini);
    g_brain2 = GetPrivateProfileIntA("debug", "path_brain2", 1, ini);
    g_hot_ms = GetPrivateProfileIntA("debug", "path_hot_ms", 15000, ini);
    GetPrivateProfileStringA("debug", "path_linear_human", "1.15", v, sizeof v, ini); g_human = (float)atof(v);
    GetPrivateProfileStringA("debug", "path_lag_min", "80", v, sizeof v, ini); g_lag_min = (float)atof(v);
    GetPrivateProfileStringA("debug", "path_lag_max", "300", v, sizeof v, ini); g_lag_max = (float)atof(v);
    GetPrivateProfileStringA("debug", "path_linear_vehicle", "1.4", v, sizeof v, ini); g_tank = (float)atof(v);
    if (g_test) rc_log("pathing: on with the competitive balance: smooth %d, linear %.2f / %.2f, think %d ms, second pass %d, server %d", g_smooth, g_human, g_tank, g_hurry_ms, g_brain2, server);
}

// What makes units slow to answer is in two files, not in the paths: a soldier turns at 360 to 600 degrees a second, a vehicle
// takes 0.6 s and more to start and to end a turn and 0.5 s to change gear. Same length each, in what the game reads: a test.
static int swap_all(uint8_t *b, size_t n, const char *from, const char *to) {
    size_t k = strlen(from);
    int done = 0;
    for (size_t i = 0; i + k <= n; i++) if (!memcmp(b + i, from, k)) { memcpy(b + i, to, k); done++; i += k - 1; }
    return done;
}
int pathing_wants(void) { return g_test && g_balance_on; }
// An order goes when the right button comes up. Measured, a squad answers 12 to 60 ms after that, at full speed: most of what is
// felt is the click itself. So in a battle the button comes up for the game at once; Alt held keeps the drag of a line formation.
// Not in direct control, where the button is held on purpose.
int pathing_press_order(void) {
    if (!g_test || !g_press || !g_balance_on || !g_live_match || (GetAsyncKeyState(VK_MENU) & 0x8000)) return 0;
    // the window's thread, not the game's: a read that cannot fault
    uintptr_t owner = 0, manual = 0;
    uint8_t on = 0;
    SIZE_T got;
    HANDLE self = GetCurrentProcess();
    ReadProcessMemory(self, g_exe + 0x159e218, &owner, 8, &got);
    if (owner) ReadProcessMemory(self, (void *)(owner + 0x3b0), &manual, 8, &got);
    if (manual) ReadProcessMemory(self, (void *)(manual + 0x10), &on, 1, &got);
    return !on;
}
// A soldier goes from standing to running at once, in the same quant, only from a state the switch "walk" lists (switch.inc; the
// request "move" looks its state up, 7fe2ea, and every state of a chain under way, 7fe381). The game lists stand, the recoil and the
// bolt, and by the upper body what has a walking twin of the same name (reload, weapon change). Not aiming, not getting ready to
// aim, not firing, nothing crouched. So a soldier in a fight, who is in those nearly all the time, waits for the chain of animations
// to end and to take him back to "stand". The game has walking twins for all of them (walk_aim, walk_fire ...): listed here, his legs
// go at once and his arms carry on. From crouching too: there is no moving crouched. Lying is left out: there is crawling.
// Stand's come first: going the other way the first match counts.
static const char MORE_LINKS[] =
    " {link \"stand_noaim\" \"walk\"} {link \"idle\" \"walk\"}"
    " {link \"stand_aim\" \"walk_aim\"} {link \"stand_aim_prepare\" \"walk_aim_prepare\"} {link \"stand_aim_prepare_noaim\" \"walk_aim_prepare\"}"
    " {link \"stand_fire\" \"walk_fire\"} {link \"stand_reload\" \"walk_reload\"} {link \"stand_unload\" \"walk_unload\"} {link \"stand_switch\" \"walk_switch\"}"
    " {link \"squat\" \"walk\"} {link \"squat_idle\" \"walk\"} {link \"squat_aim\" \"walk_aim\"} {link \"squat_aim_prepare\" \"walk_aim_prepare\"}"
    " {link \"squat_fire\" \"walk_fire\"} {link \"squat_recoil\" \"walk_recoil\"} {link \"squat_bolt\" \"walk_bolt\"}"
    " {link \"squat_reload\" \"walk_reload\"} {link \"squat_unload\" \"walk_unload\"} {link \"squat_switch\" \"walk_switch\"} ";
size_t pathing_more(void) { return sizeof MORE_LINKS; }
// the file as the game is about to read it, in a buffer of cap bytes: its new length
size_t pathing_edit(const char *name, uint8_t *b, size_t n, size_t cap) {
    int done = 0;
    if (!strcmp(name, "switch.inc")) {
        size_t k = sizeof MORE_LINKS - 1, end = n;
        while (end && b[end - 1] != '}') end--;
        if (end && n + k <= cap) {
            memmove(b + end - 1 + k, b + end - 1, n - end + 1);
            memcpy(b + end - 1, MORE_LINKS, k);
            n += k;
            done = 1;
        }
    } else if (!strcmp(name, "request.inc")) {
        // above what used to take a running soldier out of his run: reload, unload, look-out, squat-cover, rotate, grenade-lie; under
        // up and down. And two quants more: the moment between one order's end and the next is no reason to stop and start again.
        // Not {interrupt}: it makes a chain of its own at once (7fe150), and with a chain under way the switch has to find every
        // state of it, "walk" itself is not there, and the request waits for the chain instead of going at once.
        static const char key[] = "{Request \"move\"";
        for (size_t i = 0; i + 64 < n; i++) {
            if (memcmp(b + i, key, sizeof key - 1)) continue;
            for (size_t k = i; k < i + 64; k++) {
                if (!memcmp(b + k, "priority 30", 11)) { b[k + 10] = '6'; done++; }
                if (!memcmp(b + k, "duration 2", 10)) { b[k + 9] = '4'; done++; }
            }
            break;
        }
    } else if (!strcmp(name, "human.ext")) {
        const char *run = "{Run", *strafe = "{Strafe";
        size_t from = 0, to = 0;
        for (size_t i = 0; i + 8 < n; i++) {
            if (!from && !memcmp(b + i, run, 4)) from = i;
            if (from && !memcmp(b + i, strafe, 7)) { to = i; break; }
        }
        if (to) {
            done += swap_all(b + from, to - from, "600 }", "2000}");
            done += swap_all(b + from, to - from, "420 }", "2000}");
            done += swap_all(b + from, to - from, "360 }", "2000}");
        }
    } else {
        done += swap_all(b, n, "{GearSwitchDelay 0.5}", "{GearSwitchDelay 0.1}");
        done += swap_all(b, n, "{TurnStart (+ 0.6 ", "{TurnStart (+ 0.2 ");
        done += swap_all(b, n, "{TurnStop (+ 0.6 ", "{TurnStop (+ 0.2 ");
    }
    rc_log("pathing: %s read with %d quicker values", name, done);
    return n;
}

// How long a soldier takes to answer, measured: from the right button down to the first of the local player's standing soldiers
// moving, in the quant of each one (eHuman's slot 23, the same turn as eActor's). The test's only; the log says it.
#define G_LOCAL_PLAYER 0x1487a88
typedef struct { uintptr_t actor; float x, y, dirx, diry; int still, gen, started, fast, moving, turned; double prev; } Track;
static Track g_tracks[2048];
static volatile double g_click, g_release;
static volatile long g_gen, g_n_start, g_n_fast, g_n_back;
static float g_start_ms[64], g_fast_ms[64], g_back_ms[64];
static void *(*g_human_real)(void *);

// What a soldier went through after an order, quant by quant: the state of the move (the chassis' +0x64: 2 going, 3 stopping for
// someone in the way, 4 waiting), his speed, the orders his brain holds. Only what changed, for the log. Read with a read that
// cannot fault: an order may go away under it.
#define TRACE_SOLDIERS 4
#define TRACE_STEPS 16
typedef struct { short ms, move, speed, n, q, cover, state, chain; uint32_t flags; const char *front, *back, *beo; } Step;
typedef struct { uintptr_t actor; int running, n; Step s[TRACE_STEPS]; } Trace;
static Trace g_trace[TRACE_SOLDIERS];
static int g_traced, g_brain_at;
static int peek(const void *from, void *to, size_t n) {
    SIZE_T got = 0;
    return from && ReadProcessMemory(GetCurrentProcess(), from, to, n, &got) && got == n;
}
// slot 5 of an order is its name: lea rax, [rip + x]; ret
static const char *order_name(const void *o) {
    uint8_t *vt = NULL, *fn = NULL, c[8];
    int32_t rel;
    if (!o) return "-";
    if (!peek(o, &vt, 8) || vt < g_exe || vt >= g_exe + 0x1700000 || !peek(vt + 0x28, &fn, 8) || !peek(fn, c, 8)) return "?";
    if (c[0] != 0x48 || c[1] != 0x8d || c[2] != 5 || c[7] != 0xc3) return "??";
    memcpy(&rel, c + 3, 4);
    return (const char *)(fn + 7 + rel);
}
static const uint8_t *actor_brain(const uint8_t *a) {
    const uint8_t *p = NULL, *vt = NULL;
    if (!g_brain_at) {
        for (int at = 0x300; at < 0x1000 && !g_brain_at; at += 8)
            if (peek(a + at, &p, 8) && peek(p, &vt, 8) && vt == g_exe + W->vt_brain) g_brain_at = at;
        rc_log("pathing: a soldier's brain is at +%x", g_brain_at);
        if (!g_brain_at) g_brain_at = -1;
    }
    if (g_brain_at <= 0 || !peek(a + g_brain_at, &p, 8)) return NULL;
    return p;
}
static void trace_quant(const uint8_t *a, int running, float ms, float speed) {
    Trace *r = NULL;
    for (int i = 0; i < g_traced; i++) if (g_trace[i].actor == (uintptr_t)a) r = &g_trace[i];
    if (!r || r->n == TRACE_STEPS || ms > 1500) return;
    const uint8_t *chassis = *(const uint8_t *const *)(a + 0xde0), *brain = actor_brain(a);
    Step s = {(short)ms, -1, (short)(speed / 2), 0, 0, 0, -1, 0, 0, "-", "-", "-"};      // decimetres a second: units are 20 to the metre
    const uint8_t *cover = *(const uint8_t *const *)(a + 0xdc0);
    if (cover) s.cover = (short)*(const int *)(cover + 0x24);
    const uint8_t *fsm = *(const uint8_t *const *)(a + 0xdb8), *now_state = NULL;
    int index = -1;
    if (fsm && peek(fsm + 0x9f0, &now_state, 8) && peek(now_state, &index, 4)) s.state = (short)index;
    if (fsm) s.chain = fsm[0xa18];
    if (chassis) s.move = (short)*(const int *)(chassis + 0x64);
    if (brain) {
        uint64_t off = 0, size = 0, q0 = 0, q1 = 0;
        const uint8_t *beo = NULL, *map = NULL, *block = NULL, *o = NULL;
        int count = 0;
        peek(brain + 0xf0, &beo, 8);
        s.beo = order_name(beo);
        peek(brain + 0x98, &q0, 8);
        peek(brain + 0xa0, &q1, 8);
        s.q = (short)((q1 - q0) / 16);
        peek(brain + 0x70, &map, 8);
        peek(brain + 0x78, &size, 8);
        peek(brain + 0x80, &off, 8);
        peek(brain + 0x88, &count, 4);
        s.n = (short)count;
        if (count > 0 && size) {
            if (peek(map + ((off >> 1) & (size - 1)) * 8, &block, 8) && peek(block + (off & 1) * 8, &o, 8)) {
                s.front = order_name(o);
                peek(o + 0x18, &s.flags, 4);
            }
            off += count - 1;
            if (peek(map + ((off >> 1) & (size - 1)) * 8, &block, 8) && peek(block + (off & 1) * 8, &o, 8)) s.back = order_name(o);
        }
    }
    if (r->n) {
        const Step *l = &r->s[r->n - 1];
        if (l->move == s.move && l->state == s.state && l->chain == s.chain && l->cover == s.cover && l->n == s.n && l->q == s.q && l->front == s.front && l->back == s.back && l->beo == s.beo && l->flags == s.flags && (l->speed > 5) == (s.speed > 5)) return;
    }
    r->s[r->n++] = s;
}

// Who told the brain what, after a click
typedef struct { float ms; uintptr_t actor; const char *name; int a, b; char kind; } Event;
static Event g_events[512], g_pipe[128];
static volatile long g_n_pipe;
static volatile long g_n_events;
static void event(char kind, const uint8_t *actor, const char *name, int a, int b) {
    double click = g_click;
    uint8_t owner = 255;
    if (click == 0 || !peek(actor + 0xda8, &owner, 1) || owner != g_exe[G_LOCAL_PLAYER]) return;
    long i = InterlockedIncrement(&g_n_events) - 1;
    if (i >= 512) return;
    Event e = {(float)((uistat_now() - click) / 1000), (uintptr_t)actor, name, a, b, kind};
    g_events[i] = e;
    // the soldiers an order reached: the first move stopped, or a brain given something
    if (e.ms > 1000 || (kind != 'X' && kind != 'A' && !(kind == 'B' && name[0] == 'e'))) return;
    for (int k = 0; k < g_traced; k++) if (g_trace[k].actor == (uintptr_t)actor) return;
    if (g_traced < TRACE_SOLDIERS) {
        Trace *r = &g_trace[g_traced++];
        r->actor = (uintptr_t)actor;
        r->running = kind == 'X';
        r->n = 0;
    }
}
// the way of an order before it reaches a soldier, in the log: what the server takes in, the depth of the client's queue
static void pipe_event(char kind, int a, int b) {
    double click = g_click;
    if (click == 0 || uistat_now() - click > 400e3) return;
    long i = InterlockedIncrement(&g_n_pipe) - 1;
    if (i >= 128) return;
    Event e = {(float)((uistat_now() - click) / 1000), 0, "", a, b, kind};
    g_pipe[i] = e;
}
#define FN_SERVER_MESSAGE 0xc7b6e0
#define FN_CLIENT_EXEC 0x53aca0
#define VT_CLIENT_EXEC 0x10b7bb8
static const uint32_t SERVER_SLOTS[3] = {0x1038bb8, 0x10b7958, 0x113e008};
static void *(*g_server_real)(void *, uint8_t *, void *, void *);
static void *(*g_client_real)(void *);
static void *server_message(void *self, uint8_t *msg, void *x, void *y) {
    uint8_t type = 0;
    peek(msg, &type, 1);
    if (type != 0x28) pipe_event('S', type, 0);
    return g_server_real(self, msg, x, y);
}
static void *client_exec(void *self) {
    static long last = -1;
    long n = 0;
    peek((uint8_t *)self + 0x38, &n, 4);
    void *r = g_client_real(self);
    long m = 0;
    peek((uint8_t *)self + 0x38, &m, 4);
    if (m != n || n != last) pipe_event('C', (int)n, (int)m);
    last = m;
    return r;
}
#define FN_MOVE_EXEC 0x8f3e00
#define VT_MOVE 0x10e9770
static void (*g_add_real)(void *, void *, void *);
static void (*g_beo_real)(void *, void *);
static void *(*g_message_real)(void *, long, void *, void *);
static int (*g_exec_real)(void *, int);
static const uint8_t *brain_actor(const void *brain) { const uint8_t *a = NULL; peek((const uint8_t *)brain + 0x38, &a, 8); return a; }
// From a squad's order to a soldier's step a brain takes one quant for each thing it does: the change of pace, then the move; or the
// follower's order, then its move; the chassis, ahead of the brain in an actor's quant, one more. Traced: 4 quants standing still.
// So a soldier given something works his brain once more in that quant, as long as he is not moving: a move going would step twice.
static uintptr_t g_ordered[2048];
static volatile long g_second;
static void ordered(void *brain) { if (g_brain2) g_ordered[((uintptr_t)brain >> 4) % 2048] = (uintptr_t)brain; }
static void second_pass(const uint8_t *a) {
    uint8_t *brain = *(uint8_t *const *)(a + 0xdc8);
    const uint8_t *chassis = *(const uint8_t *const *)(a + 0xde0);
    if (!brain || !chassis) return;
    uintptr_t *o = &g_ordered[((uintptr_t)brain >> 4) % 2048];
    if (*o != (uintptr_t)brain) return;
    *o = 0;
    if (!g_balance_on || *(const int *)(chassis + 0x64) != 0) return;
    (*(void (**)(void *))(*(uint8_t **)brain + 0xc0))(brain);
    g_second++;
}
static void add_order(void *brain, void *order, void *params) {
    if (!g_server) {
        int v[2] = {0, 0};
        peek(params, v, 8);
        event('A', brain_actor(brain), order_name(order), v[0], v[1]);
    }
    g_add_real(brain, order, params);
    ordered(brain);
}
// A soldier's brain thinks when his sensors tick (7efe7a), or at once with bit 25 of +0x39c. A squad's new order takes the old one
// off a follower at once and he has nothing until he next thinks, standing, for up to a third of a second: measured 137 to 343 ms,
// the leader 40. So a soldier who has just lost his order thinks every quant until he has one again.
#define BRAIN_THINK 0x2000000
typedef struct { uintptr_t brain; uint32_t until; } Hurry;
static Hurry g_hurry[2048];
static volatile long g_hurried;
static void set_beo(void *brain, void *order) {
    if (!g_server) {
        const char *rule = NULL;
        peek((uint8_t *)brain + 0x110, &rule, 8);
        event('B', brain_actor(brain), order_name(order), 0, 0);
        if (order && rule) event('R', brain_actor(brain), rule, 0, 0);
    }
    g_beo_real(brain, order);
    if (order) ordered(brain);
    if (!order && g_hurry_ms && g_balance_on) {
        Hurry *h = &g_hurry[((uintptr_t)brain >> 4) % 2048];
        h->brain = (uintptr_t)brain;
        h->until = *(const uint32_t *)(g_exe + W->now) + g_hurry_ms;
        *(uint32_t *)((uint8_t *)brain + 0x39c) |= BRAIN_THINK;
        g_hurried++;
    }
}
static void hurry_quant(const uint8_t *a) {
    uint8_t *brain = *(uint8_t *const *)(a + 0xdc8);
    if (!brain) return;
    Hurry *h = &g_hurry[((uintptr_t)brain >> 4) % 2048];
    if (h->brain != (uintptr_t)brain) return;
    if (*(void *const *)(brain + 0xf0) || (int32_t)(*(const uint32_t *)(g_exe + W->now) - h->until) >= 0) { h->brain = 0; return; }
    *(uint32_t *)(brain + 0x39c) |= BRAIN_THINK;
}
// What a soldier does is a list of rules, top to bottom, gone through when he thinks (903f60; eHS_RootGeneric 955a90: restore_order,
// burning, avoid_*, ..., interface, then the state's own: eBattleGeneric enemy_alarm, board_cannon, stop_advance, stop_detour,
// stop_fight, move_like_commander, move_follow, reload_if_low, healing_*, injured_by_enemy; eCoverBattle ... hide_check). Each rule
// starts with a check (9121c0) that stops the list at the rule that gave the order he is on: whatever is above it can take over, and
// when it ends the next thing down takes over, however little it has to do with what the player said.
// Why a soldier told to run lies down first: between the order taken off him and the next (a follower has none for a few quants) the
// first rule of "outdoors" (949aa0) is enemy_alarm: byte +0x3da of the brain, set when his enemy changes (941d43); in one stance (0xc00 of +0x398) it calls Hide, and with
// nothing to hide behind eCover's table says open_lie, terrain_lie, bush_lie. He leaves that only when his eMove starts (898c50).
// Here the check is ours. For a soldier told to go (0x407: his orders cleared for a new one), until he has arrived or g_hot_ms have
// gone:
// - the alarm is put aside, not stopped at: below it are the rules that give a follower his order (following, move_follow), so the
//   list has to go on. He gets it back when he is done.
// - before he moves, hide_check and injured_by_enemy stop the list, as the rule of his order would. Nothing he needs is below them.
// - going, the rest of what the fight wants stops the list too. Fire, grenades, being run over still come first.
typedef struct { uintptr_t brain; uint32_t from, until; uint8_t alarm; } Hot;
static Hot g_hot[2048];
static const char *const FIGHT_RULES[] = {"hide_check", "injured_by_enemy", "enemy_alarm", "board_cannon", "stop_advance", "stop_detour", "stop_fight", "reload_if_low",
                                          "healing_stop", "healing_restore"};
#define NFIGHT (sizeof FIGHT_RULES / sizeof FIGHT_RULES[0])
#define NBEFORE 2                   // the first ones hold before he moves too
#define BRAIN_ALARM 0x3da
static volatile long g_held[NFIGHT], g_alarms;
#define RULE_STOP 0x400000
// 0 not told, 1 told and not moving yet, 2 going
static int told(uint8_t *brain) {
    Hot *h = &g_hot[((uintptr_t)brain >> 4) % 2048];
    if (h->brain != (uintptr_t)brain) return 0;
    uint32_t now = *(const uint32_t *)(g_exe + W->now);
    const uint8_t *actor = *(const uint8_t *const *)(brain + 0x38), *chassis = actor ? *(const uint8_t *const *)(actor + 0xde0) : NULL;
    int moving = chassis && *(const int *)(chassis + 0x64) >= 2;
    // he has a moment after the order to set off
    if ((int32_t)(now - h->until) >= 0 || (!moving && now - h->from > 700)) {
        if (h->alarm) brain[BRAIN_ALARM] = 1;
        h->brain = 0;
        return 0;
    }
    return moving ? 2 : 1;
}
static uint8_t rule_check(uint8_t *state, const char *name) {
    uint8_t *brain = *(uint8_t **)(state + 0x30);
    int how = g_hot_ms && g_balance_on ? told(brain) : 0;
    if (how) {
        if (brain[BRAIN_ALARM] && !strcmp(name, "enemy_alarm")) {
            g_hot[((uintptr_t)brain >> 4) % 2048].alarm = 1;
            brain[BRAIN_ALARM] = 0;
            g_alarms++;
        }
        size_t n = how == 2 ? NFIGHT : NBEFORE;
        for (size_t i = 0; i < n; i++) if (!strcmp(name, FIGHT_RULES[i])) {
            *(uint32_t *)(brain + 0x39c) |= RULE_STOP;
            g_held[i]++;
            return 0;
        }
    }
    *(const char **)(brain + 0x110) = name;
    if (name == *(const char **)(brain + 0x108)) *(uint32_t *)(brain + 0x39c) |= RULE_STOP;
    return !(*(uint32_t *)(brain + 0x39c) & RULE_STOP);
}
static void held_report(void) {
    char line[320];
    int n = 0;
    for (size_t i = 0; i < NFIGHT; i++) if (g_held[i]) { n += snprintf(line + n, sizeof line - n, " %s %ld", FIGHT_RULES[i], g_held[i]); g_held[i] = 0; }
    if (n || g_alarms) rc_log("pathing: alarms put aside %ld, rules held back:%s", g_alarms, n ? line : " none");
    g_alarms = 0;
}
static void *brain_message(void *brain, long code, void *x, void *y) {
    if (!g_server) event('M', brain_actor(brain), "", (int)code, 0);
    void *r = g_message_real(brain, code, x, y);
    if (code == 0x407 && g_hot_ms && g_balance_on) {
        Hot *h = &g_hot[((uintptr_t)brain >> 4) % 2048];
        uint32_t now = *(const uint32_t *)(g_exe + W->now);
        h->brain = (uintptr_t)brain;
        h->from = now;
        h->until = now + g_hot_ms;
        h->alarm = 0;
    }
    return r;
}
static int move_exec(void *move, int phase) {
    const uint8_t *iface = NULL;
    int r = g_exec_real(move, phase);
    peek((const uint8_t *)move + 8, &iface, 8);
    if (iface && phase != 1) event('X', iface - 0xaf0, "eMove", phase, r);
    return r;
}

static void *human_quant(void *self) {
    const uint8_t *a = self;
    if (g_hurry_ms) hurry_quant(a);
    if (!g_server && a[0xda8] == g_exe[G_LOCAL_PLAYER]) {
        Track *t = &g_tracks[((uintptr_t)self >> 4) % 2048];
        float x = *(const float *)(a + 0x74), y = *(const float *)(a + 0x78);
        double now = uistat_now();
        if (t->actor != (uintptr_t)self) { memset(t, 0, sizeof *t); t->actor = (uintptr_t)self; t->x = x; t->y = y; t->prev = now; }
        float dx = x - t->x, dy = y - t->y, dist = sqrtf(dx * dx + dy * dy);
        double gap = now - t->prev, click = g_click;
        if (gap > 1000) {                                // the same actor's next turn, not a second call in one
            int moved = dist > .02f;
            if (click != 0) {
                if (t->gen != g_gen) { t->gen = g_gen; t->started = t->fast = t->turned = 0; t->moving = dist > 1; t->dirx = dx; t->diry = dy; }
                float ms = (float)((now - click) / 1000);
                trace_quant(a, t->moving, ms, dist / (float)(gap / 1e6));
                if (t->moving && !t->turned && dx * t->dirx + dy * t->diry < 0) {       // running one way, now the other
                    t->turned = 1;
                    long i = InterlockedIncrement(&g_n_back) - 1;
                    if (i < 64) g_back_ms[i] = ms;
                }
                if (moved && !t->started && t->still >= 3) {
                    t->started = 1;
                    long i = InterlockedIncrement(&g_n_start) - 1;
                    if (i < 64) g_start_ms[i] = ms;
                }
                // 13 km/h is 72 units a second: 80% of that
                if (t->started && !t->fast && dist / (gap / 1e6) > 58) {
                    t->fast = 1;
                    long i = InterlockedIncrement(&g_n_fast) - 1;
                    if (i < 64) g_fast_ms[i] = ms;
                }
            }
            t->still = moved ? 0 : t->still + 1;
            t->x = x; t->y = y; t->prev = now;
        }
    }
    void *r = g_human_real(self);
    if (g_brain2) second_pass(a);
    return r;
}

// An eMove that is about to set off asks the collision resolver (ePP_Collision, 8aa360) whether its way is free of whoever else
// moves. When not, it stops (state 3), waits at least 0.3 s (state 4, 8f4201) and asks again: a squad turned around runs into
// itself, so every soldier stands for that long. Under the test a soldier ordered less than g_free_ms ago goes on: he is the one
// who was told to. A vehicle still waits, and so does a soldier well on his way.
#define SITE_ASK_MOVING 0x8f44f5
#define SITE_ASK_WAITING 0x8f425b
#define SITE_WAIT 0x8f4201
#define FN_RESOLVER 0x8aa360
static volatile long g_stopped, g_let_go;
static float *g_wait;
static int resolver_ask(void *resolver, uint8_t *actor, const uint8_t *move) {
    int r = ((int (*)(void *, void *))(g_exe + FN_RESOLVER))(resolver, actor);
    if (r != 1 || *(uint8_t **)actor != g_exe + W->vt_human) return r;
    if (g_free_ms && g_balance_on) {
        uint32_t since = *(const uint32_t *)(g_exe + W->now) - *(const uint32_t *)(move + 0x1a4);
        if (since < (uint32_t)g_free_ms) { g_let_go++; return 0; }
    }
    g_stopped++;
    return r;
}

static int by_value(const void *a, const void *b) { float d = *(const float *)a - *(const float *)b; return d < 0 ? -1 : d > 0; }
// three seconds after a click: what the squad did
static void squad_report(void) {
    double click = g_click;
    if (click == 0 || uistat_now() - click < 3e6) return;
    g_click = 0;
    long ns = g_n_start > 64 ? 64 : g_n_start, nf = g_n_fast > 64 ? 64 : g_n_fast, nb = g_n_back > 64 ? 64 : g_n_back;
    qsort(g_start_ms, ns, sizeof(float), by_value);
    qsort(g_fast_ms, nf, sizeof(float), by_value);
    qsort(g_back_ms, nb, sizeof(float), by_value);
    if (ns) rc_log("pathing: %ld soldiers moved at %.0f / %.0f / %.0f ms; %ld at full speed at %.0f / %.0f / %.0f; balance %s, paths %s", ns, g_start_ms[0], g_start_ms[ns / 2], g_start_ms[ns - 1],
                   nf, nf ? g_fast_ms[0] : 0, nf ? g_fast_ms[nf / 2] : 0, nf ? g_fast_ms[nf - 1] : 0, g_balance_on ? "on" : "off", g_sync ? "at once" : "async");
    if (nb) rc_log("pathing: %ld soldiers turned back at %.0f / %.0f / %.0f ms, paths %s", nb, g_back_ms[0], g_back_ms[nb / 2], g_back_ms[nb - 1], g_sync ? "at once" : "async");
    if (g_hurried || g_second) rc_log("pathing: %ld soldiers told to think at once, %ld second passes", g_hurried, g_second);
    g_hurried = g_second = 0;
    held_report();
    if (g_stopped || g_let_go) rc_log("pathing: someone in the way: %ld waited, %ld went on", g_stopped, g_let_go);
    g_stopped = g_let_go = 0;
    long np = g_n_pipe > 128 ? 128 : g_n_pipe;
    for (long k = 0; k < np && k < 40; k++) rc_log("pathing:   pipe %.1f ms: %c %d %d", g_pipe[k].ms, g_pipe[k].kind, g_pipe[k].a, g_pipe[k].b);
    g_n_pipe = 0;
    for (int i = 0; i < g_traced; i++) {
        rc_log("pathing: soldier %d, %s", i, g_trace[i].running ? "running" : "standing");
        for (int k = 0; k < g_trace[i].n; k++) {
            const Step *s = &g_trace[i].s[k];
            rc_log("pathing:   %d ms: fsm %d chain %d, move %d, cover %d, %.1f m/s, %d orders %s .. %s (%x), own %s, %d later", s->ms, s->state, s->chain, s->move, s->cover, s->speed / 10., s->n, s->front, s->back, s->flags, s->beo, s->q);
        }
        long ne = g_n_events > 512 ? 512 : g_n_events;
        for (long k = 0; k < ne; k++) {
            const Event *e = &g_events[k];
            if (e->actor == g_trace[i].actor) rc_log("pathing:   %.0f ms: %c %s %x %d", e->ms, e->kind, e->name, e->a, e->b);
        }
    }
    g_traced = 0;
    g_n_events = 0;
}

// A quant takes its commands in (sqi:exec), works the actors (entity_1) and only then applies the commands (ecexec, 14d080): an order
// reaches the units a whole quant late. So here they are applied before the actors too; the step at the end finds nothing left.
// The way in is the call just before entity_1, 5e4190 (the debug stack, no arguments), through a stub near the code.
static void settings(int on) {
    *(int *)(g_exe + W->stat) = on ? g_stat : 0;
    *(int *)(g_exe + W->smooth) = on ? g_smooth : 0;
    *(uint8_t *)(g_exe + W->idle) = on && g_no_idle;
    *(float *)(g_exe + W->human) = on ? g_human : 1.05f;
    *(float *)(g_exe + W->tank) = on ? g_tank : 1.3f;
    rc_log("pathing: %s", on ? "settings of the competitive balance" : "the game's settings");
}
// the server has no tick of ours: its quant does
static void server_quant(void) {
    static int was = -1;
    static long quants;
    static DWORD at;
    int on = g_balance_on != 0;
    if (on != was) { was = on; settings(on); }
    if (g_sync && on) *(uint8_t *)(g_exe + W->async) = 0;
    quants++;
    DWORD now = GetTickCount();
    if (now - at >= 30000) {
        held_report();
        if (at) rc_log("pathing: %ld quants in %lu ms, switch %d, %ld told to think, %ld second passes", quants, now - at, on, g_hurried, g_second);
        at = now;
        quants = g_hurried = g_second = 0;
    }
}
static void *quant_mid(void) {
    if (g_server) server_quant();
    if (g_early && g_balance_on) ((void (*)(void *))(g_exe + W->ec_exec))(((void *(*)(void))(g_exe + W->ec_get))());
    return ((void *(*)(void))(g_exe + W->debug_stack))();
}

void pathing_install(uint8_t *exe, uint8_t *(*alloc)(size_t), void (*patch)(void *, const void *, size_t)) {
    void *slot, *ours = (void *)human_quant;
    if (!g_test) return;
    int32_t rel;
    const uint32_t site = W->site_entity;
    memcpy(&rel, exe + site + 1, 4);
    if (exe[site] == 0xe8 && site + 5 + rel == W->debug_stack) {
        uint8_t *stub = alloc(16);
        if (stub) {
            uint64_t to = (uint64_t)(uintptr_t)quant_mid;
            static const uint8_t JMP[6] = {0xff, 0x25, 0, 0, 0, 0};
            memcpy(stub, JMP, 6);
            memcpy(stub + 6, &to, 8);
            int32_t near_rel = (int32_t)(stub - (exe + site + 5));
            patch(exe + site + 1, &near_rel, 4);
            rc_log("pathing: commands before the actors ready");
        }
    } else rc_log("pathing: the quant is not as expected");
    if (g_server) goto brain;
    uint8_t *ask = g_free_ms ? alloc(32) : NULL;
    int32_t r1, r2;
    memcpy(&r1, exe + SITE_ASK_MOVING + 1, 4);
    memcpy(&r2, exe + SITE_ASK_WAITING + 1, 4);
    if (ask && SITE_ASK_MOVING + 5 + r1 == FN_RESOLVER && SITE_ASK_WAITING + 5 + r2 == FN_RESOLVER && !memcmp(exe + SITE_WAIT, "\x0f\x2f\x05", 3)) {
        // mov r8, rdi: the eMove; then on to resolver_ask. After that the float the wait is compared with
        static const uint8_t HEAD[9] = {0x49, 0x89, 0xf8, 0xff, 0x25, 0, 0, 0, 0};
        uint64_t to = (uint64_t)(uintptr_t)resolver_ask;
        memcpy(ask, HEAD, 9);
        memcpy(ask + 9, &to, 8);
        g_wait = (float *)(ask + 20);
        *g_wait = .3f;
        int32_t a1 = (int32_t)(ask - (exe + SITE_ASK_MOVING + 5)), a2 = (int32_t)(ask - (exe + SITE_ASK_WAITING + 5));
        int32_t w = (int32_t)((uint8_t *)g_wait - (exe + SITE_WAIT + 7));
        patch(exe + SITE_ASK_MOVING + 1, &a1, 4);
        patch(exe + SITE_ASK_WAITING + 1, &a2, 4);
        patch(exe + SITE_WAIT + 3, &w, 4);
        rc_log("pathing: the resolver's answer is ours");
    }
    {
        void *vt_exec = NULL, *our;
        memcpy(&vt_exec, exe + VT_MOVE + 0x98, 8);
        if (vt_exec == exe + FN_MOVE_EXEC) {
            g_exec_real = (int (*)(void *, int))vt_exec;
            our = (void *)move_exec; patch(exe + VT_MOVE + 0x98, &our, 8);
            rc_log("pathing: orders watched");
        } else rc_log("pathing: the trace is not as expected");
    }
    {
        void *c = NULL, *our;
        int ok = 1;
        memcpy(&c, exe + VT_CLIENT_EXEC, 8);
        if (c != exe + FN_CLIENT_EXEC) ok = 0;
        for (int i = 0; i < 3; i++) { void *s; memcpy(&s, exe + SERVER_SLOTS[i], 8); if (s != exe + FN_SERVER_MESSAGE) ok = 0; }
        if (ok) {
            g_client_real = (void *(*)(void *))c;
            g_server_real = (void *(*)(void *, uint8_t *, void *, void *))(void *)(exe + FN_SERVER_MESSAGE);
            our = (void *)client_exec; patch(exe + VT_CLIENT_EXEC, &our, 8);
            our = (void *)server_message;
            for (int i = 0; i < 3; i++) patch(exe + SERVER_SLOTS[i], &our, 8);
            rc_log("pathing: the order's way watched");
        } else rc_log("pathing: the net is not as expected");
    }
brain:;
    void *vt_add = NULL, *vt_beo = NULL, *vt_msg = NULL, *our;
    memcpy(&vt_add, exe + W->vt_brain + 0x90, 8);
    memcpy(&vt_beo, exe + W->vt_brain + 0x68, 8);
    memcpy(&vt_msg, exe + W->vt_brain + 0x98, 8);
    memcpy(&slot, exe + W->vt_human + 8 * 23, 8);
    if (vt_add != exe + W->add_order || vt_beo != exe + W->set_beo || vt_msg != exe + W->message || (uint8_t *)slot < exe || (uint8_t *)slot >= exe + 0x1000000) { rc_log("pathing: the brain is not as expected"); return; }
    g_add_real = (void (*)(void *, void *, void *))vt_add;
    g_beo_real = (void (*)(void *, void *))vt_beo;
    our = (void *)add_order; patch(exe + W->vt_brain + 0x90, &our, 8);
    our = (void *)set_beo; patch(exe + W->vt_brain + 0x68, &our, 8);
    g_message_real = (void *(*)(void *, long, void *, void *))vt_msg;
    our = (void *)brain_message; patch(exe + W->vt_brain + 0x98, &our, 8);
    // the rule check is eleven bytes of plain moves first: it goes whole, by a jump
    static const uint8_t RULE_HEAD[11] = {0x48, 0x8b, 0x41, 0x30, 0x48, 0x89, 0x90, 0x10, 0x01, 0x00, 0x00};
    uint8_t *rule_stub = alloc(16);
    if (rule_stub && !memcmp(exe + W->rule, RULE_HEAD, 11)) {
        static const uint8_t JMP[6] = {0xff, 0x25, 0, 0, 0, 0};
        uint64_t to = (uint64_t)(uintptr_t)rule_check;
        uint8_t jump[5] = {0xe9};
        int32_t d = (int32_t)(rule_stub - (exe + W->rule + 5));
        memcpy(rule_stub, JMP, 6);
        memcpy(rule_stub + 6, &to, 8);
        memcpy(jump + 1, &d, 4);
        patch(exe + W->rule, jump, 5);
        rc_log("pathing: the rules are ours");
    } else rc_log("pathing: the rules are not as expected");
    rc_log("pathing: eHuman's quant is %llx", (unsigned long long)((uint8_t *)slot - exe));
    g_human_real = (void *(*)(void *))slot;
    patch(exe + W->vt_human + 8 * 23, &ours, 8);
}

// the game's thread, between quants
void pathing_tick(void) {
    static DWORD at;
    static int was = -1, button;
    if (!g_test || !g_exe) return;
    int down = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    if (down && !button && g_human_real) { g_n_start = g_n_fast = g_n_back = 0; g_traced = 0; g_n_events = 0; g_n_pipe = 0; g_stopped = g_let_go = 0; InterlockedIncrement(&g_gen); g_click = uistat_now(); }
    // Paths searched in the quant, not on the pather's thread. Measured, every other order: 171 to 212 ms from the click to a
    // squad moving on the thread, 48 to 69 ms in the quant. The game sets its flag again at each scene's start.
    if (g_sync && g_balance_on) *(uint8_t *)(g_exe + W->async) = 0;
    if (!down && button) g_release = uistat_now();
    squad_report();
    button = down;
    DWORD now = GetTickCount();
    if (now - at < 2000) return;
    at = now;
    int on = g_balance_on != 0;
    if (on == was) return;
    was = on;
    settings(on);
    *(int *)(g_exe + V_DQ) = on ? g_dq : 0;
    if (g_wait) *g_wait = on ? g_wait_ms / 1000.f : .3f;
    // The client plays its queue of quants at their pace and catches up only past 80 ms of lag (53aa21): an order can sit that long
    // in the queue. How fast the client plays is its own business: the simulation is the same.
    *(float *)(g_exe + V_LAG_MIN) = on ? g_lag_min : 80.f;
    *(float *)(g_exe + V_LAG_MAX) = on ? g_lag_max : 300.f;
}
