// WHEA-Logger event counting (see whea.h). The subscription callback runs on a wevtapi worker thread.
#include "core/whea.h"
#include "core/timer.h"
#include <windows.h>
#include <winevt.h>
#include <atomic>
#include <string.h>

static const wchar_t* const WHEA_QUERY = L"*[System[Provider[@Name='Microsoft-Windows-WHEA-Logger']]]";

static SRWLOCK s_lock = SRWLOCK_INIT;
static u32 s_refs;
static EVT_HANDLE s_sub, s_render;
static std::atomic<u32> s_corrected, s_uncorrected, s_last_id;
static std::atomic<f64> s_last_time;

// Level and event id of one event. False if it can't be rendered.
static bool event_level_id(EVT_HANDLE render, EVT_HANDLE ev, u8* level, u32* id) {
    alignas(8) u8 buf[2048];
    DWORD used = 0, count = 0;
    if (!EvtRender(render, ev, EvtRenderEventValues, sizeof(buf), buf, &used, &count) || count <= EvtSystemEventID)
        return false;
    const EVT_VARIANT* v = (const EVT_VARIANT*)buf;
    *level = v[EvtSystemLevel].Type == EvtVarTypeByte ? v[EvtSystemLevel].ByteVal : 0;
    *id = v[EvtSystemEventID].Type == EvtVarTypeUInt16 ? v[EvtSystemEventID].UInt16Val : 0;
    return true;
}

static bool is_uncorrected(u8 level) { return level == 1 || level == 2; }    // critical / error

static DWORD WINAPI on_event(EVT_SUBSCRIBE_NOTIFY_ACTION action, void*, EVT_HANDLE ev) {
    if (action != EvtSubscribeActionDeliver) return 0;
    u8 level = 0;
    u32 id = 0;
    if (!event_level_id(s_render, ev, &level, &id)) level = 2;     // unreadable: count it, conservatively uncorrected
    (is_uncorrected(level) ? s_uncorrected : s_corrected).fetch_add(1, std::memory_order_relaxed);
    s_last_id.store(id, std::memory_order_relaxed);
    s_last_time.store(timer_now(), std::memory_order_release);
    return 0;
}

void whea_start() {
    AcquireSRWLockExclusive(&s_lock);
    if (s_refs++ == 0) {
        s_render = EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem);
        if (s_render)
            s_sub = EvtSubscribe(nullptr, nullptr, L"System", WHEA_QUERY, nullptr, nullptr, on_event,
                                 EvtSubscribeToFutureEvents);
    }
    ReleaseSRWLockExclusive(&s_lock);
}

void whea_stop() {
    AcquireSRWLockExclusive(&s_lock);
    if (s_refs && --s_refs == 0) {
        if (s_sub) EvtClose(s_sub);         // waits for a running callback
        if (s_render) EvtClose(s_render);
        s_sub = s_render = nullptr;
    }
    ReleaseSRWLockExclusive(&s_lock);
}

void whea_snapshot(WheaCounts* out) {
    out->corrected = s_corrected.load(std::memory_order_relaxed);
    out->uncorrected = s_uncorrected.load(std::memory_order_relaxed);
    out->last_event_id = s_last_id.load(std::memory_order_relaxed);
    out->last_time = s_last_time.load(std::memory_order_acquire);
    AcquireSRWLockShared(&s_lock);
    out->active = s_sub != nullptr;
    ReleaseSRWLockShared(&s_lock);
}

bool whea_history(u32* corrected, u32* uncorrected) {
    *corrected = *uncorrected = 0;
    EVT_HANDLE q = EvtQuery(nullptr, L"System", WHEA_QUERY, EvtQueryChannelPath | EvtQueryForwardDirection);
    if (!q) return false;
    EVT_HANDLE render = EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem);
    EVT_HANDLE evs[64];
    DWORD n = 0;
    while (render && EvtNext(q, ARRAY_COUNT(evs), evs, INFINITE, 0, &n)) {
        for (DWORD i = 0; i < n; ++i) {
            u8 level = 0;
            u32 id = 0;
            if (!event_level_id(render, evs[i], &level, &id)) level = 2;
            ++*(is_uncorrected(level) ? uncorrected : corrected);
            EvtClose(evs[i]);
        }
    }
    const bool ok = render && GetLastError() == ERROR_NO_MORE_ITEMS;
    if (render) EvtClose(render);
    EvtClose(q);
    return ok;
}
