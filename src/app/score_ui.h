#pragma once
// Score and ranking views (FUTURE.md sections 10 and 11) for the main window's output panel, plus the
// "About the score" page (formula, weights, version), usable from any window.
#include "core/score.h"

void score_ui_draw();               // "Score" tab: the current run's score and components
void ranking_ui_draw();             // "Ranking" tab: this run against the built-in library, per score or test
// Call every frame in a window that offers the About page; opens it when *request is set (then clears it).
void score_ui_about(bool* request);
// Hover text with a score's breakdown (compare window, history).
void score_ui_tooltip(const ScoreResult* s, const char* title);
// Tooltip body: one component's inputs with value, reference, change and weight.
void score_ui_inputs_table(const ScoreResult* s, ScoreComp c);
