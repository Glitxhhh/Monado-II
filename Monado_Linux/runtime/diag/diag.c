// Diagnostics (M3, T029/T033): deterministic mode, per-frame hash, input record/replay, strict mode, HLE coverage.
#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform.h"
#include "../kernel.h"
#include "../rt.h"

#define FRAME_PERIOD_NS 33333333ull  // the game runs at 30 fps

static int env_flag(const char *name) {
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0");
}

bool diag_deterministic(void) {
    static int on = -1;
    if (on < 0) on = env_flag("XCDE_DETERMINISTIC");
    return on;
}

bool diag_strict(void) {
    static int on = -1;
    if (on < 0) on = env_flag("XCDE_STRICT");
    return on;
}

// ------------------------------------------------------------------ virtual time
// XCDE_HASH_MEM="<hex addr>:<hex size>[,...]": fnv1a of guest memory ranges, logged at each presented frame and at
// each queue submission ("[hashmem] f<frame> <tag> <addr> <hash>"; determinism checks, T119).
// XCDE_HASH_MEM_FROM=<frame>: start there (the memory is not mapped before).
static volatile uint64_t g_frame;
void diag_hash_mem(const char *tag) {
    static unsigned long long r_addr[4], r_size[4];
    static int r_n = -1;
    static long from;
    if (r_n < 0) {
        r_n = 0;
        for (const char *p = getenv("XCDE_HASH_MEM"); p && *p && r_n < 4;) {
            if (sscanf(p, "%llx:%llx", &r_addr[r_n], &r_size[r_n]) == 2) r_n++;
            p = strchr(p, ',');
            if (p) p++;
        }
        from = getenv("XCDE_HASH_MEM_FROM") ? atol(getenv("XCDE_HASH_MEM_FROM")) : 0;
    }
    for (int i = 0; i < r_n && (long)g_frame >= from; i++) {
        uint64_t hv = 0xcbf29ce484222325ull;
        const uint8_t *m = (const uint8_t *)(uintptr_t)r_addr[i];
        for (unsigned long long k = 0; k < r_size[i]; k++) hv = (hv ^ m[k]) * 0x100000001b3ull;
        LOG("[hashmem] f%llu %s %llx %016llx\n", (unsigned long long)g_frame, tag, r_addr[i], (unsigned long long)hv);
    }
}

static volatile uint64_t g_last_ns;

