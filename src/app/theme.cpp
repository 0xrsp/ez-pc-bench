#include "app/theme.h"
#include <windows.h>
#include <math.h>
#include <wchar.h>

ThemeFonts g_fonts;
Palette g_pal;

static f32 s_scale = 1.0f;
static bool s_dark = false;
static bool s_changed = false;

static constexpr ImVec4 rgb(u32 hex, f32 a = 1.0f) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, a);
}

static ImVec4 mix(ImVec4 a, ImVec4 b, f32 t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// Material 3 schemes from seed #0B57D0.
static Palette light_palette() {
    Palette p;
    p.page = rgb(0xF3F5FA);
    p.card = rgb(0xFFFFFF);
    p.container = rgb(0xECEEF4);
    p.container_high = rgb(0xE6E8EE);
    p.container_highest = rgb(0xE1E2E9);
    p.on_surface = rgb(0x191C20);
    p.on_surface_variant = rgb(0x44474F);
    p.outline = rgb(0x74777F);
    p.outline_variant = rgb(0xC4C6D0);
    p.primary = rgb(0x0B57D0);
    p.on_primary = rgb(0xFFFFFF);
    p.primary_container = rgb(0xD8E2FF);
    p.on_primary_container = rgb(0x001A41);
    p.secondary_container = rgb(0xDCE2F9);
    p.on_secondary_container = rgb(0x151B2C);
    p.ok = rgb(0x1E6B35);
    p.error = rgb(0xBA1A1A);
    p.warn = rgb(0x8A5100);
    p.warn_container = rgb(0xFFDDB5);
    p.on_warn_container = rgb(0x2C1600);
    p.heat_lo = rgb(0xC8F0D2);
    p.heat_hi = rgb(0xFFDAD6);
    return p;
}

static Palette dark_palette() {
    Palette p;
    p.page = rgb(0x111318);
    p.card = rgb(0x191C20);
    p.container = rgb(0x1D2024);
    p.container_high = rgb(0x282A2F);
    p.container_highest = rgb(0x33353A);
    p.on_surface = rgb(0xE2E2E9);
    p.on_surface_variant = rgb(0xC4C6D0);
    p.outline = rgb(0x8E9099);
    p.outline_variant = rgb(0x44474E);
    p.primary = rgb(0xAEC6FF);
    p.on_primary = rgb(0x002E6B);
    p.primary_container = rgb(0x0B4AAE);
    p.on_primary_container = rgb(0xD8E2FF);
    p.secondary_container = rgb(0x3C4758);
    p.on_secondary_container = rgb(0xD8E2F9);
    p.ok = rgb(0x7DDA8F);
    p.error = rgb(0xFFB4AB);
    p.warn = rgb(0xFFB95C);
    p.warn_container = rgb(0x4F2D00);
    p.on_warn_container = rgb(0xFFDDB5);
    p.heat_lo = rgb(0x1B4D2A);
    p.heat_hi = rgb(0x6B1D1A);
    return p;
}

static ThemeMode s_mode = THEME_SYSTEM;

// Windows "Choose your app mode" (Settings > Personalization > Colors). Missing value = light.
static bool system_prefers_dark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

static void set_dark(bool dark) {
    if (dark != s_dark) { s_dark = dark; s_changed = true; }
}

bool theme_is_dark() { return s_dark; }
ThemeMode theme_mode() { return s_mode; }

void theme_set_mode(ThemeMode mode) {
    s_mode = mode;
    set_dark(mode == THEME_DARK || (mode == THEME_SYSTEM && system_prefers_dark()));
}

void theme_system_changed() {
    if (s_mode == THEME_SYSTEM) set_dark(system_prefers_dark());
}
bool theme_take_change() {
    const bool c = s_changed;
    s_changed = false;
    return c;
}

void theme_bg_rgb(f32 out[3]) {
    const ImVec4 c = (s_dark ? dark_palette() : light_palette()).page;
    out[0] = c.x; out[1] = c.y; out[2] = c.z;
}

// ---- fonts -----------------------------------------------------------------------------

// Sharp text: rasterize at the exact pixel size, no horizontal oversampling, glyphs snapped to pixels.
static ImFontConfig sharp_config() {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;   // resource memory, not ours to free
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    return cfg;
}

static ImFont* load_resource_font(int id) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));     // 10 = RT_RCDATA
    if (!res) return nullptr;
    HGLOBAL mem = LoadResource(nullptr, res);
    void* data = mem ? LockResource(mem) : nullptr;
    const DWORD size = SizeofResource(nullptr, res);
    if (!data || !size) return nullptr;
    ImFontConfig cfg = sharp_config();
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, (int)size, 0.0f, &cfg);
}

