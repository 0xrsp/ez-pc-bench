#include "app/theme.h"
#include <windows.h>
#include <math.h>
#include <string.h>
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

// Palette "graphite / cyan / orchid". Dark is the main look: blue-black surfaces in three steps (page < card <
// container = raised: frames, popups), a cyan primary for actions and data (buttons, checkboxes, sliders,
// progress, graphs) and an orchid secondary for navigation and selection (tabs, toggles, selected list rows).
// Separation comes from 1 px outlines, not tonal fills. Contrast (WCAG 2.x) is measured against the card color.
//
//  role                     dark      light     use                                         dark / light
//  page                     #0B0E14   #F2F4F8   window background
//  card                     #11161F   #FFFFFF   panels, cards
//  container                #161C27   #EFF2F6   raised: frames, popups (dark)
//  container_high           #1B2230   #E7EBF1   frame hover, headers
//  container_highest        #232C3D   #DCE2EA   frame active, progress track
//  on_surface               #E4E9F2   #121826   body text                                   15.9 / 17.4 : 1
//  on_surface_variant       #9BA6B9   #4A5466   secondary text, labels                       7.4 / 7.6 : 1
//  outline                  #5D6A80   #7C8799   button / checkbox borders                    3.3 / 3.6 : 1
//  outline_variant          #2D3749   #CDD4DE   card, group and table lines (decorative)
//  primary                  #3CCFEA   #00779C   filled buttons, checks, sliders, data        10.0 / 5.0 : 1
//  on_primary               #002A33   #FFFFFF   text on primary
//  primary_container        #0C3440   #D3F1FA   selected chip fill
//  on_primary_container     #B4EDFA   #00303E
//  secondary                #D07BFF   #9C2BC4   selected tab border / text accent             7.0 / 5.9 : 1
//  on_secondary             #2B0740   #FFFFFF
//  secondary_container      #2C1A3B   #F5E3FC   selected tab / row fill
//  on_secondary_container   #F0D6FF   #3B0B4F
//  ok                       #5BD68A   #17784A   pass, done, good                              9.6 / 5.4 : 1
//  error                    #FF6E6E   #C42B2B   fail, errors                                  6.6 / 5.6 : 1
//  warn                     #FFB648   #9A5800   caution, notes                               10.4 / 5.5 : 1
//  warn_container           #33240C   #FFEBCC   warning banner
//  on_warn_container        #FFE3B3   #3D2400   banner text                                  13.9 / 13.0 : 1 (on banner)
//  heat_lo .. heat_hi       #123D2B..#5C1C24   #CBF2DA..#FFD8D8   matrix cells (green .. red)
static Palette light_palette() {
    Palette p;
    p.page = rgb(0xF2F4F8);
    p.card = rgb(0xFFFFFF);
    p.container = rgb(0xEFF2F6);
    p.container_high = rgb(0xE7EBF1);
    p.container_highest = rgb(0xDCE2EA);
    p.on_surface = rgb(0x121826);
    p.on_surface_variant = rgb(0x4A5466);
    p.outline = rgb(0x7C8799);
    p.outline_variant = rgb(0xCDD4DE);
    p.primary = rgb(0x00779C);
    p.on_primary = rgb(0xFFFFFF);
    p.primary_container = rgb(0xD3F1FA);
    p.on_primary_container = rgb(0x00303E);
    p.secondary = rgb(0x9C2BC4);
    p.on_secondary = rgb(0xFFFFFF);
    p.secondary_container = rgb(0xF5E3FC);
    p.on_secondary_container = rgb(0x3B0B4F);
    p.ok = rgb(0x17784A);
    p.error = rgb(0xC42B2B);
    p.warn = rgb(0x9A5800);
    p.warn_container = rgb(0xFFEBCC);
    p.on_warn_container = rgb(0x3D2400);
    p.heat_lo = rgb(0xCBF2DA);
    p.heat_hi = rgb(0xFFD8D8);
    return p;
}

