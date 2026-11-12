#pragma once
// Self-contained HTML report (FUTURE.md section 11): the comparison set (compare.h; one run or several) as
// one HTML file with inline CSS and a tiny filter script, no external files. Score, system info and every
// raw value with its change vs the reference run.
#include "core/types.h"

bool report_write_html(const wchar_t* path);