static ImFont* load_system_font(const wchar_t* file) {
    wchar_t path[MAX_PATH];
    GetWindowsDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\Fonts\\");
    wcscat_s(path, file);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return nullptr;
    char path8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, path8, sizeof(path8), nullptr, nullptr);
    ImFontConfig cfg = sharp_config();
    cfg.FontDataOwnedByAtlas = true;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path8, 0.0f, &cfg);
}

void theme_load_fonts() {
    g_fonts.regular = load_resource_font(101);     // gui.rc: Inter-Regular.ttf
    g_fonts.medium = load_resource_font(102);      // gui.rc: Inter-Medium.ttf
    if (!g_fonts.regular) g_fonts.regular = load_system_font(L"segoeui.ttf");
    if (!g_fonts.medium) g_fonts.medium = load_system_font(L"seguisb.ttf");
    if (!g_fonts.medium) g_fonts.medium = g_fonts.regular;
    g_fonts.mono = load_system_font(L"CascadiaMono.ttf");
    if (!g_fonts.mono) g_fonts.mono = load_system_font(L"consola.ttf");
    if (!g_fonts.mono) g_fonts.mono = g_fonts.regular;
}

f32 theme_font_size(f32 mult) {
    // FontSizeBase is pre-DPI; the rendered size is base * FontScaleDpi. Snap that product to whole pixels.
    const f32 px = roundf(ImGui::GetStyle().FontSizeBase * mult * s_scale);
    return px / s_scale;
}

// ---- style -----------------------------------------------------------------------------

