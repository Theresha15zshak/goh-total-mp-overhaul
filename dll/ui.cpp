// In-game replay panel: Dear ImGui drawn into the game's own frames.
// The game's swapchain Present/ResizeBuffers are taken over through the vtable of a throwaway
// swapchain of the same class (works with or without ReShade in front of DXGI).
// Mouse clicks over the panel are kept from the game; movement passes so its cursor keeps moving.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <shellapi.h>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "goh_replay.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);


static IDXGISwapChain *g_sc;            // the game's swapchain, once seen
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11RenderTargetView *g_rtv;
static HWND g_hwnd;
static WNDPROC g_wndproc;
static volatile bool g_ready;
static volatile bool g_visible = true;  // F8
static volatile bool g_charts;          // match charts window: F9, the panel button, or the results page
static volatile bool g_music_win;       // music window: F10
#ifdef COMPETITIVE_ONLY
static volatile bool g_balance_win = true;   // the test build opens with its switch on screen
#else
static volatile bool g_balance_win;     // balance switch: F7
#endif
static volatile bool g_online_win;      // who is online with the mod: F6
static bool g_chat_tab;                 // the lobby's chat panel shows our GLOBAL CHAT tab (lobby_chat_tab)
static volatile bool g_chat_drawn;      // the global chat was drawn this frame: mouse and keyboard over it are ours
static volatile bool g_pvp_drawn;       // the PVP block over the game list was drawn this frame: the mouse over it is ours
static ImFont *g_game_font;             // the game's anka 15 (the game list's rows), loaded from interface.pak
static ImFont *g_game_font_small;       // anka 12: MMR under the names in the lobby
static bool g_failed;

static const float SPEEDS[] = {0.25f, 0.5f, 1, 2, 4, 8, 16};
static const char *SPEED_LABELS[] = {"\xC2\xBC\xC3\x97", "\xC2\xBD\xC3\x97", "1\xC3\x97", "2\xC3\x97", "4\xC3\x97", "8\xC3\x97", "16\xC3\x97"};
static const ImU32 ACCENT = IM_COL32(217, 164, 65, 255);
static const bool SHOW_PREPARE = false;  // "prepare the whole replay": off and its button hidden for now (2026-11-02)

#ifdef COMPETITIVE_ONLY
static bool panel_shown() { return false; }
static bool charts_shown() { return false; }
static bool music_shown() { return false; }
static bool prep_shown() { return false; }
#else
static bool panel_shown() {
    return g_visible && (g_ctl.state == ST_REPLAY || g_ctl.state == ST_SEEKING || g_ctl.state == ST_REWIND);
}

// never in a battle being played: the charts show every player's points and army, the enemy's too
static bool charts_shown() { return g_charts && g_ms.n > 1 && !g_live_match; }
static bool music_shown() { return g_music_win; }
static bool prep_shown() { return g_ctl.preparing && g_ctl.state != ST_IDLE; }
#endif
static bool balance_shown() { return g_balance_win; }
static bool online_shown() { return g_online_win; }
// asked once, outside battles: may we show this player in the online list
// the Steam sign-in window (login_window)
static bool g_login_win = true;         // this session: "Позже" puts it away
static bool g_login_opened;             // the sign-in page was opened once (the overlay or a browser)

static bool login_shown() {
    return g_login_win && g_online_consent == 1 && (g_steam_verified == 0 || g_steam_verified == 2 || g_login_opened) &&
           !g_live_match && g_ctl.state == ST_IDLE && (g_login_opened || online_remind());
}
static bool consent_shown() { return g_online_consent == -1 && !g_live_match && g_ctl.state == ST_IDLE; }

static bool raw_mouse_button(LPARAM l) {
    RAWINPUT ri;
    UINT size = sizeof ri;
    if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return false;
    return ri.header.dwType == RIM_TYPEMOUSE && (ri.data.mouse.usButtonFlags != 0);
}

static bool raw_keyboard(LPARAM l) {
    RAWINPUTHEADER hd;
    UINT size = sizeof hd;
    return GetRawInputData((HRAWINPUT)l, RID_HEADER, &hd, &size, sizeof hd) != (UINT)-1 && hd.dwType == RIM_TYPEKEYBOARD;
}

// F6 / F7 / F8 / F9 / F10, read once per frame: F10 is a system key (WM_SYSKEYDOWN) and the game may take
// the keyboard through raw input, so window messages cannot be relied on
static void hotkeys() {
    static const int KEYS[5] = {VK_F8, VK_F9, VK_F10, VK_F7, VK_F6};
    static bool was[5];
    bool fg = GetForegroundWindow() == g_hwnd;
    for (int i = 0; i < 5; i++) {
        bool down = fg && (GetAsyncKeyState(KEYS[i]) & 0x8000);
        if (down && !was[i]) {
            if (i == 0) g_visible = !g_visible;
            else if (i == 1) g_charts = !g_charts;
            else if (i == 2) g_music_win = !g_music_win;
            else if (i == 3) g_balance_win = !g_balance_win;
            else g_online_win = !g_online_win;
        }
        was[i] = down;
    }
    static bool census_was;                  // Ctrl+F12: memory census (census.c)
    bool census = fg && (GetAsyncKeyState(VK_F12) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
    if (census && !census_was) census_run("Ctrl+F12", g_census_game, g_census_page);
    census_was = census;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_ready) {
        if (panel_shown() || charts_shown() || music_shown() || prep_shown() || balance_shown() || online_shown() ||
            consent_shown() || login_shown() || g_pvp_drawn || g_chat_drawn) {
            ImGui_ImplWin32_WndProcHandler(h, m, w, l);
            // typing into our chat line: the keys are not the game's
            if (ImGui::GetIO().WantTextInput && ((m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m == WM_INPUT && raw_keyboard(l))))
                return m == WM_INPUT ? DefWindowProcW(h, m, w, l) : 0;
            if (ImGui::GetIO().WantCaptureMouse) {
                bool button = (m >= WM_LBUTTONDOWN && m <= WM_MOUSELAST && m != WM_MOUSEMOVE);
                if (button) return 0;
                if (m == WM_INPUT && raw_mouse_button(l)) return DefWindowProcW(h, m, w, l);
            }
        }
    }
    return CallWindowProcW(g_wndproc, h, m, w, l);
}

static void fmt_time(char *out, size_t n, int s) {
    if (s < 0) s = 0;
    snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static void panel() {
    const Ctl &c = g_ctl;
    ImGuiIO &io = ImGui::GetIO();
    float k = ImGui::GetFontSize() / 18.f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 110 * k), ImGuiCond_FirstUseEver, ImVec2(0.5f, 1));
    ImGui::SetNextWindowSize(ImVec2(660 * k, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("##goh_replay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    if (c.state == ST_REWIND) {         // the replay is being reloaded: nothing to control yet
        ImGui::TextDisabled("%s", c.msg);
        ImGui::End();
        return;
    }

    if (ImGui::Button(c.paused ? "\xD0\x9F\xD1\x83\xD1\x81\xD0\xBA" : "\xD0\x9F\xD0\xB0\xD1\x83\xD0\xB7\xD0\xB0", ImVec2(70 * k, 0)))   // Пуск / Пауза
        rc_send(CMD_PAUSE, c.paused ? 0 : 1);
    for (int i = 0; i < (int)(sizeof SPEEDS / sizeof SPEEDS[0]); i++) {
        ImGui::SameLine(0, i ? 2 * k : 10 * k);
        bool cur = c.state == ST_REPLAY && c.speed == SPEEDS[i];
        if (cur) ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
        if (ImGui::Button(SPEED_LABELS[i])) rc_send(CMD_SPEED, SPEEDS[i]);
        if (cur) ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, 10 * k);
    if (ImGui::Button("+30\xD1\x81")) rc_send(CMD_SEEK, c.game_ms + 30000.0);    // +30с
    ImGui::SameLine(0, 2 * k);
    if (ImGui::Button("+2\xD0\xBC")) rc_send(CMD_SEEK, c.game_ms + 120000.0);    // +2м
    if (c.snap_state == 2) {             // snapshots: the whole replay can be made instant
        ImGui::SameLine(0, 10 * k);
        if (c.preparing) {
            if (ImGui::Button("Стоп")) rc_send(CMD_PREPARE, 0);
        } else if (SHOW_PREPARE && (!c.duration_s || c.snap_to_ms < c.duration_s * 1000 - 30000)) {   // hidden for now
            if (ImGui::Button("Подготовить")) rc_send(CMD_PREPARE, 1);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Прогнать реплей до конца на максимальной скорости и вернуться сюда.\n"
                                  "После этого любая точка шкалы открывается сразу.");
        }
    }
    if (c.has_pov) {                    // our own camera was recorded for this game
        ImGui::SameLine(0, 10 * k);
        if (c.pov) ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
        if (ImGui::Button("\xD0" "\x92" "\xD0" "\xB8" "\xD0" "\xB4 " "\xD0" "\xB8" "\xD0" "\xB3" "\xD1" "\x80" "\xD0" "\xBE" "\xD0" "\xBA" "\xD0" "\xB0 (" "\xD1" "\x80" "\xD0" "\xB0" "\xD0" "\xB1" "\xD0" "\xBE" "\xD1" "\x82" "\xD0" "\xB0" "\xD0" "\xB5" "\xD1" "\x82 " "\xD1" "\x82" "\xD0" "\xBE" "\xD0" "\xBB" "\xD1" "\x8C" "\xD0" "\xBA" "\xD0" "\xBE " "\xD0" "\xB4" "\xD0" "\xBB" "\xD1" "\x8F " "\xD0" "\xB2" "\xD0" "\xB0" "\xD1" "\x88" "\xD0" "\xB5" "\xD0" "\xB3" "\xD0" "\xBE " "\xD0" "\xB8" "\xD0" "\xB3" "\xD1" "\x80" "\xD0" "\xBE" "\xD0" "\xBA" "\xD0" "\xB0)")) rc_send(CMD_POV, c.pov ? 0 : 1);   // Вид игрока (работает только для вашего игрока)
        if (c.pov) ImGui::PopStyleColor();
    }
    char now[16], total[16];
    fmt_time(now, sizeof now, c.game_ms / 1000);
    if (c.duration_s) fmt_time(total, sizeof total, c.duration_s); else strcpy(total, "--:--");
    ImGui::SameLine(0, 12 * k);
    ImGui::Text("%s / %s", now, total);

    // progress bar; a click seeks there
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 12 * k;
    ImGui::InvisibleButton("##bar", ImVec2(w, h));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(58, 61, 64, 255), 3 * k);
    if (c.duration_s) {
        float f = c.game_ms / (c.duration_s * 1000.f);
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        dl->AddRectFilled(p0, ImVec2(p0.x + w * f, p0.y + h), ACCENT, 3 * k);
        if (c.state == ST_SEEKING) {
            float t = c.seek_target_ms / (c.duration_s * 1000.f);
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            dl->AddLine(ImVec2(p0.x + w * t, p0.y), ImVec2(p0.x + w * t, p0.y + h), IM_COL32_WHITE, 2 * k);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            // where a click lands: inside the prepared stretch, the snapshot at or before the spot
            float x = (io.MousePos.x - p0.x) / w;
            int at = (int)(x * c.duration_s * 1000.0), snap = c.snap_state == 2 ? snap_best(at) : -1;
            char a[16], b[16];
            fmt_time(a, sizeof a, at / 1000);
            if (snap >= 0 && at < c.snap_to_ms + SNAP_EVERY_MS && !(snap <= c.game_ms && c.game_ms < at)) {
                fmt_time(b, sizeof b, snap / 1000);
                ImGui::SetTooltip("%s â %s, ÑÑÐ°Ð·Ñ", a, b);   // "a → b, сразу"
            } else {
                ImGui::SetTooltip("%s", a);
            }
        }
        if (ImGui::IsItemClicked()) {
            float x = (io.MousePos.x - p0.x) / w;
            rc_send(CMD_SEEK, x * c.duration_s * 1000.0);
        }
        // under the bar: the stretch covered by snapshots, where a click jumps at once
        if (c.snap_state == 2 && c.snap_from_ms >= 0) {
            float a = c.snap_from_ms / (c.duration_s * 1000.f), b = (c.snap_to_ms + SNAP_EVERY_MS) / (c.duration_s * 1000.f);
            a = a < 0 ? 0 : a > 1 ? 1 : a;
            b = b < a ? a : b > 1 ? 1 : b;
            dl->AddRectFilled(ImVec2(p0.x + w * a, p0.y + h + 2 * k), ImVec2(p0.x + w * b, p0.y + h + 5 * k), IM_COL32(90, 175, 120, 255), 1.5f * k);
        }
        ImGui::Dummy(ImVec2(w, 4 * k));
    }

    const char *name = strrchr(c.path, '/');
    name = name ? name + 1 : c.path;
    if (c.state == ST_SEEKING) {
        char t[16];
        fmt_time(t, sizeof t, c.seek_target_ms / 1000);
        int span = c.seek_target_ms - c.seek_from_ms, done = c.game_ms - c.seek_from_ms;
        int pct = span > 0 ? (int)((int64_t)done * 100 / span) : 0;
        pct = pct < 0 ? 0 : pct > 99 ? 99 : pct;
        if (c.preparing) {
            char at[16];
            fmt_time(at, sizeof at, c.game_ms / 1000);
            if (c.duration_s && c.rate >= 1) {
                int left = (int)((c.duration_s * 1000.f - c.game_ms) / 1000.f / c.rate / 60.f + 0.99f);
                ImGui::TextDisabled("Готовлю перемотку: %s, %d%% \xC2\xB7 \xC3\x97%.0f \xC2\xB7 ещё ~%d мин", at,
                                    (int)((int64_t)c.game_ms * 100 / (c.duration_s * 1000)), c.rate, left < 1 ? 1 : left);
            } else {
                ImGui::TextDisabled("Готовлю перемотку: %s \xC2\xB7 \xC3\x97%.0f", at, c.rate);
            }
        } else if (c.rate >= 1) {
            int left = (int)((c.seek_target_ms - c.game_ms) / 1000.f / c.rate + 0.5f);
            ImGui::TextDisabled("Перематываю к %s\xE2\x80\xA6 %d%% \xC2\xB7 \xC3\x97%.0f \xC2\xB7 ещё ~%d с", t, pct, c.rate, left < 0 ? 0 : left);
        } else {
            ImGui::TextDisabled("Перематываю к %s\xE2\x80\xA6 %d%%", t, pct);
        }
    } else if (c.msg[0]) {
        ImGui::TextDisabled("%s", c.msg);
    } else {
        ImGui::TextDisabled("%s \xC2\xB7 %s", name, c.paused ? "\xD0\xBF\xD0\xB0\xD1\x83\xD0\xB7\xD0\xB0" : "");   // name · пауза
        if (!c.paused) { ImGui::SameLine(0, 0); ImGui::TextDisabled("%g\xC3\x97", c.speed); }
        if (c.snap_state == 1) {
            ImGui::SameLine(0, 10 * k);
            ImGui::TextDisabled("\xC2\xB7 готовлю мгновенную перемотку\xE2\x80\xA6");
        } else if (c.snap_state == 2 && c.snap_from_ms >= 0) {
            char a[16], b[16];
            fmt_time(a, sizeof a, c.snap_from_ms / 1000);
            fmt_time(b, sizeof b, c.snap_to_ms / 1000);
            ImGui::SameLine(0, 10 * k);
            ImGui::TextDisabled("\xC2\xB7 мгновенно %s\xE2\x80\x93%s, %d МБ", a, b, c.snap_mb);
        }
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 345 * k);
    if (ImGui::SmallButton("Музыка (F10)")) g_music_win = !g_music_win;
    if (g_ms.n > 1) {
        ImGui::SameLine(ImGui::GetWindowWidth() - 230 * k);
        if (ImGui::SmallButton("Графики (F9)")) g_charts = !g_charts;
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 120 * k);
    ImGui::TextDisabled("F8 \xE2\x80\x94 \xD1\x81\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82\xD1\x8C");   // F8 — скрыть
    ImGui::End();
}

