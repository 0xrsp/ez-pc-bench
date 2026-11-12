// System report (sysinfo_ext.h): every piece of system information as sections of key / value lines and
// tables, rendered as plain text, TOON, or by the System details window.
#include "core/sysinfo.h"
#include "core/sysinfo_ext.h"
#include "core/toon.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// ---- building ---------------------------------------------------------------------------------------------

static u32 rep_store(SysReport* r, const char* s, u8* cells, bool split) {
    const u32 at = r->used;
    const size_t n = strlen(s);
    if (r->used + n + 1 > sizeof(r->text)) return UINT32_MAX;
    memcpy(r->text + r->used, s, n + 1);
    *cells = 1;
    if (split)
        for (size_t i = 0; i < n; ++i)
            if (r->text[at + i] == '|') { r->text[at + i] = 0; ++*cells; }
    r->used += (u32)n + 1;
    return at;
}

static void rep_push(SysReport* r, RepKind kind, bool in_results, const char* s, bool split) {
    if (r->count >= ARRAY_COUNT(r->items)) return;
    u8 cells = 0;
    const u32 at = rep_store(r, s, &cells, split);
    if (at == UINT32_MAX) return;
    r->items[r->count++] = { kind, cells, in_results, at };
}

// "Label|key" pair separated by '|': the section title and its TOON key.
static void rep_section(SysReport* r, const char* title, const char* key, bool in_results) {
    char s[160];
    snprintf(s, sizeof(s), "%s|%s", title, key);
    rep_push(r, REP_SECTION, in_results, s, true);
}

static void rep_kv(SysReport* r, const char* label, const char* fmt, ...) {
    char s[9000];
    int n = snprintf(s, sizeof(s), "%s|", label);
    va_list a;
    va_start(a, fmt);
    vsnprintf(s + n, sizeof(s) - n, fmt, a);
    va_end(a);
    for (char* c = s + n; *c; ++c)
        if (*c == '|') *c = '/';
    rep_push(r, REP_KV, false, s, true);
}

static void rep_table(SysReport* r, const char* title, const char* fields, bool in_results) {
    char s[512];
    snprintf(s, sizeof(s), "%s|%s", title, fields);
    rep_push(r, REP_TABLE, in_results, s, true);
}

