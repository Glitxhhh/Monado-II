// [XCDE] T112/T043: sampling profiler. XCDE_SAMPLE=<from>:<to> (presented frames) samples every thread of the process
// (guest threads, render thread, driver and worker threads) at XCDE_SAMPLE_HZ (default 1000) and writes
// build/samples.txt (XCDE_SAMPLE_OUT=<file>): CPU share per thread, per module and per function (guest functions are
// f_<addr>, runtime ones by name from the PDB, DLLs as module+offset folded to the module).
// A thread counts in an interval only if it used CPU cycles in it (QueryThreadCycleTime), so blocked threads cost
// nothing and don't show up as "busy in a wait function". The sample is the instruction pointer at the tick.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "../rt.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <pthread.h>
#include <intrin.h>

const char *plat_symbol(uint64_t pc, uint64_t *offset);     // runtime/platform/windows.c
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

#define MAXTH 256
#define NPC (1 << 20)
typedef struct { DWORD tid; HANDLE h; ULONG64 cycles; uint64_t samples, cyc_total; char name[64]; } Th;
static Th th[MAXTH];
static int nth;
static struct { uint64_t pc; uint64_t n; uint16_t th; } pcs[NPC];  // n: cycles charged to that pc  // open addressing (pc, thread) -> count
static uint64_t n_ticks, n_lost;
// [XCDE] XCDE_SAMPLE_CALLERS=1: a tick that lands outside the exe (a DLL: the driver, ntdll's clock or wait functions) is
// also charged to the first exe frame up the stack (RtlVirtualUnwind): which of our functions spends time in DLLs.
static struct { uint64_t pc; uint64_t n; uint16_t th; } cpcs[1 << 16];
static uint64_t g_exe_lo, g_exe_hi;
static int g_callers = -1;
static void add_caller(uint64_t pc, int ti, uint64_t w) {
    uint64_t k = (pc * 0x9E3779B97F4A7C15ull ^ (uint64_t)ti) >> 48;
    for (uint64_t i = 0; i < (1 << 16); i++) {
        uint64_t s = (k + i) & ((1 << 16) - 1);
        if (cpcs[s].n && (cpcs[s].pc != pc || cpcs[s].th != ti)) continue;
        cpcs[s].pc = pc, cpcs[s].th = (uint16_t)ti, cpcs[s].n += w;
        return;
    }
}
static uint64_t exe_caller(CONTEXT *ctx) {  // first return address inside the exe, or 0
    CONTEXT c = *ctx;
    for (int depth = 0; depth < 16; depth++) {
        if (c.Rip >= g_exe_lo && c.Rip < g_exe_hi) return depth ? c.Rip : 0;
        DWORD64 base = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (!rf) {  // leaf function: the return address is at the stack top
            if (!c.Rsp) return 0;
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
        } else {
            PVOID hd = NULL; DWORD64 ef = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &ef, NULL);
        }
        if (!c.Rip) return 0;
    }
    return 0;
}
static volatile long g_from, g_to;
static volatile int g_state;
static double g_secs, g_tsc;  // 0 idle, 1 sampling, 2 done

static Th *thread_slot(DWORD tid) {
    for (int i = 0; i < nth; i++) if (th[i].tid == tid) return &th[i];
    if (nth == MAXTH) return NULL;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return NULL;
    Th *t = &th[nth++];
    memset(t, 0, sizeof *t);
    t->tid = tid, t->h = h;
    QueryThreadCycleTime(h, &t->cycles);
    PWSTR desc = NULL;
    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc && *desc) WideCharToMultiByte(CP_UTF8, 0, desc, -1, t->name, sizeof t->name, NULL, NULL);
    if (desc) LocalFree(desc);
    if (!t->name[0]) snprintf(t->name, sizeof t->name, "tid %lu", tid);
    return t;
}

