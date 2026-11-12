#pragma once
// Frame capture window (frames/frame_capture.h): pick a running app, record its presents for N seconds, show
// fps / frame times / lows and the frame-time graph, save as TOON or open it in the compare window.

void frames_ui_on_open();       // lists the processes with windows
void frames_ui_on_close();      // stops a running capture
void frames_ui_draw();
