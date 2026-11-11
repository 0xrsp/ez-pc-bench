// Frame capture from ETW present events: session, event decoding, analysis and the results file.
// See frame_capture.h for the providers and what is measured.
#include "frames/frame_capture.h"
#include "core/results.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include "core/toon.h"
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tlhelp32.h>
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const GUID DXGI_PROVIDER = { 0xca11c036, 0x0102, 0x4a2d, { 0xa6, 0xad, 0xf0, 0x3c, 0xfe, 0xd5, 0xd3, 0xc9 } };
static const GUID DXGKRNL_PROVIDER = { 0x802ec45a, 0x1e99, 0x4b83, { 0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d } };
constexpr u64 DXGI_KEYWORD_EVENTS = 0x2;
constexpr u64 DXGKRNL_KEYWORD_PRESENT = 0x8000000;
static const USHORT DXGI_IDS[] = { 42, 43, 55, 56 };
static const USHORT DXGKRNL_IDS[] = { 168, 171, 184, 215 };
static const char* const KERNEL_NAMES[FE_KIND_COUNT] = { "", "", "Flip", "PresentHistory", "Present", "PresentHistoryDetailed" };
static const wchar_t SESSION_NAME[] = L"EzPcBench-FrameCapture";
constexpr u32 DXGI_PRESENT_ALLOW_TEARING_FLAG = 0x200;

CaptureLive g_capture;

struct Capture {
    FrameEvent* events;             // CAPTURE_MAX_EVENTS, VirtualAlloc'd on first use
    f32* frame_ms;                  // CAPTURE_MAX_EVENTS each
    f32* frame_t;
    u32 count;
    u32 live[FE_KIND_COUNT];        // events per kind, for the live present count
    std::atomic<u32> lost;          // events dropped because the buffer was full
    u32 pids[CAPTURE_MAX_PIDS];
    u32 pid_count;                  // 0 = every process but ours
    char exe[64];
    TRACEHANDLE session, trace;
    HANDLE thread, stop_event;
    FrameCapture result;
};
static Capture s_cap;
static f32 s_sorted[CAPTURE_MAX_EVENTS];