void theme_apply_style(f32 scale) {
    s_scale = scale;
    g_pal = s_dark ? dark_palette() : light_palette();
    const Palette& p = g_pal;

    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(20, 16);
    s.FramePadding = ImVec2(14, 7);
    s.ItemSpacing = ImVec2(10, 10);
    s.ItemInnerSpacing = ImVec2(10, 6);
    s.CellPadding = ImVec2(10, 7);
    s.IndentSpacing = 20;
    s.ScrollbarSize = 8;
    s.GrabMinSize = 12;
    s.WindowRounding = 0;
    s.ChildRounding = 12;
    s.FrameRounding = 6;
    s.PopupRounding = 8;
    s.ScrollbarRounding = 8;
    s.GrabRounding = 6;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.SeparatorTextBorderSize = 1;
    s.DisabledAlpha = 0.38f;
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
    s.FontSizeBase = roundf(16.5f * scale) / scale;    // whole-pixel body text (17 px at 100 %)

    ImVec4* c = s.Colors;
    const ImVec4 none = { 0, 0, 0, 0 };
    const ImVec4 hover = mix(p.container_high, p.on_surface, 0.06f);
    c[ImGuiCol_Text] = p.on_surface;
    c[ImGuiCol_TextDisabled] = p.on_surface_variant;
    c[ImGuiCol_WindowBg] = p.page;
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = p.card;
    c[ImGuiCol_TitleBg] = p.container_high;            // modal dialogs (save, library)
    c[ImGuiCol_TitleBgActive] = p.container_high;
    c[ImGuiCol_TitleBgCollapsed] = p.container_high;
    c[ImGuiCol_Border] = p.outline_variant;
    c[ImGuiCol_BorderShadow] = none;
    c[ImGuiCol_FrameBg] = p.container_high;
    c[ImGuiCol_FrameBgHovered] = p.container_highest;
    c[ImGuiCol_FrameBgActive] = p.container_highest;
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(p.outline.x, p.outline.y, p.outline.z, 0.35f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(p.outline.x, p.outline.y, p.outline.z, 0.55f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(p.outline.x, p.outline.y, p.outline.z, 0.75f);
    c[ImGuiCol_CheckMark] = p.primary;
    c[ImGuiCol_CheckboxSelectedBg] = p.primary;
    c[ImGuiCol_SliderGrab] = p.primary;
    c[ImGuiCol_SliderGrabActive] = p.primary;
    c[ImGuiCol_Button] = p.secondary_container;
    c[ImGuiCol_ButtonHovered] = mix(p.secondary_container, p.on_secondary_container, 0.08f);
    c[ImGuiCol_ButtonActive] = mix(p.secondary_container, p.on_secondary_container, 0.12f);
    c[ImGuiCol_Header] = p.container;
    c[ImGuiCol_HeaderHovered] = hover;
    c[ImGuiCol_HeaderActive] = p.container_highest;
    c[ImGuiCol_Separator] = p.outline_variant;
    c[ImGuiCol_SeparatorHovered] = p.primary;
    c[ImGuiCol_SeparatorActive] = p.primary;
    c[ImGuiCol_ResizeGrip] = none;
    c[ImGuiCol_ResizeGripHovered] = none;
    c[ImGuiCol_ResizeGripActive] = none;
    c[ImGuiCol_PlotLines] = p.primary;
    c[ImGuiCol_PlotLinesHovered] = p.error;
    c[ImGuiCol_PlotHistogram] = p.primary;
    c[ImGuiCol_PlotHistogramHovered] = p.primary;
    c[ImGuiCol_TableHeaderBg] = none;
    c[ImGuiCol_TableBorderStrong] = p.outline_variant;
    c[ImGuiCol_TableBorderLight] = p.outline_variant;
    c[ImGuiCol_TableRowBg] = none;
    c[ImGuiCol_TableRowBgAlt] = none;
    c[ImGuiCol_TextSelectedBg] = ImVec4(p.primary.x, p.primary.y, p.primary.z, 0.25f);
    c[ImGuiCol_NavCursor] = p.primary;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.4f);
}

// ---- components ---------------------------------------------------------------------------

// Scrolling panels nest a plain scroll region inside the rounded card, so the scrollbar sits within the
// card's padding instead of on (and over) its rounded edge. A small stack remembers what to close.
static bool s_panel_scroll[8];
static u32 s_panel_depth;

void theme_begin_panel(const char* id, ImVec2 size, bool scroll) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, g_pal.card);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s_scale, 14 * s_scale));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (scroll) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##scroll", ImVec2(0, 0), ImGuiChildFlags_None);
        ImGui::PopStyleVar();
    }
    if (s_panel_depth < 8) s_panel_scroll[s_panel_depth] = scroll;
    ++s_panel_depth;
}

void theme_end_panel() {
    --s_panel_depth;
    if (s_panel_depth < 8 && s_panel_scroll[s_panel_depth]) ImGui::EndChild();
    ImGui::EndChild();
}

static bool pill_button(const char* label, ImVec4 bg, ImVec4 fg) {
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mix(bg, fg, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, mix(bg, fg, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 100 * s_scale);   // clamped to half the height
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18 * s_scale, 7 * s_scale));
    ImGui::PushFont(g_fonts.medium, 0.0f);
    const bool pressed = ImGui::Button(label);
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return pressed;
}

bool theme_button_primary(const char* label) { return pill_button(label, g_pal.primary, g_pal.on_primary); }
bool theme_button(const char* label) { return pill_button(label, g_pal.secondary_container, g_pal.on_secondary_container); }

