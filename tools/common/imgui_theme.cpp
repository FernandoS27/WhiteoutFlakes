#include "imgui_theme.h"

#include <imgui.h>

namespace whiteout::flakes {

void ApplyImGuiTheme() {
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.6f;
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowMinSize = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(4.0f, 3.0f);
    style.FrameRounding = 4.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style.CellPadding = ImVec2(4.0f, 2.0f);
    style.IndentSpacing = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    style.ScrollbarSize = 14.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize = 10.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.TabBorderSize = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.0f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.9490196f, 0.95686275f, 0.9764706f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.35686275f, 0.41960785f, 0.46666667f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.14901961f, 0.1764706f, 0.21960784f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    c[ImGuiCol_Border] = ImVec4(0.078431375f, 0.09803922f, 0.11764706f, 1.0f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.11764706f, 0.2f, 0.2784314f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.08627451f, 0.11764706f, 0.13725491f, 1.0f);
    c[ImGuiCol_TitleBg] = ImVec4(0.08627451f, 0.11764706f, 0.13725491f, 0.65f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.078431375f, 0.09803922f, 0.11764706f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.14901961f, 0.1764706f, 0.21960784f, 1.0f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.39f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.1764706f, 0.21960784f, 0.24705882f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.08627451f, 0.20784314f, 0.30980393f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.2784314f, 0.5568628f, 1.0f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.2784314f, 0.5568628f, 1.0f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.36862746f, 0.60784316f, 1.0f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.2784314f, 0.5568628f, 1.0f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.05882353f, 0.5294118f, 0.9764706f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.2f, 0.24705882f, 0.28627452f, 0.55f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    c[ImGuiCol_SeparatorHovered] = ImVec4(0.09803922f, 0.4f, 0.7490196f, 0.78f);
    c[ImGuiCol_SeparatorActive] = ImVec4(0.09803922f, 0.4f, 0.7490196f, 1.0f);
    c[ImGuiCol_ResizeGrip] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.25f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    c[ImGuiCol_ResizeGripActive] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.95f);
    c[ImGuiCol_Tab] = ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    c[ImGuiCol_TabActive] = ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    c[ImGuiCol_TabUnfocused] = ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    c[ImGuiCol_PlotLines] = ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    c[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.35f);
    c[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    c[ImGuiCol_NavHighlight] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

void ApplyImGuiDpiScale(float scale) {
    if (scale <= 0.0f)
        scale = 1.0f;
    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle& style = ImGui::GetStyle();
    // Bake the default font *at* the monitor's pixel density. style.FontScaleDpi
    // would be tempting (1.92+ exposes it), but it's a draw-time multiplier
    // that assumes the dynamic-atlas pipeline. Our engine ImGui renderer uses
    // the legacy GetTexDataAsRGBA32 path — atlas baked once at SizePixels —
    // so anything > 1.0 there just bilinear-upscales 13px glyphs at draw time
    // (readable but soft). Rebaking at the scaled SizePixels keeps glyphs
    // hinted and crisp.
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.SizePixels = 13.0f * scale;
    io.Fonts->AddFontDefault(&cfg);
    // ScaleAllSizes multiplies the absolute pixel values set in
    // ApplyImGuiTheme (padding, spacing, rounding, scrollbar/grab sizes …).
    style.ScaleAllSizes(scale);
}

} // namespace whiteout::flakes