// ---- match charts ------------------------------------------------------------------------------
static const ImU32 PLAYER_COL[MS_PLAYERS] = {
    IM_COL32(86, 152, 230, 255), IM_COL32(230, 120, 70, 255), IM_COL32(96, 190, 110, 255), IM_COL32(220, 80, 95, 255),
    IM_COL32(160, 120, 220, 255), IM_COL32(70, 195, 195, 255), IM_COL32(225, 195, 75, 255), IM_COL32(225, 120, 175, 255)};
static bool g_player_on[MS_PLAYERS] = {true, true, true, true, true, true, true, true};

static float nice_ceil(float v) {
    if (v <= 0) return 1;
    float p = powf(10, floorf(log10f(v))), m = v / p;
    return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 2.5f ? 2.5f : m <= 5 ? 5 : 10) * p;
}

// one series for every player over game time; hovering shows the values at that moment
static void player_label(char *out, size_t n, const MatchStats &s, int p);

static void chart(const char *id, int series, const char *title, float height) {
    const MatchStats &s = g_ms;
    int n = (int)s.n, players = s.players;
    if (n < 2 || players <= 0) return;
    float k = ImGui::GetFontSize() / 18.f;
    ImGui::TextUnformatted(title);
    height -= ImGui::GetTextLineHeightWithSpacing();
    float vmax = 0;
    for (int p = 0; p < players; p++)
        if (g_player_on[p])
            for (int i = 0; i < n; i++) vmax = fmaxf(vmax, s.v[series][p][i]);
    vmax = nice_ceil(vmax * 1.05f);
    float t0 = s.t_ms[0] / 1000.f, t1 = s.t_ms[n - 1] / 1000.f;
    if (t1 <= t0) t1 = t0 + 1;

    ImVec2 o = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, gutter = 52 * k, bottom = 20 * k;
    ImGui::InvisibleButton(id, ImVec2(w, height));
    bool hovered = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a(o.x + gutter, o.y + 6 * k), b(o.x + w - 8 * k, o.y + height - bottom);
    auto X = [&](float t) { return a.x + (t - t0) / (t1 - t0) * (b.x - a.x); };
    auto Y = [&](float v) { return b.y - v / vmax * (b.y - a.y); };
    const ImU32 grid = IM_COL32(255, 255, 255, 22), label = IM_COL32(200, 200, 200, 150);
    char buf[32];
    for (int g = 0; g <= 4; g++) {
        float v = vmax * g / 4, y = Y(v);
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), grid);
        snprintf(buf, sizeof buf, "%g", v);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(a.x - ts.x - 6 * k, y - ts.y / 2), label, buf);
    }
    float span = t1 - t0, step = span <= 120 ? 15 : span <= 300 ? 30 : span <= 900 ? 60 : span <= 2400 ? 300 : 600;
    for (float t = ceilf(t0 / step) * step; t <= t1; t += step) {
        fmt_time(buf, sizeof buf, (int)t);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddLine(ImVec2(X(t), a.y), ImVec2(X(t), b.y), grid);
        dl->AddText(ImVec2(X(t) - ts.x / 2, b.y + 2 * k), label, buf);
    }
    dl->PushClipRect(ImVec2(a.x - 2, a.y - 3), ImVec2(b.x + 2, b.y + 3), true);
    static ImVec2 pts[MS_SAMPLES];
    for (int p = 0; p < players; p++) {
        if (!g_player_on[p]) continue;
        for (int i = 0; i < n; i++) pts[i] = ImVec2(X(s.t_ms[i] / 1000.f), Y(s.v[series][p][i]));
        dl->AddPolyline(pts, n, PLAYER_COL[p], 0, 2 * k);
    }
    dl->PopClipRect();

    if (hovered) {
        float mx = ImGui::GetIO().MousePos.x, t = t0 + (mx - a.x) / (b.x - a.x) * (t1 - t0);
        int lo = 0, hi = n - 1;
        while (lo < hi) { int mid = (lo + hi) / 2; if (s.t_ms[mid] / 1000.f < t) lo = mid + 1; else hi = mid; }
        float x = X(s.t_ms[lo] / 1000.f);
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), IM_COL32(255, 255, 255, 90));
        ImGui::BeginTooltip();
        fmt_time(buf, sizeof buf, s.t_ms[lo] / 1000);
        ImGui::TextDisabled("%s", buf);
        for (int p = 0; p < players; p++) {
            if (!g_player_on[p]) continue;
            dl->AddCircleFilled(ImVec2(x, Y(s.v[series][p][lo])), 3.5f * k, PLAYER_COL[p]);
            char who[72];
            player_label(who, sizeof who, s, p);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(PLAYER_COL[p]), "%s: %.0f", who, s.v[series][p][lo]);
        }
        ImGui::EndTooltip();
    }
}

// a player's name from the replay, or their number until it is known
static void player_label(char *out, size_t n, const MatchStats &s, int p) {
    if (s.name[p][0]) snprintf(out, n, "%s", s.name[p]);
    else snprintf(out, n, "Игрок %d", s.id[p]);
}

static void charts() {
    ImGuiIO &io = ImGui::GetIO();
    float k = ImGui::GetFontSize() / 18.f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 24 * k, 70 * k), ImGuiCond_FirstUseEver, ImVec2(1, 0));
    ImGui::SetNextWindowSize(ImVec2(860 * k, 600 * k), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.94f);
    bool open = true;
    if (!ImGui::Begin("Графики матча###goh_charts", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        g_charts = open;
        return;
    }
    const MatchStats &s = g_ms;
    for (int p = 0; p < s.players; p++) {        // legend: a click hides or shows that player's lines
        if (p) ImGui::SameLine(0, 16 * k);
        ImVec2 c = ImGui::GetCursorScreenPos();
        float h = ImGui::GetFontSize();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(c.x, c.y + h * 0.35f), ImVec2(c.x + h * 0.9f, c.y + h * 0.65f + 2 * k),
                                                  g_player_on[p] ? PLAYER_COL[p] : IM_COL32(90, 90, 90, 255), 2 * k);
        ImGui::Dummy(ImVec2(h, h));
        ImGui::SameLine(0, 4 * k);
        char who[72], label[96];
        player_label(who, sizeof who, s, p);
        snprintf(label, sizeof label, "%s##pl%d", who, p);
        if (!g_player_on[p]) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Selectable(label, false, 0, ImGui::CalcTextSize(label, nullptr, true))) g_player_on[p] = !g_player_on[p];
        if (!g_player_on[p]) ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    if (ImGui::BeginTabBar("##tabs")) {
        float full = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - 4 * k;
        float half = (full - ImGui::GetStyle().ItemSpacing.y) / 2;
        if (ImGui::BeginTabItem("Траты")) {
            chart("##spent", MS_SPENT, "Потрачено очков, всего", full);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Экономика")) {
            chart("##armymp", MS_ARMY_MP, "Стоимость армии на поле, ЛС (оценка)", half);
            chart("##budget", MS_BUDGET, "Очки на руках", half);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Армия")) {
            chart("##army", MS_ARMY, "Стоимость армии на поле (очки лимита)", full);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Убийства")) {
            chart("##kills_inf", MS_KILLS_INF, "Убито пехоты противника, всего", half);
            chart("##kills_veh", MS_KILLS_VEH, "Уничтожено техники противника, всего", half);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Потери")) {
            float third = (full - 2 * ImGui::GetStyle().ItemSpacing.y) / 3;
            chart("##deaths_inf", MS_DEATHS_INF, "Потеряно пехоты, всего", third);
            chart("##deaths_veh", MS_DEATHS_VEH, "Потеряно техники, всего", third);
            chart("##lost", MS_LOST, "Потеряно стоимости армии (очки лимита), всего", third);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    g_charts = open;
}

// ---- a replay being prepared: run through to its end, snapshots on the way ------------------------------------
static void prep_window() {
    const Ctl &c = g_ctl;
    ImGuiIO &io = ImGui::GetIO();
    float k = ImGui::GetFontSize() / 18.f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.42f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##goh_prep", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("Готовлю реплей к перемотке");
    // progress in snapshots, one per SNAP_EVERY_MS: the clock in between runs too fast to read and looked
    // as if every second were being saved
    int done = c.snap_to_ms > 0 ? c.snap_to_ms : 0;
    int shots = c.snap_from_ms >= 0 && c.snap_to_ms >= c.snap_from_ms ? (c.snap_to_ms - c.snap_from_ms) / SNAP_EVERY_MS + 1 : 0;
    char at[16], label[64];
    fmt_time(at, sizeof at, done / 1000);
    float f = c.duration_s ? done / (c.duration_s * 1000.f) : 0;
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    if (c.duration_s) snprintf(label, sizeof label, "готово до %s · снимков %d · %d%%", at, shots, (int)(f * 100));
    else snprintf(label, sizeof label, "готово до %s · снимков %d", at, shots);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.35f, 0.69f, 0.47f, 1));
    ImGui::ProgressBar(f, ImVec2(380 * k, 0), label);
    ImGui::PopStyleColor();
    if (c.duration_s && c.rate >= 1) {
        int left = (int)((c.duration_s * 1000.f - c.game_ms) / 1000.f / c.rate / 60.f + 0.99f);
        ImGui::TextDisabled("×%.0f · ещё ~%d мин · звук выключен", c.rate, left < 1 ? 1 : left);
    } else {
        ImGui::TextDisabled("×%.0f · звук выключен", c.rate);
    }
    ImGui::TextDisabled("Снимок раз в %d с. Бой между снимками игра всё равно считает целиком:", SNAP_EVERY_MS / 1000);
    ImGui::TextDisabled("без этого следующий снимок не получить. Потом любая точка шкалы — сразу.");
    if (ImGui::Button("Пропустить")) rc_send(CMD_PREPARE, 0);
    ImGui::End();
}