bool theme_tab(const char* label, bool selected) {
    const ImVec4 bg = selected ? g_pal.secondary_container : ImVec4(0, 0, 0, 0);
    const ImVec4 fg = selected ? g_pal.on_secondary_container : g_pal.on_surface_variant;
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? bg : ImVec4(fg.x, fg.y, fg.z, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, selected ? bg : ImVec4(fg.x, fg.y, fg.z, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 100 * s_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16 * s_scale, 6 * s_scale));
    ImGui::PushFont(g_fonts.medium, 0.0f);
    const bool pressed = ImGui::Button(label);
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return pressed;
}

// Material checkbox: outlined box when off, filled primary box with a contrasting tick when on, and with
// a dash when `mixed` (some of a group selected; a click then checks it).
bool theme_checkbox(const char* label, bool* v, bool mixed) {
    const bool on = *v || mixed;
    const ImVec4 fill = on ? g_pal.primary : ImVec4(0, 0, 0, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, fill);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, on ? mix(g_pal.primary, g_pal.on_primary, 0.08f)
                                                      : ImVec4(g_pal.on_surface.x, g_pal.on_surface.y, g_pal.on_surface.z, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, on ? mix(g_pal.primary, g_pal.on_primary, 0.12f)
                                                     : ImVec4(g_pal.on_surface.x, g_pal.on_surface.y, g_pal.on_surface.z, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_CheckboxSelectedBg, g_pal.primary);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, g_pal.on_primary);
    ImGui::PushStyleColor(ImGuiCol_Border, g_pal.on_surface_variant);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, on ? 0.0f : 2.0f * s_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * s_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f * s_scale, 3.0f * s_scale));
    const bool changed = ImGui::Checkbox(label, v);
    if (mixed && !*v) {     // ImGui's own mixed mark is a square; Material uses a horizontal dash
        const f32 box = ImGui::GetFrameHeight();
        const ImVec2 mn = ImGui::GetItemRectMin();
        const f32 cy = mn.y + box * 0.5f, half_h = 1.0f * s_scale;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(mn.x + box * 0.25f, cy - half_h), ImVec2(mn.x + box * 0.75f, cy + half_h),
                                                  ImGui::GetColorU32(g_pal.on_primary), half_h);
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(6);
    return changed;
}

void theme_progress(f32 fraction, f32 height, f32 width) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, height * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, s_dark ? g_pal.container_highest : g_pal.primary_container);
    ImGui::ProgressBar(fraction, ImVec2(width, height), "");
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

bool theme_warnings(const char* const* lines, u32 count, i32 button_line, const char* button_label) {
    if (count == 0) return false;
    const f32 pad = 12 * s_scale;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, g_pal.warn_container);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * s_scale, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * s_scale, 6 * s_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * s_scale, 3 * s_scale));    // button row height
    // Auto-sized to the content, so top and bottom padding are equal.
    ImGui::BeginChild("##warnings", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool clicked = false;
    for (u32 i = 0; i < count; ++i) {
        const bool has_button = (i32)i == button_line;
        if (has_button) ImGui::AlignTextToFramePadding();     // text centered on the button's height
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const f32 row_h = has_button ? ImGui::GetFrameHeight() : ImGui::GetTextLineHeight();
        dl->AddCircleFilled(ImVec2(c.x + 3 * s_scale, c.y + row_h * 0.5f), 2.5f * s_scale,
                            ImGui::GetColorU32(g_pal.on_warn_container), 8);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14 * s_scale);
        ImGui::TextColored(g_pal.on_warn_container, "%s", lines[i]);
        if (has_button) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(g_pal.warn.x, g_pal.warn.y, g_pal.warn.z, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(g_pal.warn.x, g_pal.warn.y, g_pal.warn.z, 0.20f));
            ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_warn_container);
            ImGui::PushStyleColor(ImGuiCol_Border, g_pal.on_warn_container);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 100 * s_scale);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * s_scale);
            ImGui::PushFont(g_fonts.medium, 0.0f);
            clicked = ImGui::Button(button_label);
            ImGui::PopFont();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(5);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * s_scale);     // breathing room below the card
    return clicked;
}