static void add_pc(uint64_t pc, int ti, uint64_t w) {
    uint64_t k = (pc * 0x9E3779B97F4A7C15ull ^ (uint64_t)ti) >> 44;
    for (uint64_t i = 0; i < NPC; i++) {
        uint64_t s = (k + i) & (NPC - 1);
        if (pcs[s].n && (pcs[s].pc != pc || pcs[s].th != ti)) continue;
        pcs[s].pc = pc, pcs[s].th = (uint16_t)ti, pcs[s].n += w;
        return;
    }
    n_lost++;
}

static void refresh_threads(DWORD self) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {.dwSize = sizeof te};
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) thread_slot(te.th32ThreadID);
    CloseHandle(snap);
}

// [XCDE] hitch mode (XCDE_HITCH_SAMPLE=1): sampling runs the whole session and every sample also goes into a ring
// (time, thread, pc, cycles). device.c reports a slow frame (diag_hitch_sample); the sampler thread then logs what
// each thread did during that frame only - the battle hitches are one frame in a hundred, lost in an average profile.
#define RING (1 << 17)
static struct { int64_t t; uint64_t pc, cy; uint16_t th; } ring[RING];
static volatile uint64_t ring_n;
static int g_hitch;
static volatile int64_t hitch_lo, hitch_hi;  // (QPC) a requested window, 0: none
static int64_t qpc_now(void) { LARGE_INTEGER n; QueryPerformanceCounter(&n); return n.QuadPart; }

// Each thread's cycles since the last tick are charged to the pc it is at now (a thread that ran 1 % of the interval
// weighs 1 %): blocked threads cost nothing, busy ones are sampled where they are.
static void sample_once(void) {
    for (int i = 0; i < nth; i++) {
        Th *t = &th[i];
        ULONG64 cy = 0;
        if (!QueryThreadCycleTime(t->h, &cy) || cy == t->cycles) continue;
        uint64_t d = cy - t->cycles;
        t->cycles = cy;
        if (SuspendThread(t->h) == (DWORD)-1) continue;
        CONTEXT ctx = {.ContextFlags = g_callers ? CONTEXT_FULL : CONTEXT_CONTROL};
        if (GetThreadContext(t->h, &ctx)) {
            if (g_hitch) {
                uint64_t k = ring_n++ & (RING - 1);
                ring[k].t = qpc_now(); ring[k].pc = ctx.Rip; ring[k].cy = d; ring[k].th = (uint16_t)i;
                if (g_callers && (ctx.Rip < g_exe_lo || ctx.Rip >= g_exe_hi)) { uint64_t cp = exe_caller(&ctx); if (cp) ring[k].pc = cp; }
                ResumeThread(t->h);
                continue;
            }
            t->cyc_total += d; t->samples++; add_pc(ctx.Rip, i, d);
            if (g_callers && (ctx.Rip < g_exe_lo || ctx.Rip >= g_exe_hi)) { uint64_t cp = exe_caller(&ctx); if (cp) add_caller(cp, i, d); }
        }
        ResumeThread(t->h);
    }
    n_ticks++;
}