static bool ensure_buffers() {
    if (s_cap.events) return true;
    const u64 ev = (u64)CAPTURE_MAX_EVENTS * sizeof(FrameEvent), fr = (u64)CAPTURE_MAX_EVENTS * sizeof(f32);
    u8* p = (u8*)VirtualAlloc(nullptr, ev + 2 * fr, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return false;
    s_cap.events = (FrameEvent*)p;
    s_cap.frame_ms = (f32*)(p + ev);
    s_cap.frame_t = (f32*)(p + ev + fr);
    return true;
}

// ---- event decoding -----------------------------------------------------------------------------------

// Decodes a present event into `out`; false for anything else. DXGI Present_Start / PresentMPO_Start payload
// (manifest template): pIDXGISwapChain (pointer), Flags (u32), SyncInterval (u32), ...
static bool decode_record(const EVENT_RECORD* r, FrameEvent* out) {
    const EVENT_HEADER* h = &r->EventHeader;
    const u16 id = h->EventDescriptor.Id;
    u8 kind;
    if (IsEqualGUID(h->ProviderId, DXGI_PROVIDER)) {
        if (id == 42 || id == 55) kind = FE_DXGI_START;
        else if (id == 43 || id == 56) kind = FE_DXGI_STOP;
        else return false;
    } else if (IsEqualGUID(h->ProviderId, DXGKRNL_PROVIDER)) {
        if (id == 168) kind = FE_K_FLIP;
        else if (id == 171) kind = FE_K_HISTORY;
        else if (id == 184) kind = FE_K_PRESENT;
        else if (id == 215) kind = FE_K_HISTORY_DETAILED;
        else return false;
    } else {
        return false;
    }
    *out = {};
    out->time = (u64)h->TimeStamp.QuadPart;
    out->pid = h->ProcessId;
    out->tid = h->ThreadId;
    out->kind = kind;
    if (kind == FE_DXGI_START && r->UserData) {
        const u32 ptr = (h->Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : 8;
        const u8* d = (const u8*)r->UserData;
        if (r->UserDataLength >= ptr + 8) {
            memcpy(&out->swapchain, d, ptr);
            memcpy(&out->flags, d + ptr, 4);
            u32 sync;
            memcpy(&sync, d + ptr + 4, 4);
            out->sync_interval = (u16)(sync < 0xFFFF ? sync : 0xFFFF);
        }
    }
    return true;
}

static bool pid_wanted(u32 pid) {
    if (s_cap.pid_count == 0) return pid != GetCurrentProcessId() && pid != 0;
    for (u32 i = 0; i < s_cap.pid_count; ++i)
        if (s_cap.pids[i] == pid) return true;
    return false;
}

static void WINAPI on_event(EVENT_RECORD* r) {
    FrameEvent e;
    if (!decode_record(r, &e) || !pid_wanted(e.pid)) return;
    if (s_cap.count >= CAPTURE_MAX_EVENTS) { s_cap.lost.fetch_add(1, std::memory_order_relaxed); return; }
    s_cap.events[s_cap.count++] = e;
    g_capture.events.store(s_cap.count, std::memory_order_relaxed);
    // Live count: DXGI presents, or the most frequent kernel present event when there are none (approximate).
    if (e.kind != FE_DXGI_STOP) {
        ++s_cap.live[e.kind];
        const u32 k = s_cap.live[FE_K_FLIP] > s_cap.live[FE_K_HISTORY] ? s_cap.live[FE_K_FLIP] : s_cap.live[FE_K_HISTORY];
        const u32 kk = s_cap.live[FE_K_PRESENT] > s_cap.live[FE_K_HISTORY_DETAILED] ? s_cap.live[FE_K_PRESENT] : s_cap.live[FE_K_HISTORY_DETAILED];
        g_capture.presents.store(s_cap.live[FE_DXGI_START] ? s_cap.live[FE_DXGI_START] : k > kk ? k : kk, std::memory_order_relaxed);
    }
}

// ---- analysis -------------------------------------------------------------------------------------------

struct PidStat { u32 pid; u32 counts[FE_KIND_COUNT]; };
struct ChainStat { u64 swapchain; u32 count; };

static u32 kernel_best(const PidStat* p) {
    u32 best = 0;
    for (u32 k = FE_K_FLIP; k < FE_KIND_COUNT; ++k)
        if (p->counts[k] > p->counts[best] || best == 0) best = k;
    return best;
}

bool capture_analyze(const FrameEvent* ev, u32 n, u32 only_pid, f32* ms, f32* t, FrameCapture* out, char* err, u32 err_size) {
    const char* exe_keep = out->process;
    char process[64];
    snprintf(process, sizeof(process), "%s", exe_keep);
    memset(out, 0, sizeof(*out));
    out->present_call_ms = NAN;
    out->sync_interval = out->tearing = -1;

    // Presents per process and kind.
    static PidStat pids[256];
    u32 np = 0;
    for (u32 i = 0; i < n; ++i) {
        if (only_pid && ev[i].pid != only_pid) continue;
        u32 k = 0;
        while (k < np && pids[k].pid != ev[i].pid) ++k;
        if (k == np) {
            if (np == ARRAY_COUNT(pids)) continue;
            pids[np] = {};
            pids[np++].pid = ev[i].pid;
        }
        pids[k].counts[ev[i].kind]++;
    }
    // Busiest DXGI presenter, else busiest kernel presenter.
    const PidStat* pick = nullptr;
    for (u32 k = 0; k < np; ++k)
        if (pids[k].counts[FE_DXGI_START] && (!pick || pids[k].counts[FE_DXGI_START] > pick->counts[FE_DXGI_START])) pick = &pids[k];
    const bool dxgi = pick != nullptr;
    if (!pick)
        for (u32 k = 0; k < np; ++k) {
            const u32 kb = kernel_best(&pids[k]);
            if (pids[k].counts[kb] && (!pick || pids[k].counts[kb] > pick->counts[kernel_best(pick)])) pick = &pids[k];
        }
    if (!pick) { snprintf(err, err_size, "no present events from the target"); return false; }
    out->pid = pick->pid;
    const u32 kind = dxgi ? (u32)FE_DXGI_START : kernel_best(pick);

    // DXGI: the swapchain with the most presents (a game's main swapchain, not an overlay's).
    if (dxgi) {
        static ChainStat chains[64];
        u32 nc = 0;
        for (u32 i = 0; i < n; ++i) {
            if (ev[i].pid != out->pid || ev[i].kind != FE_DXGI_START) continue;
            u32 c = 0;
            while (c < nc && chains[c].swapchain != ev[i].swapchain) ++c;
            if (c == nc) {
                if (nc == ARRAY_COUNT(chains)) continue;
                chains[nc++] = { ev[i].swapchain, 0 };
            }
            chains[c].count++;
        }
        u32 best = 0;
        for (u32 c = 1; c < nc; ++c)
            if (chains[c].count > chains[best].count) best = c;
        out->swapchain = chains[best].swapchain;
        snprintf(out->api, sizeof(out->api), "DXGI");
    } else {
        snprintf(out->api, sizeof(out->api), "DxgKrnl %s", KERNEL_NAMES[kind]);
    }

    // Frame times, time spent in Present() (start -> stop on the same thread), sync interval, tearing.
    struct Pending { u32 tid; u64 time; };
    Pending pend[64] = {};
    u32 sync_hist[5] = {}, tearing = 0, starts = 0, call_n = 0;
    f64 call_sum = 0.0;
    u64 first = 0, prev = 0;
    u32 frames = 0;
    for (u32 i = 0; i < n; ++i) {
        const FrameEvent* e = &ev[i];
        if (e->pid != out->pid) continue;
        if (dxgi && e->kind == FE_DXGI_STOP) {
            for (u32 k = 0; k < ARRAY_COUNT(pend); ++k)
                if (pend[k].time && pend[k].tid == e->tid) {
                    call_sum += (f64)(e->time - pend[k].time) * 1e-4;
                    ++call_n;
                    pend[k].time = 0;
                    break;
                }
            continue;
        }
        if (e->kind != kind || (dxgi && e->swapchain != out->swapchain)) continue;
        if (dxgi) {
            u32 k = 0;
            while (k < ARRAY_COUNT(pend) - 1 && pend[k].time && pend[k].tid != e->tid) ++k;
            pend[k] = { e->tid, e->time };
            sync_hist[e->sync_interval < 4 ? e->sync_interval : 4]++;
            tearing += (e->flags & DXGI_PRESENT_ALLOW_TEARING_FLAG) != 0;
            ++starts;
        }
        if (prev) {
            ms[frames] = (f32)((f64)(e->time - prev) * 1e-4);      // 100 ns -> ms
            t[frames] = (f32)((f64)(e->time - first) * 1e-7);
            ++frames;
        } else {
            first = e->time;
        }
        prev = e->time;
    }
    if (frames < 1) { snprintf(err, err_size, "fewer than 2 presents from process %u", out->pid); return false; }

    snprintf(out->process, sizeof(out->process), "%s", process[0] ? process : "");
    if (!out->process[0]) snprintf(out->process, sizeof(out->process), "pid %u", out->pid);
    out->frames = frames;
    out->frame_ms = ms;
    out->frame_t = t;
    out->seconds = (f64)(prev - first) * 1e-7;
    if (dxgi) {
        if (call_n) out->present_call_ms = call_sum / call_n;
        u32 mode = 0;
        for (u32 k = 1; k < 5; ++k)
            if (sync_hist[k] > sync_hist[mode]) mode = k;
        out->sync_interval = (i32)mode;
        out->tearing = tearing * 2 > starts ? 1 : 0;
    }

    memcpy(s_sorted, ms, frames * sizeof(f32));
    std::sort(s_sorted, s_sorted + frames);
    f64 sum = 0.0;
    for (u32 i = 0; i < frames; ++i) sum += s_sorted[i];
    auto pct = [&](f64 q) { const u32 i = (u32)(q * (frames - 1) + 0.5); return (f64)s_sorted[i < frames ? i : frames - 1]; };
    auto slowest_avg_fps = [&](f64 frac) {
        u32 k = (u32)(frames * frac + 0.5);
        if (k < 1) k = 1;
        f64 s = 0.0;
        for (u32 i = frames - k; i < frames; ++i) s += s_sorted[i];
        return s > 0.0 ? 1000.0 * k / s : 0.0;
    };
    out->avg_ms = sum / frames;
    out->p50_ms = pct(0.5);
    out->p95_ms = pct(0.95);
    out->p99_ms = pct(0.99);
    out->p999_ms = pct(0.999);
    out->max_ms = s_sorted[frames - 1];
    out->avg_fps = sum > 0.0 ? 1000.0 * frames / sum : 0.0;
    out->low1_avg_fps = slowest_avg_fps(0.01);
    out->low01_avg_fps = slowest_avg_fps(0.001);
    return true;
}

// ---- processes ----------------------------------------------------------------------------------------

bool capture_allowed() {
    HANDLE token = nullptr;
    bool ok = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e = {};
        DWORD len = 0;
        ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &len) && e.TokenIsElevated;
        CloseHandle(token);
    }
    if (ok) return true;
    // BUILTIN\Performance Log Users (S-1-5-32-559) may also run ETW sessions.
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID sid = nullptr;
    BOOL member = FALSE;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_LOGGING_USERS, 0, 0, 0, 0, 0, 0, &sid)) {
        if (!CheckTokenMembership(nullptr, sid, &member)) member = FALSE;
        FreeSid(sid);
    }
    return member != FALSE;
}

