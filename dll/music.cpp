// Music director: our own battle music in place of the maps' music triggers.
// The maps switch tracks on any shot anywhere and cut them with stop_all; here a match builds up from
// calm into a lasting tension, peaks in big fights, leaves room for silence and lets every track play out.
// Tracks are the player's own files in binaries\x64\music\{calm,tension,battle} (mp3, wav, flac, m4a, wma),
// decoded with Media Foundation and played through XAudio2 2.9 - both part of Windows.
// Client side only: the game's simulation and resources stay untouched.
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>
#include <shellapi.h>
#include <math.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <algorithm>
#include "goh_replay.h"

MusicCtl g_music;
volatile long g_music_stop_vanilla;

// level 0 is the main menu playlist (it also plays over loading screens)
static const char *LEVEL_DIR[4] = {"main theme", "calm", "tension", "battle"};
static const char *LEVEL_NAME[4] = {"главное меню", "спокойно", "напряжение", "бой"};

// ---- tuning ----------------------------------------------------------------------------------------
// heat: +1 for every unit lost by anyone, fading with this time constant (game seconds)
static const double HEAT_TAU = 75;
// thresholds for a 1v1, scaled up for bigger games; downward ones are lower (no flapping)
static const double TO_TENSION = 2.5, TO_BATTLE = 10, BATTLE_KEEP = 4.5, TENSION_KEEP = 1.0;
static const double ENGAGED_AFTER_S = 7 * 60;  // by then the match counts as under way even without fights
// silence after a track, by the level the match is at
static const double SILENCE_CHANCE[4] = {0, 0.45, 0.30, 0.12};
static const double SILENCE_MIN[4] = {0, 25, 15, 8}, SILENCE_MAX[4] = {0, 70, 40, 20};
static const double FADE_IN = 2.5, CROSSFADE = 7, FADE_OUT_END = 5;

static char g_dir[MAX_PATH];
static volatile LONG g_in_match, g_game_ms, g_players, g_kills;
static volatile LONG g_menu_request;     // the game asked for music outside a battle: menu, loading, lobby
static volatile LONG g_hush;             // a replay is being run through: nothing plays, ours or the game's

static double now_s() { return GetTickCount64() / 1000.0; }

static unsigned g_rng = 0x9e3779b9u;
static double frand() {                  // [0, 1)
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (g_rng >> 8) / 16777216.0;
}
static double frange(double a, double b) { return a + (b - a) * frand(); }

static std::string utf8(const std::wstring &w) {
    char buf[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, buf, sizeof buf, nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string();
}

static void status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_music.now, sizeof g_music.now, fmt, ap);
    va_end(ap);
}

// ---- library -----------------------------------------------------------------------------------------
struct Track { std::wstring path; std::string name; };
static std::vector<Track> g_lib[4];
static std::vector<int> g_bag[4];        // shuffled order still to play, per level
static std::wstring g_last_path;

static bool audio_file(const wchar_t *n) {
    const wchar_t *e = wcsrchr(n, L'.');
    if (!e) return false;
    static const wchar_t *ok[] = {L".mp3", L".wav", L".flac", L".m4a", L".aac", L".wma"};
    for (auto x : ok) if (!_wcsicmp(e, x)) return true;
    return false;
}

