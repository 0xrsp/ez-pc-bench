#include "app/theme.h"
#include <windows.h>
#include <wchar.h>

ThemeFonts g_fonts;

const ImVec4 TH_TEXT         = { 0.933f, 0.918f, 0.984f, 1.0f };   // #EEEAFB
const ImVec4 TH_TEXT_DIM     = { 0.612f, 0.588f, 0.749f, 1.0f };   // #9C96BF
const ImVec4 TH_ACCENT       = { 0.357f, 0.294f, 0.859f, 1.0f };   // #5B4BDB
const ImVec4 TH_ACCENT_LIGHT = { 0.600f, 0.545f, 1.000f, 1.0f };   // #998BFF
const ImVec4 TH_OK           = { 0.494f, 0.886f, 0.659f, 1.0f };   // #7EE2A8
const ImVec4 TH_RUN          = { 0.690f, 0.640f, 1.000f, 1.0f };
const ImVec4 TH_BAD          = { 1.000f, 0.478f, 0.541f, 1.0f };
const ImVec4 TH_WARN         = { 1.000f, 0.765f, 0.529f, 1.0f };   // #FFC387

static const ImVec4 PANEL    = { 0.165f, 0.145f, 0.314f, 0.62f };
static const ImVec4 BANNER   = { 0.290f, 0.180f, 0.150f, 0.62f };

static f32 s_scale = 1.0f;

static ImFont* load(const wchar_t* file, ImFont* fallback) {
    wchar_t path[MAX_PATH];
    GetWindowsDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\Fonts\\");
    wcscat_s(path, file);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return fallback;
    char path8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, path8, sizeof(path8), nullptr, nullptr);
    ImFont* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(path8);
    return f ? f : fallback;
}

void theme_load_fonts() {
    g_fonts.regular = load(L"segoeui.ttf", nullptr);       // first font added = default font
    g_fonts.semibold = load(L"seguisb.ttf", g_fonts.regular);
    g_fonts.mono = load(L"CascadiaMono.ttf", nullptr);
    if (!g_fonts.mono) g_fonts.mono = load(L"consola.ttf", g_fonts.regular);
}

void theme_apply_style(f32 scale) {
    s_scale = scale;
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.FontSizeBase = 16.0f;
    s.WindowPadding = ImVec2(16, 14);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.CellPadding = ImVec2(8, 5);
    s.IndentSpacing = 18;
    s.ScrollbarSize = 10;
    s.GrabMinSize = 10;
    s.WindowRounding = 0;
    s.ChildRounding = 14;
    s.FrameRounding = 8;
    s.PopupRounding = 10;
    s.ScrollbarRounding = 8;
    s.GrabRounding = 8;
    s.TabRounding = 9;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.TabBorderSize = 0;
    s.TabBarBorderSize = 0;
    s.TabBarOverlineSize = 0;
    s.SeparatorTextBorderSize = 1;
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;

    ImVec4* c = s.Colors;
    const ImVec4 none = { 0, 0, 0, 0 };
    const ImVec4 hover = { 0.290f, 0.260f, 0.520f, 0.90f };
    const ImVec4 active = { 0.330f, 0.290f, 0.600f, 1.00f };
    c[ImGuiCol_Text] = TH_TEXT;
    c[ImGuiCol_TextDisabled] = TH_TEXT_DIM;
    c[ImGuiCol_WindowBg] = none;
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = { 0.110f, 0.100f, 0.210f, 0.97f };
    c[ImGuiCol_Border] = { 1, 1, 1, 0.07f };
    c[ImGuiCol_BorderShadow] = none;
    c[ImGuiCol_FrameBg] = { 0.200f, 0.180f, 0.370f, 0.85f };
    c[ImGuiCol_FrameBgHovered] = { 0.260f, 0.230f, 0.460f, 0.90f };
    c[ImGuiCol_FrameBgActive] = active;
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = { 1, 1, 1, 0.10f };
    c[ImGuiCol_ScrollbarGrabHovered] = { 1, 1, 1, 0.18f };
    c[ImGuiCol_ScrollbarGrabActive] = { 1, 1, 1, 0.25f };
    c[ImGuiCol_CheckMark] = TH_ACCENT_LIGHT;
    c[ImGuiCol_SliderGrab] = TH_ACCENT;
    c[ImGuiCol_SliderGrabActive] = TH_ACCENT_LIGHT;
    c[ImGuiCol_Button] = { 0.220f, 0.200f, 0.400f, 0.85f };
    c[ImGuiCol_ButtonHovered] = hover;
    c[ImGuiCol_ButtonActive] = active;
    c[ImGuiCol_Header] = { 0.240f, 0.210f, 0.440f, 0.70f };
    c[ImGuiCol_HeaderHovered] = hover;
    c[ImGuiCol_HeaderActive] = active;
    c[ImGuiCol_Separator] = { 1, 1, 1, 0.07f };
    c[ImGuiCol_SeparatorHovered] = { TH_ACCENT.x, TH_ACCENT.y, TH_ACCENT.z, 0.6f };
    c[ImGuiCol_SeparatorActive] = TH_ACCENT;
    c[ImGuiCol_ResizeGrip] = none;
    c[ImGuiCol_ResizeGripHovered] = none;
    c[ImGuiCol_ResizeGripActive] = none;
    c[ImGuiCol_Tab] = none;
    c[ImGuiCol_TabHovered] = { 0.290f, 0.260f, 0.520f, 0.70f };
    c[ImGuiCol_TabSelected] = { 0.300f, 0.265f, 0.560f, 0.95f };
    c[ImGuiCol_TabSelectedOverline] = none;
    c[ImGuiCol_TabDimmed] = none;
    c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
    c[ImGuiCol_TabDimmedSelectedOverline] = none;
    c[ImGuiCol_PlotLines] = TH_ACCENT_LIGHT;
    c[ImGuiCol_PlotLinesHovered] = TH_WARN;
    c[ImGuiCol_PlotHistogram] = { 0.486f, 0.424f, 0.961f, 1.0f };    // progress fill #7C6CF5
    c[ImGuiCol_PlotHistogramHovered] = TH_ACCENT_LIGHT;
    c[ImGuiCol_TableHeaderBg] = { 0.200f, 0.180f, 0.370f, 0.90f };
    c[ImGuiCol_TableBorderStrong] = { 1, 1, 1, 0.06f };
    c[ImGuiCol_TableBorderLight] = { 1, 1, 1, 0.05f };
    c[ImGuiCol_TableRowBg] = none;
    c[ImGuiCol_TableRowBgAlt] = { 1, 1, 1, 0.025f };
    c[ImGuiCol_TextSelectedBg] = { TH_ACCENT.x, TH_ACCENT.y, TH_ACCENT.z, 0.35f };
    c[ImGuiCol_NavCursor] = TH_ACCENT_LIGHT;
    c[ImGuiCol_ModalWindowDimBg] = { 0, 0, 0, 0.5f };
}