static void rep_row(SysReport* r, const char* fmt, ...) {
    char s[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(s, sizeof(s), fmt, a);
    va_end(a);
    rep_push(r, REP_ROW, false, s, true);
}

static void rep_note(SysReport* r, const char* fmt, ...) {
    char s[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(s, sizeof(s), fmt, a);
    va_end(a);
    rep_push(r, REP_NOTE, false, s, false);
}

const char* rep_cell(const SysReport* r, const RepItem* it, u32 cell) {
    const char* s = r->text + it->text;
    for (u32 i = 0; i < cell && i < it->cells; ++i) s += strlen(s) + 1;
    return cell < it->cells ? s : "";
}

static const char* yes_no(i8 v) { return v < 0 ? "unknown" : v ? "yes" : "no"; }

static void fmt_bytes(char* buf, u32 size, u64 b) {
    if (b >= GiB) snprintf(buf, size, "%.1f GiB", b / (f64)GiB);
    else snprintf(buf, size, "%llu MiB", b / MiB);
}

static void link_text(char* buf, u32 size, u8 gen, u8 width, u8 gen_max, u8 width_max) {
    if (!gen && !gen_max) { buf[0] = 0; return; }
    snprintf(buf, size, "Gen %u x%u (max Gen %u x%u)", gen, width, gen_max, width_max);
}

// ---- sections -----------------------------------------------------------------------------------------------

static void cpu_section(SysReport* r) {
    const CpuInfo* c = &g_sys.cpu;
    const CpuDetails* d = &g_cpux;
    rep_section(r, "CPU", "cpu", true);
    rep_kv(r, "Name", "%s", c->brand);
    rep_kv(r, "Codename", "%s", d->codename[0] ? d->codename : "unknown");
    rep_kv(r, "Microarchitecture", "%s", d->microarch[0] ? d->microarch : "unknown");
    rep_kv(r, "Family / model / stepping", "%Xh / %Xh / %u", c->family, c->model, c->stepping);
    if (d->microcode) rep_kv(r, "Microcode", "0x%08X", d->microcode);
    if (d->socket[0]) rep_kv(r, "Socket", "%s", d->socket);
    rep_kv(r, "Cores / threads", "%u / %u", c->cores, c->threads);
    if (d->hybrid) rep_kv(r, "Performance / efficient cores", "%u / %u", d->perf_cores, d->eff_cores);
    if (d->l3_count > 1) rep_kv(r, "L3 groups (CCX / CCD)", "%u", d->l3_count);
    if (d->base_mhz) rep_kv(r, "Base clock", "%u MHz", d->base_mhz);
    if (d->max_mhz) rep_kv(r, "Max clock (SMBIOS)", "%u MHz", d->max_mhz);
    if (d->bus_mhz) rep_kv(r, "Bus clock", "%u MHz", d->bus_mhz);
    char isa[2048];
    u32 used = 0;
    isa[0] = 0;
    for (u32 i = 0; i < d->isa_count; ++i) {
        const int w = snprintf(isa + used, sizeof(isa) - used, "%s%s", i ? " " : "", d->isa[i]);
        if (w < 0 || used + (u32)w >= sizeof(isa)) break;
        used += (u32)w;
    }
    rep_kv(r, "Instruction sets", "%s", isa);
    rep_table(r, "Caches", "Level|Type|Size|Ways|Line|Instances", true);
    for (u32 i = 0; i < d->cache_count; ++i) {
        const CpuCache* k = &d->caches[i];
        char ways[16];
        if (k->ways == 0xFF) snprintf(ways, sizeof(ways), "full");
        else snprintf(ways, sizeof(ways), "%u", k->ways);
        rep_row(r, "L%u|%s|%u KiB|%s|%u B|%u", k->level, k->type == 'D' ? "data" : k->type == 'I' ? "instruction" : "unified",
                k->size_kib, ways, k->line, k->count);
    }
    rep_table(r, "Cores", "Core|Logical processors|Efficiency class|Preferred rank|L3 group", false);
    for (u32 i = 0; i < d->core_count; ++i) {
        const CpuCore* k = &d->cores[i];
        char lps[32];
        if (k->lp_count > 1) snprintf(lps, sizeof(lps), "%u-%u", k->lp_first, k->lp_first + k->lp_count - 1);
        else snprintf(lps, sizeof(lps), "%u", k->lp_first);
        rep_row(r, "%u|%s|%u%s|%u|%u", i, lps, k->efficiency, d->hybrid ? (k->efficiency ? " (P)" : " (E)") : "", k->sched, k->l3);
    }
}

static void memory_section(SysReport* r) {
    const RamInfo* m = &g_sys.ram;
    rep_section(r, "Memory", "memory", false);
    char a[32], b[32];
    fmt_bytes(a, sizeof(a), m->installed_bytes);
    fmt_bytes(b, sizeof(b), m->total_bytes);
    rep_kv(r, "Installed / usable", "%s / %s", a, b);
    rep_table(r, "Modules", "Slot|Size|Type|Speed|Configured|Manufacturer|Part", false);
    for (u32 i = 0; i < m->module_count; ++i) {
        const RamModule* k = &m->modules[i];
        rep_row(r, "%s|%llu GiB|%s|%u MT/s|%u MT/s|%s|%s", k->locator, k->size_bytes / GiB, k->type, k->speed_mts,
                k->configured_mts, k->manufacturer, k->part);
    }
    // SPD contents and current timings need SMBus / memory-controller access, i.e. a kernel driver.
    rep_note(r, "SPD contents and memory timings aren't read: they need a kernel driver, which EzPcBench doesn't install.");
}

static void board_section(SysReport* r) {
    const BoardInfo* b = &g_sys.board;
    rep_section(r, "Motherboard", "board", true);
    rep_kv(r, "System", "%s %s", b->sys_vendor, b->sys_product);
    rep_kv(r, "Board", "%s %s", b->board_vendor, b->board_product);
    rep_kv(r, "BIOS", "%s %s (%s)", b->bios_vendor, b->bios_version, b->bios_date);
    rep_kv(r, "Chipset", "%s", g_dev.chipset[0] ? g_dev.chipset : "unknown");
    if (g_dev.host_bridge[0]) rep_kv(r, "Host bridge", "%s", g_dev.host_bridge);
}

static void gpu_sections(SysReport* r) {
    const u32 ready = g_gpux_ready.load(std::memory_order_acquire);
    for (u32 i = 0; i < g_sys.gpu_count && i < 8; ++i) {
        const GpuInfo* g = &g_sys.gpus[i];
        const GpuDevInfo* d = &g_dev.gpus[i];
        char title[160], key[16], v[64];
        snprintf(title, sizeof(title), "GPU %u", i);
        snprintf(key, sizeof(key), "gpu%u", i);
        rep_section(r, title, key, true);
        rep_kv(r, "Name", "%s", g->name);
        rep_kv(r, "PCI ID", "%04X:%04X", g->vendor_id, g->device_id);
        fmt_bytes(v, sizeof(v), g->dedicated_vram);
        rep_kv(r, "Dedicated memory", "%s%s", v, d->integrated ? " (integrated GPU, shares system RAM)" : "");
        fmt_bytes(v, sizeof(v), g->shared_mem);
        rep_kv(r, "Shared system memory", "%s", v);
        rep_kv(r, "Driver", "%s%s%s%s%s", g->driver, d->driver_date[0] ? ", " : "", d->driver_date,
               d->provider[0] ? ", " : "", d->provider);
        if (d->wddm[0]) rep_kv(r, "WDDM", "%s", d->wddm);
        rep_kv(r, "Hardware-accelerated scheduling", "%s", g->hags < 0 ? "not supported" : g->hags ? "on" : "off");
        if (d->chip[0]) rep_kv(r, "Chip (driver)", "%s", d->chip);
        if (d->bios[0]) rep_kv(r, "Video BIOS", "%s", d->bios);
        link_text(v, sizeof(v), d->link_gen, d->link_width, d->link_gen_max, d->link_width_max);
        if (v[0]) rep_kv(r, "PCIe link", "%s", v);
        if (d->mem_mhz_max) rep_kv(r, "Max memory clock (driver)", "%u MHz", d->mem_mhz_max);
        if (d->mem_bw_max) rep_kv(r, "Max memory bandwidth (driver)", "%.1f GB/s", d->mem_bw_max / 1e9);
        if (!(ready & (1u << i))) {
            rep_note(r, "Vulkan / D3D12 details (compute units, feature level, Resizable BAR, extensions) are read on "
                        "request because creating a device wakes a sleeping GPU.");
            continue;
        }
        const GpuApiInfo* x = &g_gpux[i];
        if (x->d3d12) {
            rep_kv(r, "D3D12 feature level", "%s", x->feature_level);
            rep_kv(r, "Shader model", "%s", x->shader_model);
            rep_kv(r, "Raytracing tier", "%s", x->raytracing_tier >= 10 ? (x->raytracing_tier >= 11 ? "1.1" : "1.0") : "none");
            rep_kv(r, "Mesh shaders", "%s", x->mesh_tier ? "tier 1" : "no");
            rep_kv(r, "Variable-rate shading", "tier %u", x->vrs_tier);
            rep_kv(r, "Sampler feedback", "%s", x->sampler_feedback_tier >= 100 ? "1.0" : x->sampler_feedback_tier >= 90 ? "0.9" : "no");
            rep_kv(r, "Resource binding tier", "%u", x->binding_tier);
            rep_kv(r, "GPU upload heaps", "%s", yes_no(x->upload_heaps));
        }
        if (x->vk) {
            rep_kv(r, "Vulkan", "%s", x->vk_api);
            rep_kv(r, "Vulkan driver", "%s", x->vk_driver);
            if (x->compute_units) rep_kv(r, "Compute units", "%u %s", x->compute_units, x->cu_name);
            if (x->subgroup) rep_kv(r, "Wave / warp size", "%u", x->subgroup);
            fmt_bytes(v, sizeof(v), x->bar_heap);
            rep_kv(r, "Resizable BAR", "%s (CPU-visible VRAM %s)",
                   x->rebar < 0 ? "n/a" : x->rebar ? "on" : "off", x->bar_heap ? v : "none");
            rep_kv(r, "Vulkan extensions", "%u", x->vk_ext_count);
            rep_table(r, "Vulkan extensions", "Extension", false);
            for (const char* s = x->vk_exts; *s;) {
                const char* e = strchr(s, ' ');
                const u32 n = e ? (u32)(e - s) : (u32)strlen(s);
                rep_row(r, "%.*s", (int)n, s);
                s += n + (e ? 1 : 0);
            }
        }
    }
}

static void display_section(SysReport* r) {
    rep_section(r, "Displays", "displays", true);
    rep_kv(r, "Tearing support (VRR in windows)", "%s", yes_no(g_dev.tearing));
    rep_kv(r, "Windows VRR optimizations", "%s", g_dev.vrr_optimizations < 0 ? "default" : g_dev.vrr_optimizations ? "on" : "off");
    rep_table(r, "Displays", "Name|Maker|Connector|Adapter|Mode|Native|VRR range|HDR|Size", true);
    for (u32 i = 0; i < g_dev.display_count; ++i) {
        const DisplayInfo* d = &g_dev.displays[i];
        char mode[48], native[48], vrr[32], hdr[32], size[16];
        snprintf(mode, sizeof(mode), "%ux%u @ %.2f Hz", d->width, d->height, d->refresh_hz);
        if (d->native_w) snprintf(native, sizeof(native), "%ux%u @ %.2f Hz", d->native_w, d->native_h, d->native_hz);
        else native[0] = 0;
        if (d->vrr_max_hz) snprintf(vrr, sizeof(vrr), "%u-%u Hz", d->vrr_min_hz, d->vrr_max_hz);
        else snprintf(vrr, sizeof(vrr), "not reported");
        snprintf(hdr, sizeof(hdr), "%s%s", d->hdr_supported < 0 ? "unknown" : d->hdr_supported ? "supported" : "no",
                 d->hdr_on == 1 ? ", on" : "");
        if (d->size_in > 0.0f) snprintf(size, sizeof(size), "%.1f in", d->size_in);
        else size[0] = 0;
        rep_row(r, "%s|%s %04X|%s|%s|%s|%s|%s|%s|%s", d->name, d->maker, d->product, d->connector, d->adapter, mode, native,
                vrr, hdr, size);
    }
}

static void windows_section(SysReport* r) {
    const WinState* w = &g_sys.win;
    rep_section(r, "Windows", "windows", false);
    rep_kv(r, "Version", "%s", g_sys.os);
    rep_kv(r, "Running as", "%s", g_sys.elevated ? "administrator" : "standard user");
    char s[192];
    winstate_describe(w, s, sizeof(s));
    rep_kv(r, "Security / gaming", "%s", s);
    rep_kv(r, "Power", "%s, plan \"%s\", mode \"%s\"", g_sys.power.on_ac ? "AC" : "battery", g_sys.power.scheme_name,
           power_mode_name(g_sys.power.mode));
}

static void drive_section(SysReport* r) {
    rep_section(r, "Drives", "drives", false);
    rep_table(r, "Volumes", "Drive|Description", false);
    for (u32 i = 0; i < g_sys.drive_count; ++i) {
        char d[192];
        drive_describe(&g_sys.drives[i], d, sizeof(d));
        for (char* c = d; *c; ++c)
            if (*c == '|') *c = '/';
        rep_row(r, "%c:|%s", g_sys.drives[i].letter, d);
    }
}

static void pci_section(SysReport* r) {
    rep_section(r, "PCI devices", "pci", false);
    rep_table(r, "Devices", "Location|ID|Class|Device|PCIe link", false);
    for (u32 i = 0; i < g_dev.pci_count; ++i) {
        const PciDev* p = &g_dev.pci[i];
        char loc[24], link[48], indent[40];
        if (p->bus != 0xFFFFFFFF) snprintf(loc, sizeof(loc), "%02X:%02X.%X", p->bus, p->dev, p->fn);
        else snprintf(loc, sizeof(loc), "?");
        link_text(link, sizeof(link), p->link_gen, p->link_width, p->link_gen_max, p->link_width_max);
        const u32 depth = p->depth < 12 ? p->depth : 12;
        for (u32 k = 0; k < depth * 2; ++k) indent[k] = ' ';
        indent[depth * 2] = 0;
        rep_row(r, "%s|%04X:%04X|%s|%s%s|%s", loc, p->vendor, p->device,
                p->base_class == 0xFF ? "" : pci_class_name(p->base_class, p->sub_class), indent, p->name, link);
    }
}

static void usb_section(SysReport* r) {
    rep_section(r, "USB devices", "usb", false);
    rep_table(r, "Devices", "Device|ID", false);
    for (u32 i = 0; i < g_dev.usb_count; ++i) {
        const UsbDev* u = &g_dev.usb[i];
        char indent[40], id[16];
        const u32 depth = u->depth < 12 ? u->depth : 12;
        for (u32 k = 0; k < depth * 2; ++k) indent[k] = ' ';
        indent[depth * 2] = 0;
        if (u->vid || u->pid) snprintf(id, sizeof(id), "%04X:%04X", u->vid, u->pid);
        else id[0] = 0;
        rep_row(r, "%s%s|%s", indent, u->name, id);
    }
}

static void net_audio_sections(SysReport* r) {
    rep_section(r, "Network", "network", false);
    rep_table(r, "Adapters", "Adapter|Type|Status|Link speed", false);
    for (u32 i = 0; i < g_dev.net_count; ++i) {
        const NetAdapter* n = &g_dev.net[i];
        char speed[24];
        if (!n->link_bps || !n->up) speed[0] = 0;
        else if (n->link_bps >= 1000000000ull) snprintf(speed, sizeof(speed), "%.1f Gbit/s", n->link_bps / 1e9);
        else snprintf(speed, sizeof(speed), "%.0f Mbit/s", n->link_bps / 1e6);
        rep_row(r, "%s|%s|%s|%s", n->name, n->type, n->up ? "up" : "down", speed);
    }
    rep_section(r, "Audio", "audio", false);
    rep_table(r, "Endpoints", "Device|Direction|Format|Default", false);
    for (u32 i = 0; i < g_dev.audio_count; ++i) {
        const AudioDev* a = &g_dev.audio[i];
        char fmt[48];
        if (a->sample_rate) snprintf(fmt, sizeof(fmt), "%u Hz, %u bit, %u ch", a->sample_rate, a->bits, a->channels);
        else fmt[0] = 0;
        rep_row(r, "%s|%s|%s|%s", a->name, a->capture ? "recording" : "playback", fmt, a->is_default ? "yes" : "");
    }
}

void sysreport_build(SysReport* r) {
    devices_ensure();
    r->count = r->used = 0;
    cpu_section(r);
    memory_section(r);
    board_section(r);
    gpu_sections(r);
    display_section(r);
    windows_section(r);
    drive_section(r);
    pci_section(r);
    usb_section(r);
    net_audio_sections(r);
}

// ---- plain text ----------------------------------------------------------------------------------------------

struct TextBuf {
    char* p;
    u32 size, used;
};

static void out(TextBuf* t, const char* fmt, ...) {
    if (t->used + 1 >= t->size) return;
    va_list a;
    va_start(a, fmt);
    const int n = vsnprintf(t->p + t->used, t->size - t->used, fmt, a);
    va_end(a);
    if (n > 0) t->used = t->used + (u32)n < t->size ? t->used + (u32)n : t->size - 1;
}

static u32 text_width(const char* s) {     // UTF-8 code points
    u32 n = 0;
    for (; *s; ++s) n += ((u8)*s & 0xC0) != 0x80;
    return n;
}

static void out_padded(TextBuf* t, const char* s, u32 width) {
    out(t, "%s", s);
    for (u32 w = text_width(s); w < width; ++w) out(t, " ");
}

void sysreport_text(const SysReport* r, char* buf, u32 size) {
    TextBuf t = { buf, size, 0 };
    buf[0] = 0;
    char stamp[32];
    time_t now = time(nullptr);
    struct tm tm_local;
    localtime_s(&tm_local, &now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &tm_local);
    out(&t, "EzPcBench system report, %s\r\n", stamp);
    for (u32 i = 0; i < r->count; ++i) {
        const RepItem* it = &r->items[i];
        if (it->kind == REP_SECTION) {
            out(&t, "\r\n== %s ==\r\n", rep_cell(r, it, 0));
            u32 label_w = 0;     // align the section's key / value lines
            for (u32 k = i + 1; k < r->count && r->items[k].kind != REP_SECTION; ++k)
                if (r->items[k].kind == REP_KV) {
                    const u32 w = text_width(rep_cell(r, &r->items[k], 0));
                    label_w = w > label_w ? w : label_w;
                }
            for (u32 k = i + 1; k < r->count && r->items[k].kind == REP_KV; ++k) {
                out(&t, "  ");
                out_padded(&t, rep_cell(r, &r->items[k], 0), label_w + 2);
                out(&t, "%s\r\n", rep_cell(r, &r->items[k], 1));
            }
        } else if (it->kind == REP_NOTE) {
            out(&t, "  (%s)\r\n", rep_cell(r, it, 0));
        } else if (it->kind == REP_TABLE) {
            const u32 cols = it->cells - 1u < 16u ? it->cells - 1u : 16u;
            u32 w[16] = {};
            for (u32 c = 0; c < cols; ++c) w[c] = text_width(rep_cell(r, it, c + 1));
            u32 k = i + 1;
            for (; k < r->count && r->items[k].kind == REP_ROW; ++k)
                for (u32 c = 0; c < cols; ++c) {
                    const u32 cw = text_width(rep_cell(r, &r->items[k], c));
                    w[c] = cw > w[c] ? cw : w[c];
                }
            out(&t, "  %s:\r\n    ", rep_cell(r, it, 0));
            for (u32 c = 0; c < cols; ++c) out_padded(&t, rep_cell(r, it, c + 1), c + 1 < cols ? w[c] + 2 : 0);
            out(&t, "\r\n");
            for (u32 j = i + 1; j < k; ++j) {
                out(&t, "    ");
                for (u32 c = 0; c < cols; ++c) out_padded(&t, rep_cell(r, &r->items[j], c), c + 1 < cols ? w[c] + 2 : 0);
                out(&t, "\r\n");
            }
        }
    }
}

// ---- TOON ----------------------------------------------------------------------------------------------------

// "Family / model / stepping" -> "family_model_stepping".
static void toon_key_of(const char* label, char* out_key, u32 size) {
    u32 n = 0;
    bool sep = false;
    for (const char* c = label; *c && n + 1 < size; ++c) {
        char ch = *c;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            if (sep && n) out_key[n++] = '_';
            if (n + 1 < size) out_key[n++] = ch;
            sep = false;
        } else {
            sep = true;
        }
    }
    out_key[n] = 0;
}

void sysreport_toon(const SysReport* r, FILE* f, u32 depth, bool results_only) {
    bool section_on = false;
    for (u32 i = 0; i < r->count; ++i) {
        const RepItem* it = &r->items[i];
        char key[64];
        if (it->kind == REP_SECTION) {
            section_on = !results_only || it->in_results;
            if (section_on) toon_key(f, depth, rep_cell(r, it, 1));
        } else if (!section_on) {
            continue;
        } else if (it->kind == REP_KV) {
            toon_key_of(rep_cell(r, it, 0), key, sizeof(key));
            toon_str(f, depth + 1, key, rep_cell(r, it, 1));
        } else if (it->kind == REP_TABLE) {
            u32 rows = 0;
            for (u32 k = i + 1; k < r->count && r->items[k].kind == REP_ROW; ++k) ++rows;
            if (results_only && !it->in_results) { i += rows; continue; }
            char fields[512];
            u32 used = 0;
            fields[0] = 0;
            const u32 cols = it->cells - 1u < TOON_MAX_FIELDS ? it->cells - 1u : TOON_MAX_FIELDS;
            for (u32 c = 0; c < cols; ++c) {
                toon_key_of(rep_cell(r, it, c + 1), key, sizeof(key));
                used += (u32)snprintf(fields + used, sizeof(fields) - used, "%s%s", c ? "," : "", key);
            }
            toon_key_of(rep_cell(r, it, 0), key, sizeof(key));
            toon_table(f, depth + 1, key, rows, fields);
            for (u32 k = 0; k < rows; ++k) {
                const RepItem* row = &r->items[i + 1 + k];
                ToonRow tr = toon_row(f, depth + 2);
                for (u32 c = 0; c < cols; ++c) {
                    const char* cell = rep_cell(r, row, c);
                    while (*cell == ' ') ++cell;    // tree indentation is for the text view
                    toon_cell_str(&tr, cell);
                }
                toon_row_end(&tr);
            }
            i += rows;
        }
    }
}

bool sysreport_save(const wchar_t* base) {
    static SysReport rep;
    sysreport_build(&rep);
    wchar_t path[MAX_PATH + 16];
    swprintf(path, ARRAY_COUNT(path), L"%s.txt", base);
    static char text[512 * 1024];
    sysreport_text(&rep, text, sizeof(text));
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    fwrite(text, 1, strlen(text), f);
    bool ok = ferror(f) == 0;
    fclose(f);
    swprintf(path, ARRAY_COUNT(path), L"%s.toon.txt", base);
    f = _wfopen(path, L"wb");
    if (!f) return false;
    char stamp[32];
    time_t now = time(nullptr);
    struct tm tm_local;
    localtime_s(&tm_local, &now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm_local);
    toon_str(f, 0, "app", "ez-pc-bench");
    toon_str(f, 0, "kind", "system_report");
    toon_str(f, 0, "timestamp", stamp);
    sysreport_toon(&rep, f, 0, false);
    ok = ok && ferror(f) == 0;
    fclose(f);
    return ok;
}

// Results files: the in_results sections under system.details (CPU details and caches, board + chipset, GPUs,
// displays). GPU API details only when they were queried (a GPU suite ran, or the user asked).
void sysreport_write_results(FILE* f, u32 depth) {
    static SysReport rep;
    sysreport_build(&rep);
    toon_key(f, depth, "details");
    sysreport_toon(&rep, f, depth + 1, true);
}