static void scan(std::vector<Track> &out, const std::wstring &dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) scan(out, p);     // albums in subfolders are fine
        else if (audio_file(fd.cFileName)) {
            std::wstring stem = fd.cFileName;
            stem = stem.substr(0, stem.rfind(L'.'));
            out.push_back({p, utf8(stem)});
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static void load_library() {
    wchar_t base[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, g_dir, -1, base, MAX_PATH);
    std::wstring root = std::wstring(base) + L"music";
    CreateDirectoryW(root.c_str(), nullptr);
    for (int l = MUS_MENU; l <= MUS_BATTLE; l++) {
        wchar_t sub[16];
        MultiByteToWideChar(CP_ACP, 0, LEVEL_DIR[l], -1, sub, 16);
        std::wstring d = root + L"\\" + sub;
        CreateDirectoryW(d.c_str(), nullptr);
        g_lib[l].clear();
        g_bag[l].clear();
        scan(g_lib[l], d);
        g_music.tracks[l] = (int)g_lib[l].size();
    }
    rc_log("music: %d menu, %d calm, %d tension, %d battle tracks", g_music.tracks[0], g_music.tracks[1], g_music.tracks[2],
           g_music.tracks[3]);
}

static int total_tracks() { return g_music.tracks[1] + g_music.tracks[2] + g_music.tracks[3]; }   // battle levels

// an empty folder borrows from the nearest level (tension first, it fits both ways)
static int level_with_tracks(int want) {
    static const int order[4][3] = {{0, 0, 0}, {1, 2, 3}, {2, 3, 1}, {3, 2, 1}};
    for (int l : order[want]) if (!g_lib[l].empty()) return l;
    return 0;
}

static const Track *pick(int level) {
    auto &bag = g_bag[level];
    if (bag.empty()) {
        for (int i = 0; i < (int)g_lib[level].size(); i++) bag.push_back(i);
        for (int i = (int)bag.size() - 1; i > 0; i--) std::swap(bag[i], bag[(int)(frand() * (i + 1))]);
        // a fresh round must not open with what just played
        if (bag.size() > 1 && g_lib[level][bag.back()].path == g_last_path) std::swap(bag.front(), bag.back());
    }
    int i = bag.back();
    bag.pop_back();
    g_last_path = g_lib[level][i].path;
    return &g_lib[level][i];
}

// ---- playback: two decks, so tracks can cross ---------------------------------------------------------
static IXAudio2 *g_xa;
static IXAudio2MasteringVoice *g_master;

struct Deck {
    IMFSourceReader *rd = nullptr;
    IXAudio2SourceVoice *v = nullptr;
    std::vector<BYTE> buf[4];
    int next = 0, rate = 0, block = 0;
    bool eof = false;
    float vol = 0, target = 0, speed = 1;   // fade: volume moves toward target by speed per second
    double length = 0;                      // seconds, 0 if unknown
    int level = 0;
    std::string name;

    bool open(const Track &t, int lvl) {
        close();
        if (FAILED(MFCreateSourceReaderFromURL(t.path.c_str(), nullptr, &rd))) { rd = nullptr; return false; }
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        IMFMediaType *mt = nullptr;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        mt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        mt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        HRESULT hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, mt);
        mt->Release();
        IMFMediaType *cur = nullptr;
        if (FAILED(hr) || FAILED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) { close(); return false; }
        UINT32 ch = 0, hz = 0;
        cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &hz);
        cur->Release();
        if (!ch || !hz) { close(); return false; }
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = (WORD)ch;
        wf.nSamplesPerSec = hz;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = (WORD)(ch * 2);
        wf.nAvgBytesPerSec = hz * wf.nBlockAlign;
        if (FAILED(g_xa->CreateSourceVoice(&v, &wf))) { v = nullptr; close(); return false; }
        rate = (int)hz;
        block = wf.nBlockAlign;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        length = SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))
                     ? pv.uhVal.QuadPart / 1e7 : 0;
        PropVariantClear(&pv);
        eof = false;
        next = 0;
        vol = target = 0;
        level = lvl;
        name = t.name;
        feed();
        v->SetVolume(0);
        v->Start(0);
        return true;
    }

    void close() {
        if (v) { v->DestroyVoice(); v = nullptr; }      // stops and drops queued buffers first
        if (rd) { rd->Release(); rd = nullptr; }
        for (auto &b : buf) { b.clear(); b.shrink_to_fit(); }
        level = 0;
        vol = target = 0;
        name.clear();
    }

    bool on() const { return v != nullptr; }

    // keep ~1.5 s queued; buffer `next` is free whenever fewer than 3 are queued
    void feed() {
        if (!v || eof) return;
        XAUDIO2_VOICE_STATE st;
        v->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        UINT32 queued = st.BuffersQueued;
        while (queued < 3 && !eof) {
            auto &b = buf[next];
            b.clear();
            size_t want = (size_t)rate * block / 2;
            for (int empty = 0; b.size() < want && empty < 64;) {
                DWORD flags = 0;
                IMFSample *s = nullptr;
                HRESULT hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s);
                if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) { if (s) s->Release(); eof = true; break; }
                if (!s) { empty++; continue; }
                IMFMediaBuffer *mb = nullptr;
                if (SUCCEEDED(s->ConvertToContiguousBuffer(&mb))) {
                    BYTE *p = nullptr;
                    DWORD n = 0;
                    if (SUCCEEDED(mb->Lock(&p, nullptr, &n))) { b.insert(b.end(), p, p + n); mb->Unlock(); }
                    mb->Release();
                }
                s->Release();
            }
            if (b.empty()) break;
            XAUDIO2_BUFFER xb = {};
            xb.AudioBytes = (UINT32)b.size();
            xb.pAudioData = b.data();
            if (eof) xb.Flags = XAUDIO2_END_OF_STREAM;
            if (FAILED(v->SubmitSourceBuffer(&xb))) { eof = true; break; }
            next = (next + 1) % 4;
            queued++;
        }
    }

    bool finished() {
        if (!v) return true;
        XAUDIO2_VOICE_STATE st;
        v->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return eof && st.BuffersQueued == 0;
    }

    double played() {
        if (!v || !rate) return 0;
        XAUDIO2_VOICE_STATE st;
        v->GetState(&st, 0);
        return st.SamplesPlayed / (double)rate;
    }

    void fade(float to, double seconds) { target = to; speed = seconds > 0 ? (float)(1 / seconds) : 1000.f; }

    void step(double dt, float master) {
        if (!v) return;
        if (vol < target) vol = std::min(target, vol + speed * (float)dt);
        else if (vol > target) vol = std::max(target, vol - speed * (float)dt);
        v->SetVolume(vol * vol * master);          // squared: fades sound even to the ear
        if (vol <= 0 && target <= 0) close();
        else feed();
    }
};

