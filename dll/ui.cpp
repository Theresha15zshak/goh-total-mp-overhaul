// In-game replay panel: Dear ImGui drawn into the game's own frames.
// The game's swapchain Present/ResizeBuffers are taken over through the vtable of a throwaway
// swapchain of the same class (works with or without ReShade in front of DXGI).
// Mouse clicks over the panel are kept from the game; movement passes so its cursor keeps moving.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <math.h>
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
static volatile bool g_pvp_drawn;       // the PVP block over the game list was drawn this frame: the mouse over it is ours
static ImFont *g_game_font;             // the game's anka 15 (the game list's rows), loaded from interface.pak
static bool g_failed;

static const float SPEEDS[] = {0.25f, 0.5f, 1, 2, 4, 8, 16};
static const char *SPEED_LABELS[] = {"\xC2\xBC\xC3\x97", "\xC2\xBD\xC3\x97", "1\xC3\x97", "2\xC3\x97", "4\xC3\x97", "8\xC3\x97", "16\xC3\x97"};
static const ImU32 ACCENT = IM_COL32(217, 164, 65, 255);

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
static bool consent_shown() { return g_online_consent == -1 && !g_live_match && g_ctl.state == ST_IDLE; }

static bool raw_mouse_button(LPARAM l) {
    RAWINPUT ri;
    UINT size = sizeof ri;
    if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return false;
    return ri.header.dwType == RIM_TYPEMOUSE && (ri.data.mouse.usButtonFlags != 0);
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
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_ready) {
        if (panel_shown() || charts_shown() || music_shown() || prep_shown() || balance_shown() || online_shown() ||
            consent_shown() || g_pvp_drawn) {
            ImGui_ImplWin32_WndProcHandler(h, m, w, l);
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
        } else if (!c.duration_s || c.snap_to_ms < c.duration_s * 1000 - 30000) {
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
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(PLAYER_COL[p]), "Игрок %d: %.0f", s.id[p], s.v[series][p][lo]);
        }
        ImGui::EndTooltip();
    }
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
        char label[32];
        snprintf(label, sizeof label, "Игрок %d##pl%d", s.id[p], p);
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
            chart("##income", MS_INCOME, "Заработано очков, всего", half);
            chart("##budget", MS_BUDGET, "Очки на руках", half);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Армия")) {
            chart("##army", MS_ARMY, "Стоимость армии на поле (очки лимита)", full);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Убийства")) {
            chart("##kills", MS_KILLS, "Убито бойцов и техники противника, всего", full);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Потери")) {
            chart("##deaths", MS_DEATHS, "Потеряно бойцов и техники, всего", half);
            chart("##lost", MS_LOST, "Потеряно стоимости армии (очки лимита), всего", half);
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
    "Чтобы вы видели друг друга в списке, мод раз в 2 минуты отправляет на сервер мода "
    "(goh-online.goh-mod.workers.dev) ваш SteamID, ник Steam и версию баланса. Больше ничего.";

static void consent_window() {
    float k = ImGui::GetFontSize() / 18.f;
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460 * k, 0));
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::Begin("Мод: игроки онлайн###goh_consent", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                                                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::TextWrapped("Показывать вас в списке игроков с модом, которые сейчас в игре?");
    ImGui::Spacing();
    ImGui::TextDisabled("%s", CONSENT_TEXT);
    ImGui::Spacing();
    if (ImGui::Button("Да, показывать")) online_set_consent(1);
    ImGui::SameLine();
    if (ImGui::Button("Нет")) online_set_consent(0);
    ImGui::TextDisabled("Поменять можно в окне F6.");
    ImGui::End();
}

