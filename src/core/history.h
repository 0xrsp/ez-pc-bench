#pragma once
// Automatic run history (FUTURE.md section 11): every finished run (GUI and CLI) is saved as
// <exe dir>\results\history\<yyyymmdd_hhmmss>_<cpu>_<gpu>.toon.txt. The History window reads them back.
#include "core/types.h"

void history_dir(wchar_t* dir);         // MAX_PATH chars; created on demand
// Writes the last run of this process. False if no test finished (nothing written).
bool history_save(wchar_t* path_out);   // path_out: MAX_PATH chars or null
