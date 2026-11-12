#pragma once
// History tool window (FUTURE.md section 11): the runs saved in results\history, a trend chart of the score or
// any metric across them, and a list to send runs to the compare window.

void history_ui_on_open();      // scans the history folder
void history_ui_on_close();     // frees the loaded runs
void history_ui_draw();