static Deck g_deck[2];
static int g_cur = -1;                   // deck of the track that counts, -1 = none

// ---- director ------------------------------------------------------------------------------------------
enum Mode { M_OFF, M_WAIT, M_PLAY, M_SILENCE };
static Mode g_mode = M_OFF;
static double g_until;                   // M_WAIT / M_SILENCE end (real seconds)
static double g_silence_from;
static int g_silence_level;
static bool g_last_silence;              // the last gap was a real silence: no two in a row
static double g_heat;
static long g_seen_kills;
static int g_seen_ms, g_heat_level = MUS_CALM;
static bool g_engaged;                   // the match has warmed up: tension is the floor from now on
static int g_target = MUS_CALM;
static double g_target_since;

// next track of a playlist on the free deck; whatever played before fades out over the same time
static bool open_track(int lvl, double fade_in) {
    int d = g_cur < 0 ? 0 : 1 - g_cur;
    for (int tries = 0; tries < 3; tries++) {
        const Track *t = pick(lvl);
        if (g_deck[d].open(*t, lvl)) {
            if (g_cur >= 0 && g_deck[g_cur].on()) g_deck[g_cur].fade(0, fade_in);   // crossfade
            g_deck[d].fade(1, fade_in);
            g_cur = d;
            g_music.level = lvl;
            rc_log("music: %s -> \"%s\" (%.0f s)", LEVEL_DIR[lvl], t->name.c_str(), g_deck[d].length);
            return true;
        }
        rc_log("music: cannot play \"%s\"", t->name.c_str());
    }
    return false;
}

static void start_track(int want, double fade_in) {
    int lvl = level_with_tracks(want);
    if (lvl && open_track(lvl, fade_in)) { g_mode = M_PLAY; return; }
    g_mode = M_WAIT;                     // broken files: try again a bit later
    g_until = now_s() + 10;
}

static void begin_gap(double seconds, bool real_silence, int level) {
    g_mode = real_silence ? M_SILENCE : M_WAIT;
    g_until = now_s() + seconds;
    g_silence_from = now_s();
    g_silence_level = level;
    g_last_silence = real_silence;
    g_music.level = 0;
    if (real_silence) rc_log("music: silence %.0f s", seconds);
}

// what comes after a track that played out
static void after_track(int finished_level) {
    if (g_target > finished_level) { g_last_silence = false; start_track(g_target, 1.5); return; }   // the fight grew: answer it
    if (!g_last_silence && frand() < SILENCE_CHANCE[g_target])
        begin_gap(frange(SILENCE_MIN[g_target], SILENCE_MAX[g_target]), true, g_target);
    else
        begin_gap(frange(2, 5), false, g_target);                  // a breath between tracks
}

static void stop_all(double fade) {
    for (auto &d : g_deck) if (d.on()) d.fade(0, fade);
    g_cur = -1;
    g_music.level = 0;
}