void theme_draw_background() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 a = vp->Pos, b = ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    const ImU32 top = IM_COL32(0x14, 0x12, 0x2A, 255), bottom = IM_COL32(0x2E, 0x1C, 0x4C, 255);
    dl->AddRectFilledMultiColor(a, b, top, top, bottom, bottom);

    // Warm horizon glow over the bottom third.
    const f32 gy = a.y + vp->Size.y * 0.62f;
    const ImU32 clear = IM_COL32(0xC2, 0x4E, 0x8A, 0), warm = IM_COL32(0xC2, 0x4E, 0x8A, 52);
    dl->AddRectFilledMultiColor(ImVec2(a.x, gy), b, clear, clear, warm, warm);

    // Sparse stars in the upper part, positions fixed relative to the window size.
    u32 h = 0x9E3779B9u;
    for (u32 i = 0; i < 90; ++i) {
        h ^= h << 13; h ^= h >> 17; h ^= h << 5;
        const f32 x = (h & 0xFFFF) / 65535.0f;
        const f32 y = ((h >> 16) & 0xFFFF) / 65535.0f * 0.7f;
        const u32 alpha = 30 + (h >> 24) % 110;
        const f32 r = (0.6f + (f32)((h >> 8) % 8) / 10.0f) * s_scale;
        dl->AddCircleFilled(ImVec2(a.x + x * vp->Size.x, a.y + y * vp->Size.y), r, IM_COL32(255, 255, 255, alpha), 6);
    }
}

void theme_begin_panel(const char* id, ImVec2 size, bool scroll) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, PANEL);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s_scale, 12 * s_scale));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      scroll ? 0 : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void theme_end_panel() { ImGui::EndChild(); }

bool theme_button_primary(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, TH_ACCENT);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.427f, 0.369f, 0.922f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.298f, 0.239f, 0.784f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(22 * s_scale, 6 * s_scale));
    ImGui::PushFont(g_fonts.semibold, 0.0f);
    const bool pressed = ImGui::Button(label);
    ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return pressed;
}

void theme_progress(f32 fraction, f32 height) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, height * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.08f));
    ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, height), "");
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

bool theme_warnings(const char* const* lines, u32 count, i32 button_line, const char* button_label) {
    if (count == 0) return false;
    const f32 pad = 6 * s_scale;
    const f32 line_h = ImGui::GetTextLineHeight() + 4 * s_scale;
    const f32 h = count * line_h + pad * 2 + (button_line >= 0 ? ImGui::GetFrameHeight() - line_h : 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, BANNER);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(TH_WARN.x, TH_WARN.y, TH_WARN.z, 0.18f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s_scale, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * s_scale, 4 * s_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10 * s_scale);
    ImGui::BeginChild("##warnings", ImVec2(0, h), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    dl->AddRectFilled(p, ImVec2(p.x + 3 * s_scale, p.y + h), ImGui::GetColorU32(TH_WARN), 10 * s_scale,
                      ImDrawFlags_RoundTopLeft | ImDrawFlags_RoundBottomLeft);
    bool clicked = false;
    for (u32 i = 0; i < count; ++i) {
        const bool has_button = (i32)i == button_line;
        if (has_button) ImGui::AlignTextToFramePadding();
        // Small amber dot as a bullet.
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const f32 mid = c.y + (has_button ? ImGui::GetFrameHeight() : ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddCircleFilled(ImVec2(c.x + 3 * s_scale, mid), 2.5f * s_scale, ImGui::GetColorU32(TH_WARN), 8);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14 * s_scale);
        ImGui::TextColored(TH_WARN, "%s", lines[i]);
        if (has_button) {
            ImGui::SameLine();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * s_scale, 3 * s_scale));
            clicked = ImGui::Button(button_label);
            ImGui::PopStyleVar();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    return clicked;
}

ImU32 theme_heat(f32 t) {
    const ImVec4 lo = TH_OK, hi = TH_BAD;
    return ImGui::GetColorU32(ImVec4(lo.x + (hi.x - lo.x) * t, lo.y + (hi.y - lo.y) * t, lo.z + (hi.z - lo.z) * t, 0.30f));
}