f32 theme_checkbox_height() { return ImGui::GetFontSize() + 6.0f * s_scale; }

ImU32 theme_heat(f32 t) { return ImGui::GetColorU32(mix(g_pal.heat_lo, g_pal.heat_hi, t)); }

// ---- icon buttons ---------------------------------------------------------------------------------

static void draw_icon(ImDrawList* dl, ThemeIcon icon, ImVec2 c, f32 r, ImU32 col, ImU32 bg) {
    const f32 line = 1.6f * s_scale;
    switch (icon) {
        case ICON_GEAR: {
            const f32 tooth_w = r * 0.42f;
            for (int k = 0; k < 8; ++k) {
                const f32 a = k * 0.785398f;
                const ImVec2 d(cosf(a), sinf(a)), p(-d.y, d.x);
                const f32 r0 = r * 0.55f, r1 = r * 1.0f, hw = tooth_w * 0.5f;
                dl->AddQuadFilled(ImVec2(c.x + d.x * r0 - p.x * hw, c.y + d.y * r0 - p.y * hw),
                                  ImVec2(c.x + d.x * r1 - p.x * hw, c.y + d.y * r1 - p.y * hw),
                                  ImVec2(c.x + d.x * r1 + p.x * hw, c.y + d.y * r1 + p.y * hw),
                                  ImVec2(c.x + d.x * r0 + p.x * hw, c.y + d.y * r0 + p.y * hw), col);
            }
            dl->AddCircleFilled(c, r * 0.76f, col, 24);
            dl->AddCircleFilled(c, r * 0.32f, bg, 16);
            break;
        }
        case ICON_THERMOMETER: {
            const f32 sw = r * 0.26f;
            dl->AddRect(ImVec2(c.x - sw, c.y - r), ImVec2(c.x + sw, c.y + r * 0.45f), col, sw, line);
            dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.58f), r * 0.42f, col, 20);
            dl->AddRectFilled(ImVec2(c.x - sw * 0.35f, c.y - r * 0.35f), ImVec2(c.x + sw * 0.35f, c.y + r * 0.5f), col);
            break;
        }
        case ICON_DRIVE: {
            dl->AddRect(ImVec2(c.x - r, c.y - r * 0.62f), ImVec2(c.x + r, c.y + r * 0.62f), col, r * 0.22f, line);
            dl->AddLine(ImVec2(c.x - r, c.y + r * 0.08f), ImVec2(c.x + r, c.y + r * 0.08f), col, line);
            dl->AddCircleFilled(ImVec2(c.x + r * 0.58f, c.y + r * 0.36f), r * 0.13f, col, 10);
            dl->AddLine(ImVec2(c.x - r * 0.62f, c.y + r * 0.36f), ImVec2(c.x + r * 0.05f, c.y + r * 0.36f), col, line);
            break;
        }
        case ICON_HISTORY: {        // clock face
            dl->AddCircle(c, r * 0.92f, col, 24, line);
            dl->AddLine(c, ImVec2(c.x, c.y - r * 0.55f), col, line);
            dl->AddLine(c, ImVec2(c.x + r * 0.42f, c.y + r * 0.22f), col, line);
            break;
        }
        case ICON_CHIP: {     // package with three pins per side
            const f32 b = r * 0.62f;
            dl->AddRect(ImVec2(c.x - b, c.y - b), ImVec2(c.x + b, c.y + b), col, r * 0.12f, line);
            dl->AddRectFilled(ImVec2(c.x - b * 0.42f, c.y - b * 0.42f), ImVec2(c.x + b * 0.42f, c.y + b * 0.42f), col, r * 0.06f);
            for (int k = -1; k <= 1; ++k) {
                const f32 o = k * b * 0.6f;
                dl->AddLine(ImVec2(c.x + o, c.y - b), ImVec2(c.x + o, c.y - r), col, line);
                dl->AddLine(ImVec2(c.x + o, c.y + b), ImVec2(c.x + o, c.y + r), col, line);
                dl->AddLine(ImVec2(c.x - b, c.y + o), ImVec2(c.x - r, c.y + o), col, line);
                dl->AddLine(ImVec2(c.x + b, c.y + o), ImVec2(c.x + r, c.y + o), col, line);
            }
            break;
        }
        case ICON_PULSE: {      // heartbeat line
            const ImVec2 pts[] = { ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.45f, c.y), ImVec2(c.x - r * 0.2f, c.y - r * 0.8f),
                                   ImVec2(c.x + r * 0.15f, c.y + r * 0.8f), ImVec2(c.x + r * 0.4f, c.y),
                                   ImVec2(c.x + r, c.y) };
            dl->AddPolyline(pts, (int)ARRAY_COUNT(pts), col, line);
            break;
        }
        case ICON_SLIDERS: {    // three horizontal sliders with their knobs at different positions
            const f32 knob[] = { 0.35f, -0.4f, 0.1f };
            for (int k = 0; k < 3; ++k) {
                const f32 y = c.y + (k - 1) * r * 0.62f, x = c.x + knob[k] * r;
                dl->AddLine(ImVec2(c.x - r, y), ImVec2(c.x + r, y), col, line);
                dl->AddCircleFilled(ImVec2(x, y), r * 0.24f, bg, 12);
                dl->AddCircle(ImVec2(x, y), r * 0.24f, col, 12, line);
            }
            break;
        }
        case ICON_DOWNLOAD: {   // arrow down onto a tray
            dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r * 0.35f), col, line);
            const ImVec2 head[] = { ImVec2(c.x - r * 0.5f, c.y - r * 0.12f), ImVec2(c.x, c.y + r * 0.38f),
                                    ImVec2(c.x + r * 0.5f, c.y - r * 0.12f) };
            dl->AddPolyline(head, 3, col, line);
            const ImVec2 tray[] = { ImVec2(c.x - r, c.y + r * 0.3f), ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y + r),
                                    ImVec2(c.x + r, c.y + r * 0.3f) };
            dl->AddPolyline(tray, 4, col, line);
            break;
        }
    }
}