static Palette dark_palette() {
    Palette p;
    p.page = rgb(0x0B0E14);
    p.card = rgb(0x11161F);
    p.container = rgb(0x161C27);
    p.container_high = rgb(0x1B2230);
    p.container_highest = rgb(0x232C3D);
    p.on_surface = rgb(0xE4E9F2);
    p.on_surface_variant = rgb(0x9BA6B9);
    p.outline = rgb(0x5D6A80);
    p.outline_variant = rgb(0x2D3749);
    p.primary = rgb(0x3CCFEA);
    p.on_primary = rgb(0x002A33);
    p.primary_container = rgb(0x0C3440);
    p.on_primary_container = rgb(0xB4EDFA);
    p.secondary = rgb(0xD07BFF);
    p.on_secondary = rgb(0x2B0740);
    p.secondary_container = rgb(0x2C1A3B);
    p.on_secondary_container = rgb(0xF0D6FF);
    p.ok = rgb(0x5BD68A);
    p.error = rgb(0xFF6E6E);
    p.warn = rgb(0xFFB648);
    p.warn_container = rgb(0x33240C);
    p.on_warn_container = rgb(0xFFE3B3);
    p.heat_lo = rgb(0x123D2B);
    p.heat_hi = rgb(0x5C1C24);
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
    // Small radii, 1 px outlines on frames (inputs, combos, buttons, checkboxes), cards and popups.
    s.WindowRounding = 0;
    s.ChildRounding = 4;
    s.FrameRounding = 3;
    s.PopupRounding = 4;
    s.ScrollbarRounding = 2;
    s.GrabRounding = 2;
    s.TabRounding = 3;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 1;
    s.SeparatorTextBorderSize = 1;
    s.DisabledAlpha = 0.38f;
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
    s.FontSizeBase = roundf(16.5f * scale) / scale;    // whole-pixel body text (17 px at 100 %)

    ImVec4* c = s.Colors;
    const ImVec4 none = { 0, 0, 0, 0 };
    const ImVec4 t = p.on_surface;     // state layers: on-surface at low alpha
    c[ImGuiCol_Text] = p.on_surface;
    c[ImGuiCol_TextDisabled] = p.on_surface_variant;
    c[ImGuiCol_WindowBg] = p.page;
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = s_dark ? p.container : p.card;     // raised one step above the cards
    c[ImGuiCol_TitleBg] = p.container_high;            // modal dialogs (save, library)
    c[ImGuiCol_TitleBgActive] = p.container_high;
    c[ImGuiCol_TitleBgCollapsed] = p.container_high;
    c[ImGuiCol_Border] = mix(p.outline_variant, p.outline, 0.4f);     // frames, popups; cards use outline_variant
    c[ImGuiCol_BorderShadow] = none;
    c[ImGuiCol_FrameBg] = p.container;
    c[ImGuiCol_FrameBgHovered] = p.container_high;
    c[ImGuiCol_FrameBgActive] = p.container_highest;
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(p.outline.x, p.outline.y, p.outline.z, 0.45f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(p.outline.x, p.outline.y, p.outline.z, 0.7f);
    c[ImGuiCol_ScrollbarGrabActive] = p.outline;
    c[ImGuiCol_CheckMark] = p.primary;
    c[ImGuiCol_CheckboxSelectedBg] = p.primary;
    c[ImGuiCol_SliderGrab] = p.primary;
    c[ImGuiCol_SliderGrabActive] = mix(p.primary, p.on_surface, 0.25f);
    c[ImGuiCol_Button] = none;                         // outlined (frame border); combo arrows sit on the frame
    c[ImGuiCol_ButtonHovered] = ImVec4(t.x, t.y, t.z, 0.08f);
    c[ImGuiCol_ButtonActive] = ImVec4(t.x, t.y, t.z, 0.14f);
    c[ImGuiCol_Header] = p.container_high;             // framed headers, selected list rows
    c[ImGuiCol_HeaderHovered] = mix(p.container_high, p.on_surface, 0.06f);
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
    c[ImGuiCol_TableRowBgAlt] = ImVec4(t.x, t.y, t.z, 0.03f);     // tables with RowBg: faint zebra
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
    ImGui::PushStyleColor(ImGuiCol_Border, g_pal.outline_variant);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s_scale, 14 * s_scale));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
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

