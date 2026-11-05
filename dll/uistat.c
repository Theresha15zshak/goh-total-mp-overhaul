// [debug] ui_stats=1 in goh_snapshots.ini: what the mod's own work costs a frame, in the log every 10 seconds. Off: one branch.
#include <windows.h>
#include <stdint.h>
#include "goh_replay.h"

int g_uistat_on;
static const char *const NAMES[UISTAT_N] = {"logic tick", "camo tick", "draw", "frame gap"};
static struct { double sum, worst; long n, over; } g_s[UISTAT_N];
static double g_tick_us;

double uistat_now(void) {
    LARGE_INTEGER t;
    if (g_tick_us == 0) { QueryPerformanceFrequency(&t); g_tick_us = 1e6 / (double)t.QuadPart; }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * g_tick_us;
}

void uistat_add(int slot, double from) {
    double us = uistat_now() - from;
    g_s[slot].sum += us;
    g_s[slot].n++;
    if (us > g_s[slot].worst) g_s[slot].worst = us;
    if (us > 1000) g_s[slot].over++;
}

// the draw thread, once a frame
void uistat_frame(void) {
    static double last, shown;
    double now = uistat_now();
    if (last) {
        double gap = now - last;
        g_s[UISTAT_GAP].sum += gap; g_s[UISTAT_GAP].n++;
        if (gap > g_s[UISTAT_GAP].worst) g_s[UISTAT_GAP].worst = gap;
        if (gap > 33000) g_s[UISTAT_GAP].over++;
    }
    last = now;
    if (now - shown < 10e6) return;
    shown = now;
    for (int i = 0; i < UISTAT_N; i++) {
        if (g_s[i].n) rc_log("ui stats: %s: %ld times, %.0f us on average, worst %.0f, over the mark %ld", NAMES[i], g_s[i].n, g_s[i].sum / g_s[i].n, g_s[i].worst, g_s[i].over);
        g_s[i].sum = g_s[i].worst = 0; g_s[i].n = g_s[i].over = 0;
    }
}