u32 capture_find_pids(const char* target, u32* pids, u32 max, char* exe, u32 exe_size) {
    if (exe_size) exe[0] = 0;
    char* end = nullptr;
    const unsigned long num = strtoul(target, &end, 10);
    const bool by_pid = end != target && *end == 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    char want[MAX_PATH];
    snprintf(want, sizeof(want), "%s", target);
    const size_t wl = strlen(want);
    const bool has_ext = wl > 4 && !_stricmp(want + wl - 4, ".exe");
    PROCESSENTRY32W pe = { sizeof(pe) };
    u32 n = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && n < max; ok = Process32NextW(snap, &pe)) {
        char name[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, name, sizeof(name), nullptr, nullptr);
        bool match;
        if (by_pid) {
            match = pe.th32ProcessID == num;
        } else {
            char stem[MAX_PATH];
            snprintf(stem, sizeof(stem), "%s", name);
            const size_t sl = strlen(stem);
            if (!has_ext && sl > 4 && !_stricmp(stem + sl - 4, ".exe")) stem[sl - 4] = 0;
            match = !_stricmp(stem, want);
        }
        if (!match) continue;
        pids[n++] = pe.th32ProcessID;
        if (exe_size && !exe[0]) snprintf(exe, exe_size, "%s", name);
    }
    CloseHandle(snap);
    return n;
}

