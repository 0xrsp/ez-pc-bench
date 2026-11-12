#pragma once
// Themes with Inter: plain Light / Dark (neutral grays, one blue accent) plus named color schemes (Solarized,
// Monokai, Dracula, Nord, ...), 1 px outlines, small radii. One static table in theme.cpp; each row maps the
// scheme's published colors onto the palette roles below.
#include "core/types.h"
#include "imgui.h"

struct ThemeFonts {
    ImFont* regular;    // Inter Regular (body)
    ImFont* medium;     // Inter Medium (titles, labels, buttons)
    ImFont* mono;       // Cascadia Mono (result numbers), falls back to Consolas / Inter
};
extern ThemeFonts g_fonts;

constexpr u32 THEME_SERIES = 9;     // graph hues per theme, GraphKind order (graph.h)

// Color roles of the current theme.
struct Palette {
    ImVec4 page;                    // window background
    ImVec4 card;                    // cards / panels
    ImVec4 container, container_high, container_highest;
    ImVec4 on_surface, on_surface_variant;
    ImVec4 outline, outline_variant;
    ImVec4 group_title;             // group box titles
    ImVec4 primary, on_primary, primary_container, on_primary_container;          // actions, data
    ImVec4 secondary, on_secondary, secondary_container, on_secondary_container;  // navigation, selection
    ImVec4 ok, error, warn, warn_container, on_warn_container;
    ImVec4 heat_lo, heat_hi;        // matrix cell colors
    u32 series[THEME_SERIES];       // graph line colors 0xRRGGBB (graph.cpp maps units to them)
};
extern Palette g_pal;

void theme_load_fonts();                    // Inter from the exe's resources (Segoe UI fallback)
void theme_apply_style(f32 dpi_scale);      // uses the current theme

// Theme choice: 0 = follow Windows (plain Light or Dark from the app mode), 1 = Light, 2 = Dark, then the named
// themes (theme_count() in all). Saved in the ini by id ("system", "light", "dark", "solarized-dark", ...).
enum ThemeMode : u8 { THEME_SYSTEM, THEME_LIGHT, THEME_DARK };     // values past THEME_DARK: named themes
u32 theme_count();
const char* theme_name(ThemeMode t);        // "Follow Windows", "Light", "Solarized Dark", ...
const char* theme_id(ThemeMode t);          // "system", "light", "solarized-dark", ...
bool theme_find(const char* id, ThemeMode* out);    // by id, case-insensitive
bool theme_is_dark();                       // the effective theme: title bar mode, dark / light variants
ThemeMode theme_mode();
void theme_set_mode(ThemeMode mode);        // takes effect between frames
void theme_system_changed();                // call on WM_SETTINGCHANGE "ImmersiveColorSet"
bool theme_take_change();                   // true once after the effective theme changed
void theme_bg_rgb(f32 out[3]);              // page color (window clear / title bar)
void theme_text_rgb(f32 out[3]);            // body text color (title bar text)
u32 theme_series_rgb(ThemeMode t, u32 kind);    // a theme's graph hue, any thread (the overlay uses Dark's)
// Small preview of theme `t` at `pos`, `h` tall (combo entries): its card color with text and accent marks.
// Returns the width. Follow Windows shows Light and Dark side by side.
f32 theme_swatch(ImDrawList* dl, ImVec2 pos, f32 h, ThemeMode t);

f32 theme_font_size(f32 mult);              // base font size * mult, snapped to whole pixels
// Result numbers. Cascadia Mono looks larger than Inter at the same size, so it's drawn a bit smaller.
inline void theme_push_numbers() { ImGui::PushFont(g_fonts.mono, theme_font_size(0.92f)); }

// Outlined card. Always pair with theme_end_panel().
void theme_begin_panel(const char* id, ImVec2 size, bool scroll = true);
void theme_end_panel();

bool theme_button_primary(const char* label);   // filled primary
bool theme_button(const char* label);           // outlined
bool theme_checkbox(const char* label, bool* v, bool mixed = false);   // mixed: dash, "some selected"
f32 theme_checkbox_height();                    // box height, for centering next to taller items
bool theme_tab(const char* label, bool selected);  // segmented tab, returns true when clicked
void theme_progress(f32 fraction, f32 height, f32 width = -FLT_MIN);  // linear indicator (default: full width)
// One warning banner, one line per warning; line `button_line` (or -1) gets a button. True when clicked.
bool theme_warnings(const char* const* lines, u32 count, i32 button_line, const char* button_label);
ImU32 theme_heat(f32 t);                        // 0 = low (green) .. 1 = high (red)

// Small outlined button with a drawn icon and an optional label, `h` tall (fits in a header row). The icons are
// vector-drawn (Inter has no icon glyphs). `tooltip` shows on hover.
enum ThemeIcon : u8 { ICON_GEAR, ICON_THERMOMETER, ICON_DRIVE, ICON_HISTORY, ICON_CHIP, ICON_PULSE, ICON_SLIDERS,
                     ICON_DOWNLOAD };
f32 theme_icon_button_width(const char* label, f32 h);
// `primary`: filled with the primary color (calls for attention, e.g. an available update).
bool theme_icon_button(const char* id, ThemeIcon icon, const char* label, f32 h, const char* tooltip,
                       bool primary = false);
// Wrapped tooltip for the item just drawn (also when it is disabled), after a short hover delay.
void theme_help(const char* text);
// Rows don't light up on hover: wrap a TreeNode / CollapsingHeader call in these. `framed` (CollapsingHeader,
// ImGuiTreeNodeFlags_Framed) keeps its fill, other tree nodes draw no background. Dropdown and list items
// (Selectable) keep their hover highlight: it shows what a click picks.
void theme_header_push(bool framed);
void theme_header_pop();
// ImGui::TableHeadersRow without the hover fill (our tables aren't sortable: headers can't be clicked).
void theme_table_headers();
// Text list entry (Selectable) whose text is inset from the highlight's left edge; full available width. In a
// panel ImGui clips a plain Selectable's highlight to the text, so it had no padding. Caller pushes colors.
bool theme_list_item(const char* label, bool selected);

// Titled, outlined section that groups related controls (settings groups, tool-window sections): a 1 px frame
// with the title on its top line. Height fits the content; width = `width` (default: the full available width).
// `help` (optional) shows when the title is hovered. Always pair with theme_group_end().
void theme_group_begin(const char* title, f32 width = 0.0f, const char* help = nullptr);
void theme_group_end();
