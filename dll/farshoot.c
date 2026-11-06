// No farshoot from first person, a test in the Game tab of the options. Looking through a vehicle's sight in direct control the
// shot's distance is shown by the sight; past the range the vehicle's gun has by a plain order it is red, and the left button
// does not fire. Only on this machine: it is the player's own limit. eGameControl keeps where the sight's ray ends (+0x100,
// 4000 units at most), eManualControl where it starts (+0x470) and whose it is (+0x28), the actor's name at +0xf0. 20 units are
// a metre: the game's own 126.8 m at the cursor was 258 at 10. The range is not in the game's objects in any way that lasts: tools/vehicle_ranges.py reads it out of the game's files.
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "goh_replay.h"

#define TPC_CAMERA 0x159e218        // eGameControl; eManualControl at +0x3b0, on if its byte +0x10
#define CAMERA     0x159ae88        // +0xc distance: 2 in a sight, hundreds over the field
#define RAY_MAX    3990.f
#define UNITS_PER_M 20.f
#define SIGHT_BEHIND_M 2.2f         // the game counts from further forward than the sight: 129 here was its 126.8

typedef struct { const char *name; int range; } VehicleRange;
static const VehicleRange RANGES[] = {
#include "vehicle_ranges.h"
};

static uint8_t *g_exe;
static volatile long g_on, g_shown, g_tenths, g_far;

static int peek(uintptr_t p, void *out, size_t n) {
    SIZE_T got = 0;
    return p && ReadProcessMemory(GetCurrentProcess(), (void *)p, out, n, &got) && got == n;
}
static uintptr_t pq(uintptr_t p) { uintptr_t v = 0; peek(p, &v, 8); return v; }

static int compare(const void *a, const void *b) { return strcmp((const char *)a, ((const VehicleRange *)b)->name); }

static int range_of(uintptr_t actor) {
    struct { char buf[16]; size_t size, cap; } s;
    char name[64];
    if (!peek(actor + 0xf0, &s, sizeof s) || !s.size || s.size >= sizeof name) return 0;
    if (s.cap <= 15) memcpy(name, s.buf, s.size);
    else if (!peek(*(uintptr_t *)s.buf, name, s.size)) return 0;
    name[s.size] = 0;
    for (char *c = name; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    const VehicleRange *r = bsearch(name, RANGES, sizeof RANGES / sizeof RANGES[0], sizeof RANGES[0], compare);
    static char told[64];
    if (strcmp(told, name)) { strcpy(told, name); rc_log("farshoot: %s, range %d", name, r ? r->range : 0); }
    return r ? r->range : 0;
}

void farshoot_init(uint8_t *exe) { g_exe = exe; }
void farshoot_enable(int on) { g_on = on; }

// the game's thread
void farshoot_tick(void) {
    static double at;
    if (!g_exe) return;
    if (!g_on) { g_shown = g_far = 0; return; }
    double us = uistat_now();
    if (us - at < 8000) return;
    at = us;
    uintptr_t owner = pq((uintptr_t)g_exe + TPC_CAMERA), manual = owner ? pq(owner + 0x3b0) : 0;
    uint8_t on = 0;
    float dist = 1e9f, from[3], to[3];
    peek((uintptr_t)g_exe + CAMERA + 0xc, &dist, 4);
    if (!manual || !peek(manual + 0x10, &on, 1) || !on || dist > 5 || !peek(manual + 0x470, from, 12) || !peek(owner + 0x100, to, 12)) {
        g_shown = g_far = 0;
        return;
    }
    float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2], units = sqrtf(dx * dx + dy * dy + dz * dz);
    int range = range_of(pq(manual + 0x28));
    // past the ray's end it is only "more than": the ray stops there, whatever stands further on
    float metres = units / UNITS_PER_M - SIGHT_BEHIND_M;
    int ended = units >= RAY_MAX;
    g_tenths = (long)(metres * 10 + .5f) * (ended ? -1 : 1);
    g_far = range > 0 && (metres > range || (ended && range <= RAY_MAX / UNITS_PER_M));
    static int told;
    if (ended && range > 200 && !told++) rc_log("farshoot: the ray ended at %.0f units, range %d", units, range);
    g_shown = 1;
}

// tenths of a metre to show (below 0: more than that), 1 if shown; beyond: past the gun's range
int farshoot_state(int *tenths, int *beyond) {
    if (!g_shown) return 0;
    *tenths = (int)g_tenths;
    *beyond = (int)g_far;
    return 1;
}

int farshoot_blocks(void) { return g_on && g_shown && g_far; }