// ---- session ------------------------------------------------------------------------------------------

struct TraceProps {
    EVENT_TRACE_PROPERTIES p;
    wchar_t name[64];
};

static void props_init(TraceProps* t) {
    memset(t, 0, sizeof(*t));
    t->p.Wnode.BufferSize = sizeof(*t);
    t->p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    t->p.Wnode.ClientContext = 1;       // QPC time stamps
    t->p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    t->p.BufferSize = 64;               // KiB
    t->p.MinimumBuffers = 16;
    t->p.MaximumBuffers = 128;
    t->p.FlushTimer = 1;                // s: deliver at least once a second
    t->p.LoggerNameOffset = offsetof(TraceProps, name);
}

static void stop_session() {
    TraceProps props;
    props_init(&props);
    ControlTraceW(0, SESSION_NAME, &props.p, EVENT_TRACE_CONTROL_STOP);
}

// Enables a provider for the given event ids only; falls back to no id filter if the OS refuses it.
static ULONG enable_provider(const GUID* guid, u64 keyword, const USHORT* ids, u32 count) {
    u8 buf[sizeof(EVENT_FILTER_EVENT_ID) + 16 * sizeof(USHORT)] = {};
    EVENT_FILTER_EVENT_ID* f = (EVENT_FILTER_EVENT_ID*)buf;
    f->FilterIn = TRUE;
    f->Count = (USHORT)count;
    memcpy(f->Events, ids, count * sizeof(USHORT));
    EVENT_FILTER_DESCRIPTOR desc = {};
    desc.Ptr = (ULONGLONG)buf;
    desc.Size = (ULONG)(offsetof(EVENT_FILTER_EVENT_ID, Events) + count * sizeof(USHORT));
    desc.Type = EVENT_FILTER_TYPE_EVENT_ID;
    ENABLE_TRACE_PARAMETERS params = {};
    params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    params.EnableFilterDesc = &desc;
    params.FilterDescCount = 1;
    ULONG r = EnableTraceEx2(s_cap.session, guid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, keyword, 0, 0, &params);
    if (r != ERROR_SUCCESS)
        r = EnableTraceEx2(s_cap.session, guid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, keyword, 0, 0, nullptr);
    return r;
}