// Rectangular button: `border` 1 px outline (alpha 0 = none), small radius from the style.
static bool rect_button(const char* label, ImVec4 bg, ImVec4 fg, ImVec4 border, ImVec2 pad) {
    const bool filled = bg.w > 0.0f;
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, filled ? mix(bg, fg, 0.10f) : ImVec4(fg.x, fg.y, fg.z, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, filled ? mix(bg, fg, 0.18f) : ImVec4(fg.x, fg.y, fg.z, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, pad);
    ImGui::PushFont(g_fonts.medium, 0.0f);
    const bool pressed = ImGui::Button(label);
    ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    return pressed;
}

static ImVec2 button_padding() { return ImVec2(16 * s_scale, 7 * s_scale); }

bool theme_button_primary(const char* label) {
    return rect_button(label, g_pal.primary, g_pal.on_primary, g_pal.primary, button_padding());
}
bool theme_button(const char* label) {
    return rect_button(label, ImVec4(0, 0, 0, 0), g_pal.on_surface, g_pal.outline, button_padding());
}

// Segmented tab / toggle: outlined; selected = secondary container fill with a secondary outline.
bool theme_tab(const char* label, bool selected) {
    return rect_button(label, selected ? g_pal.secondary_container : ImVec4(0, 0, 0, 0),
                       selected ? g_pal.on_secondary_container : g_pal.on_surface_variant,
                       selected ? g_pal.secondary : g_pal.outline_variant, ImVec2(14 * s_scale, 6 * s_scale));
}

// Checkbox: outlined box when off, filled primary box with a contrasting tick when on, and with a dash when
// `mixed` (some of a group selected; a click then checks it).
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
    ImGui::PushStyleColor(ImGuiCol_Border, on ? g_pal.primary : g_pal.on_surface_variant);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f * s_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f * s_scale);
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
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0f * s_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, g_pal.container_highest);
    ImGui::ProgressBar(fraction, ImVec2(width, height), "");
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

bool theme_warnings(const char* const* lines, u32 count, i32 button_line, const char* button_label) {
    if (count == 0) return false;
    const f32 pad = 12 * s_scale, stripe = 3 * s_scale;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, g_pal.warn_container);
    ImGui::PushStyleColor(ImGuiCol_Border, mix(g_pal.warn_container, g_pal.warn, 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 3 * s_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * s_scale + stripe, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * s_scale, 6 * s_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * s_scale, 3 * s_scale));    // button row height
    // Auto-sized to the content, so top and bottom padding are equal.
    ImGui::BeginChild("##warnings", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {   // warning-colored stripe along the left edge
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        dl->PushClipRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y));     // the window clips off half its padding
        dl->AddRectFilled(wp, ImVec2(wp.x + stripe, wp.y + ws.y), ImGui::GetColorU32(g_pal.warn), 3 * s_scale,
                          ImDrawFlags_RoundLeft);
        dl->PopClipRect();
    }
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
            ImGui::PushFont(g_fonts.medium, 0.0f);
            clicked = ImGui::Button(button_label);
            ImGui::PopFont();
            ImGui::PopStyleColor(5);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
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
    }
}

f32 theme_icon_button_width(const char* label, f32 h) {
    if (!label) return h;                                   // icon only: a square
    return h * 0.36f + h * 0.55f + h * 0.28f + ImGui::CalcTextSize(label).x + h * 0.42f;
}

