#include "core/threads.h"
#include "core/perf.h"
#include <windows.h>
#include <string.h>

CpuTopology g_topo;

void topo_init() {
    CpuTopology* t = &g_topo;
    memset(t, 0, sizeof(*t));

    ULONG size = 0;
    GetSystemCpuSetInformation(nullptr, 0, &size, GetCurrentProcess(), 0);
    u8* buf = (u8*)HeapAlloc(GetProcessHeap(), 0, size);
    if (!buf || !GetSystemCpuSetInformation((SYSTEM_CPU_SET_INFORMATION*)buf, size, &size, GetCurrentProcess(), 0)) {
        // Fallback: treat every LP of group 0 as its own core.
        t->lp_count = t->core_count = GetActiveProcessorCount(0) < MAX_LP ? GetActiveProcessorCount(0) : MAX_LP;
        for (u32 i = 0; i < t->lp_count; ++i) { t->number[i] = (u8)i; t->core[i] = (u16)i; t->order[i] = i; }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        return;
    }

    u32 core_key[MAX_LP];   // Windows CoreIndex of each of our cores
    u8 core_sched[MAX_LP] = {}, core_eff[MAX_LP] = {};
    for (ULONG off = 0; off < size && t->lp_count < MAX_LP;) {
        const SYSTEM_CPU_SET_INFORMATION* e = (const SYSTEM_CPU_SET_INFORMATION*)(buf + off);
        off += e->Size;
        if (e->Type != CpuSetInformation) continue;
        const u32 lp = t->lp_count++;
        t->group[lp] = e->CpuSet.Group;
        t->number[lp] = e->CpuSet.LogicalProcessorIndex;
        t->sched_class[lp] = e->CpuSet.SchedulingClass;
        u32 c = 0;
        while (c < t->core_count && !(core_key[c] == e->CpuSet.CoreIndex)) ++c;
        if (c == t->core_count) {
            core_key[c] = e->CpuSet.CoreIndex;
            core_sched[c] = e->CpuSet.SchedulingClass;
            core_eff[c] = e->CpuSet.EfficiencyClass;
            t->core_count++;
        }
        t->core[lp] = (u16)c;
    }
    HeapFree(GetProcessHeap(), 0, buf);

    // Rank cores: efficiency class (big cores) then scheduling class (preferred cores), stable by index.
    u32 core_rank[MAX_LP];
    for (u32 c = 0; c < t->core_count; ++c) core_rank[c] = c;
    for (u32 i = 1; i < t->core_count; ++i) {
        u32 v = core_rank[i], j = i;
        auto better = [&](u32 a, u32 b) {
            if (core_eff[a] != core_eff[b]) return core_eff[a] > core_eff[b];
            return core_sched[a] > core_sched[b];
        };
        while (j > 0 && better(v, core_rank[j - 1])) { core_rank[j] = core_rank[j - 1]; --j; }
        core_rank[j] = v;
    }

    // Placement: first LP of each core in rank order, then second LPs, etc.
    u32 n = 0;
    bool used[MAX_LP] = {};
    while (n < t->lp_count) {
        for (u32 r = 0; r < t->core_count; ++r) {
            for (u32 lp = 0; lp < t->lp_count; ++lp) {
                if (!used[lp] && t->core[lp] == core_rank[r]) {
                    used[lp] = true;
                    t->order[n++] = lp;
                    break;
                }
            }
        }
    }
    t->best_lp = t->order[0];
}

void pin_current_thread(u32 lp) {
    GROUP_AFFINITY ga = {};
    ga.Group = g_topo.group[lp];
    ga.Mask = (KAFFINITY)1 << g_topo.number[lp];
    SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr);
}

u32 thread_count_for(u32 requested) {
    return (requested == 0 || requested > g_topo.lp_count) ? g_topo.lp_count : requested;
}

// ---- pool -------------------------------------------------------------------------
// Workers sleep on a generation counter (WaitOnAddress); the dispatcher sleeps on `remaining`.

struct Pool {
    std::atomic<u32> generation;
    std::atomic<u32> remaining;
    u32 count;
    PoolFn fn;
    void* user;
    u32 thread_count;
};
static Pool s_pool;

static DWORD WINAPI pool_worker(void* arg) {
    const u32 index = (u32)(uintptr_t)arg;
    perf_setup_worker_thread();
    pin_current_thread(g_topo.order[index]);
    u32 seen = 0;
    for (;;) {
        u32 gen = s_pool.generation.load(std::memory_order_acquire);
        while (gen == seen) {
            WaitOnAddress(&s_pool.generation, &seen, sizeof(seen), INFINITE);
            gen = s_pool.generation.load(std::memory_order_acquire);
        }
        seen = gen;
        if (index < s_pool.count) {
            s_pool.fn(index, s_pool.count, s_pool.user);
            pin_current_thread(g_topo.order[index]);    // tasks may re-pin themselves
            if (s_pool.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) WakeByAddressAll(&s_pool.remaining);
        }
    }
}

void pool_init() {
    if (s_pool.thread_count) return;
    if (!g_topo.lp_count) topo_init();
    for (u32 i = 0; i < g_topo.lp_count; ++i) {
        HANDLE h = CreateThread(nullptr, 256 * 1024, pool_worker, (void*)(uintptr_t)i, 0, nullptr);
        if (h) CloseHandle(h);
    }
    s_pool.thread_count = g_topo.lp_count;
}

void pool_dispatch(u32 count, PoolFn fn, void* user) {
    if (count > s_pool.thread_count) count = s_pool.thread_count;
    s_pool.count = count;
    s_pool.fn = fn;
    s_pool.user = user;
    s_pool.remaining.store(count, std::memory_order_release);
    s_pool.generation.fetch_add(1, std::memory_order_acq_rel);
    WakeByAddressAll(&s_pool.generation);
}

void pool_wait() {
    u32 r = s_pool.remaining.load(std::memory_order_acquire);
    while (r != 0) {
        WaitOnAddress(&s_pool.remaining, &r, sizeof(r), INFINITE);
        r = s_pool.remaining.load(std::memory_order_acquire);
    }
}