// ---- music --------------------------------------------------------------------------------------------
static void music_window() {
    ImGuiIO &io = ImGui::GetIO();
    float k = ImGui::GetFontSize() / 18.f;
    ImGui::SetNextWindowPos(ImVec2(24 * k, 70 * k), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(470 * k, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    bool open = true;
    (void)io;
    if (!ImGui::Begin("Музыка###goh_music", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                                                         ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        g_music_win = open;
        return;
    }
    bool on = g_music.enabled != 0;
    if (ImGui::Checkbox("Своя музыка (меню и бои)", &on)) { g_music.enabled = on; music_save(); }
    float vol = g_music.volume * 100;
    ImGui::SetNextItemWidth(260 * k);
    if (ImGui::SliderFloat("Громкость", &vol, 0, 100, "%.0f%%")) g_music.volume = vol / 100;
    if (ImGui::IsItemDeactivatedAfterEdit()) music_save();

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
    ImGui::TextWrapped("%s", g_music.now);
    ImGui::PopStyleColor();
    if (g_music.active) {
        static const char *lv[4] = {"—", "спокойно", "напряжение", "бой"};
        ImGui::TextDisabled("Бой требует: %s \xC2\xB7 накал %.1f", lv[g_music.target & 3], g_music.heat);
    }
    ImGui::TextDisabled("Треки: меню %d \xC2\xB7 спокойные %d \xC2\xB7 напряжение %d \xC2\xB7 бой %d", g_music.tracks[MUS_MENU],
                        g_music.tracks[MUS_CALM], g_music.tracks[MUS_TENSION], g_music.tracks[MUS_BATTLE]);
    if (ImGui::Button("Открыть папку music")) music_open_folder();
    ImGui::SameLine();
    ImGui::TextDisabled("main theme / calm / tension / battle \xC2\xB7 mp3, flac, wav");
    ImGui::TextDisabled("Новые файлы подхватываются со следующего боя. F10 \xE2\x80\x94 скрыть");
    ImGui::End();
    g_music_win = open;
}

// ---- competitive balance -------------------------------------------------------------------------------
static void balance_window() {
    float k = ImGui::GetFontSize() / 18.f;
    ImGui::SetNextWindowPos(ImVec2(24 * k, 70 * k), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    bool open = true;
    if (!ImGui::Begin("Баланс###goh_balance", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                                                         ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        g_balance_win = open;
        return;
    }
    if (!g_balance_ready) {
        ImGui::TextWrapped("Недоступно: версия игры или её файлы не те, под которые сделан мод (подробности в goh_replay.log).");
    } else {
        bool on = g_balance_on != 0;
        ImGui::BeginDisabled(g_balance_locked != 0);
        if (ImGui::Checkbox("Компетитив", &on)) g_balance_on = on;
        ImGui::EndDisabled();
        ImGui::TextDisabled("Германия, Оборонительная: т2 \xE2\x80\x94 15 см NbW 41, т3 \xE2\x80\x94 12 см sGrW 42");
        if (g_balance_locked) ImGui::TextDisabled("Идёт бой или реплей: переключить можно после него.");
        else ImGui::TextDisabled("Действует на следующий матч или реплей, который вы запустите.");
        ImGui::Text("Версия баланса: %s", g_balance_tag);
        ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
        ImGui::TextWrapped("Игры это пока не сверяют: у всех игроков матча должны быть эта же версия и галочка, "
                           "иначе рассинхрон или вылет.");
        ImGui::PopStyleColor();
        if (g_balance_reads)
            ImGui::TextDisabled("Игра читала файл баланса: %ld раз, последний \xE2\x80\x94 %s", g_balance_reads,
                                g_balance_last ? "компетитив" : "ваниль");
    }
    ImGui::TextDisabled("F7 \xE2\x80\x94 скрыть");
    ImGui::End();
    g_balance_win = open;
}

// ---- who is online with the mod -----------------------------------------------------------------------
static const char *CONSENT_TEXT =
    "Мод отправляет на сервер мода (goh-online.goh-mod.workers.dev): раз в 2 минуты ваш SteamID, ник Steam "
    "и версию баланса (список онлайн), а после рейтингового матча (Ось против Союзников, «Зоны боевых действий» "
    "или «Превосходство») его итог из реплея игры: режим, карту, время, игроков (SteamID, ник, нация) и победителя. "
    "Больше ничего.";

static void consent_window() {
    float k = ImGui::GetFontSize() / 18.f;
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460 * k, 0));
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::Begin("Мод: онлайн и рейтинг PVP###goh_consent", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                                                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::TextWrapped("Показывать вас в списке игроков с модом и засчитывать ваши матчи в рейтинг PVP?");
    ImGui::Spacing();
    ImGui::TextDisabled("%s", CONSENT_TEXT);
    ImGui::Spacing();
    if (ImGui::Button("Да, показывать")) online_set_consent(1);
    ImGui::SameLine();
    if (ImGui::Button("Нет")) online_set_consent(0);
    ImGui::TextDisabled("Поменять можно в окне F6.");
    ImGui::End();
}

// ---- Steam sign-in (online.c, server/src/verify.js): asked at the start until the player signs in --------

// in the game's Steam overlay when it is on, else in the browser
static void open_login(const char *url, bool browser) {
    HMODULE h = GetModuleHandleA("steam_api64.dll");
    auto friends_if = h ? (void *(*)())GetProcAddress(h, "SteamAPI_SteamFriends_v017") : nullptr;
    auto utils_if = h ? (void *(*)())GetProcAddress(h, "SteamAPI_SteamUtils_v010") : nullptr;
    auto overlay = h ? (void (*)(void *, const char *, int))GetProcAddress(h, "SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage") : nullptr;
    auto enabled = h ? (bool (*)(void *))GetProcAddress(h, "SteamAPI_ISteamUtils_IsOverlayEnabled") : nullptr;
    void *fr = friends_if ? friends_if() : nullptr, *ut = utils_if ? utils_if() : nullptr;
    if (!browser && fr && ut && overlay && enabled && enabled(ut)) overlay(fr, url, 0);
    else ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

static const char *LOGIN_WHY =
    "Чтобы ваши матчи засчитывались в рейтинг PVP, а в глобальном чате был виден ваш ник, подтвердите, что этот "
    "аккаунт Steam ваш.\n\n"
    "Откроется официальная страница steamcommunity.com (в оверлее Steam или в браузере): нажмите там «Войти». "
    "Steam сообщит серверу мода только ваш SteamID. Пароль, сессия и доступ к аккаунту моду не передаются, "
    "регистрироваться нигде не нужно.\n\n"
    "Без входа: в чате вы Anonymous, ваши матчи не отправляются в рейтинг, личная статистика недоступна.";

static void login_window() {
    float k = ImGui::GetFontSize() / 18.f;
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520 * k, 0));
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::Begin("Мод: вход через Steam###goh_login", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                                                           ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    static char url[1024];
    bool have = online_login_url(url, sizeof url);
    if (g_steam_verified == 1) {
        ImGui::TextColored(ImColor(120, 200, 100), "Готово: аккаунт Steam подтверждён.");
        ImGui::TextWrapped("Ваши матчи теперь идут в рейтинг, в чате виден ваш ник.");
        if (ImGui::Button("Закрыть")) g_login_win = false;
        ImGui::End();
        return;
    }
    if (g_steam_verified == 2)
        ImGui::TextColored(ImColor(209, 120, 54), "Ваш SteamID на сервере мода занят другим ключом. Войдите через Steam, чтобы вернуть его.");
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(LOGIN_WHY);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!g_login_opened) {
        if (ImGui::Button("Войти через Steam")) { online_steam_login(); g_login_opened = true; }
        ImGui::SameLine();
        if (ImGui::Button("Позже")) g_login_win = false;
        bool remind = online_remind() != 0;
        if (ImGui::Checkbox("Напоминать при запуске игры", &remind)) online_set_remind(remind);
    } else if (!have) {
        ImGui::TextDisabled("Получаем ссылку от сервера мода...");
    } else {
        static bool auto_opened;
        if (!auto_opened) { auto_opened = true; open_login(url, false); }
        ImGui::TextColored(ImColor(ACCENT), "Ждём подтверждения от Steam...");
        ImGui::TextWrapped("Если страница не открылась, откройте её в браузере. Это страница входа самого Steam "
                           "(steamcommunity.com), ссылка одноразовая и действует 15 минут:");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##login_url", url, sizeof url, ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button("Открыть в браузере")) open_login(url, true);
        ImGui::SameLine();
        if (ImGui::Button("Скопировать ссылку")) ImGui::SetClipboardText(url);
        ImGui::SameLine();
        if (ImGui::Button("Позже")) g_login_win = false;
    }
    ImGui::End();
}

// ---- bottom left, in the menus: is this DLL an official build (online.c integrity_check) ------------------
static void integrity_window() {
    if (g_live_match || g_ctl.state != ST_IDLE) return;
    long st = g_integrity;
    if (st == INTEGRITY_CHECKING) return;
    ImGuiIO &io = ImGui::GetIO();
    float k = ImGui::GetFontSize() / 18.f;
    ImU32 col = IM_COL32(150, 150, 150, 255), bg = IM_COL32(16, 16, 15, 200);
    char line1[96], line2[160] = "";
    snprintf(line1, sizeof line1, "Мод %s", g_mod_version);
    if (st == INTEGRITY_CURRENT) snprintf(line2, sizeof line2, "Официальная сборка, последняя версия");
    else if (st == INTEGRITY_UNREACHABLE) snprintf(line2, sizeof line2, "Не удалось проверить сборку: сервер мода не отвечает");
    else if (st == INTEGRITY_OLD) {
        col = IM_COL32(230, 160, 70, 255);
        snprintf(line2, sizeof line2, "Устаревшая версия: обновите мод%s%s", g_integrity_newest[0] ? " до " : "", g_integrity_newest);
    } else {            // not on the list: blinks red
        float a = 0.55f + 0.45f * sinf((float)ImGui::GetTime() * 6.f);
        col = IM_COL32(255, 70, 60, 255);
        bg = IM_COL32(110, 10, 8, (int)(90 + 140 * a));
        snprintf(line2, sizeof line2, "На вашем компьютере НЕ официальная DLL мода. Используйте на свой страх и риск.");
    }
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImVec2 t1 = ImGui::CalcTextSize(line1), t2 = ImGui::CalcTextSize(line2);
    float pad = 8 * k, w = (t1.x > t2.x ? t1.x : t2.x) + 2 * pad, h = t1.y + t2.y + 2 * pad + 2 * k;
    ImVec2 p0(12 * k, io.DisplaySize.y - 12 * k - h);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), bg, 4 * k);
    dl->AddText(ImVec2(p0.x + pad, p0.y + pad), IM_COL32(120, 120, 120, 255), line1);
    dl->AddText(ImVec2(p0.x + pad, p0.y + pad + t1.y + 2 * k), col, line2);
}

// the players online with the mod; the table fills the height left but `bottom`
static void online_list(float bottom) {
    static OnlinePlayer list[ONLINE_MAX];
    int state = 0;
    int n = online_copy(list, ONLINE_MAX, &state);
    bool me = g_online_consent == 1;
    if (ImGui::Checkbox("Показывать меня и засчитывать мои матчи", &me)) online_set_consent(me);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", CONSENT_TEXT);
    if (g_online_held) {
        ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
        ImGui::TextWrapped("Ваш SteamID закреплён за другим ключом (goh_mod_key.txt в Документы\\My Games\\gates of hell): напишите автору мода.");
        ImGui::PopStyleColor();
    }
    if (state == 0) ImGui::TextDisabled("Загрузка...");
    else {
        if (state == 2) {
            ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
            ImGui::TextUnformatted("Сервер не отвечает, список может быть старым.");
            ImGui::PopStyleColor();
        }
        ImGui::Text("Сейчас в игре с модом: %d", n);
        ImGui::Separator();
        const char *my = online_my_id();
        if (ImGui::BeginTable("##players", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg, ImVec2(0, -bottom))) {
            ImGui::TableSetupColumn("Игрок", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Баланс", ImGuiTableColumnFlags_WidthFixed);
            for (int i = 0; i < n; i++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                bool self = !strcmp(list[i].id, my);
                if (self) ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
                if (ImGui::Selectable(list[i].name)) {
                    char url[64];
                    snprintf(url, sizeof url, "steam://url/SteamIDPage/%s", list[i].id);
                    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
                }
                if (self) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Открыть профиль в Steam");
                ImGui::TableNextColumn();
                bool same = !strcmp(list[i].version, g_balance_tag);
                if (same) ImGui::TextDisabled("%s", list[i].version);
                else ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1), "%s", list[i].version);
                if (!same && ImGui::IsItemHovered()) ImGui::SetTooltip("Другая версия баланса: компетитив вместе не сыграть");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
}

static void online_window() {
    float k = ImGui::GetFontSize() / 18.f;
    ImGui::SetNextWindowPos(ImVec2(24 * k, 260 * k), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380 * k, 360 * k), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    bool open = true;
    if (!ImGui::Begin("Онлайн###goh_online", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        g_online_win = open;
        return;
    }
    online_list(ImGui::GetFrameHeightWithSpacing());
    ImGui::TextDisabled("F6 \xE2\x80\x94 скрыть");
    ImGui::End();
    g_online_win = open;
}

// ---- the PVP block: over the game list of the Internet page, after its PVP button (pvp.c) ---------------
// Set like the page's own list (its frame and background stay, pvp.c takes its rows and column titles off):
// the game list's font and colours (skin main_menu_dark_skin, anka 15), its 32-unit rows, and columns where
// the list has Host / Game / Map / Players.
static const ImU32 VAN_TITLE = IM_COL32(195, 195, 195, 255);    // column titles
static const ImU32 VAN_TEXT = IM_COL32(232, 229, 208, 255);     // rows
static const ImU32 VAN_HOVER = IM_COL32(235, 178, 108, 255);    // listbox item, focused
static const ImU32 VAN_HOVER_BG = IM_COL32(44, 37, 30, 200);
static const ImU32 VAN_WARN = IM_COL32(209, 120, 54, 255);      // the skin's "disabled" orange
static const float VAN_COL[4] = {0.142f, 0.438f, 0.66f, 0.882f};
static const float VAN_COL_ONLINE[4] = {0.12f, 0.37f, 0.57f, 0.78f};   // the online tab: its last title is wide

static const ImU32 VAN_TAB = IM_COL32(240, 240, 240, 255);      // the page's tabs (ДРУЗЬЯ | ПОСЛЕДНЯЯ ИГРА)
static const ImU32 VAN_TAB_OFF = IM_COL32(150, 150, 150, 255);
static const ImU32 VAN_WIN = IM_COL32(120, 200, 100, 255);

static void van_text(ImDrawList *dl, float x, float y, ImU32 col, const char *s) { dl->AddText(ImVec2(floorf(x + 0.5f), floorf(y + 0.5f)), col, s); }

// cut at the right edge of its column
static void van_cell(ImDrawList *dl, float x, float y, float w, ImU32 col, const char *s) {
    ImVec4 clip(x, y - 2, x + w, y + ImGui::GetTextLineHeight() + 2);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(floorf(x + 0.5f), floorf(y + 0.5f)), col, s, nullptr, 0, &clip);
}

static void open_profile(const char *id) {
    if (!steam_id_ok(id)) return;
    char url[64];
    snprintf(url, sizeof url, "steam://url/SteamIDPage/%s", id);
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

// a text of the rating server cut into lines and tab-separated fields, in place
struct Doc { static const int MAXL = 160; char text[32768]; int n; char *f[MAXL][12]; int nf[MAXL]; int state; };

static void doc_get(Doc &d, int which) {
    online_want(which);
    online_doc(which, d.text, sizeof d.text, &d.state);
    d.n = 0;
    for (char *line = d.text; *line && d.n < Doc::MAXL;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        int k = 0;
        d.f[d.n][k++] = line;
        for (char *q = line; *q && k < 12; q++) if (*q == '\t') { *q = 0; d.f[d.n][k++] = q + 1; }
        d.nf[d.n++] = k;
        if (!end) break;
        line = end + 1;
    }
}

static const char *NATIONS[5][2] = {{"ger", "Рейх"}, {"fin", "Финляндия"}, {"eng", "Британия"}, {"usa", "США"}, {"rus", "СССР"}};
static const char *nation_name(const char *n) {
    for (auto &x : NATIONS) if (!strcmp(x[0], n)) return x[1];
    return n;
}
static const char *mode_name(const char *m) {
    return !strcmp(m, "battle_zones") ? "Зоны боевых действий" : !strcmp(m, "ammunition") ? "Превосходство" : m;
}
static void mmr_text(char *out, size_t n, const char *mmr, int games) {
    if (games <= 0) snprintf(out, n, "-");
    else if (games < 10) snprintf(out, n, "%s (калибровка %d/10)", mmr, games);
    else snprintf(out, n, "%s", mmr);
}

// what every tab draws in: the block's place, its line height, rows and scale
struct Pane { ImDrawList *dl; ImVec2 p0, size; float lh, row, sx, sy; };

static int g_pvp_tab;                   // 0 online, 1 my stats, 2 leaders
static int g_board_team = 1;            // leaders: 0 one on one, 1 teams

// a row that lights up under the mouse; true when clicked
static bool van_row(const Pane &p, ImDrawList *dl, ImVec2 r0, float h, bool *hot) {
    ImGui::SetCursorScreenPos(r0);
    bool click = ImGui::InvisibleButton("##row", ImVec2(p.size.x - 10 * p.sy, h));
    *hot = ImGui::IsItemHovered();
    if (*hot) dl->AddRectFilled(r0, ImVec2(r0.x + p.size.x - 10 * p.sy, r0.y + h), VAN_HOVER_BG);
    return click;
}

static void van_empty(const Pane &p, ImDrawList *dl, ImVec2 at, const char *msg) {
    float tw = ImGui::CalcTextSize(msg).x;
    van_text(dl, at.x + (p.size.x - tw) * 0.5f, at.y + p.row * 1.5f, VAN_TITLE, msg);
}

static void tab_online(const Pane &p, float top, float bottom) {
    static OnlinePlayer list[ONLINE_MAX];
    static Doc rd;
    int state = 0, n = online_copy(list, ONLINE_MAX, &state);
    const char *my = online_my_id();
    char ids[16 * 18 + 1] = "";
    for (int i = 0; i < n && i < 16; i++) { if (i) strcat(ids, ","); strcat(ids, list[i].id); }
    online_doc_ids(DOC_RATINGS, ids);
    doc_get(rd, DOC_RATINGS);
    float col[4];
    for (int i = 0; i < 4; i++) col[i] = p.p0.x + VAN_COL_ONLINE[i] * p.size.x;
    float ty = top + (p.row - p.lh) * 0.5f;
    van_text(p.dl, col[0], ty, VAN_TITLE, "Игрок");
    van_text(p.dl, col[1], ty, VAN_TITLE, "Версия баланса");
    van_text(p.dl, col[2], ty, VAN_TITLE, "Совместимость");
    van_text(p.dl, col[3], ty, VAN_TITLE, "MMR 1\xC3\x97" "1 / ком.");
    ImGui::SetCursorScreenPos(ImVec2(p.p0.x, top + p.row));
    ImGui::BeginChild("##online", ImVec2(p.size.x, bottom - top - p.row), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::SetWindowFontScale(p.sy);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    if (state != 1 || n == 0) van_empty(p, dl, o, state == 0 ? "Загрузка..." : state == 2 && !n ? "Сервер мода не отвечает" : "Сейчас никого нет");
    for (int i = 0; i < n; i++) {
        ImGui::PushID(i);
        ImVec2 r0(o.x, o.y + i * p.row);
        bool hot, click = van_row(p, dl, r0, p.row, &hot);
        bool self = !strcmp(list[i].id, my), same = !strcmp(list[i].version, g_balance_tag);
        float yy = r0.y + (p.row - p.lh) * 0.5f;
        ImU32 c = hot ? VAN_HOVER : VAN_TEXT;
        if (self) dl->AddRectFilled(ImVec2(r0.x + 14 * p.sx, yy + p.lh * 0.3f), ImVec2(r0.x + 20 * p.sx, yy + p.lh * 0.3f + 6 * p.sy), ACCENT);
        van_text(dl, col[0], yy, c, list[i].name);
        van_text(dl, col[1], yy, same ? c : VAN_WARN, list[i].version);
        van_text(dl, col[2], yy, self ? VAN_TITLE : same ? c : VAN_WARN, self ? "это вы" : same ? "можно играть компетитив" : "другая версия");
        const char *m1 = "-", *mt = "-";
        for (int k = 0; k < rd.n; k++)
            if (rd.nf[k] >= 3 && !strcmp(rd.f[k][0], list[i].id)) (strcmp(rd.f[k][1], "1v1") ? mt : m1) = rd.f[k][2];
        char mm[48];
        snprintf(mm, sizeof mm, "%s / %s", m1, mt);
        van_text(dl, col[3], yy, c, mm);
        if (click && list[i].verified) open_profile(list[i].id);
        ImGui::PopID();
    }
    if (n) ImGui::Dummy(ImVec2(1, 0));
    ImGui::EndChild();
}

static void tab_stats(const Pane &p, float top, float bottom) {
    static Doc d;
    doc_get(d, DOC_PLAYER);
    ImDrawList *dl = p.dl;
    float x0 = p.p0.x + 0.03f * p.size.x, y = top + (p.row - p.lh) * 0.5f;
    if (g_steam_verified != 1) {            // only for players signed in with Steam: the stats are theirs then
        van_empty(p, dl, ImVec2(p.p0.x, top), "Личная статистика - после входа через Steam");
        const char *b = "ВОЙТИ ЧЕРЕЗ STEAM";
        float tw = ImGui::CalcTextSize(b).x, bx = p.p0.x + (p.size.x - tw) * 0.5f, by = top + p.row * 2.6f;
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        if (ImGui::InvisibleButton("##login", ImVec2(tw, p.lh))) { g_login_win = true; online_steam_login(); g_login_opened = true; }
        van_text(dl, bx, by, ImGui::IsItemHovered() ? VAN_HOVER : ACCENT, b);
        return;
    }
    if (!online_my_id()[0] || d.state != 1) {
        van_empty(p, dl, ImVec2(p.p0.x, top), d.state == 2 ? "Сервер мода не отвечает" : "Загрузка...");
        return;
    }
    // the two ladders
    const char *lad[2] = {"1v1", "team"}, *lad_name[2] = {"1 НА 1", "КОМАНДЫ (2\xC3\x97" "2, 3\xC3\x97" "3)"};
    for (int l = 0; l < 2; l++) {
        float x = x0 + l * 0.5f * p.size.x;
        van_text(dl, x, y, VAN_TITLE, lad_name[l]);
        char line[128] = "Ещё не играл";
        for (int k = 0; k < d.n; k++)
            if (d.nf[k] >= 5 && !strcmp(d.f[k][0], "L") && !strcmp(d.f[k][1], lad[l])) {
                int g = atoi(d.f[k][3]), w = atoi(d.f[k][4]);
                char mm[64];
                mmr_text(mm, sizeof mm, d.f[k][2], g);
                snprintf(line, sizeof line, "MMR %s  \xC2\xB7  игр %d  \xC2\xB7  побед %d%%", mm, g, g ? w * 100 / g : 0);
            }
        van_text(dl, x, y + p.lh * 1.3f, VAN_TEXT, line);
    }
    // by nation
    float ny = y + p.lh * 3.2f, nrow = p.lh * 1.45f;
    float nc[3] = {x0, x0 + 0.22f * p.size.x, x0 + 0.58f * p.size.x};
    van_text(dl, nc[0], ny, VAN_TITLE, "Нация");
    van_text(dl, nc[1], ny, VAN_TITLE, "1 на 1: MMR \xC2\xB7 игр \xC2\xB7 побед");
    van_text(dl, nc[2], ny, VAN_TITLE, "Команды: MMR \xC2\xB7 игр \xC2\xB7 побед");
    for (int i = 0; i < 5; i++) {
        float yy = ny + (i + 1) * nrow;
        van_text(dl, nc[0], yy, VAN_TEXT, NATIONS[i][1]);
        for (int l = 0; l < 2; l++) {
            char cell[96] = "-";
            for (int k = 0; k < d.n; k++)
                if (d.nf[k] >= 6 && !strcmp(d.f[k][0], "N") && !strcmp(d.f[k][1], lad[l]) && !strcmp(d.f[k][2], NATIONS[i][0])) {
                    int g = atoi(d.f[k][4]), w = atoi(d.f[k][5]);
                    snprintf(cell, sizeof cell, "%s  \xC2\xB7  %d  \xC2\xB7  %d%%", d.f[k][3], g, g ? w * 100 / g : 0);
                }
            van_text(dl, nc[1 + l], yy, VAN_TEXT, cell);
        }
    }
    // history
    float hy = ny + 6 * nrow + p.lh * 0.6f;
    static const float HC[7] = {0.03f, 0.13f, 0.37f, 0.43f, 0.55f, 0.67f, 0.83f};
    static const char *HT[7] = {"Дата", "Режим \xC2\xB7 карта", "Бой", "Нация", "Итог", "Союзники", "Противники"};
    float hc[8];
    for (int i = 0; i < 7; i++) hc[i] = p.p0.x + HC[i] * p.size.x;
    hc[7] = p.p0.x + 0.99f * p.size.x;
    for (int i = 0; i < 7; i++) van_text(dl, hc[i], hy, VAN_TITLE, HT[i]);
    ImGui::SetCursorScreenPos(ImVec2(p.p0.x, hy + p.lh * 1.4f));
    ImGui::BeginChild("##history", ImVec2(p.size.x, bottom - hy - p.lh * 1.4f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::SetWindowFontScale(p.sy);
    ImDrawList *hl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float hrow = p.row * 0.85f;
    int shown = 0;
    for (int k = 0; k < d.n; k++) {
        if (d.nf[k] < 12 || strcmp(d.f[k][0], "M")) continue;
        char **f = d.f[k];
        ImGui::PushID(k);
        ImVec2 r0(o.x, o.y + shown++ * hrow);
        bool hot;
        van_row(p, hl, r0, hrow, &hot);
        float yy = r0.y + (hrow - p.lh) * 0.5f;
        ImU32 c = hot ? VAN_HOVER : VAN_TEXT;
        char date[32] = "", what[160], size[16], result[48];
        time_t t = (time_t)atoll(f[2]);
        struct tm lt;
        if (!localtime_s(&lt, &t)) strftime(date, sizeof date, "%d.%m %H:%M", &lt);
        snprintf(what, sizeof what, "%s \xC2\xB7 %s", mode_name(f[3]), f[4]);
        snprintf(size, sizeof size, "%s\xC3\x97%s", f[5], f[5]);
        bool won = atoi(f[7]) != 0, rated = !strcmp(f[9], "rated");
        int delta = atoi(f[8]);
        if (rated) snprintf(result, sizeof result, "%s %+d", won ? "Победа" : "Поражение", delta);
        else snprintf(result, sizeof result, "%s (спорный)", won ? "Победа" : "Поражение");
        van_cell(hl, hc[0], yy, hc[1] - hc[0] - 6, c, date);
        van_cell(hl, hc[1], yy, hc[2] - hc[1] - 6, c, what);
        van_cell(hl, hc[2], yy, hc[3] - hc[2] - 6, c, size);
        van_cell(hl, hc[3], yy, hc[4] - hc[3] - 6, c, nation_name(f[6]));
        van_cell(hl, hc[4], yy, hc[5] - hc[4] - 6, !rated ? VAN_TITLE : won ? VAN_WIN : VAN_WARN, result);
        van_cell(hl, hc[5], yy, hc[6] - hc[5] - 6, c, f[10][0] ? f[10] : "-");
        van_cell(hl, hc[6], yy, hc[7] - hc[6] - 6, c, f[11]);
        if (hot && (f[10][0] || f[11][0])) ImGui::SetTooltip("Союзники: %s\nПротивники: %s", f[10][0] ? f[10] : "-", f[11]);
        ImGui::PopID();
    }
    if (!shown)
        van_text(hl, o.x + 0.03f * p.size.x, o.y + p.lh * 0.5f, VAN_TITLE,
                 "Рейтинговых игр пока нет. Засчитываются: Ось против Союзников, «Зоны боевых действий» или «Превосходство», "
                 "равные команды, от 3 минут.");
    else ImGui::Dummy(ImVec2(1, 0));
    ImGui::EndChild();
}

// ---- Steam avatars (32x32) by SteamID, through the game's own steam_api64.dll ----------------------------------
// Steam has them at once for friends and players met lately; anyone else is asked for (RequestUserInformation)
// and looked at again a little later. Made into textures on the render thread, kept for the session.
struct Avatar { uint64_t id; ID3D11ShaderResourceView *srv; DWORD next; int tries; };
static Avatar g_avatars[256];
static int g_navatars;

static ID3D11ShaderResourceView *steam_avatar(const char *id_text) {
    uint64_t id = _strtoui64(id_text, nullptr, 10);
    if (!id || !g_dev) return nullptr;
    Avatar *a = nullptr;
    for (int i = 0; i < g_navatars && !a; i++) if (g_avatars[i].id == id) a = &g_avatars[i];
    if (!a) {
        if (g_navatars >= 256) return nullptr;
        a = &g_avatars[g_navatars++];
        *a = Avatar{id, nullptr, 0, 0};
    }
    DWORD now = GetTickCount();
    if (a->srv || a->tries >= 20 || (a->next && (int)(now - a->next) < 0)) return a->srv;
    a->tries++;
    a->next = now + 1500;
    HMODULE h = GetModuleHandleA("steam_api64.dll");
    if (!h) return nullptr;
    auto friends_if = (void *(*)())GetProcAddress(h, "SteamAPI_SteamFriends_v017");
    auto utils_if = (void *(*)())GetProcAddress(h, "SteamAPI_SteamUtils_v010");
    auto small = (int (*)(void *, uint64_t))GetProcAddress(h, "SteamAPI_ISteamFriends_GetSmallFriendAvatar");
    auto request = (bool (*)(void *, uint64_t, bool))GetProcAddress(h, "SteamAPI_ISteamFriends_RequestUserInformation");
    auto size = (bool (*)(void *, int, uint32_t *, uint32_t *))GetProcAddress(h, "SteamAPI_ISteamUtils_GetImageSize");
    auto rgba = (bool (*)(void *, int, uint8_t *, int))GetProcAddress(h, "SteamAPI_ISteamUtils_GetImageRGBA");
    void *fr = friends_if ? friends_if() : nullptr, *ut = utils_if ? utils_if() : nullptr;
    if (!fr || !ut || !small || !request || !size || !rgba) return nullptr;
    int img = small(fr, id);
    if (img <= 0) { request(fr, id, false); return nullptr; }     // not there yet: Steam fetches it
    uint32_t w = 0, hh = 0;
    if (!size(ut, img, &w, &hh) || !w || !hh || w > 256 || hh > 256) return nullptr;
    static uint8_t px[256 * 256 * 4];
    if (!rgba(ut, img, px, (int)(w * hh * 4))) return nullptr;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = hh; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {px, w * 4, 0};
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&td, &sd, &tex))) return nullptr;
    g_dev->CreateShaderResourceView(tex, nullptr, &a->srv);
    tex->Release();
    return a->srv;
}

static void tab_leaders(const Pane &p, float top, float bottom) {
    static Doc d;
    doc_get(d, g_board_team ? DOC_BOARD_TEAM : DOC_BOARD_1V1);
    ImDrawList *dl = p.dl;
    float x0 = p.p0.x + 0.03f * p.size.x, y = top + (p.row - p.lh) * 0.5f;
    // which ladder, as the page's tabs do it
    const char *names[2] = {"1 на 1", "Команды"};
    float x = x0;
    for (int i = 0; i < 2; i++) {
        float tw = ImGui::CalcTextSize(names[i]).x;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(100 + i);
        if (ImGui::InvisibleButton("##lad", ImVec2(tw, p.lh))) g_board_team = i;
        bool hot = ImGui::IsItemHovered();
        ImGui::PopID();
        van_text(dl, x, y, g_board_team == i ? VAN_TAB : hot ? VAN_HOVER : VAN_TAB_OFF, names[i]);
        x += tw + 14 * p.sx;
        if (!i) { van_text(dl, x - 9 * p.sx, y, VAN_TAB_OFF, "|"); }
    }
    static const float LC[5] = {0.03f, 0.10f, 0.44f, 0.70f, 0.82f};     // MMR wide: "1674 (калибровка 5/10)"
    static const char *LT[5] = {"#", "Игрок", "MMR", "Игр", "Побед"};
    float lc[5];
    for (int i = 0; i < 5; i++) lc[i] = p.p0.x + LC[i] * p.size.x;
    float hy = top + p.row;
    for (int i = 0; i < 5; i++) van_text(dl, lc[i], hy + (p.row - p.lh) * 0.5f, VAN_TITLE, LT[i]);
    ImGui::SetCursorScreenPos(ImVec2(p.p0.x, hy + p.row));
    ImGui::BeginChild("##board", ImVec2(p.size.x, bottom - hy - p.row), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::SetWindowFontScale(p.sy);
    ImDrawList *bl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    const char *my = online_my_id();
    int shown = 0;
    for (int k = 0; k < d.n; k++) {
        if (d.nf[k] < 6) continue;
        char **f = d.f[k];
        ImGui::PushID(k);
        ImVec2 r0(o.x, o.y + shown++ * p.row);
        bool hot, click = van_row(p, bl, r0, p.row, &hot);
        float yy = r0.y + (p.row - p.lh) * 0.5f;
        bool self = !strcmp(f[1], my);
        ImU32 c = hot ? VAN_HOVER : self ? ACCENT : VAN_TEXT;
        int g = atoi(f[4]), w = atoi(f[5]);
        char mm[64], wr[16];
        mmr_text(mm, sizeof mm, f[3], g);
        snprintf(wr, sizeof wr, "%d%%", g ? w * 100 / g : 0);
        van_text(bl, lc[0], yy, VAN_TITLE, f[0]);
        float av = p.row * 0.8f;                 // the player's Steam avatar, left of the name
        if (ID3D11ShaderResourceView *img = steam_avatar(f[1]))
            bl->AddImage((ImTextureID)(intptr_t)img, ImVec2(lc[1], r0.y + (p.row - av) * 0.5f), ImVec2(lc[1] + av, r0.y + (p.row + av) * 0.5f));
        van_cell(bl, lc[1] + av + 8 * p.sx, yy, lc[2] - lc[1] - av - 16 * p.sx, c, f[2]);
        van_text(bl, lc[2], yy, c, mm);
        van_text(bl, lc[3], yy, c, f[4]);
        van_text(bl, lc[4], yy, c, wr);
        if (click) open_profile(f[1]);
        ImGui::PopID();
    }
    if (!shown) van_empty(p, bl, o, d.state == 0 ? "Загрузка..." : d.state == 2 ? "Сервер мода не отвечает" : "Пока никто не сыграл рейтинговых игр");
    else ImGui::Dummy(ImVec2(1, 0));
    ImGui::EndChild();
}

// ---- MMR under each name in the lobby's player list (pvp.c lobby_rows) ------------------------------------
static const ImU32 VAN_ITEM = IM_COL32(176, 151, 121, 255);     // the skin's listbox item text

static void lobby_overlay() {
    static LobbyRow rows[16];
    static Doc d;
    int n = lobby_rows(rows, 16);
    if (!n || g_chat_tab) return;           // our global chat is over the panel
    char ids[16 * 18 + 1] = "";
    for (int i = 0; i < n; i++) { if (i) strcat(ids, ","); strcat(ids, rows[i].id); }
    online_doc_ids(DOC_LOBBY, ids);
    doc_get(d, DOC_LOBBY);
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = g_game_font_small ? g_game_font_small : ImGui::GetFont();
    for (int i = 0; i < n; i++) {
        const LobbyRow &r = rows[i];
        float sx = io.DisplaySize.x / r.root_w, sy = io.DisplaySize.y / r.root_h, size = font->FontSize * sy;
        const char *m1 = nullptr, *mt = nullptr;
        for (int k = 0; k < d.n; k++)
            if (d.nf[k] >= 3 && !strcmp(d.f[k][0], r.id)) (strcmp(d.f[k][1], "1v1") ? mt : m1) = d.f[k][2];
        // the lobby's own ladder: two players are a 1x1, more a team game; the other one if that is all there is
        const char *m = n == 2 ? (m1 ? m1 : mt) : (mt ? mt : m1);
        char text[32];
        snprintf(text, sizeof text, "MMR %s", m ? m : d.state == 1 ? "-" : "...");
        // under the name, in the room pvp.c makes there (r is the name's own place)
        ImVec2 at(floorf(r.x * sx + 0.5f), floorf((r.y + r.h) * sy + 0.5f));
        dl->AddText(font, size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), text);
        dl->AddText(font, size, at, VAN_ITEM, text);
    }
}

// ---- global chat: in the frame our layout puts over the friends on the Internet / LAN page (pvp.c) --------
// Tabs RU / ENG on top, the messages left, who is online with the mod right; a moderator gets a menu on any
// name (right click) and the list of punishments in force.
static int g_chat_ch;                  // 0 RU, 1 EN

static void chat_mod_menu(const char *id, const char *name, bool mod) {
    if (!mod || !ImGui::BeginPopup("##chatmod")) return;
    ImGui::TextDisabled("%s", name);
    ImGui::Separator();
    static const int DAYS[4] = {1, 3, 7, 30};
    for (int d : DAYS) {
        char t[48];
        snprintf(t, sizeof t, "Замутить на %d дн.", d);
        if (ImGui::Selectable(t)) chat_mod("mute", id, d);
    }
    if (ImGui::Selectable("Удалить его сообщения")) chat_mod("del", id, 0);
    if (ImGui::Selectable("Забанить в чате")) chat_mod("ban", id, 0);
    if (ImGui::Selectable("Разбанить")) chat_mod("unban", id, 0);
    ImGui::EndPopup();
}

// a name: left click the Steam profile (players signed in with Steam), right click (moderators) the menu
static void chat_name(const char *id, const char *name, ImU32 col, bool verified, bool mod, char *menu_id, char *menu_name) {
    ImGui::PushStyleColor(ImGuiCol_Text, verified ? col : VAN_TAB_OFF);
    ImGui::TextUnformatted(name);
    ImGui::PopStyleColor();
    if (verified && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (verified && ImGui::IsItemClicked(ImGuiMouseButton_Left)) open_profile(id);
    if (mod && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        snprintf(menu_id, 18, "%s", id);
        snprintf(menu_name, ONLINE_NAME, "%s", name);
        ImGui::OpenPopup("##chatmod");
    }
}

// ---- /invite: a button in the chat that takes others to the sender's lobby ---------------------------------
// The game's lobbies are Steam lobbies (SteamMatchMaking009, "+connect_lobby"): the one the player is in comes
// from steamlobby.c, and steam://joinlobby/<app>/<lobby>/<player> makes the running game join it, as accepting
// a Steam invite does. The message is "/invite <lobby id>"; the sender is the one inviting.
static uint64_t my_lobby() { return steam_lobby_now(); }

static uint32_t steam_app() {
    HMODULE h = GetModuleHandleA("steam_api64.dll");
    auto utils_if = h ? (void *(*)())GetProcAddress(h, "SteamAPI_SteamUtils_v010") : nullptr;
    auto app = h ? (uint32_t (*)(void *))GetProcAddress(h, "SteamAPI_ISteamUtils_GetAppID") : nullptr;
    void *ut = utils_if ? utils_if() : nullptr;
    uint32_t id = ut && app ? app(ut) : 0;
    return id ? id : 400750;
}

// "/invite <lobby id>": the lobby, else 0
static uint64_t invite_lobby(const char *text) {
    if (strncmp(text, "/invite ", 8)) return 0;
    const char *d = text + 8;
    size_t n = strspn(d, "0123456789");
    return n >= 15 && n <= 20 && !d[n] ? _strtoui64(d, nullptr, 10) : 0;
}

static void join_lobby(uint64_t lobby, const char *inviter) {
    if (!lobby || !steam_id_ok(inviter)) return;
    char url[96];
    snprintf(url, sizeof url, "steam://joinlobby/%u/%llu/%s", steam_app(), (unsigned long long)lobby, inviter);
    rc_log("chat: joining lobby %llu of %s", (unsigned long long)lobby, inviter);
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

static const char *CHAT_RULES[] = {
    "0. Не быть мудаком.",
    "1. Старайтесь не обсуждать других и не переходить на личности и конфликты.",
    "2. Не выкладывайте материалы жестокого, насильственного или сексуального характера.",
    "3. Ссылки удаляются, кроме discord.gg и youtube.com.",
};

// The lobby's chat panel: a GLOBAL CHAT tab after the game's, ours over the channel while it is on; a click on
// one of the game's tabs gives the panel back. Over the channel our chat lays itself out as the game's session
// chat does (messages left, the people in a sunken box right, the line to write in at the bottom) on the
// panel's own background: the texture of its frame (window1, MenuChat.xml), tiled at its size from the panel's
// corner as the game does.
static ID3D11ShaderResourceView *g_panel_tex;
static int g_panel_tex_w, g_panel_tex_h;
static ImVec4 g_panel_rect;             // the panel on the screen (pixels): where the tiles start

static unsigned char *tga_bgra(const unsigned char *b, size_t n, int *w_out, int *h_out);   // below, with the fonts

static ID3D11ShaderResourceView *panel_texture() {
    static bool tried;
    if (g_panel_tex || tried || !g_dev) return g_panel_tex;
    tried = true;
    size_t n = 0;
    unsigned char *t = pvp_pak_file("skin/game/frame/window1/client.tga", &n);
    int w = 0, h = 0;
    unsigned char *px = t ? tga_bgra(t, n, &w, &h) : nullptr;
    free(t);
    if (!px) { rc_log("ui: panel texture not loaded"); return nullptr; }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {px, (UINT)w * 4, 0};
    ID3D11Texture2D *tex = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &tex))) {
        g_dev->CreateShaderResourceView(tex, nullptr, &g_panel_tex);
        tex->Release();
        g_panel_tex_w = w; g_panel_tex_h = h;
    }
    free(px);
    return g_panel_tex;
}

// the panel's background over a part of it (pixels), the tiles lined up with the panel's corner
static void panel_background(ImDrawList *dl, ImVec2 a, ImVec2 b, float scale) {
    dl->AddRectFilled(a, b, IM_COL32(24, 27, 29, 255));
    ID3D11ShaderResourceView *t = panel_texture();
    if (!t) return;
    float tw = g_panel_tex_w * scale, th = g_panel_tex_h * scale, ox = g_panel_rect.x, oy = g_panel_rect.y;
    dl->PushClipRect(a, b, true);
    for (float y = oy + floorf((a.y - oy) / th) * th; y < b.y; y += th)
        for (float x = ox + floorf((a.x - ox) / tw) * tw; x < b.x; x += tw)
            dl->AddImage((ImTextureID)(intptr_t)t, ImVec2(x, y), ImVec2(x + tw, y + th));
    dl->PopClipRect();
}

// a box sunk into the panel, as the game's list of people in a session
static void sunken_box(ImDrawList *dl, ImVec2 a, ImVec2 b) {
    dl->AddRectFilled(a, b, IM_COL32(10, 11, 12, 140));
    dl->AddRect(a, b, IM_COL32(58, 58, 56, 255));
}

static bool lobby_chat_tab(float *x, float *y, float *w, float *h, float *rw, float *rh) {
    ChatPanel cp;
    if (g_live_match || !chat_panel(&cp)) { g_chat_tab = false; return false; }
    ImGuiIO &io = ImGui::GetIO();
    float sx = io.DisplaySize.x / cp.root_w, sy = io.DisplaySize.y / cp.root_h;
    for (int i = 0; i < cp.ntabs; i++) {
        const float *t = cp.tab[i];
        if (ImGui::IsMouseClicked(0) && ImGui::IsMouseHoveringRect(ImVec2(t[0] * sx, t[1] * sy), ImVec2((t[0] + t[2]) * sx, (t[1] + t[3]) * sy), false))
            g_chat_tab = false;
    }
    if (g_game_font) ImGui::PushFont(g_game_font);
    const char *label = "GLOBAL CHAT";
    float size = (g_game_font ? g_game_font->FontSize : ImGui::GetFontSize()) * sy;
    ImFont *font = ImGui::GetFont();
    float tab_y = cp.y, tab_h = cp.body_y - cp.y;           // the game's tabs' own strip, when they were found
    for (int i = 0; i < cp.ntabs; i++) if (cp.tab[i][3] > 0) { tab_y = cp.tab[i][1]; tab_h = cp.tab[i][3]; break; }
    float tw = font->CalcTextSizeA(size, FLT_MAX, 0, label).x, gap = 12 * sx;
    float lx = cp.tabs_r * sx + gap, ly = floorf((tab_y + tab_h * 0.5f) * sy - size * 0.5f + 1 * sy);
    g_panel_rect = ImVec4(cp.x * sx, cp.y * sy, (cp.x + cp.w) * sx, (cp.y + cp.h) * sy);
    ImGui::SetNextWindowPos(ImVec2(lx - gap, cp.y * sy), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(tw + 2 * gap, (cp.body_y - cp.y) * sy), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##goh_chat_tab", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                              ImGuiWindowFlags_NoScrollbar);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(ImVec2(lx, ly));
    if (ImGui::InvisibleButton("##gtab", ImVec2(tw, size))) g_chat_tab = true;
    bool hot = ImGui::IsItemHovered();
    float mid = (tab_y + tab_h * 0.5f) * sy, sep = floorf(lx - gap * 0.5f);
    dl->AddLine(ImVec2(sep, mid - 6 * sy), ImVec2(sep, mid + 6 * sy), IM_COL32(120, 120, 120, 255));
    dl->AddText(font, size, ImVec2(floorf(lx), ly), g_chat_tab ? IM_COL32(255, 255, 255, 255) : hot ? VAN_HOVER : IM_COL32(200, 200, 200, 255), label);
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (g_game_font) ImGui::PopFont();
    g_chat_drawn = true;            // the tab takes its clicks
    if (!g_chat_tab) return false;
    *x = cp.x; *y = cp.body_y; *w = cp.w; *h = cp.y + cp.h - cp.body_y; *rw = cp.root_w; *rh = cp.root_h;
    return true;
}

static void chat_block() {
    float x, y, w, h, rw, rh;
    g_chat_drawn = false;
    bool own_frame = chat_rect(&x, &y, &w, &h, &rw, &rh);       // the Internet page: our empty frame
    if (!own_frame && !lobby_chat_tab(&x, &y, &w, &h, &rw, &rh)) return;   // the lobby: its GLOBAL CHAT tab
    chat_want(g_chat_ch);
    static ChatMsg msgs[CHAT_MAX];
    static ChatStatus st;
    static OnlinePlayer online[ONLINE_MAX];
    int n = chat_copy(g_chat_ch, msgs, CHAT_MAX, &st), ostate = 0, on = online_copy(online, ONLINE_MAX, &ostate);
    const char *my = online_my_id();
    ImGuiIO &io = ImGui::GetIO();
    float sx = io.DisplaySize.x / rw, sy = io.DisplaySize.y / rh, pad = 12 * sy;
    ImVec2 p0(x * sx, y * sy), size(w * sx, h * sy);
    ImGui::SetNextWindowPos(p0, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8 * sy);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5 * sx, 3 * sy));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(70, 66, 60, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(110, 98, 80, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, IM_COL32(178, 135, 84, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(16, 16, 15, 230));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(22, 22, 20, 230));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(22, 22, 20, 230));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(70, 66, 60, 255));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(24, 23, 21, 245));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, VAN_HOVER_BG);
    ImGui::PushStyleColor(ImGuiCol_Text, VAN_TEXT);
    ImGui::Begin("##goh_chat", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (g_game_font) ImGui::PushFont(g_game_font);
    ImGui::SetWindowFontScale(sy);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool lobby = !own_frame;
    float lh = ImGui::GetTextLineHeight(), row = 32 * sy, m = 10 * sy;
    if (lobby) panel_background(dl, p0, ImVec2(p0.x + size.x, p0.y + size.y), sy);
    static char menu_id[18], menu_name[ONLINE_NAME];
    bool mod = st.moderator != 0;
    float input_h = lh + 12 * sy;
    // where everything goes: the Internet page's frame has its tabs on top and the people right of a line; over
    // the lobby's panel (its own tabs above) a small row for the channel, the people in a sunken box
    float box_x0 = lobby ? floorf(p0.x + size.x * 0.705f) : floorf(p0.x + size.x * 0.7f);
    ImVec2 list0, list1, msg0, msg1, in0;
    float bans_r, tabs_y;
    if (lobby) {
        ImVec2 b0(box_x0, p0.y + m), b1(p0.x + size.x - m, p0.y + size.y - m);
        sunken_box(dl, b0, b1);
        list0 = ImVec2(b0.x + 6 * sx, b0.y + 5 * sy); list1 = ImVec2(b1.x - 3 * sx, b1.y - 5 * sy);
        tabs_y = p0.y + m;
        bans_r = b0.x - m;
        msg0 = ImVec2(p0.x + m + 4 * sx, tabs_y + lh + 8 * sy);
        in0 = ImVec2(msg0.x, b1.y - input_h);
        msg1 = ImVec2(b0.x - m, in0.y - lh - 8 * sy);       // a line for notes above the one to write in
    } else {
        tabs_y = p0.y + (row - lh) * 0.5f;
        bans_r = p0.x + size.x - pad;
        float top = p0.y + row + 6 * sy, bottom = p0.y + size.y - pad, foot = input_h + lh + 14 * sy;
        list0 = ImVec2(box_x0 + 8 * sx, top); list1 = ImVec2(p0.x + size.x - pad * 0.5f, bottom);
        msg0 = ImVec2(p0.x + pad, top); msg1 = ImVec2(box_x0 - 8 * sx, bottom - foot);
        in0 = ImVec2(msg0.x, bottom - foot + 6 * sy);
        dl->AddLine(ImVec2(p0.x + pad, p0.y + row), ImVec2(p0.x + size.x - pad, p0.y + row), IM_COL32(70, 66, 60, 255));
        dl->AddLine(ImVec2(box_x0, top), ImVec2(box_x0, bottom), IM_COL32(70, 66, 60, 255));
    }

    // the channels, as the page's own tabs (ДРУЗЬЯ | ПОСЛЕДНЯЯ ИГРА)
    static const char *TABS[2] = {"ЧАТ RU", "CHAT ENG"};
    float tx = lobby ? msg0.x : p0.x + pad, ty = tabs_y;
    for (int i = 0; i < 2; i++) {
        float tw = ImGui::CalcTextSize(TABS[i]).x;
        ImGui::SetCursorScreenPos(ImVec2(tx, ty));
        ImGui::PushID(300 + i);
        if (ImGui::InvisibleButton("##ctab", ImVec2(tw, lh))) g_chat_ch = i;
        bool hot = ImGui::IsItemHovered();
        ImGui::PopID();
        van_text(dl, tx, ty, g_chat_ch == i ? VAN_TAB : hot ? VAN_HOVER : VAN_TAB_OFF, TABS[i]);
        tx += tw + 18 * sx;
        if (!i) van_text(dl, tx - 11 * sx, ty, VAN_TAB_OFF, "|");
    }
    if (mod) {          // the punishments in force, for the moderator
        char t[48];
        snprintf(t, sizeof t, "Наказания: %d", st.bans);
        float tw = ImGui::CalcTextSize(t).x, bx = bans_r - tw;
        ImGui::SetCursorScreenPos(ImVec2(bx, ty));
        if (ImGui::InvisibleButton("##bans", ImVec2(tw, lh))) ImGui::OpenPopup("##chatbans");
        van_text(dl, bx, ty, ImGui::IsItemHovered() ? VAN_HOVER : VAN_TAB_OFF, t);
        if (ImGui::BeginPopup("##chatbans")) {
            if (!st.bans) ImGui::TextDisabled("никого");
            for (int i = 0; i < st.bans; i++) {
                const ChatBan &b = st.ban[i];
                char when[48] = "навсегда";
                if (!b.ban) {
                    time_t u = (time_t)b.until;
                    struct tm tmv;
                    localtime_s(&tmv, &u);
                    strftime(when, sizeof when, "мут до %d.%m %H:%M", &tmv);
                }
                ImGui::PushID(i);
                ImGui::Text("%s  ·  %s", b.name, b.ban ? "бан" : when);
                ImGui::SameLine();
                if (ImGui::SmallButton("Разбанить")) chat_mod("unban", b.sid, 0);
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
    }

    // who is online, right
    ImGui::SetCursorScreenPos(list0);
    ImGui::BeginChild("##chat_online", ImVec2(list1.x - list0.x, list1.y - list0.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::SetWindowFontScale(sy);
    {
        ImGui::PushStyleColor(ImGuiCol_Text, VAN_TITLE);
        ImGui::Text("Онлайн: %d", on);
        ImGui::PopStyleColor();
        // MMR under each name (1x1 and teams), for players signed in with Steam: the first 16 of the list
        static Doc rd;
        char ids[16 * 18 + 1] = "";
        for (int i = 0, k = 0; i < on && k < 16; i++)
            if (online[i].verified) { if (k++) strcat(ids, ","); strcat(ids, online[i].id); }
        online_doc_ids(DOC_CHAT, ids);
        doc_get(rd, DOC_CHAT);
        ImFont *small = g_game_font_small ? g_game_font_small : ImGui::GetFont();
        float small_size = small->FontSize * sy, av = lh + small_size + 2 * sy;
        for (int i = 0; i < on; i++) {
            ImGui::PushID(i);
            ImVec2 c = ImGui::GetCursorScreenPos();
            ID3D11ShaderResourceView *img = online[i].verified ? steam_avatar(online[i].id) : nullptr;
            if (img) ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)img, c, ImVec2(c.x + av, c.y + av));
            else ImGui::GetWindowDrawList()->AddRect(c, ImVec2(c.x + av, c.y + av), IM_COL32(70, 66, 60, 255));
            ImGui::SetCursorScreenPos(ImVec2(c.x + av + 6 * sx, c.y + 1 * sy));
            chat_name(online[i].id, online[i].name, !strcmp(online[i].id, my) ? ACCENT : VAN_TEXT, online[i].verified, mod, menu_id, menu_name);
            if (online[i].verified) {
                const char *m1 = "-", *mt = "-";
                for (int k = 0; k < rd.n; k++)
                    if (rd.nf[k] >= 3 && !strcmp(rd.f[k][0], online[i].id)) (strcmp(rd.f[k][1], "1v1") ? mt : m1) = rd.f[k][2];
                char mm[48];
                snprintf(mm, sizeof mm, "%s / %s", m1, mt);
                ImVec2 at(floorf(c.x + av + 6 * sx), floorf(c.y + lh + 1 * sy));
                ImGui::GetWindowDrawList()->AddText(small, small_size, at, VAN_ITEM, mm);
                if (ImGui::IsMouseHoveringRect(at, ImVec2(at.x + small->CalcTextSizeA(small_size, FLT_MAX, 0, mm).x, at.y + small_size)))
                    ImGui::SetTooltip("MMR: 1\xC3\x97" "1 / команды");
            }
            ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + av + 4 * sy));
            ImGui::Dummy(ImVec2(1, 0));
            ImGui::PopID();
        }
        chat_mod_menu(menu_id, menu_name, mod);
    }
    ImGui::EndChild();

    // the messages
    float mx = msg0.x, mw = msg1.x - msg0.x;
    ImGui::SetCursorScreenPos(msg0);
    ImGui::BeginChild("##chat_msgs", ImVec2(mw, msg1.y - msg0.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::SetWindowFontScale(sy);
    {
        static long long last_id[2];
        bool at_end = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
        if (st.banned) ImGui::TextColored(ImColor(VAN_WARN), "Вы забанены в глобальном чате.");
        else if (st.state == 0 && !n) ImGui::TextColored(ImColor(VAN_TITLE), "Загрузка...");
        else if (st.state == 2 && !n) ImGui::TextColored(ImColor(VAN_WARN), "Сервер мода не отвечает");
        if (!st.banned) {           // the rules, at the top: only here, every game
            ImGui::TextColored(ImColor(ACCENT), "Правила чата");
            ImGui::PushStyleColor(ImGuiCol_Text, VAN_TITLE);
            for (const char *r : CHAT_RULES) ImGui::TextWrapped("%s", r);
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(1, 4 * sy));
        }
        if (!st.banned && !n && st.state == 1) ImGui::TextColored(ImColor(VAN_TITLE), "Пока тихо. Сообщения стираются каждые два часа.");
        for (int i = 0; i < n; i++) {
            const ChatMsg &m = msgs[i];
            ImGui::PushID((int)m.id);
            time_t t = (time_t)(m.at_ms / 1000);
            struct tm tmv;
            localtime_s(&tmv, &t);
            char hm[8];
            strftime(hm, sizeof hm, "%H:%M", &tmv);
            ImGui::TextColored(ImColor(VAN_TAB_OFF), "%s", hm);
            ImGui::SameLine();
            char nm[ONLINE_NAME + 2];
            snprintf(nm, sizeof nm, "%s:", m.name);
            chat_name(m.sid, nm, !strcmp(m.sid, my) ? ACCENT : VAN_HOVER, m.verified, mod, menu_id, menu_name);
            if (mod && !strcmp(menu_id, m.sid)) snprintf(menu_name, sizeof menu_name, "%s", m.name);
            ImGui::SameLine();
            if (uint64_t lobby = invite_lobby(m.text)) {       // an invite: a button to the sender's lobby
                if (!strcmp(m.sid, my)) ImGui::TextColored(ImColor(VAN_TAB_OFF), "приглашает в своё лобби");
                else if (ImGui::SmallButton("Войти в лобби")) join_lobby(lobby, m.sid);
            } else
                ImGui::TextWrapped("%s", m.text);
            ImGui::PopID();
        }
        chat_mod_menu(menu_id, menu_name, mod);
        long long newest = n ? msgs[n - 1].id : 0;
        if (newest != last_id[g_chat_ch] && (at_end || !last_id[g_chat_ch])) ImGui::SetScrollHereY(1.0f);
        last_id[g_chat_ch] = newest;
    }
    ImGui::EndChild();

    // writing
    static char text[CHAT_TEXT];
    float iy = in0.y;
    ImGui::SetCursorScreenPos(ImVec2(mx, iy));
    ImGui::SetNextItemWidth(mw);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6 * sx, 6 * sy));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
    bool can = !st.banned && !st.muted_until && g_online_consent == 1;
    ImGui::BeginDisabled(!can);
    bool enter = ImGui::InputTextWithHint("##chat_in", can ? "Сообщение, Enter - отправить. /invite - позвать в своё лобби" : "", text, 300 * 3 + 1,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::EndDisabled();
    ImGui::PopStyleVar(2);
    static char local_note[160];
    if (enter) {
        if (!strcmp(text, "/invite") || !strcmp(text, "/инвайт")) {      // the player's own lobby, put in by us
            uint64_t lobby = my_lobby();
            rc_log("chat: /invite, lobby %llu", (unsigned long long)lobby);
            if (lobby) { snprintf(text, sizeof text, "/invite %llu", (unsigned long long)lobby); local_note[0] = 0; }
            else { snprintf(local_note, sizeof local_note, "Не вижу вашего лобби: зайдите в лобби игры и повторите /invite"); text[0] = 0; }
        } else
            local_note[0] = 0;
        if (text[0]) chat_send(g_chat_ch, text);
        text[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);         // stay in the line for the next one
    }
    char note[128] = "";
    if (g_online_consent != 1) snprintf(note, sizeof note, "Чтобы писать, включите \"Показывать меня\" во вкладке PVP");
    else if (st.banned) snprintf(note, sizeof note, "Вы забанены в чате");
    else if (st.muted_until) {
        time_t u = (time_t)st.muted_until;
        struct tm tmv;
        localtime_s(&tmv, &u);
        strftime(note, sizeof note, "Вам запрещено писать до %d.%m %H:%M", &tmv);
    } else if (local_note[0]) snprintf(note, sizeof note, "%s", local_note);
    else if (st.sent[0]) snprintf(note, sizeof note, "%s", st.sent);
    else if (g_steam_verified != 1) snprintf(note, sizeof note, "Вы пишете как Anonymous: войдите через Steam (вкладка PVP > Моя статистика)");
    if (note[0]) {
        ImVec4 clip(mx, 0, mx + mw, io.DisplaySize.y);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(floorf(mx), floorf(lobby ? iy - lh - 4 * sy : iy + input_h + 4 * sy)), VAN_WARN, note,
                    nullptr, 0, &clip);
    }

    ImGui::SetWindowFontScale(1);
    if (g_game_font) ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleColor(11);
    ImGui::PopStyleVar(4);
    g_chat_drawn = true;
}