// Sorts the recorded events (real-time buffers arrive per CPU) and analyzes them.
static void finish(u32 etw_lost) {
    std::stable_sort(s_cap.events, s_cap.events + s_cap.count, [](const FrameEvent& a, const FrameEvent& b) { return a.time < b.time; });
    char err[160];
    snprintf(s_cap.result.process, sizeof(s_cap.result.process), "%s", s_cap.exe);
    if (!capture_analyze(s_cap.events, s_cap.count, 0, s_cap.frame_ms, s_cap.frame_t, &s_cap.result, err, sizeof(err))) {
        snprintf(g_capture.error, sizeof(g_capture.error), "%s", err);
        g_capture.state.store(CAP_FAILED, std::memory_order_release);
        return;
    }
    s_cap.result.events = s_cap.count;
    s_cap.result.dropped = etw_lost + s_cap.lost.load();
    g_capture.state.store(CAP_DONE, std::memory_order_release);
}

static DWORD WINAPI process_thread(void*) {
    ProcessTrace(&s_cap.trace, 1, nullptr, nullptr);    // returns once the session stops
    return 0;
}

static DWORD WINAPI capture_thread(void*) {
    HANDLE worker = CreateThread(nullptr, 0, process_thread, nullptr, 0, nullptr);
    WaitForSingleObject(s_cap.stop_event, (DWORD)(g_capture.seconds * 1000.0));
    TraceProps props;
    props_init(&props);
    ControlTraceW(s_cap.session, nullptr, &props.p, EVENT_TRACE_CONTROL_STOP);     // flushes; ProcessTrace returns
    if (worker) {
        WaitForSingleObject(worker, 10000);
        CloseHandle(worker);
    }
    CloseTrace(s_cap.trace);
    finish(props.p.EventsLost + props.p.RealTimeBuffersLost);
    return 0;
}

bool capture_start(const char* target, f64 seconds, char* err, u32 err_size) {
    if (g_capture.state.load() == CAP_RUNNING) { snprintf(err, err_size, "a capture is already running"); return false; }
    if (!ensure_buffers()) { snprintf(err, err_size, "out of memory"); return false; }
    if (s_cap.thread) { WaitForSingleObject(s_cap.thread, INFINITE); CloseHandle(s_cap.thread); s_cap.thread = nullptr; }
    s_cap.count = 0;
    s_cap.lost = 0;
    s_cap.result = {};
    s_cap.exe[0] = 0;
    s_cap.pid_count = 0;
    if (target && target[0] && strcmp(target, "*") != 0) {
        s_cap.pid_count = capture_find_pids(target, s_cap.pids, CAPTURE_MAX_PIDS, s_cap.exe, sizeof(s_cap.exe));
        if (!s_cap.pid_count) { snprintf(err, err_size, "no running process matches '%s'", target); return false; }
    }

    TraceProps props;
    props_init(&props);
    ULONG r = StartTraceW(&s_cap.session, SESSION_NAME, &props.p);
    if (r == ERROR_ALREADY_EXISTS) {     // left over from a crashed capture
        stop_session();
        props_init(&props);
        r = StartTraceW(&s_cap.session, SESSION_NAME, &props.p);
    }
    if (r == ERROR_ACCESS_DENIED) {
        snprintf(err, err_size, "access denied: frame capture uses an ETW real-time session, which needs administrator "
                                "rights or membership in the Performance Log Users group. Run EzPcBench as administrator.");
        return false;
    }
    if (r != ERROR_SUCCESS) { snprintf(err, err_size, "StartTrace failed (error %lu)", r); return false; }

    r = enable_provider(&DXGI_PROVIDER, DXGI_KEYWORD_EVENTS, DXGI_IDS, ARRAY_COUNT(DXGI_IDS));
    const ULONG rk = enable_provider(&DXGKRNL_PROVIDER, DXGKRNL_KEYWORD_PRESENT, DXGKRNL_IDS, ARRAY_COUNT(DXGKRNL_IDS));
    if (r != ERROR_SUCCESS && rk != ERROR_SUCCESS) {
        stop_session();
        snprintf(err, err_size, "enabling the DXGI / DxgKrnl providers failed (error %lu / %lu)", r, rk);
        return false;
    }

    EVENT_TRACE_LOGFILEW lf = {};
    lf.LoggerName = (LPWSTR)SESSION_NAME;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = on_event;
    s_cap.trace = OpenTraceW(&lf);
    if (s_cap.trace == INVALID_PROCESSTRACE_HANDLE) {
        const DWORD e = GetLastError();
        stop_session();
        snprintf(err, err_size, "OpenTrace failed (error %lu)", e);
        return false;
    }

    if (!s_cap.stop_event) s_cap.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(s_cap.stop_event);
    g_capture.events = 0;
    g_capture.presents = 0;
    memset(s_cap.live, 0, sizeof(s_cap.live));
    g_capture.error[0] = 0;
    g_capture.start = timer_now();
    g_capture.seconds = seconds;
    g_capture.state.store(CAP_RUNNING, std::memory_order_release);
    s_cap.thread = CreateThread(nullptr, 0, capture_thread, nullptr, 0, nullptr);
    if (!s_cap.thread) {
        CloseTrace(s_cap.trace);
        stop_session();
        g_capture.state.store(CAP_FAILED);
        snprintf(err, err_size, "creating the capture thread failed");
        return false;
    }
    return true;
}

