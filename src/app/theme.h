#pragma once
// Visual theme: dark navy/violet palette, rounded translucent panels, starfield background.
#include "core/types.h"
#include "imgui.h"

struct ThemeFonts {
    ImFont* regular;
    ImFont* semibold;   // headings, primary buttons, big numbers
    ImFont* mono;       // numeric values
};
extern ThemeFonts g_fonts;

// Cascadia Mono looks larger than Segoe UI at the same size; numbers use it slightly smaller.
inline void theme_push_mono() { ImGui::PushFont(g_fonts.mono, ImGui::GetStyle().FontSizeBase * 0.88f); }

// Palette
extern const ImVec4 TH_TEXT, TH_TEXT_DIM, TH_ACCENT, TH_ACCENT_LIGHT;
extern const ImVec4 TH_OK, TH_RUN, TH_BAD, TH_WARN;
constexpr f32 TH_BG_RGB[3] = { 0x14 / 255.0f, 0x12 / 255.0f, 0x2A / 255.0f };   // also the window clear / caption color

void theme_load_fonts();                // after ImGui::CreateContext
void theme_apply_style(f32 dpi_scale);
void theme_draw_background();           // gradient + stars + horizon glow behind everything

// Rounded translucent card. Always pair with theme_end_panel().
void theme_begin_panel(const char* id, ImVec2 size, bool scroll = true);
void theme_end_panel();

bool theme_button_primary(const char* label);      // violet call-to-action button
void theme_progress(f32 fraction, f32 height);     // thin rounded bar, full available width
// One compact warning card (amber edge), one line per warning. Line `button_line` (or -1) gets a
// button on the right; returns true when it was clicked.
bool theme_warnings(const char* const* lines, u32 count, i32 button_line, const char* button_label);
// Heat color for a 0..1 value: mint (low) .. rose (high), translucent.
ImU32 theme_heat(f32 t);