static void update_heat() {
    long kills = g_kills;
    int ms = g_game_ms;
    double dt = (ms - g_seen_ms) / 1000.0;
    if (dt < 0 || dt > 60) dt = 0;       // a jump in the clock: no fading over it
    g_heat = g_heat * exp(-dt / HEAT_TAU) + (double)(kills - g_seen_kills);
    g_seen_kills = kills;
    g_seen_ms = ms;
    double s = pow(std::max(1.0, g_players / 2.0), 0.7);
    int h = g_heat_level;
    if (g_heat >= TO_BATTLE * s) h = MUS_BATTLE;
    else if (h == MUS_BATTLE && g_heat < BATTLE_KEEP * s) h = MUS_TENSION;
    if (h != MUS_BATTLE) {
        if (g_heat >= TO_TENSION * s) h = MUS_TENSION;
        else if (g_heat < TENSION_KEEP * s) h = MUS_CALM;
    }
    g_heat_level = h;
    if (!g_engaged && (g_heat >= TO_TENSION * s || ms >= ENGAGED_AFTER_S * 1000)) {
        g_engaged = true;
        rc_log("music: the match is under way (%d s)", ms / 1000);
    }
    int t = std::max<int>(g_engaged ? MUS_TENSION : MUS_CALM, h);
    if (t != g_target) { g_target = t; g_target_since = now_s(); }
    g_music.heat = (float)g_heat;
    g_music.target = g_target;
}

// ---- main menu: the playlist runs on, track after track, with a short breath between ----
static bool g_menu_on;
static double g_menu_next;

static void menu() {
    bool want = g_music.enabled && g_menu_request && g_music.tracks[MUS_MENU] > 0;
    if (!want) {
        if (g_menu_on) { stop_all(3); g_menu_on = false; rc_log("music: menu off"); }
        return;
    }
    double t = now_s();
    if (!g_menu_on) {
        g_menu_on = true;
        g_music_stop_vanilla = 1;        // whatever the game started before we took over
        g_menu_next = t + 1;
        rc_log("music: menu on");
    }
    Deck *d = g_cur >= 0 ? &g_deck[g_cur] : nullptr;
    if (d && d->on() && !d->finished()) {
        double played = d->played();
        if (d->length > 30 && played >= d->length - 1.0 && d->target > 0) d->fade(0, 1.0);
        int left = d->length > 0 ? (int)(d->length - played) : -1;
        if (left >= 0) status("Главное меню \xC2\xB7 %s \xC2\xB7 %d:%02d", d->name.c_str(), left / 60, left % 60);
        else status("Главное меню \xC2\xB7 %s", d->name.c_str());
        return;
    }
    if (d) { d->close(); g_cur = -1; g_music.level = 0; g_menu_next = t + frange(2, 4); }
    if (t >= g_menu_next && !open_track(MUS_MENU, 3)) g_menu_next = t + 10;
    status("Главное меню");
}

static void director() {
    if (g_hush) {
        if (g_mode != M_OFF || g_menu_on) {
            stop_all(1);
            g_mode = M_OFF;
            g_menu_on = false;
            g_music.active = 0;
            rc_log("music: quiet while the replay is prepared");
        }
        g_menu_request = 0;
        status("Тихо: реплей готовится");
        return;
    }
    bool want = g_music.enabled && g_in_match && total_tracks() > 0;
    if (want && g_menu_on) {             // menu (or loading screen) music gives way to the battle
        stop_all(3);
        g_menu_on = false;
        g_menu_request = 0;
    }
    if (!want) {
        if (g_mode != M_OFF) {
            rc_log("music: battle over");
            stop_all(FADE_OUT_END);
            g_mode = M_OFF;
            g_music.active = 0;
            g_menu_request = 0;          // the menu playlist comes back once the game asks for music there
        }
        menu();
        if (g_menu_on) return;
        if (!g_music.enabled) status("Выключено");
        else if (!total_tracks() && !g_music.tracks[MUS_MENU]) status("Нет треков: положите файлы в папку music");
        else status("Ждёт боя");
        return;
    }
    if (g_mode == M_OFF) {               // a battle starts: hush the game's music, begin quietly
        load_library();
        if (!total_tracks()) return;
        g_heat = 0;
        g_seen_kills = g_kills;
        g_seen_ms = g_game_ms;
        g_heat_level = g_target = MUS_CALM;
        g_engaged = false;
        g_last_silence = true;
        g_target_since = now_s();
        g_music_stop_vanilla = 1;
        g_music.active = 1;
        begin_gap(frange(6, 14), false, MUS_CALM);
        rc_log("music: battle begins");
    }
    update_heat();
    double t = now_s();
    switch (g_mode) {
    case M_WAIT:
        if (t >= g_until) start_track(g_target, FADE_IN);
        status("Пауза между треками \xC2\xB7 %s", LEVEL_NAME[g_target]);
        break;
    case M_SILENCE:
        // fighting flares up during the silence: break it, but not in the first seconds
        if (t >= g_until || (g_target > g_silence_level && t - g_silence_from >= 6)) start_track(g_target, FADE_IN);
        else status("Тишина %.0f с \xC2\xB7 %s", g_until - t, LEVEL_NAME[g_target]);
        break;
    case M_PLAY: {
        Deck &d = g_deck[g_cur];
        if (!d.on() || d.finished()) {   // the deck may already be closed by its end fade: the level lives in g_music
            int lvl = g_music.level ? g_music.level : g_target;
            d.close();
            g_cur = -1;
            after_track(lvl);
            break;
        }
        double played = d.played();
        // a calm track may give way to a fight it does not fit; tension and battle tracks always play out
        if (d.level == MUS_CALM && d.vol >= d.target &&
            ((g_target == MUS_BATTLE && played >= 40) || (g_target == MUS_TENSION && t - g_target_since >= 75 && played >= 60))) {
            rc_log("music: the fight outgrew the calm track");
            g_last_silence = false;
            start_track(g_target, CROSSFADE);
            break;
        }
        // a track that ends on a long fade or tail: let the next one in a little early
        if (d.length > 30 && played >= d.length - 1.0 && d.target > 0) d.fade(0, 1.0);
        int left = d.length > 0 ? (int)(d.length - played) : -1;
        if (left >= 0) status("%s \xC2\xB7 %s \xC2\xB7 %d:%02d", LEVEL_NAME[d.level], d.name.c_str(), left / 60, left % 60);
        else status("%s \xC2\xB7 %s", LEVEL_NAME[d.level], d.name.c_str());
        break;
    }
    default:
        break;
    }
}