static void pvp_block() {
    float x, y, w, h, head, rw, rh;
    g_pvp_drawn = false;
    if (!g_pvp_on || !pvp_rect(&x, &y, &w, &h, &head, &rw, &rh)) return;
    ImGuiIO &io = ImGui::GetIO();
    float sx = io.DisplaySize.x / rw, sy = io.DisplaySize.y / rh;
    ImVec2 p0(x * sx, y * sy), size(w * sx, h * sy);
    ImGui::SetNextWindowPos(p0, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8 * sy);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(70, 66, 60, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(110, 98, 80, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, IM_COL32(178, 135, 84, 255));
    ImGui::Begin("##goh_pvp", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (g_game_font) ImGui::PushFont(g_game_font);
    ImGui::SetWindowFontScale(sy);
    Pane p = {ImGui::GetWindowDrawList(), p0, size, ImGui::GetTextLineHeight(), 32 * sy, sx, sy};
    ImDrawList *dl = p.dl;
    float lh = p.lh, row = p.row, top = head * sy;
    float col0 = p0.x + VAN_COL[0] * size.x;

    // the tabs, where the page has its column titles
    static const char *TABS[3] = {"ОНЛАЙН", "МОЯ СТАТИСТИКА", "ЛИДЕРЫ"};
    float tx = p0.x + 0.03f * size.x, ty = p0.y + (top > lh ? (top - lh) * 0.5f : 0);
    for (int i = 0; i < 3; i++) {
        float tw = ImGui::CalcTextSize(TABS[i]).x;
        ImGui::SetCursorScreenPos(ImVec2(tx, ty));
        ImGui::PushID(200 + i);
        if (ImGui::InvisibleButton("##tab", ImVec2(tw, lh))) g_pvp_tab = i;
        bool hot = ImGui::IsItemHovered();
        ImGui::PopID();
        van_text(dl, tx, ty, g_pvp_tab == i ? VAN_TAB : hot ? VAN_HOVER : VAN_TAB_OFF, TABS[i]);
        tx += tw + 18 * sx;
        if (i < 2) van_text(dl, tx - 11 * sx, ty, VAN_TAB_OFF, "|");
    }

    float foot = row, content = p0.y + top, bottom = p0.y + size.y - foot;
    if (g_pvp_tab == 0) tab_online(p, content, bottom);
    else if (g_pvp_tab == 1) tab_stats(p, content, bottom);
    else tab_leaders(p, content, bottom);

    // bottom line: the server, our consent
    int state = 0;
    float fy = p0.y + size.y - foot + (foot - lh) * 0.5f;
    char line[160];
    static OnlinePlayer all[ONLINE_MAX];
    int online_n = online_copy(all, ONLINE_MAX, &state);
    if (g_online_held) snprintf(line, sizeof line, "Ваш SteamID закреплён за другим ключом: напишите автору мода");
    else snprintf(line, sizeof line, state == 2 ? "С модом онлайн: %d  \xC2\xB7  сервер не отвечает" : "С модом онлайн: %d", online_n);
    van_text(dl, col0, fy, state == 2 || g_online_held ? VAN_WARN : VAN_TITLE, line);
    const char *me_txt = "Показывать меня и засчитывать мои матчи";
    float box = lh * 0.7f, mw = ImGui::CalcTextSize(me_txt).x;
    ImVec2 b0(p0.x + VAN_COL[2] * size.x, fy + (lh - box) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(b0.x, fy));
    bool me = g_online_consent == 1;
    if (ImGui::InvisibleButton("##me", ImVec2(box + 8 * sx + mw, lh))) online_set_consent(!me);
    bool hot = ImGui::IsItemHovered();
    if (hot) ImGui::SetTooltip("%s", CONSENT_TEXT);
    dl->AddRect(b0, ImVec2(b0.x + box, b0.y + box), hot ? VAN_HOVER : VAN_TITLE);
    if (me) dl->AddRectFilled(ImVec2(b0.x + 3 * sy, b0.y + 3 * sy), ImVec2(b0.x + box - 3 * sy, b0.y + box - 3 * sy), ACCENT);
    van_text(dl, b0.x + box + 8 * sx, fy, hot ? VAN_HOVER : VAN_TITLE, me_txt);

    ImGui::SetWindowFontScale(1);
    if (g_game_font) ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);
    g_pvp_drawn = true;
}

// ---- the game's own font (a BMFont: .fnt + a 32-bit RLE .tga in interface.pak), for blocks set in its pages ----
// Glyphs are packed into the texture's channels (each glyph's chnl: 1 blue, 2 green, 4 red, 8 alpha). They go
// into ImGui's atlas as custom glyphs, their pixels copied in once the atlas is built.
struct GameGlyph { int rect, x, y, w, h, chnl, tex; };
static unsigned char *g_font_tex[4];        // each font's texture, BGRA, top row first, until the atlas has them
static int g_font_tex_w[4], g_font_texs;
static ImVector<GameGlyph> g_game_glyphs;

static unsigned char *tga_bgra(const unsigned char *b, size_t n, int *w_out, int *h_out) {
    if (n < 18 || (b[2] != 2 && b[2] != 10) || b[16] != 32 || b[1]) return nullptr;
    int w = b[12] | b[13] << 8, h = b[14] | b[15] << 8;
    size_t need = (size_t)w * h * 4, p = 18 + b[0], got = 0;
    unsigned char *out = (unsigned char *)malloc(need);
    if (!out) return nullptr;
    while (got < need) {
        if (b[2] == 2) { if (p + need > n) break; memcpy(out, b + p, need); got = need; break; }
        if (p >= n) break;
        int c = b[p++], k = (c & 0x7f) + 1;
        if (c & 0x80) { if (p + 4 > n) break; for (int i = 0; i < k && got < need; i++, got += 4) memcpy(out + got, b + p, 4); p += 4; }
        else { size_t m = (size_t)k * 4; if (p + m > n || got + m > need) break; memcpy(out + got, b + p, m); got += m; p += m; }
    }
    if (got < need) { free(out); return nullptr; }
    if (!(b[17] & 0x20))                    // bottom row first: turn it over
        for (int y = 0; y < h / 2; y++)
            for (int x = 0; x < w * 4; x++) { unsigned char t = out[y * w * 4 + x]; out[y * w * 4 + x] = out[(h - 1 - y) * w * 4 + x]; out[(h - 1 - y) * w * 4 + x] = t; }
    *w_out = w; *h_out = h;
    return out;
}

static ImFont *add_game_font(ImFontAtlas *atlas, const char *name) {
    char path[64];
    size_t fn = 0, tn = 0;
    snprintf(path, sizeof path, "font/%s.fnt", name);
    unsigned char *f = pvp_pak_file(path, &fn);
    if (!f || fn < 4 || memcmp(f, "BMF\x03", 4)) { free(f); return nullptr; }
    int line = 0, tw = 0, th = 0;
    const unsigned char *chars = nullptr;
    size_t nchars = 0;
    char page[64] = "";
    for (size_t p = 4; p + 5 <= fn;) {
        int type = f[p];
        uint32_t len;
        memcpy(&len, f + p + 1, 4);
        const unsigned char *blk = f + p + 5;
        if (p + 5 + len > fn) break;
        if (type == 2 && len >= 2) line = blk[0] | blk[1] << 8;
        if (type == 3) snprintf(page, sizeof page, "font/%.*s", (int)strnlen((const char *)blk, len), (const char *)blk);
        if (type == 4) { chars = blk; nchars = len / 20; }
        p += 5 + len;
    }
    unsigned char *t = page[0] && g_font_texs < 4 ? pvp_pak_file(page, &tn) : nullptr;
    unsigned char *tex = t ? tga_bgra(t, tn, &tw, &th) : nullptr;
    free(t);
    if (!tex || !chars || line <= 0) { free(tex); free(f); rc_log("ui: game font %s not loaded", name); return nullptr; }
    int ti = g_font_texs++;
    g_font_tex[ti] = tex;
    g_font_tex_w[ti] = tw;
    int before = g_game_glyphs.Size;
    ImFontConfig cfg;
    cfg.SizePixels = (float)line;
    static const ImWchar none[] = {0x20, 0x20, 0};
    cfg.GlyphRanges = none;
    snprintf(cfg.Name, sizeof cfg.Name, "%s", name);
    ImFont *font = atlas->AddFontDefault(&cfg);
    for (size_t i = 0; i < nchars; i++) {
        const unsigned char *c = chars + i * 20;
        uint32_t id;
        uint16_t x, y, w, h;
        int16_t xo, yo, adv;
        memcpy(&id, c, 4); memcpy(&x, c + 4, 2); memcpy(&y, c + 6, 2); memcpy(&w, c + 8, 2); memcpy(&h, c + 10, 2);
        memcpy(&xo, c + 12, 2); memcpy(&yo, c + 14, 2); memcpy(&adv, c + 16, 2);
        if (id > 0xffff || c[18] != 0 || x + w > tw || y + h > th) continue;
        GameGlyph g = {atlas->AddCustomRectFontGlyph(font, (ImWchar)id, w ? w : 1, h ? h : 1, adv, ImVec2(xo, yo)), x, y, w, h, c[19], ti};
        g_game_glyphs.push_back(g);
    }
    free(f);
    rc_log("ui: game font %s, %d glyphs", name, g_game_glyphs.Size - before);
    return font;
}

// once the atlas is built: the glyphs' pixels, white with the game's coverage as alpha
static void fill_game_glyphs(ImFontAtlas *atlas) {
    if (!g_font_texs) return;
    unsigned char *px;
    int w, h;
    atlas->GetTexDataAsRGBA32(&px, &w, &h);
    for (const GameGlyph &g : g_game_glyphs) {
        const ImFontAtlasCustomRect *r = atlas->GetCustomRectByIndex(g.rect);
        int ch = g.chnl == 1 ? 0 : g.chnl == 2 ? 1 : g.chnl == 4 ? 2 : 3;
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                const unsigned char *s = g_font_tex[g.tex] + ((size_t)(g.y + y) * g_font_tex_w[g.tex] + g.x + x) * 4;
                unsigned char a = g.chnl == 15 ? s[3] : s[ch];
                ((ImU32 *)px)[(size_t)(r->Y + y) * w + r->X + x] = IM_COL32(255, 255, 255, a);
            }
    }
    for (int i = 0; i < g_font_texs; i++) { free(g_font_tex[i]); g_font_tex[i] = nullptr; }
    g_font_texs = 0;
}