void diag_coverage_flush(void);
void diag_usage_flush(void);
void (*diag_watch_hook)(uint64_t lo, uint64_t hi);  // dispatch.c: arms its page watchpoint (unset in the native tests)
uint64_t g_last_shot_frame_pub(void);
void diag_frame_tick(uint64_t frame) {
    g_frame = frame;
    // XCDE_DUMP_MEM="<frame>:<hex guest addr>:<hex size>:<file>": guest memory to a file at that presented frame (debug)
    static int dm_init;
    static unsigned long long dm_frame, dm_addr, dm_size;
    static char dm_path[200];
    if (!dm_init) {
        dm_init = 1;
        const char *e = getenv("XCDE_DUMP_MEM");
        if (e) sscanf(e, "%llu:%llx:%llx:%199s", &dm_frame, &dm_addr, &dm_size, dm_path);
    }
    if (dm_frame && frame == dm_frame) {
        FILE *f = fopen(dm_path, "wb");
        if (f) { fwrite((const void *)(uintptr_t)dm_addr, 1, dm_size, f); fclose(f); LOG("[diag] dumped %llx+%llx to %s\n", dm_addr, dm_size, dm_path); }
    }
    {   // [XCDE] T240 XCDE_DUMP_MEM_EACH="<from>:<to>:<hex addr>:<hex size>:<path base>": the range at every presented
        // frame in between, to <path base>_<frame>.bin (a structure's per-frame changes: indices, counters)
        static int de_init;
        static unsigned long long de_from, de_to, de_addr, de_size;
        static char de_path[200];
        if (!de_init) {
            de_init = 1;
            const char *e = getenv("XCDE_DUMP_MEM_EACH");
            if (e) sscanf(e, "%llu:%llu:%llx:%llx:%199s", &de_from, &de_to, &de_addr, &de_size, de_path);
        }
        if (de_size && frame >= de_from && frame <= de_to) {
            char p[256];
            snprintf(p, sizeof p, "%s_%llu.bin", de_path, (unsigned long long)frame);
            FILE *f = fopen(p, "wb");
            if (f) { fwrite((const void *)(uintptr_t)de_addr, 1, de_size, f); fclose(f); }
        }
    }
    {   // [XCDE] XCDE_FIND="<frame>:<hex bytes>[,<hex bytes>...]": at that presented frame, the guest addresses (main
        // image and heap, committed pages only) where each byte pattern occurs, up to 64 each (game-structure research:
        // e.g. a chunk of the loaded save file finds the game's in-memory copy of it).
        static int f_init;
        static unsigned long long f_frame;
        static char f_pat[1024];
        if (!f_init) { f_init = 1; const char *e = getenv("XCDE_FIND"); if (e) sscanf(e, "%llu:%1023s", &f_frame, f_pat); }
        if (f_frame && frame == f_frame) {
            extern int plat_guest_find(const uint8_t *pat, size_t n, uint64_t *out, int max);
            for (char *p = f_pat; *p;) {
                uint8_t pat[256];
                size_t n = 0;
                while (*p && *p != ',' && p[1] && n < sizeof pat) { unsigned v; sscanf(p, "%2x", &v); pat[n++] = (uint8_t)v; p += 2; }
                if (*p == ',') p++;
                uint64_t hit[64];
                int nh = n ? plat_guest_find(pat, n, hit, 64) : 0;
                LOG("[find] %zu-byte pattern: %d hit(s)%s", n, nh, nh ? ":" : "");
                for (int i = 0; i < nh; i++) LOG(" %llx", (unsigned long long)hit[i]);
                LOG("%c", 10);
            }
        }
    }
    {   // [XCDE] XCDE_DUMP_HEAP="<frame>:<file>": every committed page of the guest heap at that frame, as records
        // (u64 address, u64 size, bytes) - two runs that differ in one thing are diffed offline (game-structure research)
        static int h_init;
        static unsigned long long h_frame;
        static char h_path[260];
        if (!h_init) { h_init = 1; const char *e = getenv("XCDE_DUMP_HEAP"); if (e) sscanf(e, "%llu:%259s", &h_frame, h_path); }
        if (h_frame && frame == h_frame) {
            extern uint64_t plat_guest_dump(const char *path);
            LOG("[diag] heap dump: %.1f MB to %s%c", plat_guest_dump(h_path) / 1048576.0, h_path, 10);
        }
    }
    {   // [XCDE] XCDE_POKE="<frame>:<hex guest addr>:<hex bytes>[;...]": write those bytes at that presented frame
        // (game-structure research: change a field and watch what the game does with it)
        static int p_init, p_n;
        static unsigned long long p_frame[8], p_addr[8];
        static char p_hex[8][130];
        if (!p_init) {
            p_init = 1;
            const char *e = getenv("XCDE_POKE");
            while (e && *e && p_n < 8) {
                if (sscanf(e, "%llu:%llx:%128[0-9a-fA-F]", &p_frame[p_n], &p_addr[p_n], p_hex[p_n]) == 3) p_n++;
                e = strchr(e, ';');
                if (e) e++;
            }
        }
        for (int i = 0; i < p_n; i++)
            if (frame == p_frame[i]) {
                uint8_t *m = (uint8_t *)(uintptr_t)p_addr[i];
                for (size_t k = 0; p_hex[i][2 * k] && p_hex[i][2 * k + 1]; k++) { unsigned v; sscanf(p_hex[i] + 2 * k, "%2x", &v); m[k] = (uint8_t)v; }
                LOG("[poke] f%llu %llx <- %s%c", (unsigned long long)frame, p_addr[i], p_hex[i], 10);
            }
    }
    {   // XCDE_WATCH="<frame>:<hex lo>:<hex hi>": from that frame on, report each guest function touching [lo, hi) (dispatch.c)
        static unsigned long long w_frame, w_lo, w_hi;
        static int w_init;
        if (!w_init) { w_init = 1; const char *e = getenv("XCDE_WATCH"); if (e) sscanf(e, "%llu:%llx:%llx", &w_frame, &w_lo, &w_hi); }
        if (w_frame && frame == w_frame && diag_watch_hook) {
            diag_watch_hook(w_lo, w_hi);
            LOG("[diag] watching %llx..%llx from frame %llu\n", w_lo, w_hi, w_frame);
        }
    }
    {   // XCDE_EXIT_AFTER_SHOTS=1 (scripts/drive_env.sh): end the run once the last XCDE_SHOTS capture is written
        static int ex = -1;
        if (ex < 0) ex = env_flag("XCDE_EXIT_AFTER_SHOTS");
        extern const char *diag_shot_path(uint64_t);
        if (ex) diag_shot_path(0);  // (parses XCDE_SHOTS on first use)
        if (ex && g_last_shot_frame_pub() && frame > g_last_shot_frame_pub()) {
            LOG("[diag] last capture (frame %llu) written: exiting\n", (unsigned long long)g_last_shot_frame_pub());
            fflush(stderr);
            plat_terminate();
        }
    }
    diag_hash_mem("frame");
    { extern void diag_sampler_frame(uint64_t); diag_sampler_frame(frame); }  // XCDE_SAMPLE=<from>:<to> (sampler.c)
    {   // XCDE_DET_DIGEST=1: the deterministic scheduler's SVC digest at each presented frame (where two runs part)
        static int dg = -1;
        if (dg < 0) dg = getenv("XCDE_DET_DIGEST") != NULL;
        extern uint64_t kernel_det_digest(void);
        if (dg) LOG("[digest] f%llu %016llx vnow %llu\n", (unsigned long long)frame, (unsigned long long)kernel_det_digest(), (unsigned long long)kernel_vnow(0));
    }
    if (frame % 300 == 0) { diag_coverage_flush(); diag_usage_flush(); }
    // test hook for the watchdog (tests/unit/test_watchdog.py): XCDE_TEST_HANG_AT=<frame> stops the presenting thread there
    static long hang_at = -1;
    if (hang_at < 0) hang_at = getenv("XCDE_TEST_HANG_AT") ? atol(getenv("XCDE_TEST_HANG_AT")) : 0;
    static long crash_at = -1;  // XCDE_TEST_CRASH_AT=<frame>: access violation (crash report / minidump test)
    if (crash_at < 0) crash_at = getenv("XCDE_TEST_CRASH_AT") ? atol(getenv("XCDE_TEST_CRASH_AT")) : 0;
    if (crash_at && (long)frame == crash_at) *(volatile int *)0x10 = 1;
    if (hang_at && (long)frame == hang_at) { LOG("[diag] test hang at frame %ld\n", hang_at); for (;;) plat_sleep_until_ns(plat_now_ns() + 1000000000ull); }
}
uint64_t diag_frame(void) { return g_frame; }

