// HTML report of the comparison set (see report.h).
#include "core/report.h"
#include "core/baseline.h"
#include "core/compare.h"
#include "core/integrity.h"
#include "core/results.h"
#include "core/score.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void esc(FILE* f, const char* s) {
    for (; *s; ++s) {
        switch (*s) {
            case '&': fputs("&amp;", f); break;
            case '<': fputs("&lt;", f); break;
            case '>': fputs("&gt;", f); break;
            case '"': fputs("&quot;", f); break;
            default: fputc(*s, f);
        }
    }
}

static void score_cell(FILE* f, f64 v, bool ref) {
    if (!isfinite(v)) fputs("<td class=\"num dim\">&mdash;</td>", f);
    else fprintf(f, "<td class=\"num%s\">%.0f</td>", ref ? " ref" : "", v);
}

// <thead> row: `first` header cells, then one per run (the reference highlighted).
static void table_head(FILE* f, const char* first) {
    fprintf(f, "<div class=\"card\"><table><thead><tr>%s", first);
    for (u32 r = 0; r < g_cmp.run_count; ++r) {
        fprintf(f, "<th class=\"num%s\">", r == g_cmp.reference && g_cmp.run_count > 1 ? " ref" : "");
        esc(f, g_cmp.runs[r].label);
        fputs("</th>", f);
    }
    fputs("</tr></thead><tbody>\n", f);
}

static const char* const CSS =
    ":root{--bg:#f8f9fc;--card:#fff;--fg:#191c20;--dim:#5d6067;--line:#e1e2e8;--accent:#0b57d0;--ok:#1b7f3b;--bad:#b3261e;"
    "--warn:#8a5a00;--group:#eef1f8}"
    "@media (prefers-color-scheme:dark){:root{--bg:#111318;--card:#1d2024;--fg:#e2e2e9;--dim:#a8abb3;--line:#33363c;"
    "--accent:#a8c7fa;--ok:#7dd99a;--bad:#ffb4ab;--warn:#f2c46b;--group:#262a31}}"
    "*{box-sizing:border-box}body{margin:0;padding:24px 16px;background:var(--bg);color:var(--fg);"
    "font:14px/1.45 'Segoe UI',system-ui,sans-serif}main{max-width:1200px;margin:0 auto}"
    "h1{font-size:22px;margin:0 0 4px}h2{font-size:16px;margin:28px 0 8px}p{margin:4px 0;color:var(--dim)}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:12px;overflow-x:auto}"
    "table{border-collapse:collapse;width:100%}th,td{padding:5px 10px;border-bottom:1px solid var(--line);text-align:left;"
    "white-space:nowrap}thead th{color:var(--dim);font-weight:600;vertical-align:bottom;white-space:normal;min-width:110px}"
    ".num{text-align:right;font-family:'Cascadia Mono',Consolas,monospace;font-size:13px}.dim{color:var(--dim)}"
    ".ref{color:var(--accent)}.ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}"
    "tr.grp td{background:var(--group);font-weight:600}td.d{font-size:12px;padding-left:2px}"
    "input{font:inherit;padding:6px 10px;border:1px solid var(--line);border-radius:8px;background:var(--card);"
    "color:var(--fg);width:min(320px,100%);margin:0 0 8px}";

static const char* const JS =
    "document.getElementById('q').addEventListener('input',function(e){var q=e.target.value.toLowerCase(),"
    "rows=document.querySelectorAll('#res tbody tr'),grp=null,any=false;for(var i=0;i<rows.length;i++){var r=rows[i];"
    "if(r.classList.contains('grp')){if(grp)grp.hidden=!any;grp=r;any=false;var t=r.textContent.toLowerCase().indexOf(q)>=0;"
    "r.dataset.m=t?'1':'';continue;}var show=!q||grp.dataset.m||r.textContent.toLowerCase().indexOf(q)>=0;r.hidden=!show;"
    "any=any||show;}if(grp)grp.hidden=!any;});";