static void init_imgui(IDXGISwapChain *sc) {
    DXGI_SWAP_CHAIN_DESC d;
    if (FAILED(sc->GetDesc(&d)) || !d.OutputWindow || FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&g_dev))) {
        g_failed = true;
        rc_log("ui: swapchain without a D3D11 device, panel off");
        return;
    }
    rc_log("ui: first frame of the game's window, setting up");
    g_dev->GetImmediateContext(&g_ctx);
    g_sc = sc;
    g_hwnd = d.OutputWindow;

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    RECT r;
    GetClientRect(g_hwnd, &r);
    float scale = (r.bottom - r.top) / 1080.f;
    if (scale < 1) scale = 1;
    char font[MAX_PATH];
    GetWindowsDirectoryA(font, MAX_PATH);
    strcat(font, "\\Fonts\\segoeui.ttf");
    if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF(font, 18 * scale, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    else
        io.Fonts->AddFontDefault();
    g_game_font = add_game_font(io.Fonts, "anka_15");
    g_game_font_small = add_game_font(io.Fonts, "anka_12");

    rc_log("ui: font loaded");
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 6;
    st.FrameRounding = 4;
    st.WindowPadding = ImVec2(10, 8);
    st.ScaleAllSizes(scale);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.106f, 0.114f, 0.122f, 1);
    st.Colors[ImGuiCol_Button] = ImVec4(0.17f, 0.18f, 0.19f, 1);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.27f, 0.28f, 1);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.85f, 0.64f, 0.25f, 1);

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    // shaders (D3DCompile) and the font atlas are made on a thread of ours: the game presents from a
    // fiber with a small stack, and the shader compiler overflowed it (a crash in __chkstk, 2026-10-31)
    HANDLE t = CreateThread(nullptr, 8 << 20, [](LPVOID) -> DWORD {
                                ImFontAtlas *atlas = ImGui::GetIO().Fonts;
                                atlas->Build();
                                fill_game_glyphs(atlas);
                                return ImGui_ImplDX11_CreateDeviceObjects() ? 0 : 1;
                            },
                            nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    DWORD made = 1;
    if (t) { WaitForSingleObject(t, INFINITE); GetExitCodeThread(t, &made); CloseHandle(t); }
    if (made) { g_failed = true; rc_log("ui: shaders could not be made, panel off"); return; }
    rc_log("ui: renderer set up");
    g_wndproc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)wndproc);
    g_ready = true;
    rc_log("ui: ready, window %p %ldx%ld, format %d", g_hwnd, r.right - r.left, r.bottom - r.top, d.BufferDesc.Format);
}