typedef struct { char name[160]; uint64_t n; } Agg;
static int agg_cmp(const void *a, const void *b) {
    uint64_t x = ((const Agg *)a)->n, y = ((const Agg *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}
static Agg *agg_add(Agg *v, int *n, int cap, const char *name, uint64_t k) {
    for (int i = 0; i < *n; i++) if (!strcmp(v[i].name, name)) { v[i].n += k; return v; }
    if (*n < cap) { snprintf(v[*n].name, sizeof v[*n].name, "%s", name); v[(*n)++].n = k; }
    return v;
}


// The exe's own symbols, enumerated once from its PDB and sorted (dbghelp's SymFromAddr fails on lld's PDBs with
// error 126 although SymFromName / SymEnumSymbols work).
typedef struct { uint64_t addr; uint32_t size; char *name; } ExeSym;
static ExeSym *g_es;
static size_t g_nes, g_capes;
static BOOL CALLBACK enum_cb(PSYMBOL_INFO s, ULONG size, PVOID ctx) {
    (void)size; (void)ctx;
    if (g_nes == g_capes) { g_capes = g_capes ? g_capes * 2 : 65536; g_es = realloc(g_es, g_capes * sizeof *g_es); }
    g_es[g_nes].addr = s->Address, g_es[g_nes].size = s->Size, g_es[g_nes].name = _strdup(s->Name);
    g_nes++;
    return TRUE;
}
static int es_cmp(const void *a, const void *b) {
    uint64_t x = ((const ExeSym *)a)->addr, y = ((const ExeSym *)b)->addr;
    return x < y ? -1 : x > y;
}
static const char *exe_symbol(uint64_t pc, bool *inside) {
    static int done;
    if (!done) {
        done = 1;
        uint64_t o;
        plat_symbol((uint64_t)(uintptr_t)&exe_symbol, &o);  // (initializes dbghelp with the exe's directory)
        SymEnumSymbols(GetCurrentProcess(), (ULONG64)(uintptr_t)GetModuleHandleA(NULL), "*", enum_cb, NULL);
        qsort(g_es, g_nes, sizeof *g_es, es_cmp);
    }
    size_t lo = 0, hi = g_nes;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (g_es[m].addr <= pc) lo = m + 1; else hi = m; }
    if (!lo) return NULL;
    const ExeSym *e = &g_es[lo - 1];
    *inside = !e->size || pc - e->addr < e->size;
    return e->name;
}

// pc -> "function" and "module" ([guest code] / [runtime] for the exe); system DLLs by their exports.
static bool resolve(uint64_t pc, char *name, char *mname) {
    HMODULE hm = NULL;
    char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)pc, &hm)) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(hm, path, sizeof path)) { const char *b = strrchr(path, '\\'); snprintf(mod, sizeof mod, "%s", b ? b + 1 : path); }
    }
    bool exe = hm && hm == GetModuleHandleA(NULL);
    if (exe) {
        bool inside = false;
        const char *sym = exe_symbol(pc, &inside);
        snprintf(name, 160, "%s%s", sym ? sym : "?exe", sym && !inside ? "+?" : "");
        snprintf(mname, 160, "%s", sym && !strncmp(sym, "f_", 2) ? "[guest code]" : "[runtime]");
        return true;
    }
    union { SYMBOL_INFO s; char buf[sizeof(SYMBOL_INFO) + 256]; } u = {0};
    u.s.SizeOfStruct = sizeof(SYMBOL_INFO);
    u.s.MaxNameLen = 255;
    DWORD64 disp = 0;
    { uint64_t o; plat_symbol((uint64_t)(uintptr_t)&resolve, &o); }
    bool ok = SymFromAddr(GetCurrentProcess(), pc, &disp, &u.s);
    snprintf(name, 160, "%s!%s", mod, ok ? u.s.Name : "?");
    snprintf(mname, 160, "%s", mod);
    return false;
}

