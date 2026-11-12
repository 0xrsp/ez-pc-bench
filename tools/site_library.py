"""Builds the website's results table data from the run library (FUTURE.md section 11, community results).

Reads library/*.toon.txt (our own TOON results files) and writes one JSON file the static page
site/results.html renders and searches in the browser. Run by the Pages workflow at deploy time and by
tools/serve_site.ps1 for local previews; nothing here talks to the network.

The score shown is the derived `score:` block stored in each file (written by the app since score v1; add it to
older files with `ez-pc-bench-cli --score <file> --score-write`). Files without one show raw numbers only.

Usage: python tools/site_library.py <library dir> <out.json>
"""
import json
import os
import sys


def cells(text):
    """Splits one TOON table row into decoded cells (quoted cells may hold commas)."""
    out, cur, quoted, i = [], [], False, 0
    while i < len(text):
        c = text[i]
        if quoted and c == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            cur.append({"n": "\n", "r": "\r", "t": "\t"}.get(nxt, nxt))
            i += 2
            continue
        if c == '"':
            quoted = not quoted
        elif c == "," and not quoted:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    out.append("".join(cur).strip())
    return out


def scalar(v):
    v = v.strip()
    if len(v) >= 2 and v[0] == '"' and v[-1] == '"':
        return cells(v)[0]
    return v


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def key_end(line):
    """Index of the ':' that ends the key (outside quotes, [] and {}), or -1."""
    quoted, nest = False, 0
    for i, c in enumerate(line):
        if c == '"':
            quoted = not quoted
        elif not quoted and c in "[{":
            nest += 1
        elif not quoted and c in "]}" and nest:
            nest -= 1
        elif not quoted and not nest and c == ":":
            return i
    return -1


def parse(text):
    """Minimal reader for our results layout: nested keys -> 'a.b.c' paths, tables -> lists of dicts."""
    values, tables = {}, {}
    keys = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        raw = lines[i].rstrip("\r")
        i += 1
        if not raw.strip():
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        depth = indent // 2
        line = raw.strip()
        colon = key_end(line)
        if colon == -1:
            continue
        head, value = line[:colon], line[colon + 1:].strip()
        key = head.split("[", 1)[0]
        keys = keys[:depth] + [key]
        path = ".".join(keys)
        if "{" in head:     # table: name[N]{f1,f2}:
            count = int(head[head.index("[") + 1:head.index("]")])
            fields = head[head.index("{") + 1:head.index("}")].split(",")
            rows = []
            for _ in range(count):
                if i >= len(lines):
                    break
                rows.append(dict(zip(fields, cells(lines[i].strip()))))
                i += 1
            tables[path] = rows
        else:
            values[path] = scalar(value)
    return values, tables


def metric(tables, test, name, x="0", drive=None):
    for m in tables.get("metrics", []):
        n = m.get("name", "")
        if drive:
            n = n[3:] if len(n) > 3 and n[1:3] == ": " and n[0] == drive else None
        if m.get("id") == test and n == name and str(num(m.get("x"))) == str(num(x)):
            return num(m.get("value"))
    return None


def best(tables, tests, name, lower=False):
    vals = [v for v in (metric(tables, t, name) for t in tests) if v]
    if not vals:
        return None
    return min(vals) if lower else max(vals)


def run_entry(path):
    with open(path, encoding="utf-8-sig") as f:
        values, tables = parse(f.read())
    if values.get("app") != "ez-pc-bench" or values.get("kind", "") not in ("", "benchmark"):
        return None
    name = os.path.basename(path)[: -len(".toon.txt")]
    mods = tables.get("system.ram.modules", [])
    ram_bytes = num(values.get("system.ram.installed_bytes")) or num(values.get("system.ram.total_bytes")) or 0
    ram = "%.0f GB" % (ram_bytes / 2**30) if ram_bytes else ""
    if mods:
        ram += " %s-%s" % (mods[0].get("type", ""), mods[0].get("configured_mts", ""))
    drives = values.get("params.disk_drives", "C").split(",")
    drive = "C" if "C" in drives else (drives[0].strip() or "C")
    disk = next((d.get("model", "") for d in tables.get("system.drives", []) if d.get("letter") == drive), "")
    score = None
    if "score.version" in values:
        score = {
            "version": int(num(values["score.version"]) or 0),
            "status": values.get("score.status", ""),
            "overall": num(values.get("score.overall")),
            "flags": values.get("score.flags", ""),
            "components": {c["id"]: num(c.get("value")) for c in tables.get("score.components", [])},
        }
    scene = best(tables, ["gpu_vk.3d.scene", "gpu_d3d12.3d.scene"], "Avg frame time", lower=True)
    return {
        "name": name,
        "file": "library/" + os.path.basename(path),
        "date": values.get("timestamp", "").replace("T", " ")[:16],
        "cpu": values.get("system.cpu.brand", ""),
        "gpu": values.get("params.gpu", ""),
        "ram": ram.strip(),
        "disk": disk,
        "os": values.get("system.os", ""),
        "power": ("AC" if values.get("system.power.on_ac") == "true" else "Battery")
        + (", " + values["system.power.mode"] if values.get("system.power.mode") else ""),
        "score": score,
        "raw": {
            "cpu_st_sort": metric(tables, "cpu.work.sort", "Single thread"),
            "cpu_mt_sort": metric(tables, "cpu.work.sort", "Multi-thread"),
            "mem_read": metric(tables, "mem.bw.read", "Multi-thread"),
            "dram_latency": metric(tables, "mem.lat.chase", "Latency (4 KiB pages)", "268435456")
            or metric(tables, "mem.lat.chase", "Latency (2 MiB pages)", "268435456"),
            "scene_fps": 1000.0 / scene if scene else None,
            "disk_q1_read_lat": metric(tables, "disk.rnd4k_q1t1", "Read latency", drive=drive),
            "disk_seq_read": metric(tables, "disk.seq1m_q8t1", "Read", drive=drive),
        },
    }


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    lib, out = sys.argv[1], sys.argv[2]
    runs = []
    for f in sorted(os.listdir(lib)):
        if f.endswith(".toon.txt"):
            entry = run_entry(os.path.join(lib, f))
            if entry:
                runs.append(entry)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"runs": runs}, f, indent=1)
    print("site_library: %d run(s) -> %s" % (len(runs), out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
