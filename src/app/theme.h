#pragma once
// Material 3 style theme (light / dark) with Inter. Flat surfaces, outlined cards, pill buttons.
#include "core/types.h"
#include "imgui.h"

struct ThemeFonts {
    ImFont* regular;    // Inter Regular (body)
    ImFont* medium;     // Inter Medium (titles, labels, buttons)
    ImFont* mono;       // Cascadia Mono (result numbers), falls back to Consolas / Inter
};
extern ThemeFonts g_fonts;

// Material color roles for the current mode.
struct Palette {
    ImVec4 page;                    // window background
    ImVec4 card;                    // cards / panels
    ImVec4 container, container_high, container_highest;
    ImVec4 on_surface, on_surface_variant;
    ImVec4 outline, outline_variant;
    ImVec4 primary, on_primary, primary_container, on_primary_container;
    ImVec4 secondary_container, on_secondary_container;
    ImVec4 ok, error, warn, warn_container, on_warn_container;
    ImVec4 heat_lo, heat_hi;        // matrix cell colors
};
extern Palette g_pal;

void theme_load_fonts();                    // Inter from the exe's resources (Segoe UI fallback)
void theme_apply_style(f32 dpi_scale);      // uses the current mode
enum ThemeMode : u8 { THEME_SYSTEM, THEME_LIGHT, THEME_DARK };
bool theme_is_dark();
ThemeMode theme_mode();
void theme_set_mode(ThemeMode mode);        // System follows Windows' app mode; takes effect between frames
void theme_system_changed();                // call on WM_SETTINGCHANGE "ImmersiveColorSet"
bool theme_take_change();                   // true once after the effective light/dark changed
void theme_bg_rgb(f32 out[3]);              // page color (window clear / title bar)

f32 theme_font_size(f32 mult);              // base font size * mult, snapped to whole pixels
// Result numbers. Cascadia Mono looks larger than Inter at the same size, so it's drawn a bit smaller.
inline void theme_push_numbers() { ImGui::PushFont(g_fonts.mono, theme_font_size(0.92f)); }

// Outlined card. Always pair with theme_end_panel().
void theme_begin_panel(const char* id, ImVec2 size, bool scroll = true);
void theme_end_panel();

bool theme_button_primary(const char* label);   // filled pill
bool theme_button(const char* label);           // tonal pill
bool theme_checkbox(const char* label, bool* v, bool mixed = false);   // mixed: dash, "some selected"
f32 theme_checkbox_height();                    // box height, for centering next to taller items
bool theme_tab(const char* label, bool selected);  // segmented tab, returns true when clicked
void theme_progress(f32 fraction, f32 height, f32 width = -FLT_MIN);  // linear indicator (default: full width)
// One warning banner, one line per warning; line `button_line` (or -1) gets a button. True when clicked.
bool theme_warnings(const char* const* lines, u32 count, i32 button_line, const char* button_label);
ImU32 theme_heat(f32 t);                        // 0 = low (green) .. 1 = high (red)

// Small tonal pill with a drawn icon and an optional label, `h` tall (fits in a header row). The icons are
// vector-drawn (Inter has no icon glyphs). `tooltip` shows on hover.
enum ThemeIcon : u8 { ICON_GEAR, ICON_THERMOMETER, ICON_DRIVE, ICON_HISTORY };
f32 theme_icon_button_width(const char* label, f32 h);
bool theme_icon_button(const char* id, ThemeIcon icon, const char* label, f32 h, const char* tooltip);
// Wrapped tooltip for the item just drawn (also when it is disabled), after a short hover delay.
void theme_help(const char* text);