static void report(void) {
    const char *out = getenv("XCDE_SAMPLE_OUT");
    if (!out) out = "build/samples.txt";
    FILE *f = fopen(out, "w");
    if (!f) return;
    uint64_t total = 0;
    for (int i = 0; i < nth; i++) total += th[i].cyc_total;
    enum { CAP = 20000 };
    static Agg fn[CAP], mod[512], thr[MAXTH];
    static Agg thfn[MAXTH][64];
    static int nthfn[MAXTH];
    int nfn = 0, nmod = 0, nthr = 0;
    for (uint64_t s = 0; s < NPC; s++) {
        if (!pcs[s].n) continue;
        char name[160], mname[160];
        bool exe = resolve(pcs[s].pc, name, mname);
        (void)exe;
        agg_add(fn, &nfn, CAP, name, pcs[s].n);
        agg_add(mod, &nmod, 512, mname, pcs[s].n);
        int ti = pcs[s].th;
        agg_add(thfn[ti], &nthfn[ti], 64, name, pcs[s].n);
    }
    for (int i = 0; i < nth; i++) {
        char nm[160];
        snprintf(nm, sizeof nm, "%-40s (tid %lu)", th[i].name, th[i].tid);
        agg_add(thr, &nthr, MAXTH, nm, th[i].cyc_total);
    }
    qsort(fn, nfn, sizeof *fn, agg_cmp);
    qsort(mod, nmod, sizeof *mod, agg_cmp);
    double tsc = g_tsc > 0 ? g_tsc : 1;  // cores = cycles / TSC ticks elapsed
    fprintf(f, "# XCDE sampling profile, frames %ld..%ld, %.1f s, %llu ticks (%.0f Hz), %.2f cores busy on average%s\n",
            g_from, g_to, g_secs, (unsigned long long)n_ticks, n_ticks / (g_secs > 0 ? g_secs : 1), total / tsc,
            n_lost ? " - pc table full" : "");
    fprintf(f, "\n## per thread (share of all CPU cycles; cores = average busy cores)\n");
    int order[MAXTH];
    for (int i = 0; i < nth; i++) order[i] = i;
    for (int i = 0; i < nth; i++) for (int j = i + 1; j < nth; j++) if (th[order[j]].cyc_total > th[order[i]].cyc_total) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int k = 0; k < nth; k++) {
        Th *t = &th[order[k]];
        if (t->cyc_total * 1000 < total) break;  // (under 0.1 %)
        fprintf(f, "%6.2f %%  %5.2f cores  %-40s tid %lu\n", 100.0 * t->cyc_total / (total ? total : 1), t->cyc_total / tsc, t->name, t->tid);
    }
    fprintf(f, "\n## per module\n");
    for (int i = 0; i < nmod; i++) fprintf(f, "%6.2f %%  %s\n", 100.0 * mod[i].n / (total ? total : 1), mod[i].name);
    fprintf(f, "\n## top functions (all threads)\n");
    for (int i = 0; i < nfn && i < 150; i++) fprintf(f, "%6.2f %%  %s\n", 100.0 * fn[i].n / (total ? total : 1), fn[i].name);
    fprintf(f, "\n## top functions per busy thread\n");
    for (int k = 0; k < nth && k < 12; k++) {
        int ti = order[k];
        if (!th[ti].cyc_total) break;
        qsort(thfn[ti], nthfn[ti], sizeof(Agg), agg_cmp);
        fprintf(f, "### %s (tid %lu), %.2f cores\n", th[ti].name, th[ti].tid, th[ti].cyc_total / tsc);
        for (int i = 0; i < nthfn[ti] && i < 25; i++) fprintf(f, "   %6.2f %%  %s\n", 100.0 * thfn[ti][i].n / th[ti].cyc_total, thfn[ti][i].name);
    }
    {   // hottest single instructions (rva in the exe, or module+offset) - to check attributions
        enum { TOP = 40 };
        uint64_t best[TOP] = {0}; int nb = 0;
        for (uint64_t s2 = 0; s2 < NPC; s2++) {
            if (!pcs[s2].n) continue;
            int k = nb < TOP ? nb++ : TOP - 1;
            if (k == TOP - 1 && nb == TOP && pcs[best[k]].n >= pcs[s2].n) continue;
            best[k] = s2;
            while (k > 0 && pcs[best[k - 1]].n < pcs[best[k]].n) { uint64_t t = best[k]; best[k] = best[k - 1]; best[k - 1] = t; k--; }
        }
        fprintf(f, "\n## hottest instructions\n");
        uint64_t base = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
        for (int i = 0; i < nb; i++) {
            char name[160], mname[160];
            uint64_t pc = pcs[best[i]].pc;
            bool exe = resolve(pc, name, mname);
            fprintf(f, "%6.2f %%  %s+%llx  %s  (thread %s)\n", 100.0 * pcs[best[i]].n / (total ? total : 1), exe ? "exe" : "pc",
                    (unsigned long long)(exe ? pc - base : pc), name, th[pcs[best[i]].th].name);
        }
    }
    if (g_callers) {  // [XCDE] time in DLLs by the exe function that called into them, per busy thread
        fprintf(f, "\n## time spent in DLLs, by the calling exe function (per busy thread)\n");
        for (int k = 0; k < nth && k < 8; k++) {
            int ti = order[k];
            if (!th[ti].cyc_total) break;
            static Agg cf[256];
            int ncf = 0;
            for (uint64_t s2 = 0; s2 < (1 << 16); s2++) {
                if (!cpcs[s2].n || cpcs[s2].th != ti) continue;
                char name[160], mname[160];
                resolve(cpcs[s2].pc, name, mname);
                agg_add(cf, &ncf, 256, name, cpcs[s2].n);
            }
            if (!ncf) continue;
            qsort(cf, ncf, sizeof *cf, agg_cmp);
            fprintf(f, "### %s (tid %lu)\n", th[ti].name, th[ti].tid);
            for (int i = 0; i < ncf && i < 20; i++) fprintf(f, "   %6.2f %%  %s\n", 100.0 * cf[i].n / th[ti].cyc_total, cf[i].name);
        }
    }
    fclose(f);
    LOG("[sample] profile of frames %ld..%ld -> %s (%llu ticks, %.2f cores busy)\n", g_from, g_to, out,
        (unsigned long long)n_ticks, total / tsc);
}