static DWORD WINAPI audio_thread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { rc_log("music: Media Foundation unavailable"); status("Media Foundation недоступен"); return 0; }
    HMODULE xa = LoadLibraryA("xaudio2_9.dll");
    auto create = xa ? (HRESULT(WINAPI *)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR))GetProcAddress(xa, "XAudio2Create") : nullptr;
    if (!create || FAILED(create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(g_xa->CreateMasteringVoice(&g_master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr, nullptr, AudioCategory_GameMedia))) {
        rc_log("music: XAudio2 unavailable");
        status("XAudio2 недоступен");
        return 0;
    }
    g_rng ^= GetTickCount();
    load_library();
    double last = now_s();
    for (;;) {
        Sleep(20);
        double t = now_s(), dt = t - last;
        last = t;
        director();
        float master = std::max(0.f, std::min(1.f, (float)g_music.volume));
        for (auto &d : g_deck) d.step(dt, master);
    }
}

// ---- interface for the rest of the DLL --------------------------------------------------------------
extern "C" void music_start(const char *dir) {
    lstrcpynA(g_dir, dir, MAX_PATH);
    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%sgoh_music.ini", g_dir);
    g_music.enabled = GetPrivateProfileIntA("music", "enabled", 0, ini);     // off until the player turns it on (F10)
    g_music.volume = GetPrivateProfileIntA("music", "volume", 70, ini) / 100.f;
    status("Ждёт боя");
    CloseHandle(CreateThread(nullptr, 0, audio_thread, nullptr, 0, nullptr));
}

extern "C" void music_save(void) {
    char ini[MAX_PATH], v[16];
    snprintf(ini, sizeof ini, "%sgoh_music.ini", g_dir);
    WritePrivateProfileStringA("music", "enabled", g_music.enabled ? "1" : "0", ini);
    snprintf(v, sizeof v, "%d", (int)lroundf(g_music.volume * 100));
    WritePrivateProfileStringA("music", "volume", v, ini);
}

extern "C" void music_open_folder(void) {
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%smusic", g_dir);
    ShellExecuteA(nullptr, "open", p, nullptr, nullptr, SW_SHOWNORMAL);
}

extern "C" void music_tick(int in_match, int game_ms, int players) {
    g_hush = in_match < 0;
    g_in_match = in_match > 0;
    g_game_ms = game_ms;
    g_players = players;
}

extern "C" void music_kill(void) { InterlockedIncrement(&g_kills); }

// the game is about to play one of its /music/ tracks: 1 = keep it quiet, ours plays instead
extern "C" int music_block_game_music(void) {
    if (g_hush) return 1;
    if (!g_music.enabled) return 0;
    if (g_in_match) return total_tracks() > 0;
    if (!g_music.tracks[MUS_MENU]) return 0;
    g_menu_request = 1;
    return 1;
}
