// [XCDE] T187: shader precompilation from the game's own shader containers (RomFS), before the first play.
//
// Native preparation is explicit: xcde.exe --precompile TARGETS --session HEX32 --identity HEX64.
// Play never automatically prepares shaders, retries or imports permanent blacklists. Preparation walks
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
// Native status is session keyed; successful exit requires joined supported jobs and checked cache commits.
// Unsupported/new variants still compile during play. No shipped game-derived seed is imported.
// XCDE_PRECOMPILE_THREADS defaults to CPUs-1 with a memory cap (1..64 explicit).
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
#include "../../platform/windows_cache_transaction.h"
#include <stdarg.h>

#include "../../rt.h"
#include "shaders.h"
#include "precompile_status.h"
extern bool vk_pipeline_cache_save_checked(void);

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
static _Atomic uint64_t n_attempted, n_succeeded, n_call_failed, n_errors, n_unsupported, n_primary_done;
static _Atomic uint64_t n_seed_done, n_var_done;
static _Atomic bool stop_work;

static bool cancelled_by_user;
static char session_id[33], identity_id[65];
static pthread_mutex_t diagnostic_lock=PTHREAD_MUTEX_INITIALIZER;
static void poll_cancel(void);
static bool stopped(void) { return atomic_load_explicit(&stop_work, memory_order_relaxed); }
static void precompile_error(const char *fmt, ...) {
    atomic_fetch_add(&n_errors, 1);
    va_list args; va_start(args, fmt); pthread_mutex_lock(&diagnostic_lock);
    fputs("[precompile] ", stderr);
    if(scan_input_path) {
        fprintf(stderr,"input %s",scan_input_path);
        if(scan_block_offset!=SIZE_MAX) fprintf(stderr," xbc1@%zu",scan_block_offset);
        if(scan_container_offset!=SIZE_MAX) fprintf(stderr," SPCH@%zu",scan_container_offset);
        fputs(": ",stderr);
    }
    vfprintf(stderr, fmt, args); fputc('\n', stderr);
    pthread_mutex_unlock(&diagnostic_lock); va_end(args);
}
static void allocation_failed(void) { precompile_error("allocation failed"); atomic_store(&stop_work, true); }
static void unsupported(void) { atomic_fetch_add(&n_unsupported, 1); }
static bool in_range(size_t off, size_t bytes, size_t n) { return off <= n && bytes <= n - off; }
static int compile_one(const NvnProgram *np, const ShaderKey *vk, const ShaderKey *fk, uint32_t *attr) {
    if (stopped()) return 0;
    atomic_fetch_add(&n_attempted, 1);
    int ok = vk_precompile_program(np, vk, fk, attr);
    atomic_fetch_add(ok ? &n_succeeded : &n_call_failed, 1);
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
    // HCPS is also encountered incidentally while searching arbitrary decoded
    // map data. A supported SPCH container requires its format version too.
    if(rd32(d+b+4)!=0x3f2u) {
        if(!b) {
            unsupported();
            fprintf(stderr,"[precompile] unsupported SPCH version %#x at input %s\n",rd32(d+b+4),scan_input_path?scan_input_path:"<buffer>");
        }
        return;
    }
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
        if (end<0x30) {
            // Match objects.c's valid no-constants hash, but never read beyond
            // this extracted blob to reproduce its fallback span.
            if (blob<0x800) { unsupported(); continue; }
            end=0x800;
        } else if (end>blob || coff>blob) { precompile_error("invalid SPCH shader constants extent"); continue; }
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

static FILE *precompile_input(const char *path) {
    WsGuard guard;if(wct_guard_open(&guard,path))return NULL;
    HANDLE h=CreateFileW(guard.path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    bool ok=h!=INVALID_HANDLE_VALUE&&ws_plain(h,false,true)&&GetFileType(h)==FILE_TYPE_DISK;
    ws_guard_close(&guard);if(!ok){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);return NULL;}
    int fd=_open_osfhandle((intptr_t)h,O_RDONLY|O_BINARY);if(fd<0){CloseHandle(h);return NULL;}
    FILE *f=_fdopen(fd,"rb");if(!f)_close(fd);return f;
}
static pthread_mutex_t big_lock = PTHREAD_MUTEX_INITIALIZER;  // (one file over 64 MB at a time: memory on 8 GB PCs)
static void scan_file(const char *path) {
    scan_input_path=path; scan_block_offset=scan_container_offset=SIZE_MAX;
    FILE *f = precompile_input(path);
    if (!f) { precompile_error("cannot open shader input: %s", path); return; }
#ifdef _WIN32
    if(_fseeki64(f,0,SEEK_END)){precompile_error("cannot seek shader input");fclose(f);return;}
    long long n=_ftelli64(f);
    if(n<0||(uint64_t)n>SIZE_MAX||_fseeki64(f,0,SEEK_SET)){precompile_error("invalid shader input extent");fclose(f);return;}
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
static void list_dir_depth(const char *dir,unsigned depth) {
    poll_cancel();if(stopped())return;
    if(depth>128){precompile_error("shader directory nesting is too deep");return;}
    struct Walk {char anchor[WS_PATH*3],name[WS_PATH*3],path[WS_PATH*3];wchar_t pattern[WS_PATH];WsGuard guard;WIN32_FIND_DATAW entry;};
    struct Walk *walk=malloc(sizeof *walk);if(!walk){allocation_failed();return;}
    char *anchor=walk->anchor,*name=walk->name,*path=walk->path;wchar_t *pattern=walk->pattern;
    WsGuard *guard=&walk->guard;WIN32_FIND_DATAW *entry=&walk->entry;
    if(snprintf(anchor,WS_PATH*3,"%s/.lease",dir)>=(int)WS_PATH*3){precompile_error("shader path too long");free(walk);return;}
    if(wct_guard_open(guard,anchor)){precompile_error("unsafe shader directory");free(walk);return;}
    size_t length=wcslen(guard->path);
    if(length+1>=WS_PATH){ws_guard_close(guard);precompile_error("shader path too long");free(walk);return;}
    memcpy(pattern,guard->path,(length+1)*sizeof(wchar_t));wchar_t *leaf=wcsrchr(pattern,L'\\');leaf[1]=L'*';leaf[2]=0;
    HANDLE find=FindFirstFileW(pattern,entry);
    if(find==INVALID_HANDLE_VALUE){DWORD error=GetLastError();ws_guard_close(guard);if(error!=ERROR_FILE_NOT_FOUND)precompile_error("cannot enumerate shader directory");free(walk);return;}
    for(;;){
        poll_cancel();if(stopped())break;
        if(wcscmp(entry->cFileName,L".")&&wcscmp(entry->cFileName,L"..")){
            if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,entry->cFileName,-1,name,WS_PATH*3,NULL,NULL)||snprintf(path,WS_PATH*3,"%s/%s",dir,name)>=(int)WS_PATH*3)precompile_error("shader path too long");
            else if(entry->dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)precompile_error("shader descendant reparse point refused: %s",path);
            else if(entry->dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)list_dir_depth(path,depth+1);
            else if(wanted(name)){
                if(npaths==cappaths){size_t cap=cappaths?cappaths*2:1024;char **next=realloc(paths,cap*sizeof *paths);if(!next){allocation_failed();break;}paths=next;cappaths=cap;}
                char *copy=strdup(path);if(!copy){allocation_failed();break;}paths[npaths++]=copy;
            }
        }
        if(!FindNextFileW(find,entry)){if(GetLastError()!=ERROR_NO_MORE_FILES)precompile_error("cannot read shader directory");break;}
    }
    FindClose(find);ws_guard_close(guard);free(walk);
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

static _Atomic uint64_t n_skipped;
static uint64_t job_key(const void *a,size_t n,uint64_t salt) {(void)a;(void)n;return salt;}
static bool job_begin(uint64_t key) {(void)key;return !stopped();}
static void job_end(void) {}
static void build_(const Found *vs,const Found *fs);

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

// Locally recorded variants only (exec.c records: code hashes [3] u64, has_gs u8,
// gs_in u8, 6 pad, vertex ShaderKey, fragment ShaderKey, pipeline key): each distinct (shaders, keys) built exactly -
// the versions the guesses above miss (integer textures / targets / vertex inputs: they follow the data a draw binds,
// not the shader). Geometry-stage programs are skipped.
typedef struct { uint32_t vs, fs; ShaderKey vkey, fkey; } SeedJob;
static SeedJob *seed_jobs;
static size_t nseed;
static _Atomic size_t next_seed;
static _Atomic uint64_t n_seed_built;
// Locally recorded whole optimized pipelines from this profile's cache/variants.bin.
extern int vk_precompile_variant(const NvnProgram *np, const void *rec, size_t rec_size);
extern size_t vk_precompile_variant_size(void);
typedef struct { uint32_t vs, fs; uint8_t *rec; } VarJob;
static VarJob *var_jobs;
static size_t nvar, var_rec_size;
static _Atomic size_t next_var;
static _Atomic uint64_t n_var_built, n_var_failed;
static Map var_done;
static void load_seed_file(const char *path, Map *done);
static void load_seed(void) {
    Map done = {0};
    load_seed_file("cache/variants.bin", &done);
}
static void load_seed_file(const char *path, Map *done_) {
    Map *done = done_;
    FILE *f = wct_read(path,256u*1024*1024);
    if (!f) { if(errno!=ENOENT) precompile_error("cannot read local variants: %s",path); return; }
    uint32_t hdr[2];
    if (fread(hdr,4,2,f)!=2 || hdr[0]!=0x56435843u || hdr[1]<32+2*sizeof(ShaderKey)
        || hdr[1]!=vk_precompile_variant_size()
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
        SeedJob j = {.vs=vi-1,.fs=fi?fi-1:UINT32_MAX};
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
        atomic_fetch_add(&n_attempted,1);
        int ok=vk_precompile_variant(&np,j->rec,var_rec_size);
        atomic_fetch_add(ok ? &n_var_built : &n_var_failed,1);
        atomic_fetch_add(ok ? &n_succeeded : &n_call_failed,1); atomic_fetch_add(&n_var_done,1);
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
        atomic_fetch_add(&n_seed_done,1);
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
        atomic_fetch_add(&n_primary_done,1);
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

// The supervisor validates build/input identities and owns the profile activity lease before this entry.
static void poll_cancel(void) {
    static uint64_t last;
    uint64_t now=now_ns(); if(now-last<50000000ull) return; last=now;
    char path[128];snprintf(path,sizeof path,PRECOMPILE_CANCEL_PREFIX "%s.txt",session_id);
    FILE *f=wct_read(path,256);if(!f)return;
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
    char bytes[4096],uuid[VK_UUID_SIZE*2+1];size_t offset=0;bool ok=true;
    for(size_t i=0;i<VK_UUID_SIZE;i++)snprintf(uuid+2*i,3,"%02x",g_vk.props.pipelineCacheUUID[i]);
#define APPEND(...) do {int n=snprintf(bytes+offset,sizeof bytes-offset,__VA_ARGS__);if(n<0||(size_t)n>=sizeof bytes-offset)ok=false;else offset+=(size_t)n;}while(0)
    APPEND(PRECOMPILE_WORK_MAGIC "\nsession %s\nidentity %s\nphase %s\nstate %s\n",session_id,identity_id,phase,state);
#define FIELD(name,value) APPEND(#name " %llu\n",(unsigned long long)(value))
    FIELD(files_total,files_total);FIELD(files_done,atomic_load(&files_done));FIELD(shaders,shaders);
    FIELD(programs,programs);FIELD(programs_done,atomic_load(&n_primary_done));
    FIELD(seed_total,nseed);FIELD(seed_done,atomic_load(&n_seed_done));FIELD(variants_total,nvar);FIELD(variants_done,atomic_load(&n_var_done));
    FIELD(attempted,atomic_load(&n_attempted));FIELD(built,atomic_load(&n_succeeded));FIELD(failed,atomic_load(&n_call_failed));
    FIELD(skipped,atomic_load(&n_skipped));FIELD(unsupported,atomic_load(&n_unsupported));FIELD(errors,atomic_load(&n_errors));
    FIELD(gpu_vendor,g_vk.props.vendorID);FIELD(gpu_device,g_vk.props.deviceID);FIELD(gpu_driver,g_vk.props.driverVersion);
#undef FIELD
    APPEND("gpu_cache_uuid %s\n",uuid);
#undef APPEND
    FILE *previous=wct_read(PRECOMPILE_WORK_PATH,4096);
    if(previous){char existing[4097];size_t count=fread(existing,1,sizeof existing,previous);
        if(ferror(previous)||!precompile_work_valid(existing,count))ok=false;if(fclose(previous))ok=false;}
    else if(errno!=ENOENT)ok=false;
    if(ok)ok=wct_write(PRECOMPILE_WORK_PATH,bytes,offset,PRECOMPILE_WORK_MAGIC "\n",4096);
    if(!ok){precompile_error("cannot commit owned work status");atomic_store(&stop_work,true);}
    return ok;
}
static bool work_progress(const char *phase) {
    static uint64_t last;
    uint64_t now=now_ns(); if(now-last<200000000ull) return true; last=now;
    return work_publish(phase,"running");
}
static bool state_directory(const char *path) {
    return wct_directory(path);
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
static int windows_precompile_main(const char *root) {
    uint64_t start=now_ns(); char romfs[WS_PATH*3];
    int length=snprintf(romfs,sizeof romfs,"%s/romfs",root);
    if(length<0 || (size_t)length>=sizeof romfs) { fprintf(stderr,"[precompile] input RomFS path is too long\n"); return 1; }
    const char *id=getenv(PRECOMPILE_ID_ENV), *session=getenv(PRECOMPILE_SESSION_ENV);
    if(!precompile_hex(id,64) || !precompile_hex(session,32)) { fprintf(stderr,"[precompile] checked identity/session required\n"); return 1; }
    memcpy(identity_id,id,sizeof identity_id); memcpy(session_id,session,sizeof session_id);
    if(!state_directory("cache") || !state_directory("cache/spirv")) { fprintf(stderr,"[precompile] cannot create private cache\n"); return 1; }
    WsGuard state_guard;
    if(wct_guard_open(&state_guard,"cache/.precompile.lock")){fprintf(stderr,"[precompile] cannot retain cache ancestors\n");return 1;}
    HANDLE lease=wct_owned_file(state_guard.path,GENERIC_READ|GENERIC_WRITE,0,OPEN_ALWAYS);
    LARGE_INTEGER lease_size;
    if(lease==INVALID_HANDLE_VALUE||!GetFileSizeEx(lease,&lease_size)||lease_size.QuadPart){
        if(lease!=INVALID_HANDLE_VALUE)CloseHandle(lease);ws_guard_close(&state_guard);
        fprintf(stderr,"[precompile] cache is busy or lease is not recognized\n");return 1;}
    // Only the explicit selected RomFS boundary may resolve a junction. Descendants are no-follow.
    wchar_t input[WS_PATH],final[WS_PATH];
    if(!wct_absolute(romfs,input))return 1;
    HANDLE dir=CreateFileW(input,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    BY_HANDLE_FILE_INFORMATION info;DWORD count_path=dir==INVALID_HANDLE_VALUE?0:GetFinalPathNameByHandleW(dir,final,WS_PATH,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    bool input_ok=dir!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(dir,&info)&&(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&count_path&&count_path<WS_PATH;
    if(!input_ok){if(dir!=INVALID_HANDLE_VALUE)CloseHandle(dir);precompile_error("invalid selected RomFS directory");work_publish("done","failed");return 1;}
    const wchar_t *physical=final;
    if(!wcsncmp(final,L"\\\\?\\UNC\\",8)){final[6]=L'\\';physical=final+6;}
    else if(!wcsncmp(final,L"\\\\?\\",4))physical=final+4;
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,physical,-1,romfs,sizeof romfs,NULL,NULL)){CloseHandle(dir);return 1;}
    // Retain selected input directory for the whole scan/build; never write game input.
    vkey_guess.glsl=fkey_guess.glsl=1; vkey_guess.depth_minus_one=true;
    list_dir(romfs); files_total=npaths;
    if(cancelled_by_user) early_cancel_exit();
    if(!npaths || stopped()) { precompile_error("no usable shader files found"); work_publish("done","failed"); return 1; }
    qsort(paths,npaths,sizeof *paths,path_cmp);
    const char *libs=getenv("XCDE_PRECOMPILE_LIBS"); use_libs=!libs || atoi(libs)!=0;
    if(_putenv_s("XCDE_PRECOMPILING","1") || _putenv_s("LARECOMP_HIDDEN","1")) { precompile_error("cannot establish precompile environment"); work_publish("done","failed"); return 1; }
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
    printf("[precompile] Windows %s: %zu programs, %llu attempts, %llu built, %llu failed, %llu unsupported, %llu errors in %.1f seconds\n",
        state,nprogs,(unsigned long long)atomic_load(&n_attempted),(unsigned long long)atomic_load(&n_succeeded),
        (unsigned long long)atomic_load(&n_call_failed),(unsigned long long)atomic_load(&n_unsupported),(unsigned long long)atomic_load(&n_errors),(now_ns()-start)/1e9);
    printf("[precompile] SPIR-V cache: %llu hits, %llu misses\n",(unsigned long long)vk_spirv_hits,(unsigned long long)vk_spirv_misses);
    fflush(stdout); fflush(stderr); _exit(result); // preserve result; normal renderer atexit forcibly terminates with status0
}

int vk_precompile_main(const char *root) { return windows_precompile_main(root); }
