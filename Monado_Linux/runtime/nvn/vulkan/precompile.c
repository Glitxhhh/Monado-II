// [XCDE] T187: shader precompilation from the game's own shader containers (RomFS), before the first play.
//
// The checked Linux launcher button runs the selected ELF's --precompile entry with a private state root.
// Linux Play continues normally without automatic precompilation, retry or permanent shader blacklists.
// The legacy Windows `xcde.exe --precompile <dump dir>` path also runs before its first JUGAR launch. Both walk
// <dump dir>/romfs, opens every file that holds shaders (and every xbc1 zlib block inside it), finds the game's shader
// programs and builds each one the way its first draw would (exec.c vk_precompile_program): translation, SPIR-V
// (cache/spirv) and the vertex / fragment pipeline libraries, compiled by the driver into its own shader cache. The game then finds a program's pieces compiled the first
// time it draws with it: a fast link instead of a translation, a GLSL compile and two native compiles on the spot.
// Nothing is written that the game would not write itself for the same program: a wrong guess of a translation key
// only costs work nobody uses, never a different image.
//
// Where the shaders are: shader containers ("HCPS" = SPCH: monolib/shader/*.wishp, inside models .wimdo, model streams
// .wismt, maps .wismda and effects .wiefb - mostly zlib-compressed in xbc1 blocks). Header u32s [8]/[9] = code section
// offset / size, [10]/[11] = control section offset / size (from the magic). Each shader has an NVN control section
// (magic 0x98761234; +0x6FC constants size, +0x700 constants offset, +0x704 code blob size) and a code blob (0x30-byte
// NVN header with magic 0x12345678, SPH, code, constants) packed in the same order, 256-byte aligned (a blob without
// the header is a compute shader: skipped). A program is a vertex shader and the fragment shader that follows it.
// Content hashes as the runtime's (objects.c set_shaders, T125). Measured (2026-10-20): 8,124 files, 61,908 control
// sections, 11,649 distinct shaders; 593 of the 595 the game registered in a title + field + battle run are among them.
//
// Translation keys (ShaderKey) depend on draw state the containers don't have. Guesses from the 966 variants of a cold
// title + field run (cache/variants.bin): vertex keys always depth_minus_one without flips; vertex input location 1
// unsigned-integer in 15 % (one family: such vertex shaders are built both ways), other locations float; colour targets
// float in 97 %; textures read as integers in 9 % of the fragment variants (not guessed). The game's log line
// "[shader] spirv cache: N hits, M compiled" says how many it still had to compile.
//
// Linux publishes atomic, session-keyed cache/precompile_work.txt; its supervisor owns final UI status and cancellation.
// Completion covers scheduled supported jobs with checked cache commits; unsupported/new variants still compile in play.
// Windows retains cache/precompile_progress.txt and its legacy precompile_done.txt marker/crash quarantine below.
// Threads run at background priority. XCDE_PRECOMPILE_THREADS=<n> defaults to CPUs-1 with a memory cap.
// XCDE_PRECOMPILE_LIBS=0 skips speculative graphics libraries but still initializes the hidden Vulkan device.
// XCDE_PRECOMPILE_LIST=<file> is a legacy Windows diagnostic list; Linux uses the bounded work protocol.
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>

#include "../../rt.h"
#include "shaders.h"
#ifdef __linux__
#include "precompile_status.h"
extern bool vk_pipeline_cache_save_checked(void);
#endif