bool report_write_html(const wchar_t* path) {
    const CompareSet* c = &g_cmp;
    if (!c->run_count) return false;
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    const u32 runs = c->run_count, ref = c->reference;
    char stamp[32];
    time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &t);

    fprintf(f, "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
               "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>EzPcBench report</title><style>%s</style></head><body><main>\n", CSS);
    fprintf(f, "<h1>EzPcBench report</h1><p>%u run%s, generated %s by ez-pc-bench. Raw results with units%s", runs,
            runs == 1 ? "" : "s", stamp, runs > 1 ? "; " : ".");
    if (runs > 1) {
        fputs("the change is against the reference run <b class=\"ref\">", f);
        esc(f, c->runs[ref].label);
        fputs("</b> (green = better, red = worse, more than 1 %).", f);
    }
    fputs("</p>\n", f);

    // Score
    bool any_score = false;
    for (u32 r = 0; r < runs; ++r) any_score |= c->runs[r].score.any;
    if (any_score) {
        fprintf(f, "<h2>Score v%u (%s)</h2><p>Derived from the raw results below by a published formula: the reference "
                   "machine (%s) = 1000 per component. Partial runs get component scores only.</p>\n",
                g_score.version, g_score.status, g_score.reference_desc);
        table_head(f, "<th>Score</th>");
        for (u32 p = 0; p < SP_COUNT; ++p) {
            fprintf(f, "<tr><td>%s%s%s</td>", p == SP_OVERALL ? "<b>" : "", g_score_profile_names[p], p == SP_OVERALL ? "</b>" : "");
            for (u32 r = 0; r < runs; ++r) score_cell(f, c->runs[r].score.profile[p], false);
            fputs("</tr>\n", f);
        }
        for (u32 k = 0; k < SC_COUNT; ++k) {
            fprintf(f, "<tr><td>%s</td>", g_score_comp_names[k]);
            for (u32 r = 0; r < runs; ++r) score_cell(f, c->runs[r].score.comp[k], false);
            fputs("</tr>\n", f);
        }
        fputs("<tr><td>Flags</td>", f);
        for (u32 r = 0; r < runs; ++r) {
            char flags[300];
            score_flag_text(c->runs[r].score.flags, flags, sizeof(flags));
            fputs("<td class=\"warn\" style=\"white-space:normal\">", f);
            esc(f, flags);
            fputs("</td>", f);
        }
        fputs("</tr></tbody></table></div>\n", f);
    }

    // System
    fputs("<h2>System</h2>", f);
    table_head(f, "<th></th>");
    for (u32 i = 0; i < CI_COUNT; ++i) {
        fputs("<tr><td class=\"dim\">", f);
        esc(f, g_cmp_info_names[i]);
        fputs("</td>", f);
        for (u32 r = 0; r < runs; ++r) {
            const bool differs = r != ref && i != CI_DATE && strcmp(c->runs[r].info[i], c->runs[ref].info[i]) != 0;
            fprintf(f, "<td style=\"white-space:normal\"%s>", differs ? " class=\"warn\"" : "");
            esc(f, c->runs[r].info[i]);
            fputs("</td>", f);
        }
        fputs("</tr>\n", f);
    }
    fputs("<tr><td class=\"dim\">Integrity</td>", f);
    for (u32 r = 0; r < runs; ++r)
        fprintf(f, "<td>%s</td>", c->runs[r].integrity < 0 ? "&mdash;" : integrity_name((IntegrityState)c->runs[r].integrity));
    fputs("</tr></tbody></table></div>\n", f);

    // Results
    fputs("<h2>Results</h2><input id=\"q\" type=\"search\" placeholder=\"Filter tests / metrics\">\n<div id=\"res\">", f);
    table_head(f, "<th>Metric</th><th>Unit</th>");
    static const char* const stat_names[] = { "", " min", " avg", " max" };
    i32 last_test = -1;
    char v[32], x[32];
    for (u32 i = 0; i < c->row_count; ++i) {
        const CmpRow* row = &c->rows[i];
        const CmpTest* test = &c->tests[row->test];
        if ((i32)row->test != last_test) {
            fprintf(f, "<tr class=\"grp\"><td colspan=\"%u\">", 2 + runs);
            esc(f, test->title);
            fputs("</td></tr>\n", f);
            last_test = row->test;
        }
        fputs("<tr><td>", f);
        esc(f, row->name);
        fputs(stat_names[row->stat], f);
        if (test->x_unit[0] && row->stat == CS_VALUE) {
            format_x(x, sizeof(x), row->x, test->x_unit);
            fputs(" @ ", f);
            esc(f, x);
        }
        fputs("</td><td class=\"dim\">", f);
        esc(f, row->unit);
        fputs("</td>", f);
        for (u32 r = 0; r < runs; ++r) {
            const f64 val = c->values[i][r], base = c->values[i][ref];
            if (isnan(val)) { fputs("<td class=\"num dim\">&mdash;</td>", f); continue; }
            format_value(v, sizeof(v), val);
            fprintf(f, "<td class=\"num\">%s", v);
            if (runs > 1 && r != ref && !isnan(base)) {
                const f64 pct = delta_pct(val, base);
                const i32 rating = delta_rating(pct, row->unit);
                char d[16];
                format_delta(d, sizeof(d), pct);
                fprintf(f, " <span class=\"d %s\">%s</span>", rating > 0 ? "ok" : rating < 0 ? "bad" : "dim", d);
            }
            fputs("</td>", f);
        }
        fputs("</tr>\n", f);
    }
    fprintf(f, "</tbody></table></div></div>\n<p style=\"margin-top:16px\">Made with "
               "<a href=\"https://0xrsp.github.io/ez-pc-bench/\">EzPcBench</a>.</p></main><script>%s</script></body></html>\n",
            JS);
    const bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}