// the players online with the mod; the table fills the height left but `bottom`
static void online_list(float bottom) {
    static OnlinePlayer list[ONLINE_MAX];
    int state = 0;
    int n = online_copy(list, ONLINE_MAX, &state);
    bool me = g_online_consent == 1;
    if (ImGui::Checkbox("Показывать меня в списке", &me)) online_set_consent(me);
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

static void van_text(ImDrawList *dl, float x, float y, ImU32 col, const char *s) { dl->AddText(ImVec2(floorf(x + 0.5f), floorf(y + 0.5f)), col, s); }

static void pvp_block() {
    static OnlinePlayer list[ONLINE_MAX];
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
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float lh = ImGui::GetTextLineHeight(), row = 32 * sy, top = head * sy;
    float col[4];
    for (int i = 0; i < 4; i++) col[i] = p0.x + VAN_COL[i] * size.x;

    int state = 0, n = online_copy(list, ONLINE_MAX, &state);
    const char *my = online_my_id();
    // column titles, where the page has its own
    float ty = p0.y + (top > lh ? (top - lh) * 0.5f : 0);
    van_text(dl, col[0], ty, VAN_TITLE, "Игрок");
    van_text(dl, col[1], ty, VAN_TITLE, "Версия баланса");
    van_text(dl, col[2], ty, VAN_TITLE, "Совместимость");
    van_text(dl, col[3], ty, VAN_TITLE, "Профиль");

    // the rows, scrolling between the titles and the bottom line
    float foot = row;
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + top));
    ImGui::BeginChild("##rows", ImVec2(size.x, size.y - top - foot), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::SetWindowFontScale(sy);
    ImDrawList *rl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    if (state == 0 || state == 2 || n == 0) {
        const char *msg = state == 0 ? "Загрузка..." : state == 2 && !n ? "Сервер мода не отвечает" : "Сейчас никого нет";
        float tw = ImGui::CalcTextSize(msg).x;
        van_text(rl, origin.x + (size.x - tw) * 0.5f, origin.y + row * 1.5f, VAN_TITLE, msg);
    }
    for (int i = 0; i < n; i++) {
        ImGui::PushID(i);
        ImVec2 r0(origin.x, origin.y + i * row);
        ImGui::SetCursorScreenPos(r0);
        bool click = ImGui::InvisibleButton("##row", ImVec2(size.x - 10 * sy, row));
        bool hot = ImGui::IsItemHovered();
        if (hot) rl->AddRectFilled(r0, ImVec2(r0.x + size.x - 10 * sy, r0.y + row), VAN_HOVER_BG);
        bool self = !strcmp(list[i].id, my), same = !strcmp(list[i].version, g_balance_tag);
        float yy = r0.y + (row - lh) * 0.5f;
        ImU32 c = hot ? VAN_HOVER : VAN_TEXT;
        if (self) rl->AddRectFilled(ImVec2(r0.x + 14 * sx, yy + lh * 0.3f), ImVec2(r0.x + 20 * sx, yy + lh * 0.3f + 6 * sy), ACCENT);
        van_text(rl, col[0], yy, c, list[i].name);
        van_text(rl, col[1], yy, same ? c : VAN_WARN, list[i].version);
        van_text(rl, col[2], yy, self ? VAN_TITLE : same ? c : VAN_WARN, self ? "это вы" : same ? "можно играть компетитив" : "другая версия");
        van_text(rl, col[3], yy, hot ? VAN_HOVER : VAN_TITLE, "Steam");
        if (click) {
            char url[64];
            snprintf(url, sizeof url, "steam://url/SteamIDPage/%s", list[i].id);
            ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::PopID();
    }
    if (n) ImGui::Dummy(ImVec2(1, 0));
    ImGui::EndChild();

    // bottom line: how many, the server, our consent
    float fy = p0.y + size.y - foot + (foot - lh) * 0.5f;
    char line[160];
    if (g_online_held) snprintf(line, sizeof line, "Ваш SteamID закреплён за другим ключом: напишите автору мода");
    else snprintf(line, sizeof line, state == 2 && n ? "С модом онлайн: %d  \xC2\xB7  сервер не отвечает" : "С модом онлайн: %d", n);
    van_text(dl, col[0], fy, state == 2 || g_online_held ? VAN_WARN : VAN_TITLE, line);
    const char *me_txt = "Показывать меня в списке";
    float box = lh * 0.7f, mw = ImGui::CalcTextSize(me_txt).x;
    ImVec2 b0(col[2], fy + (lh - box) * 0.5f);
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
struct GameGlyph { int rect, x, y, w, h, chnl; };
static unsigned char *g_game_font_tex;      // its texture, BGRA, top row first
static int g_game_font_tex_w;
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
    unsigned char *t = page[0] ? pvp_pak_file(page, &tn) : nullptr;
    g_game_font_tex = t ? tga_bgra(t, tn, &tw, &th) : nullptr;
    free(t);
    if (!g_game_font_tex || !chars || line <= 0) { free(f); rc_log("ui: game font %s not loaded", name); return nullptr; }
    g_game_font_tex_w = tw;
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
        GameGlyph g = {atlas->AddCustomRectFontGlyph(font, (ImWchar)id, w ? w : 1, h ? h : 1, adv, ImVec2(xo, yo)), x, y, w, h, c[19]};
        g_game_glyphs.push_back(g);
    }
    free(f);
    rc_log("ui: game font %s, %d glyphs", name, g_game_glyphs.Size);
    return font;
}

// once the atlas is built: the glyphs' pixels, white with the game's coverage as alpha
static void fill_game_glyphs(ImFontAtlas *atlas) {
    if (!g_game_font_tex) return;
    unsigned char *px;
    int w, h;
    atlas->GetTexDataAsRGBA32(&px, &w, &h);
    for (const GameGlyph &g : g_game_glyphs) {
        const ImFontAtlasCustomRect *r = atlas->GetCustomRectByIndex(g.rect);
        int ch = g.chnl == 1 ? 0 : g.chnl == 2 ? 1 : g.chnl == 4 ? 2 : 3;
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                const unsigned char *s = g_game_font_tex + ((size_t)(g.y + y) * g_game_font_tex_w + g.x + x) * 4;
                unsigned char a = g.chnl == 15 ? s[3] : s[ch];
                ((ImU32 *)px)[(size_t)(r->Y + y) * w + r->X + x] = IM_COL32(255, 255, 255, a);
            }
    }
    free(g_game_font_tex);
    g_game_font_tex = nullptr;
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
    g_online_want = online_shown() || g_pvp_drawn;
    if (online_shown()) online_window();
    if (consent_shown()) consent_window();
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
