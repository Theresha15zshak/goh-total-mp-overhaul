"""The skin hooks (GImGuiSkin) put into third_party/imgui, an upstream submodule we do not fork: tools/build.py runs this
before every build; each change is made once (a file that has it is left alone).
    python tools/imgui_skin.py third_party/imgui"""
import sys, os
I = sys.argv[1]


def patch(path, pairs):
    t = open(path, encoding='utf8', newline='').read()
    nl = '\r\n' if '\r\n' in t else '\n'
    for a, b in pairs:
        a = a.replace('\n', nl); b = b.replace('\n', nl)
        if b in t:
            continue
        assert t.count(a) == 1, (path, a[:90], t.count(a))
        t = t.replace(a, b)
    open(path, 'w', encoding='utf8', newline='').write(t)


patch(os.path.join(I, 'imgui.h'), [(
'''#ifdef IMGUI_INCLUDE_IMGUI_USER_H''',
'''// GOH (goh_replay_rewind): a skin drawn from the game's own textures (dll/ui.cpp sets these). A hook that returns
// true has drawn the part and ImGui's own drawing of it is skipped.
struct ImGuiSkinHooks
{
    bool (*WindowBg)(ImDrawList* dl, const ImVec2& a, const ImVec2& b, ImU32 col);
    bool (*TitleBg)(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool active);
    bool (*Button)(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool held);
    bool (*Checkbox)(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool checked, bool hovered);
    bool (*ScrollGrab)(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool vertical, bool hovered, bool held);
    bool (*Separator)(ImDrawList* dl, const ImVec2& a, const ImVec2& b);
};
extern IMGUI_API ImGuiSkinHooks GImGuiSkin;

#ifdef IMGUI_INCLUDE_IMGUI_USER_H''')])

patch(os.path.join(I, 'imgui.cpp'), [
('''            window->DrawList->AddRectFilled(window->Pos + ImVec2(0, window->TitleBarHeight), window->Pos + window->Size, bg_col, window_rounding, (flags & ImGuiWindowFlags_NoTitleBar) ? 0 : ImDrawFlags_RoundCornersBottom);''',
 '''            if (!GImGuiSkin.WindowBg || !GImGuiSkin.WindowBg(window->DrawList, window->Pos + ImVec2(0, window->TitleBarHeight), window->Pos + window->Size, bg_col))
            window->DrawList->AddRectFilled(window->Pos + ImVec2(0, window->TitleBarHeight), window->Pos + window->Size, bg_col, window_rounding, (flags & ImGuiWindowFlags_NoTitleBar) ? 0 : ImDrawFlags_RoundCornersBottom);'''),
('''            ImU32 title_bar_col = GetColorU32(title_bar_is_highlight ? ImGuiCol_TitleBgActive : ImGuiCol_TitleBg);
            window->DrawList->AddRectFilled(title_bar_rect.Min, title_bar_rect.Max, title_bar_col, window_rounding, ImDrawFlags_RoundCornersTop);''',
 '''            ImU32 title_bar_col = GetColorU32(title_bar_is_highlight ? ImGuiCol_TitleBgActive : ImGuiCol_TitleBg);
            if (!GImGuiSkin.TitleBg || !GImGuiSkin.TitleBg(window->DrawList, title_bar_rect.Min, title_bar_rect.Max, title_bar_is_highlight))
            window->DrawList->AddRectFilled(title_bar_rect.Min, title_bar_rect.Max, title_bar_col, window_rounding, ImDrawFlags_RoundCornersTop);'''),
('''void ImGui::RenderWindowDecorations(ImGuiWindow* window,''',
 '''ImGuiSkinHooks GImGuiSkin = {};

void ImGui::RenderWindowDecorations(ImGuiWindow* window,'''),
])

patch(os.path.join(I, 'imgui_widgets.cpp'), [
('''    const ImU32 col = GetColorU32((held && hovered) ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    RenderNavCursor(bb, id);
    RenderFrame(bb.Min, bb.Max, col, true, style.FrameRounding);''',
 '''    const ImU32 col = GetColorU32((held && hovered) ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    RenderNavCursor(bb, id);
    if (!GImGuiSkin.Button || !GImGuiSkin.Button(window->DrawList, bb.Min, bb.Max, hovered, held && hovered))
    RenderFrame(bb.Min, bb.Max, col, true, style.FrameRounding);'''),
('''        RenderNavCursor(total_bb, id);
        RenderFrame(check_bb.Min, check_bb.Max, GetColorU32((held && hovered) ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), true, style.FrameRounding);
        ImU32 check_col = GetColorU32(ImGuiCol_CheckMark);
        if (mixed_value)''',
 '''        RenderNavCursor(total_bb, id);
        if (!mixed_value && GImGuiSkin.Checkbox && GImGuiSkin.Checkbox(window->DrawList, check_bb.Min, check_bb.Max, *v, hovered)) {}
        else {
        RenderFrame(check_bb.Min, check_bb.Max, GetColorU32((held && hovered) ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), true, style.FrameRounding);
        ImU32 check_col = GetColorU32(ImGuiCol_CheckMark);
        if (mixed_value)'''),
('''            const float pad = ImMax(1.0f, IM_TRUNC(square_sz / 6.0f));
            RenderCheckMark(window->DrawList, check_bb.Min + ImVec2(pad, pad), check_col, square_sz - pad * 2.0f);
        }
    }''',
 '''            const float pad = ImMax(1.0f, IM_TRUNC(square_sz / 6.0f));
            RenderCheckMark(window->DrawList, check_bb.Min + ImVec2(pad, pad), check_col, square_sz - pad * 2.0f);
        }
        }
    }'''),
('''    window->DrawList->AddRectFilled(grab_rect.Min, grab_rect.Max, grab_col, style.ScrollbarRounding);

    return held;''',
 '''    if (!GImGuiSkin.ScrollGrab || !GImGuiSkin.ScrollGrab(window->DrawList, grab_rect.Min, grab_rect.Max, axis == ImGuiAxis_Y, hovered, held))
    window->DrawList->AddRectFilled(grab_rect.Min, grab_rect.Max, grab_col, style.ScrollbarRounding);

    return held;'''),
('''            // Draw
            window->DrawList->AddRectFilled(bb.Min, bb.Max, GetColorU32(ImGuiCol_Separator));
            if (g.LogEnabled)
                LogRenderedText(&bb.Min, "--------------------------------\\n");''',
 '''            // Draw
            if (!GImGuiSkin.Separator || !GImGuiSkin.Separator(window->DrawList, bb.Min, bb.Max))
            window->DrawList->AddRectFilled(bb.Min, bb.Max, GetColorU32(ImGuiCol_Separator));
            if (g.LogEnabled)
                LogRenderedText(&bb.Min, "--------------------------------\\n");'''),
])
print('ok')