void capture_stop() {
    if (s_cap.stop_event) SetEvent(s_cap.stop_event);
}

void capture_wait() {
    if (!s_cap.thread) return;
    WaitForSingleObject(s_cap.thread, INFINITE);
    CloseHandle(s_cap.thread);
    s_cap.thread = nullptr;
}

const FrameCapture* capture_result() { return &s_cap.result; }

bool capture_load_etl(const wchar_t* path, const char* target, char* err, u32 err_size) {
    if (g_capture.state.load() == CAP_RUNNING) { snprintf(err, err_size, "a capture is running"); return false; }
    if (!ensure_buffers()) { snprintf(err, err_size, "out of memory"); return false; }
    s_cap.count = 0;
    s_cap.lost = 0;
    s_cap.result = {};
    s_cap.exe[0] = 0;
    s_cap.pid_count = 0;
    u32 only = 0;
    if (target && target[0]) {
        char* end = nullptr;
        only = (u32)strtoul(target, &end, 10);
        if (end == target || *end) {     // a name: only processes running now can be resolved
            only = 0;
            s_cap.pid_count = capture_find_pids(target, s_cap.pids, CAPTURE_MAX_PIDS, s_cap.exe, sizeof(s_cap.exe));
            if (!s_cap.pid_count) { snprintf(err, err_size, "'%s' isn't running: give a pid for a recorded trace", target); return false; }
        } else {
            s_cap.pids[0] = only;
            s_cap.pid_count = 1;
        }
    }
    EVENT_TRACE_LOGFILEW lf = {};
    lf.LogFileName = (LPWSTR)path;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = on_event;
    TRACEHANDLE h = OpenTraceW(&lf);
    if (h == INVALID_PROCESSTRACE_HANDLE) { snprintf(err, err_size, "cannot open the trace file (error %lu)", GetLastError()); return false; }
    const ULONG r = ProcessTrace(&h, 1, nullptr, nullptr);
    CloseTrace(h);
    if (r != ERROR_SUCCESS) { snprintf(err, err_size, "reading the trace failed (error %lu)", r); return false; }
    finish(0);
    if (g_capture.state.load() != CAP_DONE) { snprintf(err, err_size, "%s", g_capture.error); return false; }
    return true;
}

// ---- results file -----------------------------------------------------------------------------------------

void capture_default_name(const FrameCapture* c, char* out, u32 size) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    char exe[64];
    snprintf(exe, sizeof(exe), "%s", c->process);
    if (char* dot = strrchr(exe, '.')) *dot = 0;
    for (char* p = exe; *p; ++p)
        if (*p == ' ' || strchr("\\/:*?\"<>|", *p)) *p = '_';
    snprintf(out, size, "frames_%04d%02d%02d_%02d%02d%02d_%s", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
             t.tm_min, t.tm_sec, exe);
}