extern void plat_thread_background(void);
extern int plat_cpu_count(void);
extern int vk_precompile_program(const NvnProgram *np, const ShaderKey *vkey, const ShaderKey *fkey, uint32_t *vs_attr);
extern void ui_window_init(void);
static uint64_t now_ns(void) {  // (plat_init, which picks the game's clock, is not run in this mode)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ------------------------------------------------------------------ found shaders and programs (deduplicated)
typedef struct {
    uint64_t hash;
    int stage;
    uint32_t coff, csz;  // constants offset / size (control section)
    uint8_t *code;       // copy of the blob (zero-padded up to the translation limit)
    uint32_t int_in;     // [XCDE] T211 vertex: input locations named as integers in the container (n*: nWgtIdx);
                         // fragment: texture units named as stencil (gTStencil / gTCurStencil / gTPrevStencil)
    bool named;          // the container named its inputs (int_in is known, also when 0)
} Found;
typedef struct { uint32_t vs, fs; } Pair;  // indices into found (fs = UINT32_MAX: a vertex shader alone)
static Found *found;
static size_t nfound, capfound;
static Pair *progs;
static size_t nprogs, capprogs;
static pthread_mutex_t flock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fcond = PTHREAD_COND_INITIALIZER;
static size_t next_job;
static bool scan_done, use_libs;
static _Atomic uint64_t n_named_int;  // [XCDE] T211 (programs built with integer inputs named by the container)
static _Atomic uint64_t n_built, n_failed, n_extra, files_done, n_controls, next_file;
static uint64_t files_total;
static _Thread_local const char *scan_input_path;
static _Thread_local size_t scan_block_offset=SIZE_MAX, scan_container_offset=SIZE_MAX;

// [XCDE] LINUX-018: counters describe attempted work, never infer success from process survival.
#ifdef __linux__
static _Atomic uint64_t n_attempted, n_succeeded, n_call_failed, n_errors, n_unsupported, n_primary_done;
static _Atomic uint64_t n_seed_done, n_var_done;
static _Atomic bool stop_work;
static volatile sig_atomic_t cancel_signal;
static bool cancelled_by_user;
static char session_id[33], identity_id[65];
static void poll_cancel(void);
static bool stopped(void) { return atomic_load_explicit(&stop_work, memory_order_relaxed); }
static void precompile_error(const char *fmt, ...) {
    atomic_fetch_add(&n_errors, 1);
    va_list args; va_start(args, fmt); flockfile(stderr);
    fputs("[precompile] ", stderr);
    if(scan_input_path) {
        fprintf(stderr,"input %s",scan_input_path);
        if(scan_block_offset!=SIZE_MAX) fprintf(stderr," xbc1@%zu",scan_block_offset);
        if(scan_container_offset!=SIZE_MAX) fprintf(stderr," SPCH@%zu",scan_container_offset);
        fputs(": ",stderr);
    }
    vfprintf(stderr, fmt, args); fputc('\n', stderr);
    funlockfile(stderr); va_end(args);
}
static void allocation_failed(void) { precompile_error("allocation failed"); atomic_store(&stop_work, true); }
static void unsupported(void) { atomic_fetch_add(&n_unsupported, 1); }
#else
static bool stopped(void) { return false; }
static void precompile_error(const char *fmt, ...) { (void)fmt; }
static void allocation_failed(void) {}
static void unsupported(void) {}
#endif
static bool in_range(size_t off, size_t bytes, size_t n) { return off <= n && bytes <= n - off; }
static int compile_one(const NvnProgram *np, const ShaderKey *vk, const ShaderKey *fk, uint32_t *attr) {
#ifdef __linux__
    if (stopped()) return 0;
    atomic_fetch_add(&n_attempted, 1);
#endif
    int ok = vk_precompile_program(np, vk, fk, attr);
#ifdef __linux__
    atomic_fetch_add(ok ? &n_succeeded : &n_call_failed, 1);
#endif
    return ok;
}

// hash -> value + 1 (open addressing; flock held)
typedef struct { uint64_t *k; uint32_t *v; size_t cap, n; } Map;
static Map shader_map, prog_map;
static uint32_t map_get(Map *m, uint64_t h) {
    if (!m->cap) return 0;
    if (!h) h = 1;
    for (size_t j = (h ^ (h >> 29)) & (m->cap - 1); m->k[j]; j = (j + 1) & (m->cap - 1)) if (m->k[j] == h) return m->v[j];
    return 0;
}
static void map_put(Map *m, uint64_t h, uint32_t v) {
    if (!h) h = 1;
    if ((m->n + 1) * 2 > m->cap) {
        Map o = *m;
        size_t cap = o.cap ? o.cap * 2 : 1 << 16;
        uint64_t *keys = calloc(cap, sizeof *keys); uint32_t *values = calloc(cap, sizeof *values);
        if (!keys || !values) { free(keys); free(values); allocation_failed(); return; }
        m->cap = cap; m->k = keys; m->v = values; m->n = 0;
        for (size_t i = 0; i < o.cap; i++) if (o.k[i]) map_put(m, o.k[i], o.v[i]);
        free(o.k); free(o.v);
    }
    size_t j = (h ^ (h >> 29)) & (m->cap - 1);
    while (m->k[j] && m->k[j] != h) j = (j + 1) & (m->cap - 1);
    if (!m->k[j]) m->n++;
    m->k[j] = h; m->v[j] = v;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// (flock held) the index of this shader, added if new
static uint32_t add_shader(const uint8_t *code, uint32_t blob, uint32_t coff, uint32_t csz, uint64_t h, int stage) {
    uint32_t v = map_get(&shader_map, h);
    if (v) return v - 1;
    if (nfound == capfound) {
        size_t cap = capfound ? capfound * 2 : 4096;
        Found *next = realloc(found, cap * sizeof *found);
        if (!next) { allocation_failed(); return UINT32_MAX; }
        found = next; capfound = cap;
    }
    Found *f = &found[nfound];
    // [XCDE] LINUX-018: realloc returns uninitialized storage. Named-input
    // metadata is accumulated later, so every new shader must start empty.
    memset(f, 0, sizeof *f);
    f->hash = h; f->stage = stage; f->coff = coff; f->csz = csz;
    uint32_t lim = coff ? coff : 0x10000, sz = lim > blob ? lim : blob;
    f->code = calloc(1, sz);
    if (!f->code) { allocation_failed(); return UINT32_MAX; }
    memcpy(f->code, code, blob);
    map_put(&shader_map, h, (uint32_t)nfound + 1);
    return (uint32_t)nfound++;
}
static void add_prog(uint32_t vs, uint32_t fs) {
    uint64_t h = found[vs].hash * 0x9E3779B97F4A7C15ull ^ (fs == UINT32_MAX ? 0x5555 : found[fs].hash);
    if (map_get(&prog_map, h)) return;
    if (nprogs == capprogs) {
        size_t cap = capprogs ? capprogs * 2 : 4096;
        Pair *next = realloc(progs, cap * sizeof *progs);
        if (!next) { allocation_failed(); return; }
        progs = next; capprogs = cap;
    }
    progs[nprogs++] = (Pair){vs, fs};
    map_put(&prog_map, h, 1);
    pthread_cond_signal(&fcond);
}

// [XCDE] T211: each program's vertex input names (container program data, the layout of ScanMountGoat/xc3_lib's
// public description of SPCH, tools/spch_attrs.py): which locations the game declares as integers. Monolith names
// them by type - v* float vectors, f* float scalars, n* integers (nWgtIdx, the skinning weight index: location 1 in
// 17,729 character programs, location 0 in 356). A draw binds an integer vertex format there, so the translation key
// has an unsigned input at that location: built that way instead of guessed (the guess was float + "input 1 also
// unsigned"). Returns the number of programs; ctl_of / mask_of: their vertex control sections and integer inputs.
static bool stencil_name(const uint8_t *s, size_t lim) {  // "...Stencil" but not "...ExpStencil" (a float map)
    size_t l = 0;
    while (l < lim && l < 64 && s[l]) l++;
    if (l < 7 || memcmp(s + l - 7, "Stencil", 7)) return false;
    return !(l >= 10 && !memcmp(s + l - 10, "ExpStencil", 10));
}
static size_t spch_inputs(const uint8_t *d, size_t n, size_t b, size_t *ctl_of, uint32_t *mask_of, size_t max) {
    size_t np_ = 0;
    if (!in_range(b, 0x30, n)) return 0;
    uint32_t count = rd32(d+b+12), list = rd32(d+b+8), sec = rd32(d+b+24), ctl = rd32(d+b+40);
    if (!in_range(b + (size_t)list, 8ull * count, n)) { precompile_error("invalid SPCH selection table"); return 0; }
    for (uint32_t i = 0; i < count && !stopped(); i++) {
        size_t sl = b + (size_t)sec + rd32(d+b+list+8ull*i);
        if (!in_range(sl, 48, n) || memcmp(d+sl,"SLCT",4)) { precompile_error("invalid SPCH selection"); continue; }
        uint32_t pn = rd32(d+sl+16), po = rd32(d+sl+20);
        size_t cur = rd32(d+sl+40);
        if (!in_range(sl+(size_t)po,8ull*pn,n)) { precompile_error("invalid SPCH program table"); continue; }
        for (uint32_t k = 0; k < pn && !stopped(); k++) {
            size_t pd = sl + (size_t)rd32(d+sl+po+8ull*k);
            uint32_t psz = rd32(d+sl+po+8ull*k+4);
            if (psz < 88 || !in_range(pd,psz,n)) { precompile_error("invalid SPCH program extent"); return np_; }
            uint32_t so=rd32(d+pd+24), sn=rd32(d+pd+28);
            if (!sn || !in_range(so,20ull*sn,psz)) { precompile_error("invalid SPCH stage table"); continue; }
            const uint8_t *e=d+pd+so+20ull*(sn-1);
            uint32_t vctl=rd32(e+12), fctl=rd32(e+16), an=rd32(d+pd+80), ao=rd32(d+pd+84);
            uint32_t mask=0;
            if (an > 32 || !in_range(ao,8ull*an,psz)) { precompile_error("invalid SPCH attribute table"); continue; }
            for (uint32_t j=0;j<an;j++) {
                uint32_t no=rd32(d+pd+ao+8ull*j), loc=rd32(d+pd+ao+8ull*j+4);
                if (!in_range(no,2,psz)) { precompile_error("invalid SPCH attribute name"); continue; }
                if (loc<32 && d[pd+no]=='n' && d[pd+no+1]>='A' && d[pd+no+1]<='Z') mask|=1u<<loc;
            }
            uint32_t smask=0, sc=d[pd+0x38] | (uint32_t)d[pd+0x39]<<8, sp=rd32(d+pd+0x3c);
            if (sc>32 || (sc && (!sp || !in_range(sp,12ull*sc,psz)))) { precompile_error("invalid SPCH sampler table"); continue; }
            for (uint32_t j=0;j<sc;j++) {
                const uint8_t *se=d+pd+sp+12ull*j; uint32_t no=rd32(se);
                if (no>=psz) { precompile_error("invalid SPCH sampler name"); continue; }
                if (se[8]<32 && se[9] && stencil_name(d+pd+no,psz-no)) smask|=1u<<se[8];
            }
            if (np_+1<max) {
                ctl_of[np_]=b+(size_t)ctl+cur; mask_of[np_++]=mask;
                if (fctl) { ctl_of[np_]=b+(size_t)ctl+cur+vctl; mask_of[np_++]=smask; }
            } else precompile_error("SPCH named program table exceeds supported extent");
            cur+=(uint64_t)vctl+fctl;
        }
    }
    return np_;
}

// one container; all offsets are checked before reading or allocating from extracted metadata.
static void scan_spch(const uint8_t *d, size_t n, size_t b) {
    if (!in_range(b,0x30,n)) return;
#ifdef __linux__
    // HCPS is also encountered incidentally while searching arbitrary decoded
    // map data. A supported SPCH container requires its format version too.
    if(rd32(d+b+4)!=0x3f2u) {
        if(!b) {
            unsupported();
            fprintf(stderr,"[precompile] unsupported SPCH version %#x at input %s\n",rd32(d+b+4),scan_input_path?scan_input_path:"<buffer>");
        }
        return;
    }
#endif
    size_t code=b+(size_t)rd32(d+b+0x20), code_size=rd32(d+b+0x24);
    size_t ctl=b+(size_t)rd32(d+b+0x28), ctl_size=rd32(d+b+0x2c);
    if (!in_range(code,code_size,n) || !in_range(ctl,ctl_size,n)) {
        precompile_error("invalid SPCH section extent (buffer %zu, code %zu+%zu, controls %zu+%zu)",n,code,code_size,ctl,ctl_size); return;
    }
    size_t code_end=code+code_size, ctl_end=ctl+ctl_size;
    enum { MAXP=4096 };
    size_t *ctl_of=malloc(MAXP*sizeof *ctl_of); uint32_t *mask_of=malloc(MAXP*sizeof *mask_of);
    if (!ctl_of || !mask_of) { free(ctl_of); free(mask_of); allocation_failed(); return; }
    size_t nnamed=spch_inputs(d,n,b,ctl_of,mask_of,MAXP), ni=0, pos=code;
    uint32_t last_vs=UINT32_MAX;
    for (size_t c=ctl; in_range(c,0x710,ctl_end) && !stopped(); c+=4) {
        if (rd32(d+c)!=0x98761234u) continue;
        size_t c0=c;
        uint32_t csz=rd32(d+c+0x6fc), coff=rd32(d+c+0x700), blob=rd32(d+c+0x704);
        if (!blob || blob>(4u<<20) || !in_range(pos,blob,code_end)) { precompile_error("invalid SPCH shader blob extent"); break; }
        size_t t=pos; pos=(pos+(size_t)blob+255)&~(size_t)255;
        n_controls++; c+=0x6fc;
        if (blob<4 || rd32(d+t)!=0x12345678u) { unsupported(); continue; } // compute blobs have no graphics header
        if (blob<0x34) { precompile_error("truncated graphics shader header"); continue; }
        uint64_t end=(uint64_t)coff+csz, h=0xcbf29ce484222325ull;
#ifdef __linux__
        if (end<0x30) {
            // Match objects.c's valid no-constants hash, but never read beyond
            // this extracted blob to reproduce its fallback span.
            if (blob<0x800) { unsupported(); continue; }
            end=0x800;
        } else if (end>blob || coff>blob) { precompile_error("invalid SPCH shader constants extent"); continue; }
#else
        if (end<0x30 || end>(4u<<20)) end=0x800;
#endif
        if (!in_range(t,(size_t)end,n) || coff>(4u<<20)) { precompile_error("invalid SPCH shader hash extent"); continue; }
        for (uint64_t k=0;k<end/8;k++) { uint64_t w; memcpy(&w,d+t+k*8,8); h=(h^w)*0x100000001b3ull; }
        uint32_t sph0=rd32(d+t+0x30), type=(sph0>>10)&15;
        int stage=type==1?STAGE_VERTEX:type==5?STAGE_FRAGMENT:-1;
        if (stage<0) { unsupported(); continue; }
        pthread_mutex_lock(&flock);
        uint32_t i=add_shader(d+t,blob,coff,csz,h,stage);
        if (i==UINT32_MAX) { pthread_mutex_unlock(&flock); break; }
        if (nnamed) {
            while (ni<nnamed && ctl_of[ni]<c0) ni++;
            if (ni<nnamed && ctl_of[ni]==c0) { found[i].int_in|=mask_of[ni]; found[i].named=true; }
        }
        if (stage==STAGE_VERTEX) { if (last_vs!=UINT32_MAX) add_prog(last_vs,UINT32_MAX); last_vs=i; }
        else if (last_vs!=UINT32_MAX) { add_prog(last_vs,i); last_vs=UINT32_MAX; }
        pthread_mutex_unlock(&flock);
    }
    if (last_vs!=UINT32_MAX && !stopped()) { pthread_mutex_lock(&flock); add_prog(last_vs,UINT32_MAX); pthread_mutex_unlock(&flock); }
    free(ctl_of); free(mask_of);
}

// every container in a buffer (a file or a decompressed xbc1 block)
static void scan_buf(const uint8_t *d, size_t n) {
    for (size_t i = 0; i + 0x30 <= n; i += 4) {
        if (!(i & 0xffff) && stopped()) break;
        if (!memcmp(d + i, "HCPS", 4)) {
            scan_container_offset=i; scan_spch(d,n,i); scan_container_offset=SIZE_MAX;
        }
    }
}

static pthread_mutex_t big_lock = PTHREAD_MUTEX_INITIALIZER;  // (one file over 64 MB at a time: memory on 8 GB PCs)
static void scan_file(const char *path) {
    scan_input_path=path; scan_block_offset=scan_container_offset=SIZE_MAX;
    FILE *f = fopen(path, "rb");
    if (!f) { precompile_error("cannot open shader input: %s", path); return; }
#ifdef _WIN32
    fseek(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    fseek(f, 0, SEEK_SET);
#else
    // [XCDE] LINUX-002: use the host's 64-bit file offsets and check seek errors.
    if (fseeko(f, 0, SEEK_END) != 0) { precompile_error("cannot seek shader input: %s",path); fclose(f); return; }
    off_t length = ftello(f);
    if (length < 0 || (uintmax_t)length > SIZE_MAX || fseeko(f, 0, SEEK_SET) != 0) {
        precompile_error("cannot determine/read file extent: %s", path);
        fclose(f); return;
    }
    long long n = (long long)length;
#endif
    bool big = n > (64 << 20);
    if (big) pthread_mutex_lock(&big_lock);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { precompile_error("cannot allocate/read shader input: %s",path); free(d); fclose(f); if (big) pthread_mutex_unlock(&big_lock); return; }
    fclose(f);
    scan_buf(d, (size_t)n);
    // xbc1 blocks: "xbc1", u32 version, u32 decompressed size, u32 compressed size, u32 hash, name; zlib data at +0x30
    for (size_t i = 0; i + 0x30 <= (size_t)n && !stopped(); i += 4) {
        if (memcmp(d + i, "xbc1", 4)) continue;
        uint32_t dsz = rd32(d + i + 8), csz = rd32(d + i + 12);
        scan_block_offset=i;
        if (!dsz || dsz > (1u << 30) || !csz || !in_range(i+0x30,csz,(size_t)n)) { precompile_error("invalid xbc1 extent: %s",path); continue; }
        uint8_t *x = malloc(dsz);
        uLongf out = dsz;
        if (x && uncompress(x, &out, d + i + 0x30, csz) == Z_OK && out == dsz) scan_buf(x, out);
        else precompile_error("cannot allocate/decompress xbc1 block: %s",path);
        free(x);
        scan_block_offset=SIZE_MAX;
        i += 0x30 + (csz & ~3u) - 4;
    }
    free(d);
    if (big) pthread_mutex_unlock(&big_lock);
    scan_input_path=NULL;
}

// files that hold shaders (measured over the whole RomFS); the monolib containers (title, menus, lighting, post
// effects) come first: the game needs them first
static bool wanted(const char *name) {
    const char *e = strrchr(name, '.');
    if (!e) return false;
    static const char *ext[] = {".wishp", ".wimdo", ".wismt", ".wismda", ".wiefb", NULL};
    for (int i = 0; ext[i]; i++) if (!strcmp(e, ext[i])) return true;
    return false;
}
static char **paths;
static size_t npaths, cappaths;
static void list_dir_depth(const char *dir, unsigned depth) {
#ifdef __linux__
    poll_cancel();
#endif
    if (depth>128 || stopped()) { if(depth>128) precompile_error("shader directory nesting is too deep"); return; }
    DIR *dd=opendir(dir);
    if (!dd) { precompile_error("cannot enumerate shader directory: %s",dir); return; }
    while (!stopped()) {
#ifdef __linux__
        poll_cancel(); if(stopped()) break;
#endif
        errno=0; struct dirent *e=readdir(dd);
        if (!e) { if(errno) precompile_error("cannot read shader directory: %s",dir); break; }
        if (e->d_name[0]=='.') continue;
        char p[1024]; int size=snprintf(p,sizeof p,"%s/%s",dir,e->d_name);
        if (size<0 || (size_t)size>=sizeof p) { precompile_error("shader path is too long"); continue; }
        struct stat sb;
#ifdef __linux__
        if (lstat(p,&sb)) { precompile_error("cannot inspect shader input: %s",p); continue; }
        if (S_ISLNK(sb.st_mode)) { precompile_error("shader input symlink refused: %s",p); continue; }
#else
        if (stat(p,&sb)) continue;
#endif
        if (S_ISDIR(sb.st_mode)) list_dir_depth(p,depth+1);
        else if (S_ISREG(sb.st_mode) && wanted(e->d_name)) {
            if (npaths==cappaths) {
                size_t cap=cappaths?cappaths*2:1024; char **next=realloc(paths,cap*sizeof *paths);
                if (!next) { allocation_failed(); break; } paths=next; cappaths=cap;
            }
            char *copy=strdup(p); if(!copy) { allocation_failed(); break; } paths[npaths++]=copy;
        }
    }
    closedir(dd);
}
static void list_dir(const char *dir) { list_dir_depth(dir,0); }
static int path_rank(const char *p) { return strstr(p, "/monolib/") ? 0 : strstr(p, ".wishp") ? 1 : 2; }
static int path_cmp(const void *a, const void *b) { return path_rank(*(char *const *)a) - path_rank(*(char *const *)b); }

// ------------------------------------------------------------------ building
static ShaderKey vkey_guess, fkey_guess;

static void stage_of(NvnProgram *np, int s, const Found *f) {
    np->stages[s].stage = s;
    np->stages[s].code_addr = (uint64_t)(uintptr_t)f->code;
    np->stages[s].const_offset = f->coff;
    np->stages[s].const_size = f->csz;
    np->stages[s].code_hash = f->hash;
    np->stage_mask |= 1u << s;
}

// [XCDE] T240: Windows-only legacy crash quarantine (idea from issn_pc): a driver that crashes on one shader / pipeline crashed every later run
// at the same place. Each thread writes the job it is building to cache/precompile_inflight.bin (8 bytes per thread,
// flushed before the driver is called, zeroed after); the next run moves what is left there to
// cache/precompile_skip.txt and skips those jobs from then on (logged). The file is removed after a clean run.
static FILE *inflight;
static pthread_mutex_t inflight_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int inflight_slots;
static _Thread_local int inflight_slot = -1;
static Map skip_map;
static _Atomic uint64_t n_skipped;
static uint64_t job_key(const void *a, size_t na, uint64_t salt) {
    uint64_t h = 0xcbf29ce484222325ull ^ salt;
    for (size_t i = 0; i < na; i++) { h ^= ((const uint8_t *)a)[i]; h *= 0x100000001B3ull; }
    return h ? h : 1;
}
static bool job_begin(uint64_t key) {  // Windows legacy crash quarantine; Linux interruption is never a permanent blacklist.
#ifdef __linux__
    (void)key; return !stopped();
#else
    if (skip_map.n && map_get(&skip_map, key)) { atomic_fetch_add(&n_skipped, 1); return false; }
    if (!inflight) return true;
    if (inflight_slot < 0) inflight_slot = atomic_fetch_add(&inflight_slots, 1);
    pthread_mutex_lock(&inflight_lock);
    fseek(inflight, (long)inflight_slot * 8, SEEK_SET);
    fwrite(&key, 8, 1, inflight);
    fflush(inflight);
    pthread_mutex_unlock(&inflight_lock);
    {   // (tests: XCDE_TEST_PRECOMPILE_CRASH=<n> - the process dies at the n-th job, as a crashing driver would)
        static _Atomic int n;
        static int at = -1;
        if (at < 0) at = getenv("XCDE_TEST_PRECOMPILE_CRASH") ? atoi(getenv("XCDE_TEST_PRECOMPILE_CRASH")) : 0;
        if (at && atomic_fetch_add(&n, 1) + 1 == at) { fflush(stdout); _exit(3); }
    }
    return true;
#endif
}
static void job_end(void) {
    if (!inflight || inflight_slot < 0) return;
    uint64_t z = 0;
    pthread_mutex_lock(&inflight_lock);
    fseek(inflight, (long)inflight_slot * 8, SEEK_SET);
    fwrite(&z, 8, 1, inflight);
    pthread_mutex_unlock(&inflight_lock);
}
static void inflight_open(void) {
    FILE *f = fopen("cache/precompile_inflight.bin", "rb");
    if (f) {  // the last run died: what it was building goes to the skip list
        FILE *o = fopen("cache/precompile_skip.txt", "a");
        uint64_t k;
        int n = 0;
        while (fread(&k, 8, 1, f) == 1) if (k) { if (o) fprintf(o, "%016llx\n", (unsigned long long)k); n++; }
        if (o) fclose(o);
        fclose(f);
        if (n) printf("[precompile] the last run stopped while building %d jobs: they are skipped from now on (cache/precompile_skip.txt)\n", n);
    }
    FILE *sk = fopen("cache/precompile_skip.txt", "r");
    char line[64];
    while (sk && fgets(line, sizeof line, sk)) { uint64_t k = strtoull(line, NULL, 16); if (k) map_put(&skip_map, k, 1); }
    if (sk) fclose(sk);
    if (skip_map.n) printf("[precompile] %zu jobs on the skip list (they crashed the driver)\n", skip_map.n);
    inflight = fopen("cache/precompile_inflight.bin", "wb+");
    if (inflight) { static uint8_t zero[64 * 8]; fwrite(zero, 1, sizeof zero, inflight); fflush(inflight); }
}

static void build_(const Found *vs, const Found *fs);
static void build(Pair pr, const Found *vs, const Found *fs) {
    (void)pr;
    uint64_t hk[2] = {vs->hash, fs ? fs->hash : 0};
    if (!job_begin(job_key(hk, sizeof hk, 1))) { atomic_fetch_add(&n_failed, 1); return; }  // (counted: the build loop waits for all)
    build_(vs, fs);
    job_end();
}
static void build_(const Found *vs, const Found *fs) {
    NvnProgram np;
    memset(&np, 0, sizeof np);
    stage_of(&np, 0, vs);
    if (fs) stage_of(&np, 1, fs);
    uint32_t attr = 0;
    int ok = compile_one(&np, &vkey_guess, &fkey_guess, &attr);
    if (ok && (attr & 2)) {  // (vertex input 1 read: also as an unsigned integer)
        ShaderKey k = vkey_guess;
        k.attr_kind[1] = 2;
        compile_one(&np, &k, &fkey_guess, NULL);
        atomic_fetch_add(&n_extra, 1);
    }
    // [XCDE] T211: in addition, the versions the container's names point to (they say what CAN be an integer: a
    // draw without that data binds a float format there - measured: names alone missed 170 variants, the guess 25)
    if (ok && vs->named && (vs->int_in & ~2u)) {  // integer inputs other than location 1 (nWgtIdx at 0: 356 programs)
        ShaderKey k = vkey_guess;
        for (int l = 0; l < 32; l++) if (vs->int_in >> l & 1) k.attr_kind[l] = 2;
        compile_one(&np, &k, &fkey_guess, NULL);
        atomic_fetch_add(&n_named_int, 1);
    }
    if (ok && fs && fs->named && fs->int_in) {  // stencil textures: also read as integers
        ShaderKey f = fkey_guess;
        f.tex_uint = fs->int_in;
        compile_one(&np, &vkey_guess, &f, NULL);
        if (attr & 2) { ShaderKey k = vkey_guess; k.attr_kind[1] = 2; compile_one(&np, &k, &f, NULL); }
        atomic_fetch_add(&n_named_int, 1);
    }
    atomic_fetch_add(ok ? &n_built : &n_failed, 1);
}

// [XCDE] T211: the shipped variant seed (data/variants_seed.bin, exec.c T163 records: code hashes [3] u64, has_gs u8,
// gs_in u8, 6 pad, vertex ShaderKey, fragment ShaderKey, pipeline key): each distinct (shaders, keys) built exactly -
// the versions the guesses above miss (integer textures / targets / vertex inputs: they follow the data a draw binds,
// not the shader). Geometry-stage programs are skipped.
typedef struct { uint32_t vs, fs; ShaderKey vkey, fkey; } SeedJob;
static SeedJob *seed_jobs;
static size_t nseed;
static _Atomic size_t next_seed;
static _Atomic uint64_t n_seed_built;
// [XCDE] T240: and every recorded variant's whole optimized pipeline (exec.c vk_precompile_variant): the shipped list
// (the debugger tours of every map and cutscene, tools/tour_batch.py) plus the player's own cache/variants.bin
extern int vk_precompile_variant(const NvnProgram *np, const void *rec, size_t rec_size);
#ifdef __linux__
extern size_t vk_precompile_variant_size(void);
#endif
typedef struct { uint32_t vs, fs; uint8_t *rec; } VarJob;
static VarJob *var_jobs;
static size_t nvar, var_rec_size;
static _Atomic size_t next_var;
static _Atomic uint64_t n_var_built, n_var_failed;
static Map var_done;
static void load_seed_file(const char *path, Map *done);
static void load_seed(void) {
    Map done = {0};
#ifndef __linux__
    load_seed_file("data/variants_seed.bin", &done);  // Linux never imports the inherited game-derived shipped seed.
#endif
    load_seed_file("cache/variants.bin", &done);
}
static void load_seed_file(const char *path, Map *done_) {
    Map *done = done_;
    FILE *f = fopen(path, "rb");
    if (!f) { if(errno!=ENOENT) precompile_error("cannot read local variants: %s",path); return; }
    uint32_t hdr[2];
    if (fread(hdr,4,2,f)!=2 || hdr[0]!=0x56435843u || hdr[1]<32+2*sizeof(ShaderKey)
#ifdef __linux__
        || hdr[1]!=vk_precompile_variant_size()
#endif
    ) { precompile_error("incompatible local variants header: %s",path); fclose(f); return; }
    uint8_t *rec=malloc(hdr[1]);
    if(!rec) { allocation_failed(); fclose(f); return; }
    for (;;) {
        size_t bytes=fread(rec,1,hdr[1],f);
        if (!bytes && feof(f)) break;
        if (bytes!=hdr[1]) { precompile_error("truncated/unreadable local variant record: %s",path); break; }
        if (stopped()) break;
        uint64_t h[3];
        memcpy(h, rec, 24);
        if (rec[24]) { unsupported(); continue; }  // geometry variants are not prepared by this pass
        uint32_t vi = map_get(&shader_map, h[0]), fi = h[1] ? map_get(&shader_map, h[1]) : 0;
        if (!vi || (h[1] && !fi)) { unsupported(); continue; }  // a local variant whose shader was not found
        SeedJob j = {vi - 1, fi ? fi - 1 : UINT32_MAX};
        memcpy(&j.vkey, rec + 32, sizeof(ShaderKey));
        memcpy(&j.fkey, rec + 32 + sizeof(ShaderKey), sizeof(ShaderKey));
        uint64_t k = 0xcbf29ce484222325ull;
        const uint8_t *b = (const uint8_t *)&j;
        for (size_t i = 0; i < sizeof j; i++) { k ^= b[i]; k *= 0x100000001B3ull; }
        {   // (the whole record: one pipeline each)
            uint64_t h = 0xcbf29ce484222325ull;
            for (uint32_t i = 0; i < hdr[1]; i++) { h ^= rec[i]; h *= 0x100000001B3ull; }
            if (!h) h = 1;
            if (!map_get(&var_done, h) && (!var_rec_size || var_rec_size == hdr[1])) {
                map_put(&var_done, h, 1);
                var_rec_size = hdr[1];
                VarJob *next=realloc(var_jobs,(nvar+1)*sizeof *var_jobs);
                if(!next) { allocation_failed(); break; } var_jobs=next;
                var_jobs[nvar].vs=j.vs; var_jobs[nvar].fs=j.fs;
                var_jobs[nvar].rec=malloc(hdr[1]);
                if(!var_jobs[nvar].rec) { allocation_failed(); break; }
                memcpy(var_jobs[nvar++].rec,rec,hdr[1]);
            }
        }
        if (!k) k = 1;
        if (map_get(done, k)) continue;
        map_put(done, k, 1);
        SeedJob *next=realloc(seed_jobs,(nseed+1)*sizeof *seed_jobs);
        if(!next) { allocation_failed(); break; } seed_jobs=next; seed_jobs[nseed++]=j;
    }
    free(rec);
    fclose(f);
}
static void *var_worker(void *arg) {
    (void)arg;
    plat_thread_background();
    for (size_t i; !stopped() && (i = atomic_fetch_add(&next_var, 1)) < nvar;) {
        VarJob *j = &var_jobs[i];
        NvnProgram np;
        memset(&np, 0, sizeof np);
        stage_of(&np, 0, &found[j->vs]);
        if (j->fs != UINT32_MAX) stage_of(&np, 1, &found[j->fs]);
        if (!job_begin(job_key(j->rec, var_rec_size, 3))) { atomic_fetch_add(&n_var_failed, 1); continue; }
#ifdef __linux__
        atomic_fetch_add(&n_attempted,1);
#endif
        int ok=vk_precompile_variant(&np,j->rec,var_rec_size);
        atomic_fetch_add(ok ? &n_var_built : &n_var_failed,1);
#ifdef __linux__
        atomic_fetch_add(ok ? &n_succeeded : &n_call_failed,1); atomic_fetch_add(&n_var_done,1);
#endif
        job_end();
    }
    return NULL;
}
static void *seed_worker(void *arg) {
    (void)arg;
    plat_thread_background();
    for (size_t i; !stopped() && (i = atomic_fetch_add(&next_seed, 1)) < nseed;) {
        SeedJob *j = &seed_jobs[i];
        NvnProgram np;
        memset(&np, 0, sizeof np);
        stage_of(&np, 0, &found[j->vs]);
        if (j->fs != UINT32_MAX) stage_of(&np, 1, &found[j->fs]);
        if (!job_begin(job_key(j, sizeof *j, 2))) continue;
        if (compile_one(&np, &j->vkey, &j->fkey, NULL)) atomic_fetch_add(&n_seed_built, 1);
#ifdef __linux__
        atomic_fetch_add(&n_seed_done,1);
#endif
        job_end();
    }
    return NULL;
}

static void *worker(void *arg) {
    (void)arg;
    plat_thread_background();
    for (;;) {
        pthread_mutex_lock(&flock);
        while (next_job >= nprogs && !scan_done && !stopped()) pthread_cond_wait(&fcond, &flock);
        if (next_job >= nprogs || stopped()) { pthread_mutex_unlock(&flock); return NULL; }
        Pair pr = progs[next_job++];
        Found vs = found[pr.vs], fs = pr.fs != UINT32_MAX ? found[pr.fs] : (Found){0};
        pthread_mutex_unlock(&flock);
        build(pr, &vs, pr.fs != UINT32_MAX ? &fs : NULL);
#ifdef __linux__
        atomic_fetch_add(&n_primary_done,1);
#endif
    }
}

static void *scanner(void *arg) {
    (void)arg;
    plat_thread_background();
    for (uint64_t i; !stopped() && (i = atomic_fetch_add(&next_file, 1)) < npaths;) {
        scan_file(paths[i]);
        atomic_fetch_add(&files_done, 1);
    }
    return NULL;
}

static void progress(const char *phase, bool force) {
    static uint64_t last, last_print;
    uint64_t now = now_ns();
    if (!force && now - last < 200000000ull) return;
    last = now;
    uint64_t done = n_built + n_failed;
    FILE *f = fopen("cache/precompile_progress.tmp", "w");
    if (f) {
        fprintf(f, "%s %llu %llu %zu %llu%c", phase, (unsigned long long)files_done, (unsigned long long)files_total, nprogs,
                (unsigned long long)done, 10);
        fclose(f);
        remove("cache/precompile_progress.txt");
        rename("cache/precompile_progress.tmp", "cache/precompile_progress.txt");
    }
    if (force || now - last_print > 2000000000ull) {
        last_print = now;
        printf("[precompile] %s: files %llu/%llu, shaders %zu, programs %zu, built %llu (failed %llu)%c", phase,
               (unsigned long long)files_done, (unsigned long long)files_total, nfound, nprogs, (unsigned long long)n_built,
               (unsigned long long)n_failed, 10);
        fflush(stdout);
    }
}

#ifdef __linux__
// The supervisor validates build/input identities and owns the profile activity lease before this entry.
static void cancel_handler(int signal_number) { cancel_signal=signal_number; }
static void poll_cancel(void) {
    if (cancel_signal) { cancelled_by_user=true; atomic_store(&stop_work,true); return; }
    static uint64_t last;
    uint64_t now=now_ns(); if(now-last<50000000ull) return; last=now;
    int fd=open(PRECOMPILE_CANCEL_PATH,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return;
    FILE *f=fdopen(fd,"r"); if(!f) { close(fd); return; }
    char magic[80], token[80], extra[2];
    bool match=fgets(magic,sizeof magic,f) && !strcmp(magic,PRECOMPILE_CANCEL_MAGIC "\n") &&
        fgets(token,sizeof token,f) && strlen(token)==strlen("session ")+32+1 &&
        !strncmp(token,"session ",8) && !strncmp(token+8,session_id,32) && token[40]=='\n' &&
        !fgets(extra,sizeof extra,f);
    fclose(f);
    if(match) { cancelled_by_user=true; atomic_store(&stop_work,true); }
}
static bool work_publish(const char *phase, const char *state) {
    size_t shaders, programs;
    pthread_mutex_lock(&flock); shaders=nfound; programs=nprogs; pthread_mutex_unlock(&flock);
    char temporary[]="cache/precompile_work.tmp.XXXXXX";
    int fd=mkstemp(temporary);
    if(fd<0) { precompile_error("cannot create work status: %s",strerror(errno)); atomic_store(&stop_work,true); return false; }
    FILE *f=fdopen(fd,"w");
    if(!f) { close(fd); unlink(temporary); precompile_error("cannot open work status"); atomic_store(&stop_work,true); return false; }
    char uuid[VK_UUID_SIZE*2+1];
    for(size_t i=0;i<VK_UUID_SIZE;i++) snprintf(uuid+2*i,3,"%02x",g_vk.props.pipelineCacheUUID[i]);
    fprintf(f,PRECOMPILE_WORK_MAGIC "\nsession %s\nidentity %s\nphase %s\nstate %s\n",session_id,identity_id,phase,state);
#define FIELD(name,value) fprintf(f,#name " %llu\n",(unsigned long long)(value))
    FIELD(files_total,files_total); FIELD(files_done,atomic_load(&files_done)); FIELD(shaders,shaders);
    FIELD(programs,programs); FIELD(programs_done,atomic_load(&n_primary_done));
    FIELD(seed_total,nseed); FIELD(seed_done,atomic_load(&n_seed_done)); FIELD(variants_total,nvar); FIELD(variants_done,atomic_load(&n_var_done));
    FIELD(attempted,atomic_load(&n_attempted)); FIELD(built,atomic_load(&n_succeeded)); FIELD(failed,atomic_load(&n_call_failed));
    FIELD(skipped,atomic_load(&n_skipped)); FIELD(unsupported,atomic_load(&n_unsupported)); FIELD(errors,atomic_load(&n_errors));
    FIELD(gpu_vendor,g_vk.props.vendorID); FIELD(gpu_device,g_vk.props.deviceID); FIELD(gpu_driver,g_vk.props.driverVersion);
#undef FIELD
    fprintf(f,"gpu_cache_uuid %s\n",uuid);
    bool ok=!ferror(f) && fflush(f)==0 && fsync(fileno(f))==0;
    if(fclose(f)) ok=false;
    if(ok && rename(temporary,PRECOMPILE_WORK_PATH)) ok=false;
    if(ok) {
        int dir=open("cache",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(dir<0) ok=false; else { if(fsync(dir)) ok=false; close(dir); }
    }
    if(!ok) { unlink(temporary); precompile_error("cannot commit work status: %s",strerror(errno)); atomic_store(&stop_work,true); }
    return ok;
}
static bool work_progress(const char *phase) {
    static uint64_t last;
    uint64_t now=now_ns(); if(now-last<200000000ull) return true; last=now;
    return work_publish(phase,"running");
}
static bool state_directory(const char *path) {
    struct stat st;
    if(mkdir(path,0700) && errno!=EEXIST) return false;
    return lstat(path,&st)==0 && S_ISDIR(st.st_mode);
}
static int thread_count(void) {
    int n=plat_cpu_count()-1;
    const char *value=getenv("XCDE_PRECOMPILE_THREADS");
    if(value) {
        char *end; errno=0; long requested=strtol(value,&end,10);
        if(errno || end==value || *end || requested<1 || requested>64) { precompile_error("invalid precompile thread count"); return 0; }
        return (int)requested;
    }
    extern int SDL_GetSystemRAM(void);
    int ram=SDL_GetSystemRAM(), cap=ram>0?ram/4/400:n;
    if(cap<n) n=cap;
    return n<1?1:n>64?64:n;
}
static bool spawn_threads(pthread_t *threads, int count, void *(*entry)(void *), int *created) {
    *created=0;
    for(int i=0;i<count;i++) {
        int error=pthread_create(&threads[i],NULL,entry,NULL);
        if(error) { precompile_error("cannot start precompile worker: %s",strerror(error)); atomic_store(&stop_work,true); return false; }
        (*created)++;
    }
    return true;
}
static bool join_threads(pthread_t *threads, int count) {
    bool ok=true;
    for(int i=0;i<count;i++) {
        int e=pthread_join(threads[i],NULL);
        if(e) { // Completion is unknown: do not flush/free Vulkan state that a worker may still be using.
            precompile_error("cannot join precompile worker: %s",strerror(e)); atomic_store(&stop_work,true);
            work_publish("done","failed"); fflush(stdout); fflush(stderr); _exit(1);
        }
    }
    return ok;
}
static void wait_jobs(const char *phase, _Atomic uint64_t *done, uint64_t total) {
    while(atomic_load(done)<total && !stopped()) {
        poll_cancel(); work_progress(phase); struct timespec pause={0,50000000}; nanosleep(&pause,NULL);
    }
}
static void early_cancel_exit(void) {
    int result=work_publish("done","cancelled")?130:1;
    fflush(stdout); fflush(stderr); _exit(result);
}
static int linux_precompile_main(const char *root) {
    uint64_t start=now_ns(); char romfs[1024];
    int length=snprintf(romfs,sizeof romfs,"%s/romfs",root);
    if(length<0 || (size_t)length>=sizeof romfs) { fprintf(stderr,"[precompile] input RomFS path is too long\n"); return 1; }
    const char *id=getenv(PRECOMPILE_ID_ENV), *session=getenv(PRECOMPILE_SESSION_ENV);
    if(!precompile_hex(id,64) || !precompile_hex(session,32)) { fprintf(stderr,"[precompile] checked identity/session required\n"); return 1; }
    memcpy(identity_id,id,sizeof identity_id); memcpy(session_id,session,sizeof session_id);
    if(!state_directory("cache") || !state_directory("cache/spirv")) { fprintf(stderr,"[precompile] cannot create private cache\n"); return 1; }
    struct sigaction action={0}; action.sa_handler=cancel_handler; sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT,&action,NULL) || sigaction(SIGTERM,&action,NULL)) {
        precompile_error("cannot install cancellation handlers"); work_publish("done","failed"); return 1;
    }
    if(unlink("cache/precompile_done.txt") && errno!=ENOENT) { precompile_error("cannot invalidate legacy completion marker"); work_publish("done","failed"); return 1; }
    struct stat input;
    if(lstat(romfs,&input) || !S_ISDIR(input.st_mode)) { precompile_error("invalid selected RomFS directory"); work_publish("done","failed"); return 1; }
    vkey_guess.glsl=fkey_guess.glsl=1; vkey_guess.depth_minus_one=true;
    list_dir(romfs); files_total=npaths;
    if(cancelled_by_user) early_cancel_exit();
    if(!npaths || stopped()) { precompile_error("no usable shader files found"); work_publish("done","failed"); return 1; }
    qsort(paths,npaths,sizeof *paths,path_cmp);
    const char *libs=getenv("XCDE_PRECOMPILE_LIBS"); use_libs=!libs || atoi(libs)!=0;
    if(setenv("XCDE_PRECOMPILING","1",1) || setenv("LARECOMP_HIDDEN","1",1)) { precompile_error("cannot establish precompile environment"); work_publish("done","failed"); return 1; }
    if(!work_publish("scan","running")) return 1;
    poll_cancel(); if(cancelled_by_user) early_cancel_exit();
    ui_window_init();
    bool fatal=false;
    // Retain the initial private cache, but do not fill it with every speculative GPL library.
    VkPipelineCache cache=g_vk.pcache; g_vk.pcache=VK_NULL_HANDLE;
    if(!use_libs) g_vk.gpl=false;
    int count=thread_count(); if(!count) { fatal=true; atomic_store(&stop_work,true); }
    pthread_t builders[64], scanners[64]; int nb=0, ns=0;
    if(!stopped() && !spawn_threads(scanners,count,scanner,&ns)) fatal=true;
    wait_jobs("scan",&files_done,npaths);
    if(!join_threads(scanners,ns)) fatal=true;
    pthread_mutex_lock(&flock); scan_done=true; pthread_cond_broadcast(&fcond); pthread_mutex_unlock(&flock);
    // Complete discovery first: duplicates can add named integer/stencil metadata to an existing shader.
    if(!stopped() && !spawn_threads(builders,count,worker,&nb)) fatal=true;
    wait_jobs("build",&n_primary_done,nprogs);
    if(!join_threads(builders,nb)) fatal=true;
    if(!stopped()) {
        load_seed();
        int created=0;
        if(!stopped() && nseed && !spawn_threads(builders,count,seed_worker,&created)) fatal=true;
        wait_jobs("seed",&n_seed_done,nseed);
        if(!join_threads(builders,created)) fatal=true;
    }
    // Only real locally recorded whole pipelines contribute to the persisted Vulkan cache.
    g_vk.pcache=cache;
    if(!stopped() && nvar) {
        const char *pipelines=getenv("XCDE_PRECOMPILE_PIPELINES");
        if(pipelines && atoi(pipelines)==0) { precompile_error("whole pipeline preparation was disabled"); }
        else {
            int created=0; if(!spawn_threads(builders,count,var_worker,&created)) fatal=true;
            wait_jobs("pipelines",&n_var_done,nvar); if(!join_threads(builders,created)) fatal=true;
        }
    }
    poll_cancel();
    if(stopped() && !cancelled_by_user) fatal=true;
    if(!nprogs) { precompile_error("scan produced no supported graphics programs"); fatal=true; }
    // Joined workers cannot still mutate these caches. Preserve useful partial work, but never a false proof marker.
    if(!vk_spirv_cache_flush()) precompile_error("SPIR-V cache commit failed");
    if(vk_spirv_cache_errors()) precompile_error("SPIR-V cache reported storage errors");
    if(!vk_pipeline_cache_save_checked()) precompile_error("Vulkan pipeline cache commit failed");
    const char *state=cancelled_by_user?"cancelled":fatal?"failed":(atomic_load(&n_errors)||atomic_load(&n_call_failed)||atomic_load(&n_skipped))?"partial":"complete";
    int result=cancelled_by_user?130:fatal?1:!strcmp(state,"complete")?0:2;
    if(!work_publish("done",state)) result=1;
    printf("[precompile] Linux %s: %zu programs, %llu attempts, %llu built, %llu failed, %llu unsupported, %llu errors in %.1f seconds\n",
        state,nprogs,(unsigned long long)atomic_load(&n_attempted),(unsigned long long)atomic_load(&n_succeeded),
        (unsigned long long)atomic_load(&n_call_failed),(unsigned long long)atomic_load(&n_unsupported),(unsigned long long)atomic_load(&n_errors),(now_ns()-start)/1e9);
    printf("[precompile] SPIR-V cache: %llu hits, %llu misses\n",(unsigned long long)vk_spirv_hits,(unsigned long long)vk_spirv_misses);
    fflush(stdout); fflush(stderr); _exit(result); // preserve result; normal renderer atexit forcibly terminates with status0
}
#endif

int vk_precompile_main(const char *root) {
#ifdef __linux__
    return linux_precompile_main(root);
#else
    uint64_t t0 = now_ns();
    char romfs[1024];
    // [XCDE] LINUX-004: this entry can run before main's game-path formatting.
    // Reject a truncated input path before cache writes or scanning any files.
    int romfs_len = snprintf(romfs, sizeof romfs, "%s/romfs", root);
    if (romfs_len < 0 || (size_t)romfs_len >= sizeof romfs) {
        fprintf(stderr, "[precompile] input RomFS path is too long\n");
        return 1;
    }
    mkdir("cache", 0755);
    mkdir("cache/spirv", 0755);
    remove("cache/precompile_done.txt");
    vkey_guess.glsl = fkey_guess.glsl = 1;
    vkey_guess.depth_minus_one = true;  // (the game's depth mode and window origin, as exec.c sets them per draw)
    list_dir(romfs);
    files_total = npaths;
    if (!npaths) { fprintf(stderr, "[precompile] no shader files under %s%c", romfs, 10); return 1; }
    qsort(paths, npaths, sizeof *paths, path_cmp);
    { const char *e = getenv("XCDE_PRECOMPILE_LIBS"); use_libs = !e || atoi(e) != 0; }
    setenv("XCDE_PRECOMPILING", "1", 1);  // (shaders.c: no cache-miss log lines)
    setenv("LARECOMP_HIDDEN", "1", 1);
    ui_window_init();  // (the game's own Vulkan device - same driver caches - in a hidden window)
    if (!use_libs) g_vk.gpl = false;
    // The libraries go to the driver's own disk cache only, not to cache/vk_pipelines.bin: with them that file grew to
    // 698 MB, and the game reads it whole into memory at start (+0.7 GB on 8 GB PCs). The game's own builds of these
    // libraries then hit the driver's cache (measured: tasks.md T187).
    vkDestroyPipelineCache(g_vk.dev, g_vk.pcache, NULL);
    g_vk.pcache = VK_NULL_HANDLE;
    inflight_open();  // [XCDE] T240 crash-safe
    int nthreads = plat_cpu_count() - 1;
    if (getenv("XCDE_PRECOMPILE_THREADS")) nthreads = atoi(getenv("XCDE_PRECOMPILE_THREADS"));
    {   // [XCDE] T240 (idea from SavageWindRecomp 66b110a): the driver compiles take memory per thread - measured here
        // ~1.1 GB peak for 15 threads on NVIDIA, but other drivers' largest shaders take far more: at most one thread
        // per 400 MB of a quarter of the RAM (8 GB: 5 threads; the min-spec 4-core CPU has 3 anyway)
        extern int SDL_GetSystemRAM(void);
        int ram = SDL_GetSystemRAM(), cap = ram > 0 ? ram / 4 / 400 : nthreads;
        if (!getenv("XCDE_PRECOMPILE_THREADS") && cap < nthreads) {
            printf("[precompile] %d threads (memory: %d MB of RAM)%c", cap < 1 ? 1 : cap, ram, 10);
            nthreads = cap;
        }
    }
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;
    pthread_t th[64], sc[64];
    for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < nthreads; i++) pthread_create(&sc[i], NULL, scanner, NULL);
    while (files_done < npaths) {
        progress("scan", false);
        struct timespec ts = {0, 50000000};
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < nthreads; i++) pthread_join(sc[i], NULL);
    uint64_t t_scan = now_ns() - t0;
    pthread_mutex_lock(&flock);
    scan_done = true;
    pthread_cond_broadcast(&fcond);
    pthread_mutex_unlock(&flock);
    while (n_built + n_failed < nprogs) {
        progress("build", false);
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    load_seed();  // [XCDE] T211
    if (nseed) {
        uint64_t ts0 = now_ns();
        for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, seed_worker, NULL);
        while (atomic_load(&next_seed) < nseed) { progress("seed", false); struct timespec ts = {0, 100000000}; nanosleep(&ts, NULL); }
        for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
        printf("[precompile] seed: %zu variants, %llu built in %.1f s%c", nseed, (unsigned long long)n_seed_built, (now_ns() - ts0) / 1e9, 10);
    }
    if (nvar) {  // [XCDE] T240: the whole optimized pipelines (XCDE_PRECOMPILE_PIPELINES=0: pieces only, as before)
        const char *e = getenv("XCDE_PRECOMPILE_PIPELINES");
        if (!(e && atoi(e) == 0)) {
            uint64_t tv0 = now_ns();
            for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, var_worker, NULL);
            while (atomic_load(&next_var) < nvar) {
                static uint64_t lastp;
                if (now_ns() - lastp > 2000000000ull) {
                    lastp = now_ns();
                    printf("[precompile] pipelines: %llu/%zu%c", (unsigned long long)(n_var_built + n_var_failed), nvar, 10);
                    fflush(stdout);
                }
                progress("pipelines", false);
                struct timespec ts = {0, 100000000};
                nanosleep(&ts, NULL);
            }
            for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
            printf("[precompile] pipelines: %zu recorded variants, %llu built, %llu not (shaders not found / failed) in %.1f s%c", nvar,
                   (unsigned long long)n_var_built, (unsigned long long)n_var_failed, (now_ns() - tv0) / 1e9, 10);
        }
    }
    progress("done", true);
    if (inflight) { fclose(inflight); inflight = NULL; remove("cache/precompile_inflight.bin"); }  // (a clean run)
    if (n_skipped) printf("[precompile] %llu jobs skipped (skip list)\n", (unsigned long long)n_skipped);
    double secs = (now_ns() - t0) / 1e9;
    printf("[precompile] %zu files, %llu control sections, %zu distinct shaders, %zu programs in %.1f s (scan %.1f s): "
           "%llu built (%llu also with integer input 1, %llu more from the container's names), %llu failed; %llu SPIR-V compiled, %llu from the cache; %d threads%c",
           npaths, (unsigned long long)n_controls, nfound, nprogs, secs, t_scan / 1e9, (unsigned long long)n_built,
           (unsigned long long)n_extra, (unsigned long long)n_named_int, (unsigned long long)n_failed, (unsigned long long)vk_spirv_misses,
           (unsigned long long)vk_spirv_hits, nthreads, 10);
    if (getenv("XCDE_PRECOMPILE_LIST")) {  // (tests: "S <hash> <stage>" per shader, "P <vs hash> <fs hash>" per program)
        FILE *l = fopen(getenv("XCDE_PRECOMPILE_LIST"), "w");
        for (size_t i = 0; l && i < nfound; i++) fprintf(l, "S %016llx %d%c", (unsigned long long)found[i].hash, found[i].stage, 10);
        for (size_t i = 0; l && i < nprogs; i++)
            fprintf(l, "P %016llx %016llx%c", (unsigned long long)found[progs[i].vs].hash,
                    (unsigned long long)(progs[i].fs != UINT32_MAX ? found[progs[i].fs].hash : 0), 10);
        if (l) fclose(l);
    }
    FILE *f = fopen("cache/precompile_done.txt", "w");
    if (f) {
        fprintf(f, "programs %zu built %llu failed %llu shaders %zu seconds %.0f%c", nprogs, (unsigned long long)n_built,
                (unsigned long long)n_failed, nfound, secs, 10);
        // [XCDE] T211: the build that precompiled. A new build may translate shaders differently (every translator fix
        // changes SPIR-V): its pieces would no longer match and the game compiled them while playing (10-58 ms hitches
        // on the first draws of each area). The launcher compares this line with build/xcde.exe and precompiles again.
        struct stat es;
        if (stat("build/xcde.exe", &es) == 0) fprintf(f, "exe %lld %lld%c", (long long)es.st_size, (long long)es.st_mtime, 10);
        fclose(f);
    }
    fflush(stdout);
    _exit(0);  // (no atexit work: the pipeline cache is saved above; the game's exit handler is for the game)
#endif
}