f32 theme_icon_button_width(const char* label, f32 h) {
    if (!label) return h;                                   // icon only: a circle
    return h * 0.42f + h * 0.55f + h * 0.28f + ImGui::CalcTextSize(label).x + h * 0.5f;
}

bool theme_icon_button(const char* id, ThemeIcon icon, const char* label, f32 h, const char* tooltip, bool primary) {
    const f32 w = theme_icon_button_width(label, h);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    const ImVec4 base = primary ? g_pal.primary : g_pal.container_highest;
    const ImVec4 tint = primary ? g_pal.on_primary : g_pal.on_surface;
    const ImVec4 bg = held ? mix(base, tint, 0.12f) : hovered ? mix(base, tint, 0.08f) : base;
    const ImU32 bg32 = ImGui::GetColorU32(bg);
    const ImU32 fg = ImGui::GetColorU32(primary ? g_pal.on_primary : hovered ? g_pal.on_surface : g_pal.on_surface_variant);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg32, h * 0.5f);
    const f32 icon_r = h * 0.275f;
    const f32 icon_cx = label ? p.x + h * 0.42f + icon_r : p.x + w * 0.5f;
    draw_icon(dl, icon, ImVec2(icon_cx, p.y + h * 0.5f), icon_r, fg, bg32);
    if (label) {
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(icon_cx + icon_r + h * 0.28f, p.y + (h - ts.y) * 0.5f), fg, label);
    }
    if (tooltip) theme_help(tooltip);
    return clicked;
}

void theme_help(const char* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void theme_group_begin(const char* title, f32 width) {
    ImGui::PushStyleColor(ImGuiCol_Border, g_pal.outline_variant);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetFontSize() * 0.8f, ImGui::GetFontSize() * 0.6f));
    ImGui::BeginChild(title, ImVec2(width, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
}

void theme_group_end() {
    ImGui::EndChild();
    ImGui::Spacing();
}