bool capture_write_toon(const wchar_t* path, const FrameCapture* c) {
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    char stamp[32];
    time_t now = time(nullptr);
    struct tm tm_local;
    localtime_s(&tm_local, &now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm_local);
    toon_str(f, 0, "app", "ez-pc-bench");
    toon_num(f, 0, "format", 1);
    toon_str(f, 0, "kind", "frames");
    toon_str(f, 0, "timestamp", stamp);
    results_write_system(f);
    toon_key(f, 0, "params");
    toon_num(f, 1, "duration_ms", round(c->seconds * 1000.0));
    toon_key(f, 0, "capture");
    toon_str(f, 1, "process", c->process);
    toon_num(f, 1, "pid", c->pid);
    toon_str(f, 1, "api", c->api);
    toon_num(f, 1, "seconds", c->seconds);
    toon_num(f, 1, "frames", c->frames);
    toon_num(f, 1, "sync_interval", c->sync_interval);
    toon_str(f, 1, "tearing", c->tearing < 0 ? "unknown" : c->tearing ? "yes" : "no");
    toon_num(f, 1, "events", c->events);
    toon_num(f, 1, "dropped_events", c->dropped);

    toon_table(f, 0, "tests", 1, "id,suite,group,name,status,seconds,kind,rows,cols,x_name,x_unit,error");
    ToonRow row = toon_row(f, 1);
    toon_cell_str(&row, "capture.frames");
    toon_cell_str(&row, "Frame capture");
    toon_cell_str(&row, "Capture");
    toon_cell_str(&row, c->process);
    toon_cell_str(&row, "done");
    toon_cell_num(&row, c->seconds);
    toon_cell_str(&row, "values");
    toon_cell_num(&row, 0);
    toon_cell_num(&row, 0);
    toon_cell_str(&row, "");
    toon_cell_str(&row, "");
    toon_cell_str(&row, "");
    toon_row_end(&row);

    struct M { const char* name; const char* unit; f64 v; };
    const M m[] = {
        { "Avg frame time", "ms", c->avg_ms }, { "Median frame time", "ms", c->p50_ms }, { "p95 frame time", "ms", c->p95_ms },
        { "p99 frame time", "ms", c->p99_ms }, { "p99.9 frame time", "ms", c->p999_ms }, { "Max frame time", "ms", c->max_ms },
        { "1% low average", "fps", c->low1_avg_fps }, { "0.1% low average", "fps", c->low01_avg_fps },
        { "Present call avg", "ms", c->present_call_ms },
    };
    u32 nm = 0;
    for (u32 i = 0; i < ARRAY_COUNT(m); ++i) nm += isfinite(m[i].v);
    toon_table(f, 0, "metrics", nm, "id,name,unit,value,x");
    for (u32 i = 0; i < ARRAY_COUNT(m); ++i) {
        if (!isfinite(m[i].v)) continue;
        row = toon_row(f, 1);
        toon_cell_str(&row, "capture.frames");
        toon_cell_str(&row, m[i].name);
        toon_cell_str(&row, m[i].unit);
        toon_cell_num(&row, m[i].v);
        toon_cell_num(&row, 0);
        toon_row_end(&row);
    }

    // Every frame: end time (s since the first present) and frame time (ms), rounded to 0.1 us / 1 us.
    toon_table(f, 0, "frames", c->frames, "t,ms");
    for (u32 i = 0; i < c->frames; ++i) fprintf(f, "  %.4f,%.4f\n", c->frame_t[i], c->frame_ms[i]);
    const bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

// ---- self-test (CLI --capture-selftest): decoding + analysis on synthetic ETW records ----------------------

static void fake_record(EVENT_RECORD* r, u8* payload, const GUID& provider, u16 id, u32 pid, u32 tid, u64 time,
                        u64 swapchain, u32 flags, u32 sync) {
    memset(r, 0, sizeof(*r));
    r->EventHeader.ProviderId = provider;
    r->EventHeader.EventDescriptor.Id = id;
    r->EventHeader.ProcessId = pid;
    r->EventHeader.ThreadId = tid;
    r->EventHeader.TimeStamp.QuadPart = (LONGLONG)time;
    r->EventHeader.Flags = EVENT_HEADER_FLAG_64_BIT_HEADER;
    memcpy(payload, &swapchain, 8);
    memcpy(payload + 8, &flags, 4);
    memcpy(payload + 12, &sync, 4);
    memset(payload + 16, 0, 8);         // DirtyRects, ScrollRects
    r->UserData = payload;
    r->UserDataLength = id == 42 ? 24 : id == 43 ? 4 : 0;
}

bool capture_selftest(char* report, u32 size) {
    if (!ensure_buffers()) { snprintf(report, size, "out of memory"); return false; }
    // pid 100: DXGI, main swapchain 0xA0 at 16.667 ms with one 50 ms hitch (tearing, sync 0) + an overlay
    // swapchain 0xB0 at 4 Hz; Present() takes 0.2 ms. pid 200: kernel-only (PresentHistory) at 30 fps.
    u32 n = 0;
    EVENT_RECORD r;
    u8 payload[32];
    const u64 ms = 10000;   // 100 ns units
    u64 t = 1000000;
    for (u32 i = 0; i < 600; ++i) {
        const u64 dt = i == 300 ? 50 * ms : 16667 * ms / 1000;
        t += dt;
        fake_record(&r, payload, DXGI_PROVIDER, 42, 100, 7, t, 0xA0, DXGI_PRESENT_ALLOW_TEARING_FLAG, 0);
        n += decode_record(&r, &s_cap.events[n]);
        fake_record(&r, payload, DXGI_PROVIDER, 43, 100, 7, t + 2 * ms / 10, 0, 0, 0);
        n += decode_record(&r, &s_cap.events[n]);
        if (i % 15 == 0) {
            fake_record(&r, payload, DXGI_PROVIDER, 42, 100, 9, t + 5, 0xB0, 0, 1);
            n += decode_record(&r, &s_cap.events[n]);
        }
        if (i % 2 == 0) {
            fake_record(&r, payload, DXGKRNL_PROVIDER, 171, 200, 3, t + 7, 0, 0, 0);
            n += decode_record(&r, &s_cap.events[n]);
        }
        fake_record(&r, payload, DXGKRNL_PROVIDER, 999, 100, 7, t + 9, 0, 0, 0);   // not a present event
        n += decode_record(&r, &s_cap.events[n]);
    }
    std::stable_sort(s_cap.events, s_cap.events + n, [](const FrameEvent& a, const FrameEvent& b) { return a.time < b.time; });

    FrameCapture a = {}, b = {};
    snprintf(a.process, sizeof(a.process), "synthetic.exe");
    char err[160];
    const bool ok_a = capture_analyze(s_cap.events, n, 0, s_cap.frame_ms, s_cap.frame_t, &a, err, sizeof(err));
    char ea[160];
    snprintf(ea, sizeof(ea), "%s", ok_a ? "" : err);
    static f32 ms_b[1024], t_b[1024];
    const bool ok_b = capture_analyze(s_cap.events, n, 200, ms_b, t_b, &b, err, sizeof(err));
    const bool pass = ok_a && ok_b && a.pid == 100 && a.swapchain == 0xA0 && !strcmp(a.api, "DXGI") && a.frames == 599 &&
                      fabs(a.p50_ms - 16.667) < 0.01 && fabs(a.max_ms - 50.0) < 0.01 && a.sync_interval == 0 &&
                      a.tearing == 1 && fabs(a.present_call_ms - 0.2) < 0.01 && b.pid == 200 &&
                      !strcmp(b.api, "DxgKrnl PresentHistory") && b.frames == 299 && fabs(b.avg_ms - 33.4455) < 0.01;
    snprintf(report, size,
             "%s: %u events decoded (of %u records)\n"
             "  busiest:  pid %u, %s, swapchain 0x%llX, %u frames, avg %.3f ms, median %.3f, p99 %.3f, max %.3f, "
             "1%% low avg %.1f fps, Present() %.3f ms, sync %d, tearing %d %s\n"
             "  pid 200:  %s, %u frames, avg %.3f ms %s",
             pass ? "PASS" : "FAIL", n, 600u * 3 + 40 + 300, a.pid, a.api, (unsigned long long)a.swapchain, a.frames, a.avg_ms,
             a.p50_ms, a.p99_ms, a.max_ms, a.low1_avg_fps, a.present_call_ms, a.sync_interval, a.tearing, ea, b.api,
             b.frames, b.avg_ms, ok_b ? "" : err);
    a.events = n;
    s_cap.result = a;       // capture_result() = the busiest process (pid 100)
    return pass;
}
