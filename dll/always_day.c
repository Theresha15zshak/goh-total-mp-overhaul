// Always day, for a host. A map's mission picks its weather by triggers that ask for a mission option: any_weather, day_weather
// or night_weather (the .mi's environment_any, _day and _night). The question is eTerm_mission_option's slot 9, in the game
// for a game of its own and in call_to_arms_server.exe for a hosted one. With the host's switch on, the answer is day.
// The switch reaches the server through named memory, apart from the balance's: its file is the version of the balance.
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "goh_replay.h"

#define DAY_NAME  "Local\\goh_always_day"
#define DAY_MAGIC 0x31594147u
typedef struct { uint32_t magic; volatile long on; } DayShare;
static DayShare *g_day;
static int g_server;
static unsigned char (*g_term_real)(void *, void *);

static int day_on(void) {
    if (!g_day && g_server) {
        HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, DAY_NAME);
        DayShare *s = h ? MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof *s) : NULL;
        if (h) CloseHandle(h);
        if (s && s->magic == DAY_MAGIC) g_day = s;
        else if (s) UnmapViewOfFile(s);
    }
    return g_day && g_day->on;
}

// the option the term asks for, by its name somewhere in the term: 1 any, 2 day, 3 night
static int term_kind(const unsigned char *term) {
    static const struct { const char *name; size_t n; } NAMES[] = {{"any_weather", 11}, {"day_weather", 11}, {"night_weather", 13}};
    for (size_t o = 8; o + 13 <= 0x70; o++)
        for (int k = 0; k < 3; k++)
            if (!memcmp(term + o, NAMES[k].name, NAMES[k].n)) return k + 1;
    return 0;
}

static unsigned char term_check(void *self, void *arg) {
    unsigned char real = g_term_real(self, arg);
    if (!day_on()) return real;
    int kind = term_kind((const unsigned char *)self);
    static int told;
    if (told++ < 8) rc_log("day: the mission asks for option %d, the game says %d", kind, real);
    return kind == 2 ? 1 : kind ? 0 : real;
}

void day_install(uint8_t *exe, void (*patch)(void *, const void *, size_t), int server) {
    const uintptr_t vt = server ? 0xdfb9e0 : 0x10f8da0, fn = server ? 0x80e870 : 0x9f95b0;
    void *slot;
    memcpy(&slot, exe + vt + 8 * 9, 8);
    if (slot != exe + fn) { rc_log("day: the mission's terms are not as expected, no always day"); return; }
    g_server = server;
    g_term_real = (unsigned char (*)(void *, void *))slot;
    void *ours = (void *)term_check;
    patch(exe + vt + 8 * 9, &ours, 8);
    if (server) return;
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(DayShare), DAY_NAME);
    g_day = h ? MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DayShare)) : NULL;
    if (g_day) g_day->magic = DAY_MAGIC;
}

void day_publish(int on) {
    if (g_day && !g_server && g_day->on != on) {
        g_day->on = on;
        rc_log("day: %s", on ? "always day" : "the map's weather");
    }
}
