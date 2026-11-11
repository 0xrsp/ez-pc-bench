#pragma once

bool is_elevated();
// Relaunches this exe elevated (UAC prompt) with the same args plus `extra_arg`, same working dir.
// Returns true if the elevated instance started (caller should exit), false if declined/failed.
bool relaunch_elevated(const wchar_t* extra_arg);
