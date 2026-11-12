#pragma once
// Sensor overlay: a small top-most, click-through, translucent window in a screen corner with chosen sensor
// channels (core/sensors.h) and the frame rate while a GPU test runs. It is an ordinary layered window drawn
// with GDI on its own thread (so its timer never wakes the GUI loop); nothing is injected into other
// programs, so it shows over windowed and borderless games but not over exclusive fullscreen.
#include "core/types.h"

enum OverlayCorner : u8 { OVL_TOP_LEFT, OVL_TOP_RIGHT, OVL_BOTTOM_LEFT, OVL_BOTTOM_RIGHT, OVL_CORNER_COUNT };

// Shows / updates / hides the overlay. `channel_ids`: comma-separated channel ids in display order; empty =
// a default set (CPU load and clock, CPU package power, temperatures, battery rate, GPU load).
void overlay_set(bool visible, const char* channel_ids, OverlayCorner corner);
bool overlay_visible();
void overlay_shutdown();        // at exit
