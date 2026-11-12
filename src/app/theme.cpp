#include "app/theme.h"
#include <windows.h>
#include <math.h>
#include <string.h>
#include <wchar.h>

ThemeFonts g_fonts;
Palette g_pal;

static f32 s_scale = 1.0f;

static constexpr ImVec4 rgb(u32 hex, f32 a = 1.0f) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, a);
}

static ImVec4 mix(ImVec4 a, ImVec4 b, f32 t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// ---- theme table ---------------------------------------------------------------------------------------
// One row per theme, colors as 0xRRGGBB. The rows hold the roles that carry a scheme's identity; the rest of
// the palette is derived in make_palette(). Sources and the role mapping: docs/notes/themes.md.
//   page    window background            line     decorative lines: cards, groups, tables, graph grid
//   card    panels, cards                border   control outlines (buttons, checkboxes; 3:1 where possible)
//   raised  frames (inputs), popups      text / dim  body and secondary text
//   title   group box titles             primary  buttons, checks, sliders, focus; secondary: selected tabs
//   ok / warn / error                    series   graph hues in GraphKind order: load, clock, power, temperature,
//                                                 memory, latency, disk, compute, other
// Light and Dark are deliberately plain: neutral grays, one blue accent (also the selection color), titles in
// the text color. Text and dim pass WCAG AA (4.5:1) on the card in every theme except Solarized Dark's dim
// (base0, 4.75 on base03 but 4.1 on base02); accents and status colors are the schemes' own (3:1 and up,
// lower for a few pastel hues on the light schemes).
struct ThemeDef {
    const char* name;
    const char* id;
    bool dark;
    u32 page, card, raised, line, border, text, dim, title, primary, secondary, ok, warn, error;
    u32 series[THEME_SERIES];
};

static const ThemeDef THEMES[] = {
    { "Light", "light", false,
      0xF3F3F3, 0xFFFFFF, 0xF7F7F7, 0xE0E0E0, 0x8A8A8A, 0x1F1F1F, 0x5E5E5E, 0x1F1F1F, 0x005FB8, 0x005FB8, 0x0F7B0F, 0x9D5D00, 0xC42B1C,
      { 0x0E7490, 0x7C3AED, 0xA16207, 0xE11D48, 0xC026D3, 0x2563EB, 0x15803D, 0xEA580C, 0x64748B } },
    { "Dark", "dark", true,
      0x181818, 0x1F1F1F, 0x272727, 0x333333, 0x6E6E6E, 0xE6E6E6, 0xA3A3A3, 0xE6E6E6, 0x4CA2F5, 0x4CA2F5, 0x6CCB5F, 0xE5B33E, 0xFF6B6B,
      { 0x22D3EE, 0xA78BFA, 0xFACC15, 0xFB7185, 0xE879F9, 0x60A5FA, 0x4ADE80, 0xFB923C, 0x94A3B8 } },
    { "Solarized Dark", "solarized-dark", true,
      0x00212B, 0x002B36, 0x073642, 0x0E4452, 0x657B83, 0x93A1A1, 0x839496, 0x268BD2, 0x268BD2, 0x6C71C4, 0x859900, 0xB58900, 0xDC322F,
      { 0x2AA198, 0x6C71C4, 0xB58900, 0xDC322F, 0xD33682, 0x268BD2, 0x859900, 0xCB4B16, 0x839496 } },
    { "Solarized Light", "solarized-light", false,
      0xEEE8D5, 0xFDF6E3, 0xF5EFDC, 0xD9D2BC, 0x839496, 0x073642, 0x586E75, 0x268BD2, 0x268BD2, 0x6C71C4, 0x859900, 0xB58900, 0xDC322F,
      { 0x2AA198, 0x6C71C4, 0xB58900, 0xDC322F, 0xD33682, 0x268BD2, 0x859900, 0xCB4B16, 0x657B83 } },
    { "Monokai", "monokai", true,
      0x1E1F1C, 0x272822, 0x3E3D32, 0x3E3D32, 0x75715E, 0xF8F8F2, 0xA6A28C, 0x66D9EF, 0x66D9EF, 0xAE81FF, 0xA6E22E, 0xFD971F, 0xF92672,
      { 0x66D9EF, 0xAE81FF, 0xE6DB74, 0xF92672, 0xD670D6, 0x7AA6FF, 0xA6E22E, 0xFD971F, 0xA6A28C } },
    { "Dracula", "dracula", true,
      0x21222C, 0x282A36, 0x343746, 0x44475A, 0x6272A4, 0xF8F8F2, 0xA4ACCB, 0xBD93F9, 0xBD93F9, 0xFF79C6, 0x50FA7B, 0xFFB86C, 0xFF5555,
      { 0x8BE9FD, 0xBD93F9, 0xF1FA8C, 0xFF5555, 0xFF79C6, 0x8DA2F7, 0x50FA7B, 0xFFB86C, 0xA4ACCB } },
    { "Nord", "nord", true,
      0x272C36, 0x2E3440, 0x3B4252, 0x434C5E, 0x7B88A1, 0xD8DEE9, 0xA3ACBD, 0x88C0D0, 0x88C0D0, 0x81A1C1, 0xA3BE8C, 0xEBCB8B, 0xBF616A,
      { 0x88C0D0, 0x81A1C1, 0xEBCB8B, 0xBF616A, 0xB48EAD, 0x5E81AC, 0xA3BE8C, 0xD08770, 0x8FBCBB } },
    { "Gruvbox Dark", "gruvbox-dark", true,
      0x1D2021, 0x282828, 0x3C3836, 0x3C3836, 0x7C6F64, 0xEBDBB2, 0xA89984, 0xFABD2F, 0xFABD2F, 0x8EC07C, 0xB8BB26, 0xFE8019, 0xFB4934,
      { 0x8EC07C, 0x83A598, 0xFABD2F, 0xFB4934, 0xD3869B, 0x458588, 0xB8BB26, 0xFE8019, 0x928374 } },
    { "One Dark", "one-dark", true,
      0x21252B, 0x282C34, 0x2C313A, 0x3E4451, 0x6E7787, 0xD7DAE0, 0xABB2BF, 0x61AFEF, 0x61AFEF, 0xC678DD, 0x98C379, 0xE5C07B, 0xE06C75,
      { 0x56B6C2, 0xC678DD, 0xE5C07B, 0xE06C75, 0xD373A9, 0x61AFEF, 0x98C379, 0xD19A66, 0xABB2BF } },
    { "Catppuccin Mocha", "catppuccin-mocha", true,
      0x181825, 0x1E1E2E, 0x313244, 0x313244, 0x6C7086, 0xCDD6F4, 0xA6ADC8, 0xCBA6F7, 0xCBA6F7, 0xF5C2E7, 0xA6E3A1, 0xFAB387, 0xF38BA8,
      { 0x89DCEB, 0xCBA6F7, 0xF9E2AF, 0xF38BA8, 0xF5C2E7, 0x89B4FA, 0xA6E3A1, 0xFAB387, 0x9399B2 } },
    { "Catppuccin Latte", "catppuccin-latte", false,
      0xE6E9EF, 0xEFF1F5, 0xDCE0E8, 0xCCD0DA, 0x7C7F93, 0x4C4F69, 0x5C5F77, 0x8839EF, 0x8839EF, 0x1E66F5, 0x40A02B, 0xFE640B, 0xD20F39,
      { 0x179299, 0x8839EF, 0xDF8E1D, 0xD20F39, 0xEA76CB, 0x1E66F5, 0x40A02B, 0xFE640B, 0x7C7F93 } },
    { "GitHub Light", "github-light", false,
      0xF6F8FA, 0xFFFFFF, 0xF6F8FA, 0xD8DEE4, 0x8C959F, 0x1F2328, 0x59636E, 0x1F2328, 0x0969DA, 0x0969DA, 0x1A7F37, 0x9A6700, 0xD1242F,
      { 0x1B7C83, 0x8250DF, 0x9A6700, 0xCF222E, 0xBF3989, 0x0969DA, 0x1A7F37, 0xBC4C00, 0x6E7781 } },
};
static_assert(ARRAY_COUNT(THEMES) < 255, "ThemeMode is a u8");

static f32 luminance(ImVec4 c) {     // WCAG relative luminance
    auto lin = [](f32 v) { return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * lin(c.x) + 0.7152f * lin(c.y) + 0.0722f * lin(c.z);
}
static f32 contrast(ImVec4 a, ImVec4 b) {
    const f32 la = luminance(a), lb = luminance(b);
    return la > lb ? (la + 0.05f) / (lb + 0.05f) : (lb + 0.05f) / (la + 0.05f);
}

static Palette make_palette(const ThemeDef* d) {
    Palette p;
    const f32 dk = d->dark ? 1.0f : 0.0f;
    p.page = rgb(d->page);
    p.card = rgb(d->card);
    p.container = rgb(d->raised);
    p.on_surface = rgb(d->text);
    p.on_surface_variant = rgb(d->dim);
    p.container_high = mix(p.container, p.on_surface, 0.06f);       // frame hover, headers
    p.container_highest = mix(p.container, p.on_surface, 0.12f);    // frame active, progress track
    p.outline = rgb(d->border);
    p.outline_variant = rgb(d->line);
    p.group_title = rgb(d->title);
    // Text on a filled accent: the card or the text color, whichever contrasts more.
    auto on = [&](ImVec4 fill) { return contrast(p.card, fill) >= contrast(p.on_surface, fill) ? p.card : p.on_surface; };
    p.primary = rgb(d->primary);
    p.on_primary = on(p.primary);
    p.primary_container = mix(p.card, p.primary, 0.14f + 0.08f * dk);
    p.on_primary_container = p.on_surface;
    p.secondary = rgb(d->secondary);
    p.on_secondary = on(p.secondary);
    p.secondary_container = mix(p.card, p.secondary, 0.13f + 0.07f * dk);
    p.on_secondary_container = p.on_surface;
    p.ok = rgb(d->ok);
    p.error = rgb(d->error);
    p.warn = rgb(d->warn);
    p.warn_container = mix(p.card, p.warn, 0.14f + 0.02f * dk);
    p.on_warn_container = p.on_surface;
    p.heat_lo = mix(p.card, p.ok, 0.25f + 0.05f * dk);
    p.heat_hi = mix(p.card, p.error, 0.25f + 0.07f * dk);
    for (u32 i = 0; i < THEME_SERIES; ++i) p.series[i] = d->series[i];
    return p;
}

// ---- theme choice --------------------------------------------------------------------------------------

static ThemeMode s_mode = THEME_SYSTEM;
static u32 s_cur = 0;                   // effective theme: row in THEMES
static bool s_changed = false;

// Windows "Choose your app mode" (Settings > Personalization > Colors). Missing value = light.
static bool system_prefers_dark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

static u32 row_of(ThemeMode t) {        // THEMES row; System resolves to Light or Dark
    if (t == THEME_SYSTEM) t = system_prefers_dark() ? THEME_DARK : THEME_LIGHT;
    return (u32)t - 1 < ARRAY_COUNT(THEMES) ? (u32)t - 1 : 0;
}

static void set_current(u32 row) {
    if (row != s_cur) { s_cur = row; s_changed = true; }
}

u32 theme_count() { return 1 + ARRAY_COUNT(THEMES); }
const char* theme_name(ThemeMode t) { return t == THEME_SYSTEM ? "Follow Windows" : t < theme_count() ? THEMES[t - 1].name : "?"; }
const char* theme_id(ThemeMode t) { return t == THEME_SYSTEM ? "system" : t < theme_count() ? THEMES[t - 1].id : "system"; }

bool theme_find(const char* id, ThemeMode* out) {
    for (u32 i = 0; i < theme_count(); ++i)
        if (!_stricmp(id, theme_id((ThemeMode)i))) { *out = (ThemeMode)i; return true; }
    return false;
}

bool theme_is_dark() { return THEMES[s_cur].dark; }
ThemeMode theme_mode() { return s_mode; }

void theme_set_mode(ThemeMode mode) {
    s_mode = mode < theme_count() ? mode : THEME_SYSTEM;
    set_current(row_of(s_mode));
}

void theme_system_changed() {
    if (s_mode == THEME_SYSTEM) set_current(row_of(THEME_SYSTEM));
}
bool theme_take_change() {
    const bool c = s_changed;
    s_changed = false;
    return c;
}

static void to_rgb(u32 hex, f32 out[3]) {
    const ImVec4 c = rgb(hex);
    out[0] = c.x; out[1] = c.y; out[2] = c.z;
}
void theme_bg_rgb(f32 out[3]) { to_rgb(THEMES[s_cur].page, out); }
void theme_text_rgb(f32 out[3]) { to_rgb(THEMES[s_cur].text, out); }

u32 theme_series_rgb(ThemeMode t, u32 kind) {
    const u32 row = t != THEME_SYSTEM && t < theme_count() ? (u32)t - 1 : 0;     // no registry read: any thread
    return THEMES[row].series[kind < THEME_SERIES ? kind : THEME_SERIES - 1];
}

static void draw_swatch(ImDrawList* dl, ImVec2 a, ImVec2 b, const ThemeDef* d) {
    const f32 r = 2.0f * s_scale, h = b.y - a.y;
    dl->AddRectFilled(a, b, ImGui::GetColorU32(rgb(d->card)), r);
    // A line of "text" over two accent dots.
    const f32 pad = h * 0.22f, dot = h * 0.16f;
    dl->AddRectFilled(ImVec2(a.x + pad, a.y + pad), ImVec2(b.x - pad, a.y + pad + h * 0.14f), ImGui::GetColorU32(rgb(d->text)), h * 0.07f);
    const f32 cy = b.y - pad - dot;
    dl->AddCircleFilled(ImVec2(a.x + pad + dot, cy), dot, ImGui::GetColorU32(rgb(d->primary)), 12);
    dl->AddCircleFilled(ImVec2(a.x + pad + dot * 3.6f, cy), dot, ImGui::GetColorU32(rgb(d->secondary != d->primary ? d->secondary : d->ok)), 12);
    dl->AddCircleFilled(ImVec2(a.x + pad + dot * 6.2f, cy), dot, ImGui::GetColorU32(rgb(d->error)), 12);
}

f32 theme_swatch(ImDrawList* dl, ImVec2 pos, f32 h, ThemeMode t) {
    const f32 w = floorf(h * 1.9f);
    const ImVec2 a(floorf(pos.x), floorf(pos.y)), b(a.x + w, floorf(pos.y + h));
    if (t == THEME_SYSTEM) {    // Light | Dark
        const f32 mid = floorf(a.x + w * 0.5f);
        dl->PushClipRect(a, ImVec2(mid, b.y), true);
        draw_swatch(dl, a, b, &THEMES[0]);
        dl->PopClipRect();
        dl->PushClipRect(ImVec2(mid, a.y), b, true);
        draw_swatch(dl, a, b, &THEMES[1]);
        dl->PopClipRect();
    } else {
        draw_swatch(dl, a, b, &THEMES[row_of(t)]);
    }
    dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), 2.0f * s_scale);
    return w;
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
    g_pal = make_palette(&THEMES[s_cur]);
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
    c[ImGuiCol_PopupBg] = theme_is_dark() ? p.container : p.card;     // raised one step above the cards
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
        ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(g_pal.group_title), title, title_end);
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