// the backbuffer's view is made for the frame and let go after it: holding it would make the game's
// ResizeBuffers fail, and hooking ResizeBuffers to let go of it hung the game (Steam overlay, 2026-10-31)
static bool make_rtv(IDXGISwapChain *sc) {
    ID3D11Texture2D *bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb))) return false;
    HRESULT hr = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
    return SUCCEEDED(hr);
}

static void draw(IDXGISwapChain *sc) {
    if (!make_rtv(sc)) return;
    ImGuiIO &io = ImGui::GetIO();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    hotkeys();
    static long results_was;
    if (g_ms_results != results_was) {         // the results page opens the charts and takes them along when it goes
        results_was = g_ms_results;
        g_charts = results_was != 0;
    }
    if (panel_shown()) panel();
    if (charts_shown()) charts();
    if (music_shown()) music_window();
    if (balance_shown()) balance_window();
    pvp_block();
    chat_block();
    lobby_overlay();
    g_online_want = online_shown() || g_pvp_drawn || g_chat_drawn;
    if (online_shown()) online_window();
    integrity_window();
    if (consent_shown()) consent_window();
    else if (login_shown()) login_window();
    if (prep_shown()) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
        prep_window();
    }
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    if (!dd->CmdListsCount) { g_rtv->Release(); g_rtv = nullptr; return; }
    ID3D11RenderTargetView *old_rtv = nullptr;
    ID3D11DepthStencilView *old_dsv = nullptr;
    g_ctx->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(dd);       // saves and restores the rest of the pipeline state
    g_ctx->OMSetRenderTargets(1, &old_rtv, old_dsv);
    if (old_rtv) old_rtv->Release();
    if (old_dsv) old_dsv->Release();
    g_rtv->Release();
    g_rtv = nullptr;
}

// Our frame is drawn when the game presents its own: the game's eRenderDevice present (4fc010, swapchain
// at +0xa8, see goh_replay.c) calls ui_present first. Nothing in DXGI is touched. Hooking
// IDXGISwapChain::Present itself broke the game without ReShade: a swapchain of our own made on a side
// thread hung it at start, and with the Steam overlay (it hooks Present too and called "the original"
// back through our slot) the stack ran out (2026-10-31).
extern "C" void ui_present(void *device) {
    IDXGISwapChain *sc = *(IDXGISwapChain **)((uint8_t *)device + 0xa8);
    if (!sc || !g_ui_draw) return;
    if (!g_ready && !g_failed) {
        DXGI_SWAP_CHAIN_DESC d;
        if (SUCCEEDED(sc->GetDesc(&d)) && IsWindowVisible(d.OutputWindow)) init_imgui(sc);
    }
    if (g_ready && sc == g_sc) draw(sc);
}

extern "C" int g_ui_draw = 1;   // [debug] ui_draw=0 in goh_snapshots.ini: nothing drawn