uint64_t diag_now_ns(void) {
    if (kernel_det()) return kernel_vnow(1);  // the scheduler owns virtual time
    // Time only moves with presented frames, plus 1 us per query so that two reads in one frame still differ.
    uint64_t base = g_frame * FRAME_PERIOD_NS + 1000000000ull;  // (start at 1 s: some code treats 0 as "never")
    for (;;) {
        uint64_t last = __atomic_load_n(&g_last_ns, __ATOMIC_RELAXED);
        uint64_t t = base > last ? base : last + 1000;
        if (__atomic_compare_exchange_n(&g_last_ns, &last, t, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return t;
    }
}

uint64_t diag_random64(void) {
    if (!diag_deterministic()) return ((uint64_t)arc4random() << 32) | arc4random();
    static uint64_t s = 0x9E3779B97F4A7C15ull;  // splitmix64, fixed seed
    uint64_t z = __atomic_add_fetch(&s, 0x9E3779B97F4A7C15ull, __ATOMIC_RELAXED);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// ------------------------------------------------------------------ per-frame hash
static FILE *g_hash_file;
static int g_hash_init;

static void hash_open(void) {
    if (g_hash_init) return;
    g_hash_init = 1;
    const char *p = getenv("XCDE_FRAME_HASH");
    // (only on request: it reads the window image back and waits for the GPU every frame - deterministic runs used to
    // write build/frame_hashes.txt implicitly; image hashes aren't reproducible anyway (T124), compare XCDE_DET_DIGEST)
    if (p && *p) {
        g_hash_file = fopen(p, "w");
        if (g_hash_file) { fputs("# xcde frame hashes v1: <frame> <fnv1a-64 of the RGBA window image>\n", g_hash_file); LOG("[diag] frame hashes -> %s\n", p); }
    }
}

bool diag_hash_wanted(void) {
    hash_open();
    return g_hash_file != NULL;
}

void diag_frame_hash(uint64_t frame, const uint8_t *rgba, uint32_t w, uint32_t h) {
    if (!g_hash_file) return;
    uint64_t hv = 0xcbf29ce484222325ull;
    const uint64_t n = (uint64_t)w * h;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t px;
        memcpy(&px, rgba + i * 4, 4);
        hv = (hv ^ px) * 0x100000001b3ull;
    }
    fprintf(g_hash_file, "%llu %016llx\n", (unsigned long long)frame, (unsigned long long)hv);
    if ((frame & 63) == 0) fflush(g_hash_file);
}

// ------------------------------------------------------------------ input record / replay
typedef struct { uint64_t frame, buttons; int32_t lx, ly, rx, ry; } InEv;
static InEv *g_replay;
static size_t g_replay_n;
static FILE *g_rec;
static int g_in_init;

static void input_open(void) {
    if (g_in_init) return;
    g_in_init = 1;
    const char *rp = getenv("XCDE_INPUT_REPLAY"), *wp = getenv("XCDE_INPUT_RECORD");
    if (rp && *rp) {
        FILE *f = fopen(rp, "r");
        if (!f) { LOG("[diag] cannot open input replay %s\n", rp); }
        else {
            char line[256];
            size_t cap = 0;
            while (fgets(line, sizeof line, f)) {
                if (line[0] == '#' || line[0] == '\n') continue;
                InEv e;
                unsigned long long fr, b;
                if (sscanf(line, "%llu %llx %d %d %d %d", &fr, &b, &e.lx, &e.ly, &e.rx, &e.ry) != 6) continue;
                e.frame = fr, e.buttons = b;
                if (g_replay_n == cap) g_replay = realloc(g_replay, (cap = cap ? cap * 2 : 256) * sizeof *g_replay);
                g_replay[g_replay_n++] = e;
            }
            fclose(f);
            LOG("[diag] input replay: %zu events from %s\n", g_replay_n, rp);
        }
    }
    if (wp && *wp) {
        g_rec = fopen(wp, "w");
        if (g_rec) fputs("# xcde input v1: <frame> <buttons hex> <lx> <ly> <rx> <ry>\n", g_rec);
    }
}

void diag_input(uint64_t frame, uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    input_open();
    if (g_replay_n) {  // the last event at or before this frame (events are sorted by frame)
        size_t lo = 0, hi = g_replay_n;
        while (lo < hi) { size_t m = (lo + hi) / 2; if (g_replay[m].frame <= frame) lo = m + 1; else hi = m; }
        InEv e = lo ? g_replay[lo - 1] : (InEv){0};
        *b = e.buttons, *lx = e.lx, *ly = e.ly, *rx = e.rx, *ry = e.ry;
    }
    if (g_rec) {
        static uint64_t lb = ~0ull;
        static int32_t l[4];
        int32_t cur[4] = {*lx, *ly, *rx, *ry};
        if (*b != lb || memcmp(l, cur, sizeof l)) {
            fprintf(g_rec, "%llu %llx %d %d %d %d\n", (unsigned long long)frame, (unsigned long long)*b, cur[0], cur[1], cur[2], cur[3]);
            fflush(g_rec);
            lb = *b;
            memcpy(l, cur, sizeof l);
        }
    }
}

// ------------------------------------------------------------------ strict mode + coverage
void diag_unimplemented(const char *kind, const char *fmt, ...) {
    char what[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    diag_hle_hit(kind, what);
    LOG("[unimplemented] %s %s\n", kind, what);
    if (diag_strict()) {
        LOG("STRICT MODE: stopping at %s %s\n", kind, what);
        rt_fatal_trace();
    }
}

typedef struct { char kind[16], name[96]; uint64_t n; } Hit;
static Hit g_hits[2048];
static int g_nhits;
static pthread_mutex_t g_hit_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_cov_init;

static void cov_dump(void) {
    const char *p = getenv("XCDE_HLE_COVERAGE");
    FILE *f = p ? fopen(p, "w") : NULL;
    if (!f) return;
    for (int i = 0; i < g_nhits; i++) fprintf(f, "%s\t%s\t%llu\n", g_hits[i].kind, g_hits[i].name, (unsigned long long)g_hits[i].n);
    fclose(f);
}

void diag_hle_hit(const char *kind, const char *name) {
    if (!g_cov_init) {
        g_cov_init = 1;
        if (getenv("XCDE_HLE_COVERAGE")) atexit(cov_dump);
    }
    if (!getenv("XCDE_HLE_COVERAGE")) return;
    pthread_mutex_lock(&g_hit_lock);
    int i = 0;
    for (; i < g_nhits; i++) if (!strcmp(g_hits[i].kind, kind) && !strcmp(g_hits[i].name, name)) break;
    if (i == g_nhits && g_nhits < 2048) {
        snprintf(g_hits[i].kind, sizeof g_hits[i].kind, "%s", kind);
        snprintf(g_hits[i].name, sizeof g_hits[i].name, "%s", name);
        g_nhits++;
        g_hits[i].n = 0;
        cov_dump();  // a new entry: write as we go (the process is usually stopped by a timeout, atexit would not run)
    }
    if (i < g_nhits) g_hits[i].n++;
    pthread_mutex_unlock(&g_hit_lock);
}

void diag_coverage_flush(void) {
    if (!getenv("XCDE_HLE_COVERAGE")) return;
    pthread_mutex_lock(&g_hit_lock);
    cov_dump();
    pthread_mutex_unlock(&g_hit_lock);
}

// ------------------------------------------------------------------ usage census (T034)
static Hit g_use[4096];
static int g_nuse;
static pthread_mutex_t g_use_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_svc_count[256];
static int g_use_init;
static const char *g_use_path;

bool diag_usage_on(void) {
    if (!g_use_init) { g_use_path = getenv("XCDE_USAGE"); g_use_init = 1; }
    return g_use_path != NULL;
}

static void use_dump(void) {
    FILE *f = fopen(g_use_path, "w");
    if (!f) return;
    for (unsigned i = 0; i < 256; i++) if (g_svc_count[i]) fprintf(f, "svc\t0x%02x\t%llu\n", i, (unsigned long long)g_svc_count[i]);
    for (int i = 0; i < g_nuse; i++) fprintf(f, "%s\t%s\t%llu\n", g_use[i].kind, g_use[i].name, (unsigned long long)g_use[i].n);
    fclose(f);
}

void diag_use_svc(unsigned imm) {
    if (imm < 256 && __atomic_add_fetch(&g_svc_count[imm], 1, __ATOMIC_RELAXED) == 1) {
        pthread_mutex_lock(&g_use_lock);
        use_dump();  // a new SVC number: write as we go (timeouts stop the process before any atexit)
        pthread_mutex_unlock(&g_use_lock);
    }
}

void diag_use(const char *kind, const char *fmt, ...) {
    if (!diag_usage_on()) return;
    char name[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(name, sizeof name, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_use_lock);
    int i = 0;
    for (; i < g_nuse; i++) if (!strcmp(g_use[i].kind, kind) && !strcmp(g_use[i].name, name)) break;
    if (i == g_nuse && g_nuse < 4096) {
        snprintf(g_use[i].kind, sizeof g_use[i].kind, "%s", kind);
        snprintf(g_use[i].name, sizeof g_use[i].name, "%s", name);
        g_use[i].n = 0;
        g_nuse++;
        g_use[i].n++;
        use_dump();
    } else if (i < g_nuse) {
        g_use[i].n++;
    }
    pthread_mutex_unlock(&g_use_lock);
}

void diag_usage_flush(void) {
    if (!diag_usage_on()) return;
    pthread_mutex_lock(&g_use_lock);
    use_dump();
    pthread_mutex_unlock(&g_use_lock);
}

static uint64_t g_last_shot_frame;  // (the last XCDE_SHOTS frame: XCDE_EXIT_AFTER_SHOTS)
uint64_t g_last_shot_frame_pub(void) { return g_last_shot_frame; }
const char *diag_shot_path(uint64_t frame) {
    static char paths[32][160];
    static uint64_t frames[32];
    static int n = -1;
    if (n < 0) {
        n = 0;
        const char *e = getenv("XCDE_SHOTS");
        char buf[2048];
        snprintf(buf, sizeof buf, "%s", e ? e : "");
        for (char *tok = strtok(buf, ","); tok && n < 32; tok = strtok(NULL, ",")) {
            char *c = strchr(tok, ':');
            if (!c) continue;
            *c = 0;
            frames[n] = strtoull(tok, NULL, 10);
            snprintf(paths[n], sizeof paths[n], "%s", c + 1);
            n++;
        }
    }
    for (int i = 0; i < n; i++) if (frames[i] > g_last_shot_frame) g_last_shot_frame = frames[i];
    for (int i = 0; i < n; i++) if (frames[i] == frame) return paths[i];
    // XCDE_SHOT_EVERY=<frames>[:<dir>]: a capture of every Nth presented frame (play sessions; at most 400 files)
    static long every = -1;
    static char dir[200], path[260];
    if (every < 0) {
        every = 0;
        const char *e = getenv("XCDE_SHOT_EVERY");
        if (e) {
            every = atol(e);
            const char *c = strchr(e, ':');
            snprintf(dir, sizeof dir, "%s", c ? c + 1 : "build/play");
        }
    }
    static long from = -1;  // XCDE_SHOT_FROM=<frame>: the 400 captures start there
    if (from < 0) from = getenv("XCDE_SHOT_FROM") ? atol(getenv("XCDE_SHOT_FROM")) : 0;
    if (every > 0 && frame >= (uint64_t)from && (frame - from) % (uint64_t)every == 0 && (frame - from) / (uint64_t)every <= 400) {
        snprintf(path, sizeof path, "%s/shot_%06llu.png", dir, (unsigned long long)frame);
        return path;
    }
    return NULL;
}

// [XCDE] T173: XCDE_FLICKER_CHECK=<dir> - one-frame dark flicker detector (user report: near characters, enemies and
// grass drawn black for a single frame, ~1 in 385 frames in real time). Every presented window image is reduced to
// 320x180; when a frame has clearly more near-black pixels than both its neighbours, the three frames are written to
// <dir>/flk_<frame>_{a,b,c}.png and a "[flicker]" line is logged. Runs at game speed (no per-frame PNG).
#define FL_W 320
#define FL_H 180
static uint8_t fl_img[3][FL_W * FL_H * 4];
static int fl_dark[3], fl_n;
static uint64_t fl_frame[3];
bool diag_flicker_wanted(void) {
    static int on = -1;
    if (on < 0) on = getenv("XCDE_FLICKER_CHECK") != NULL;
    return on;
}
void diag_flicker_check(uint64_t frame, const uint8_t *rgba, uint32_t w, uint32_t h,
                        void (*save_png)(const char *, const uint8_t *, uint32_t, uint32_t)) {
    int k = fl_n % 3, dark = 0;
    uint8_t *o = fl_img[k];
    for (int y = 0; y < FL_H; y++)
        for (int x = 0; x < FL_W; x++) {
            const uint8_t *p = rgba + ((uint64_t)(y * h / FL_H) * w + (uint64_t)x * w / FL_W) * 4;
            uint8_t *q = o + (y * FL_W + x) * 4;
            q[0] = p[0], q[1] = p[1], q[2] = p[2], q[3] = 255;
            dark += p[0] + p[1] + p[2] < 45;
        }
    fl_dark[k] = dark, fl_frame[k] = frame;
    fl_n++;
    if (fl_n < 3) return;
    int a = (fl_n - 3) % 3, b = (fl_n - 2) % 3, c = (fl_n - 1) % 3;  // oldest, middle, newest
    int nb = fl_dark[a] > fl_dark[c] ? fl_dark[a] : fl_dark[c];
    if (fl_dark[b] > nb + 150 && fl_dark[b] * 2 > nb * 3) {
        static int saved;
        const char *dir = getenv("XCDE_FLICKER_CHECK");
        LOG("[flicker] frame %llu: %d near-black px vs %d / %d around (320x180)\n", (unsigned long long)fl_frame[b], fl_dark[b], fl_dark[a], fl_dark[c]);
        if (saved++ < 60 && dir && *dir) {
            char path[400];
            const char *tag = "abc";
            int idx[3] = {a, b, c};
            for (int i = 0; i < 3; i++) {
                snprintf(path, sizeof path, "%s/flk_%06llu_%c.png", dir, (unsigned long long)fl_frame[b], tag[i]);
                save_png(path, fl_img[idx[i]], FL_W, FL_H);
            }
        }
    }
}