bool theme_icon_button(const char* id, ThemeIcon icon, const char* label, f32 h, const char* tooltip) {
    const f32 w = theme_icon_button_width(label, h);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    // Outlined like theme_button; the slot behind the icon's cut-outs is the surface under the button.
    const ImVec4 surface = ImGui::GetStyleColorVec4(ImGuiCol_ChildBg).w > 0.0f ? ImGui::GetStyleColorVec4(ImGuiCol_ChildBg) : g_pal.page;
    const ImVec4 bg = held ? mix(surface, g_pal.on_surface, 0.14f) : hovered ? mix(surface, g_pal.on_surface, 0.08f) : surface;
    const ImU32 bg32 = ImGui::GetColorU32(bg), fg = ImGui::GetColorU32(hovered ? g_pal.on_surface : g_pal.on_surface_variant);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const f32 r = ImGui::GetStyle().FrameRounding;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg32, r);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(g_pal.outline), r);
    const f32 icon_r = h * 0.275f;
    const f32 icon_cx = label ? p.x + h * 0.36f + icon_r : p.x + w * 0.5f;
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

// Group box: an auto-height child without its own border; the frame is drawn on the parent's draw list after
// the child ends (its height is known then), with a gap in the top line where the title sits. The parent's draw
// list renders below the child, and the child has no background, so frame and title stay visible.
struct GroupBox {
    ImVec2 title_min, title_max;    // gap in the top line (screen space)
};
static GroupBox s_groups[8];
static u32 s_group_depth;

void theme_group_begin(const char* title, f32 width, const char* help) {
    const f32 fs = ImGui::GetFontSize();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const char* title_end = strstr(title, "##");     // "Name##id": only "Name" is shown
    if (!title_end) title_end = title + strlen(title);
    ImGui::PushFont(g_fonts.medium, 0.0f);
    const f32 line_h = ImGui::GetTextLineHeight();
    const f32 title_w = title_end > title ? ImGui::CalcTextSize(title, title_end).x : 0.0f;
    const ImVec2 at(pos.x + fs * 0.75f, pos.y);
    if (title_w > 0.0f)
        ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(g_pal.primary), title, title_end);
    ImGui::PopFont();
    GroupBox g = { ImVec2(at.x - fs * 0.3f, pos.y), ImVec2(at.x + title_w + fs * 0.3f, pos.y + line_h) };
    if (title_w <= 0.0f) g.title_max.x = g.title_min.x;
    if (s_group_depth < ARRAY_COUNT(s_groups)) s_groups[s_group_depth] = g;
    ++s_group_depth;
    if (help && title_w > 0.0f && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseHoveringRect(g.title_min, g.title_max)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(fs * 26.0f);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // The frame's top line runs through the middle of the title; the content starts below the title. Without a
    // title ("##id") the frame starts at the cursor.
    const f32 head = title_w > 0.0f ? floorf(line_h * 0.5f) : 0.0f;
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + head));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 0.8f, fs * 0.6f));
    ImGui::BeginChild(title[0] ? title : "##group", ImVec2(width, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (head > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ceilf(line_h * 0.5f) - fs * 0.15f);
}

void theme_group_end() {
    ImGui::EndChild();
    if (s_group_depth) --s_group_depth;
    if (s_group_depth < ARRAY_COUNT(s_groups)) {
        const GroupBox& g = s_groups[s_group_depth];
        // Pixel centers for crisp 1 px lines; small rounded corners.
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const f32 x0 = floorf(mn.x) + 0.5f, y0 = floorf(mn.y) + 0.5f, x1 = floorf(mx.x) - 0.5f, y1 = floorf(mx.y) - 0.5f;
        const f32 r = 3.0f * s_scale;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const f32 gap0 = g.title_min.x < x0 + r ? x0 + r : g.title_min.x, gap1 = g.title_max.x > x1 - r ? x1 - r : g.title_max.x;
        dl->PathLineTo(ImVec2(gap1, y0));
        dl->PathArcToFast(ImVec2(x1 - r, y0 + r), r, 9, 12);
        dl->PathArcToFast(ImVec2(x1 - r, y1 - r), r, 0, 3);
        dl->PathArcToFast(ImVec2(x0 + r, y1 - r), r, 3, 6);
        dl->PathArcToFast(ImVec2(x0 + r, y0 + r), r, 6, 9);
        dl->PathLineTo(ImVec2(gap0, y0));
        dl->PathStroke(ImGui::GetColorU32(g_pal.outline_variant), 1.0f);
    }
    ImGui::Spacing();
}
