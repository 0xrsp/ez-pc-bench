#pragma once
// Auto-update (GUI only). On startup a background thread asks the GitHub API for the latest release and
// compares the SHA-256 of its ez-pc-bench.exe asset with the running exe. Any failure (offline, rate limit,
// no such asset) just means no update is offered. Installing downloads the asset, checks its size and hash,
// renames the running exe to <exe>.old, puts the new one in its place and restarts after a clean shutdown.
// Off unless built with EZB_UPDATER (the `dist` preset) and never with --dev / --no-update.
#include "core/types.h"

enum UpdateState : u8 {
    UPD_OFF,            // disabled, checking, up to date or the check failed: nothing to show
    UPD_AVAILABLE,      // a different exe is published
    UPD_DOWNLOADING,
    UPD_READY,          // new exe in place: quit and call updater_relaunch()
    UPD_FAILED,         // install failed (updater_error()); can be retried
};

struct UpdateInfo {
    char tag[64];           // "v0.3.0"
    char name[128];         // release title
    char published[32];     // "2026-11-11T20:09:41Z"
    char html_url[256];     // release page
    char notes[4096];       // release body (Markdown), may be truncated
    u64 size;               // asset bytes
};

void updater_init(bool check);      // cleans up after a previous update; starts the check when `check`
UpdateState updater_state();
const UpdateInfo* updater_info();   // valid once the state is past UPD_OFF
void updater_install();             // from UPD_AVAILABLE / UPD_FAILED: download, verify, replace the exe
f32 updater_progress();             // download fraction, 0..1
const char* updater_error();
void updater_relaunch();            // last thing before exiting: starts the new exe if UPD_READY