// the samples of [lo, hi): per thread, the busiest functions (ms of CPU, from cycles at the TSC rate)
static void hitch_report(int64_t lo, int64_t hi, double tsc_per_ms) {
    static Agg fn[MAXTH][24];
    static int nfn[MAXTH];
    static uint64_t tot[MAXTH];
    memset(nfn, 0, sizeof nfn); memset(tot, 0, sizeof tot);
    uint64_t end = ring_n, start = end > RING ? end - RING : 0;
    for (uint64_t k = start; k < end; k++) {
        uint64_t s = k & (RING - 1);
        if (ring[s].t < lo || ring[s].t >= hi) continue;
        char name[160], mname[160];
        resolve(ring[s].pc, name, mname);
        int ti = ring[s].th;
        agg_add(fn[ti], &nfn[ti], 24, name, ring[s].cy);
        tot[ti] += ring[s].cy;
    }
    int order[MAXTH];
    for (int i = 0; i < nth; i++) order[i] = i;
    for (int i = 0; i < nth; i++) for (int j = i + 1; j < nth; j++) if (tot[order[j]] > tot[order[i]]) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); LOG("[hitch-cpu] slow frame of %.1f ms, the busiest threads in it:\n", (hi - lo) * 1000.0 / fq.QuadPart); }
    for (int k = 0; k < nth && k < 6; k++) {
        int ti = order[k];
        if (tot[ti] < tsc_per_ms) break;  // (under 1 ms)
        qsort(fn[ti], nfn[ti], sizeof(Agg), agg_cmp);
        char line[1024];
        int n = snprintf(line, sizeof line, "[hitch-cpu]   %-24.24s %5.1f ms:", th[ti].name, tot[ti] / tsc_per_ms);
        for (int i = 0; i < nfn[ti] && i < 5 && n < (int)sizeof line - 80; i++)
            n += snprintf(line + n, sizeof line - n, " %s %.1f;", fn[ti][i].name, fn[ti][i].n / tsc_per_ms);
        LOG("%s\n", line);
    }
}

