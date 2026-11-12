#pragma once
// Sensors window: live readings from core/sensors (sampling runs while the window, a sensor log or the
// overlay is active), the sensor log and the overlay controls.

void temps_ui_on_open();
void temps_ui_on_close();
void temps_ui_draw();
void temps_ui_show_overlay();    // overlay with the saved channels / corner (GUI flag --overlay)
void temps_ui_set_filter(const wchar_t* text);  // preset the search box (dev GUI flag --sensor-filter <text>)
