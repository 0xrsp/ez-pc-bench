#pragma once
// Advanced sensors (PawnIO, core/adv_sensors.h) in the GUI: a section of the Sensors window and the Settings row.

void adv_ui_sensors_open();         // Sensors window opened / closed: starts / stops the sampling thread
void adv_ui_sensors_close();
void adv_ui_draw_sensors();         // section for the Sensors window; a one-line status when PawnIO isn't active
const char* adv_ui_settings_help(); // hover text of the Settings row (status-dependent)
void adv_ui_settings_control();     // control cell of the Settings row: on / off switch, or "not installed"