static void *sampler_thread(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);
    const char *hz_e = getenv("XCDE_SAMPLE_HZ");
    int hz = hz_e ? atoi(hz_e) : 1000;
    if (hz < 10) hz = 10;
    DWORD self = GetCurrentThreadId();
    {
        g_callers = getenv("XCDE_SAMPLE_CALLERS") && atoi(getenv("XCDE_SAMPLE_CALLERS")) != 0;
        uint8_t *b = (uint8_t *)GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS *nh = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
        g_exe_lo = (uint64_t)(uintptr_t)b; g_exe_hi = g_exe_lo + nh->OptionalHeader.SizeOfImage;
    }
    while (g_state == 0) Sleep(5);
    LARGE_INTEGER fq, t0, now;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    refresh_threads(self);  // (primes each thread's cycle counter: the first tick doesn't charge its whole past)
    uint64_t tsc0 = __rdtsc();
    double tsc_per_ms = 0;
    if (g_hitch) {  // (TSC ticks per ms, for the cycle counts)
        LARGE_INTEGER a, b; uint64_t c0 = __rdtsc(); QueryPerformanceCounter(&a);
        Sleep(50);
        uint64_t c1 = __rdtsc(); QueryPerformanceCounter(&b);
        tsc_per_ms = (double)(c1 - c0) / ((double)(b.QuadPart - a.QuadPart) * 1000.0 / fq.QuadPart);
    }
    for (uint64_t i = 1; g_state == 1; i++) {
        if (i % 100 == 0) refresh_threads(self);
        sample_once();
        if (g_hitch && hitch_hi) { int64_t lo = hitch_lo, hi = hitch_hi; hitch_hi = 0; hitch_report(lo, hi, tsc_per_ms); }
        int64_t due = t0.QuadPart + (int64_t)(i * (uint64_t)fq.QuadPart / (uint64_t)hz);
        QueryPerformanceCounter(&now);
        if (now.QuadPart < due) {
            DWORD ms = (DWORD)((due - now.QuadPart) * 1000 / fq.QuadPart);
            if (ms > 1) Sleep(ms - 1);
            while (QueryPerformanceCounter(&now), now.QuadPart < due) YieldProcessor();
        }
    }
    QueryPerformanceCounter(&now);
    g_secs = (double)(now.QuadPart - t0.QuadPart) / fq.QuadPart;
    g_tsc = (double)(__rdtsc() - tsc0);  // thread cycle counts are in TSC units: cores = cycles / TSC ticks elapsed
    report();
    timeEndPeriod(1);
    return NULL;
}

// device.c: the frame that just ended took `ms` (a hitch): report it (XCDE_HITCH_SAMPLE=1, at most 60 a run)
void diag_hitch_sample(double ms) {
    static int n;
    if (!g_hitch || g_state != 1 || hitch_hi || n >= 60) return;
    n++;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    int64_t now = qpc_now();
    hitch_lo = now - (int64_t)(ms * fq.QuadPart / 1000.0);
    hitch_hi = now;
}

void diag_sampler_frame(uint64_t frame) {
    static int init;
    if (!init) {
        init = 1;
        if (getenv("XCDE_HITCH_SAMPLE") && atoi(getenv("XCDE_HITCH_SAMPLE")) != 0) {  // (whole session, from frame 300)
            g_hitch = 1; g_from = 300; g_to = 0x7fffffff;
            pthread_t t;
            if (pthread_create(&t, NULL, sampler_thread, NULL) == 0) pthread_detach(t);
            init = 2;
            return;
        }
        const char *e = getenv("XCDE_SAMPLE");
        long a = 0, b = 0;
        if (!e || sscanf(e, "%ld:%ld", &a, &b) != 2 || b <= a) return;
        g_from = a, g_to = b;
        pthread_t t;
        if (pthread_create(&t, NULL, sampler_thread, NULL) == 0) pthread_detach(t);
        else return;
        init = 2;
    }
    if (init != 2) return;
    if (g_state == 0 && (long)frame >= g_from) { g_state = 1; LOG("[sample] sampling from frame %llu\n", (unsigned long long)frame); }
    else if (g_state == 1 && (long)frame >= g_to) g_state = 2;
}
#else
void diag_sampler_frame(uint64_t frame) { (void)frame; }
void diag_hitch_sample(double ms) { (void)ms; }
#endif
