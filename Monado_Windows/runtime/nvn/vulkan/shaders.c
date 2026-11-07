// GLSL -> SPIR-V (shaderc), descriptor layouts from the SPIR-V itself, translated NVN programs, built-in shaders.
#include <pthread.h>
#include <shaderc/shaderc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "shaders.h"

void vk_shaders_init(void) {  // (compilers are per thread: check shaderc works at all)
    shaderc_compiler_t c = shaderc_compiler_initialize();
    if (!c) FATAL("shaderc_compiler_initialize failed\n");
    shaderc_compiler_release(c);
}

static shaderc_shader_kind kind_of(int stage) {
    return stage == STAGE_VERTEX ? shaderc_vertex_shader : stage == STAGE_FRAGMENT ? shaderc_fragment_shader
         : stage == STAGE_GEOMETRY ? shaderc_geometry_shader : shaderc_compute_shader;  // [XCDE] geometry
}

// SPIR-V disk cache: cache/spirv/<hash of stage + source>.spv, so a known shader costs a file read instead of a
// GLSL compile (the driver's pipeline cache only covers the step after this one).
static uint64_t src_hash(const char *src, int stage) {
    uint64_t h = (0xcbf29ce484222325ull ^ (uint64_t)stage) + 3;  // + version of the post-processing (no_contraction)
    for (const char *p = src; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    return h;
}

// [XCDE] SPIR-V pack: cache/spirv.pack (records: u32 'XSPV', u32 bytes, u64 hash, SPIR-V) and cache/spirv.idx (24-byte
// entries: u64 hash, u64 data offset, u32 bytes, u32 check), both append-only. One file instead of ~100,000: each first
// open of a loose .spv after a reboot or a Defender update cost ~3.7 ms of scanning (a boot reads ~7,000: +25 s).
// Appends hold an exclusive lock on the index file, so test runs, the precompiler and the game can share it; a process
// re-reads the index tail on a miss to see the others' entries. A crash mid-append leaves unindexed bytes (ignored) or
// a torn index entry (overwritten by the next append). Loose files are still read, and moved into the pack when hit.
// XCDE_SPIRV_LOOSE=1: loose files only, as before.
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/file.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#ifndef O_BINARY
#define O_BINARY 0
#endif
typedef struct { uint64_t hash, off; uint32_t bytes, check; } PackEntry;
static pthread_mutex_t pack_lock = PTHREAD_MUTEX_INITIALIZER;
static int pack_fd = -1, idx_fd = -1, pack_state;  // (state: 0 not opened, 1 open, -1 off / failed)
static PackEntry *pack_tab;  // open addressing on hash (0: empty slot)
static size_t pack_cap, pack_n;
static int64_t idx_read;  // index bytes taken in

static uint32_t pack_check(const PackEntry *e) { return e->bytes ^ (uint32_t)e->hash ^ (uint32_t)(e->off >> 3) ^ 0x5853u; }
// 64-bit offsets (mingw's off_t / st_size are 32-bit; the pack grows past 2 GB)
#ifdef _WIN32
static int64_t fd_size(int fd) { return _filelengthi64(fd); }
static ssize_t io64(int fd, void *buf, size_t n, int64_t off, bool wr) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov = {0};
        uint64_t o = (uint64_t)off + done;
        ov.Offset = (DWORD)o, ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD chunk = n - done > 0x40000000 ? 0x40000000 : (DWORD)(n - done), got = 0;
        if (!(wr ? WriteFile(h, (const char *)buf + done, chunk, &got, &ov) : ReadFile(h, (char *)buf + done, chunk, &got, &ov)) || !got)
            break;
        done += got;
    }
    return (ssize_t)done;
}
#else
static int64_t fd_size(int fd) { struct stat sb; return fstat(fd, &sb) ? -1 : (int64_t)sb.st_size; }
static ssize_t io64(int fd, void *buf, size_t n, int64_t off, bool wr) { return wr ? pwrite(fd, buf, n, off) : pread(fd, buf, n, off); }
#endif
#define pread64_(fd, b, n, o) io64(fd, b, n, o, false)
#define pwrite64_(fd, b, n, o) io64(fd, (void *)(b), n, o, true)

static void pack_insert(const PackEntry *e) {
    if ((pack_n + 1) * 10 > pack_cap * 6) {
        size_t oc = pack_cap;
        PackEntry *ot = pack_tab;
        pack_cap = oc ? oc * 2 : 1u << 17;
        pack_tab = calloc(pack_cap, sizeof *pack_tab);
        pack_n = 0;
        for (size_t i = 0; i < oc; i++) if (ot[i].hash) pack_insert(&ot[i]);
        free(ot);
    }
    size_t i = (size_t)(e->hash * 0x9E3779B97F4A7C15ull) & (pack_cap - 1);
    while (pack_tab[i].hash && pack_tab[i].hash != e->hash) i = (i + 1) & (pack_cap - 1);
    if (!pack_tab[i].hash) pack_n++;
    pack_tab[i] = *e;  // (a later entry for the same hash wins)
}
static const PackEntry *pack_find(uint64_t h) {
    if (!pack_cap) return NULL;
    size_t i = (size_t)(h * 0x9E3779B97F4A7C15ull) & (pack_cap - 1);
    while (pack_tab[i].hash) { if (pack_tab[i].hash == h) return &pack_tab[i]; i = (i + 1) & (pack_cap - 1); }
    return NULL;
}

// (pack_lock held) take in the index entries other processes (or a crash) left past what we read
static void pack_refresh(void) {
    int64_t isz = fd_size(idx_fd), psz = fd_size(pack_fd);
    if (isz < 0 || psz < 0) return;
    isz -= isz % (int64_t)sizeof(PackEntry);
    while (idx_read < isz) {
        PackEntry buf[1024];
        int64_t want = isz - idx_read;
        if (want > (int64_t)sizeof buf) want = sizeof buf;
        ssize_t got = pread64_(idx_fd, buf, (size_t)want, idx_read);
        if (got <= 0) break;
        got -= got % (ssize_t)sizeof(PackEntry);
        for (ssize_t i = 0; i < got / (ssize_t)sizeof(PackEntry); i++)
            if (buf[i].hash && buf[i].check == pack_check(&buf[i]) && buf[i].off + buf[i].bytes <= (uint64_t)psz)
                pack_insert(&buf[i]);
        idx_read += got;
    }
}

// (pack_lock held)
static bool pack_open(void) {
    if (pack_state) return pack_state > 0;
    pack_state = -1;
    const char *e = getenv("XCDE_SPIRV_LOOSE");
    if (e && atoi(e)) return false;
    mkdir("cache", 0755);
    pack_fd = open("cache/spirv.pack", O_RDWR | O_CREAT | O_BINARY, 0644);
    idx_fd = open("cache/spirv.idx", O_RDWR | O_CREAT | O_BINARY, 0644);
    if (pack_fd < 0 || idx_fd < 0) {
        LOG("[shader] spirv pack unavailable: loose files\n");
        if (pack_fd >= 0) close(pack_fd);
        if (idx_fd >= 0) close(idx_fd);
        return false;
    }
    pack_state = 1;
    pack_refresh();
    LOG("[shader] spirv pack: %zu shaders (%.0f MB)\n", pack_n, fd_size(pack_fd) / 1048576.0);
    return true;
}

static void file_lock(int fd, bool on) {
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED ov = {0};
    if (on) LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov);
    else UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov);
#else
    flock(fd, on ? LOCK_EX : LOCK_UN);
#endif
}

static uint32_t *pack_get(uint64_t h, size_t *nwords) {
    pthread_mutex_lock(&pack_lock);
    if (!pack_open()) { pthread_mutex_unlock(&pack_lock); return NULL; }
    const PackEntry *e = pack_find(h);
    if (!e) { pack_refresh(); e = pack_find(h); }
    PackEntry c = e ? *e : (PackEntry){0};
    pthread_mutex_unlock(&pack_lock);
    if (!c.hash || c.bytes < 20 || c.bytes % 4 || c.off < 16) return NULL;
    uint32_t hdr[4];
    uint32_t *d = malloc(c.bytes);
    if (pread64_(pack_fd, hdr, 16, (int64_t)c.off - 16) != 16 || hdr[0] != 0x56505358u || hdr[1] != c.bytes ||
        ((uint64_t)hdr[3] << 32 | hdr[2]) != h || pread64_(pack_fd, d, c.bytes, (int64_t)c.off) != (ssize_t)c.bytes ||
        d[0] != 0x07230203) {
        free(d);
        return NULL;
    }
    *nwords = c.bytes / 4;
    return d;
}

static bool pack_has(uint64_t h) {
    pthread_mutex_lock(&pack_lock);
    bool ok = pack_open() && (pack_find(h) || (pack_refresh(), pack_find(h)));
    pthread_mutex_unlock(&pack_lock);
    return ok;
}

static bool pack_put(uint64_t h, const uint32_t *spv, size_t nwords) {
    pthread_mutex_lock(&pack_lock);
    if (!pack_open()) { pthread_mutex_unlock(&pack_lock); return false; }
    file_lock(idx_fd, true);
    pack_refresh();  // (another process may have stored it meanwhile)
    bool ok = pack_find(h) != NULL;
    int64_t psz = fd_size(pack_fd), isz = fd_size(idx_fd);
    if (!ok && psz >= 0 && isz >= 0) {
        size_t bytes = nwords * 4;
        uint8_t *rec = malloc(16 + bytes);
        uint32_t hdr[4] = {0x56505358u, (uint32_t)bytes, (uint32_t)h, (uint32_t)(h >> 32)};
        memcpy(rec, hdr, 16);
        memcpy(rec + 16, spv, bytes);
        PackEntry e = {h, (uint64_t)psz + 16, (uint32_t)bytes, 0};
        e.check = pack_check(&e);
        isz -= isz % (int64_t)sizeof e;  // (a torn entry from a crash is overwritten)
        ok = pwrite64_(pack_fd, rec, 16 + bytes, psz) == (ssize_t)(16 + bytes) &&
             pwrite64_(idx_fd, &e, sizeof e, isz) == (ssize_t)sizeof e;
        free(rec);
        if (ok) { pack_insert(&e); if (idx_read == isz) idx_read += sizeof e; }
    }
    file_lock(idx_fd, false);
    pthread_mutex_unlock(&pack_lock);
    return ok;
}

static uint32_t *loose_load(uint64_t h, size_t *nwords);
static uint32_t *cache_load(uint64_t h, size_t *nwords) {
    uint32_t *d = pack_get(h, nwords);
    if (d) return d;
    d = loose_load(h, nwords);
    if (d) pack_put(h, d, *nwords);  // (moved into the pack: the next start reads it from there)
    return d;
}

static uint32_t *loose_load(uint64_t h, size_t *nwords) {
    char path[64];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *d = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d && d[0] != 0x07230203) { free(d); d = NULL; }
    if (d) *nwords = (size_t)n / 4;
    return d;
}

static void cache_store(uint64_t h, const uint32_t *spv, size_t nwords) {
    if (pack_put(h, spv, nwords)) return;
    static int made;
    if (!made) { made = 1; mkdir("cache", 0755); mkdir("cache/spirv", 0755); }
    char path[64], tmp[80];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)&made ^ (unsigned long long)(uintptr_t)spv);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(spv, 4, nwords, f) == nwords;
    fclose(f);
    if (!ok || rename(tmp, path) != 0) remove(tmp);  // (another thread may have stored it first)
}

// Vertex shaders: every float add/sub/mul/div/fma gets NoContraction, so no compiler fuses or splits them. Depth
// pre-pass and colour pass are different shaders computing the same position, and the colour pass tests EQUAL:
// any difference in fusing leaves holes (foliage flickered black). GLSL's `precise gl_Position` only covers what
// glslang traces back from the position, which the translated register code defeats.
static bool float_arith(const uint32_t *w, uint32_t wc, uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136:  // FNegate FAdd FSub FMul FDiv
    case 142: case 143: case 144: case 145: case 146: case 148:  // vector/matrix products, Dot
        return true;
    case 12: return wc >= 5 && w[4] == 50;  // ExtInst GLSL.std.450 Fma
    default: return false;
    }
}

static uint32_t *no_contraction(uint32_t *spv, size_t *nwords) {
    enum { OP_DECORATE = 71, DEC_NO_CONTRACTION = 42 };
    size_t n = *nwords, insert = 0, count = 0;
    uint32_t bound = spv[3];
    uint8_t *done = calloc(bound, 1);  // already decorated (glslang does it for what it traces from precise)
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (!insert && op >= 19 && op <= 39) insert = i;  // first type declaration: end of the annotations
        if (op == OP_DECORATE && wc >= 3 && spv[i + 2] == DEC_NO_CONTRACTION && spv[i + 1] < bound) done[spv[i + 1]] = 1;
        i += wc;
    }
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) count++;
        i += wc;
    }
    if (!insert || !count) { free(done); return spv; }
    uint32_t *out = malloc((n + count * 3) * 4);
    memcpy(out, spv, insert * 4);
    size_t o = insert;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) {
            done[spv[i + 2]] = 1;
            out[o++] = 3u << 16 | OP_DECORATE;
            out[o++] = spv[i + 2];  // result id
            out[o++] = DEC_NO_CONTRACTION;
        }
        i += wc;
    }
    memcpy(out + o, spv + insert, (n - insert) * 4);
    *nwords = o + (n - insert);
    free(done);
    free(spv);
    return out;
}

// SPIR-V words of src, or NULL (error logged with the source). *nwords set on success. Thread-safe: one shaderc
// compiler per thread (a shared one behind a lock serialised all the pipeline workers).
// No spirv-opt by default: it rewrote the depth pre-pass and colour-pass vertex shaders differently enough that
// their depths stopped matching (EQUAL test: black holes in foliage), and it was most of the compile time; the
// driver optimises anyway. LARECOMP_VK_OPT=1 turns it back on (experiments).
uint64_t vk_spirv_hits, vk_spirv_misses;  // [XCDE] T187
_Thread_local unsigned vk_spirv_tl_misses;
// [XCDE] T187: is this source's SPIR-V in the disk cache already (precompilation skips it)
bool vk_spirv_cached(const char *src, int stage) {
    char path[64];
    struct stat sb;
    if (pack_has(src_hash(src, stage))) return true;
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)src_hash(src, stage));
    return stat(path, &sb) == 0 && sb.st_size > 20;
}

uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords) {
    uint64_t h = src_hash(src, stage);
    static int nocache = -1;
    if (nocache < 0) nocache = getenv("LARECOMP_VK_NO_SPIRV_CACHE") != NULL;
    uint32_t *out = nocache ? NULL : cache_load(h, nwords);
    if (out) { __atomic_add_fetch(&vk_spirv_hits, 1, __ATOMIC_RELAXED); return out; }
    {   // [XCDE] T187: how many shaders the cache did not have (precompilation coverage); a line every 50 misses
        uint64_t m = __atomic_add_fetch(&vk_spirv_misses, 1, __ATOMIC_RELAXED);
        vk_spirv_tl_misses++;
        if (m % 50 == 0 && !getenv("XCDE_PRECOMPILING"))
            LOG("[shader] spirv cache: %llu hits, %llu compiled%c", (unsigned long long)vk_spirv_hits, (unsigned long long)m, 10);
    }
    static __thread shaderc_compiler_t comp;
    if (!comp) comp = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(o, getenv("LARECOMP_VK_OPT") ? shaderc_optimization_level_performance
                                                                                  : shaderc_optimization_level_zero);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, src, strlen(src), kind_of(stage), name, "main", o);
    shaderc_compile_options_release(o);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        static int nlog;
        if (nlog++ < 20) LOG("[shader] %s: GLSL compile failed:\n%s\n%s\n", name, shaderc_result_get_error_message(r), src);
    } else {
        size_t n = shaderc_result_get_length(r);
        out = malloc(n);
        memcpy(out, shaderc_result_get_bytes(r), n);
        *nwords = n / 4;
        if (stage == STAGE_VERTEX) out = no_contraction(out, nwords);
        if (!nocache) cache_store(h, out, *nwords);
    }
    shaderc_result_release(r);
    return out;
}

VkShaderModule vk_module(const uint32_t *spv, size_t nwords) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nwords * 4, .pCode = spv};
    VkShaderModule m;
    return vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name) {
    size_t n;
    uint32_t *spv = vk_glsl_to_spirv(src, stage, name, &n);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    return m;
}

// ================================================================== reflection
// Just enough SPIR-V parsing to list the descriptor bindings a module declares (set, binding, type).
enum { OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29,
       OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71 };
enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_SET = 34 };
enum { SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_STORAGE_BUFFER = 12 };

int vk_reflect(const uint32_t *spv, size_t n, VkBindingInfo *out, int max) {
    if (n < 5 || spv[0] != 0x07230203) return 0;
    uint32_t bound = spv[3];
    int32_t *binding = malloc(bound * sizeof *binding), *set = malloc(bound * sizeof *set);
    uint8_t *flags = calloc(bound, 1);    // 1 Block, 2 BufferBlock
    uint32_t *op = calloc(bound, sizeof *op), *ref = calloc(bound, sizeof *ref), *sc = calloc(bound, sizeof *sc);
    uint8_t *img_storage = calloc(bound, 1);  // [XCDE] OpTypeImage with Sampled = 2: a storage image
    for (uint32_t i = 0; i < bound; i++) binding[i] = set[i] = -1;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, o = spv[i] & 0xFFFF;
        if (!wc) break;
        const uint32_t *w = spv + i;
        if (o == OP_DECORATE && wc >= 3 && w[1] < bound) {
            if (w[2] == DEC_BINDING && wc >= 4) binding[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_SET && wc >= 4) set[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_BLOCK) flags[w[1]] |= 1;
            else if (w[2] == DEC_BUFFER_BLOCK) flags[w[1]] |= 2;
        } else if ((o == OP_TYPE_IMAGE || o == OP_TYPE_SAMPLER || o == OP_TYPE_SAMPLED_IMAGE || o == OP_TYPE_STRUCT) && wc >= 2 && w[1] < bound) {
            op[w[1]] = o;
            if (o == OP_TYPE_IMAGE && wc >= 8 && w[7] == 2) img_storage[w[1]] = 1;
        } else if ((o == OP_TYPE_ARRAY || o == OP_TYPE_RUNTIME_ARRAY) && wc >= 3 && w[1] < bound) {
            op[w[1]] = o;
            ref[w[1]] = w[2];
        } else if (o == OP_TYPE_POINTER && wc >= 4 && w[1] < bound) {
            op[w[1]] = o;
            sc[w[1]] = w[2];
            ref[w[1]] = w[3];
        } else if (o == OP_VARIABLE && wc >= 4 && w[2] < bound && w[1] < bound) {
            op[w[2]] = o;
            ref[w[2]] = w[1];  // pointer type
            sc[w[2]] = w[3];
        }
        i += wc;
    }
    int cnt = 0;
    for (uint32_t id = 0; id < bound && cnt < max; id++) {
        if (op[id] != OP_VARIABLE || binding[id] < 0) continue;
        uint32_t t = ref[ref[id]];  // pointee
        while (t < bound && (op[t] == OP_TYPE_ARRAY || op[t] == OP_TYPE_RUNTIME_ARRAY)) t = ref[t];
        VkDescriptorType dt;
        if (t >= bound) continue;
        if (op[t] == OP_TYPE_SAMPLED_IMAGE) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (op[t] == OP_TYPE_IMAGE) dt = img_storage[t] ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else if (op[t] == OP_TYPE_SAMPLER) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (op[t] == OP_TYPE_STRUCT && (sc[id] == SC_STORAGE_BUFFER || (flags[t] & 2))) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (op[t] == OP_TYPE_STRUCT && sc[id] == SC_UNIFORM) dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else continue;
        out[cnt++] = (VkBindingInfo){(uint32_t)(set[id] < 0 ? 0 : set[id]), (uint32_t)binding[id], dt};
    }
    free(binding); free(set); free(flags); free(op); free(ref); free(sc); free(img_storage);
    return cnt;
}

VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding lb[96];
    uint32_t k = 0;
    for (int i = 0; i < n && k < 96; i++)
        if (b[i].set == set) lb[k++] = (VkDescriptorSetLayoutBinding){b[i].binding, b[i].type, 1, stages, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = k, .pBindings = lb};
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

// ================================================================== built-in shaders
static const char *builtin_src(const char *name, int *stage) {
    if (!strcmp(name, "present.vert")) {
        *stage = STAGE_VERTEX;
        // pv[0].x: flip (lower-left origin), pv[1]: uv crop (origin, size). Fullscreen triangle; Vulkan NDC y points down.
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; vec4 crop; } pc;\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "  vec2 u = vec2(p.x, pc.a.x > 0.5 ? 1.0 - p.y : p.y);\n"
               "  uv = pc.crop.xy + u * pc.crop.zw;\n"
               "}\n";
    }
    if (!strcmp(name, "present.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = vec4(texture(t, uv).rgb, 1.0); }\n";
    }
    // ---- post passes before the present (Video tab "Anti-aliasing" / "Upscaler"): exec.c vk_present_draw.
    // Each draws a fullscreen triangle into an image the size of its output; push constants: two vec4.
    // [XCDE] T213: the HUD put back over the upscaled scene. t0 = the upscaler's output (window size), t1 = the game
    // image with the HUD, t2 = the same image before the HUD (both render size, bilinear). What the HUD passes added
    // (t1 - t2: any colour, any transparency) is added to the upscaled scene; elsewhere the upscaled scene as it is.
    // [XCDE] T216: where the HUD changed the pixel (a difference above ~0.03) the game's own
    // image is taken instead (blending in from 1.5 / 255): up + d left the upscaler's sharpened copy minus the bilinear one of
    // whatever the HUD covered - in the main menu the field hints in the game's captured background showed as a text
    // outline over the character cards (user shot). Faint overlays still get the upscaled scene under them.
    // a.xy = 1 / window size.
    if (!strcmp(name, "tmp_hud.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D upTex;\n"
               "layout(set = 0, binding = 1) uniform sampler2D finalTex;\n"
               "layout(set = 0, binding = 2) uniform sampler2D sceneTex;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy * pc.a.xy;\n"
               "  vec3 up = texture(upTex, uv).rgb;\n"
               "  vec3 fin = texture(finalTex, uv).rgb;\n"
               "  vec3 d = fin - texture(sceneTex, uv).rgb;\n"
               "  float m = max(abs(d.r), max(abs(d.g), abs(d.b)));\n"
               // [XCDE] T220: the weight from the HUD change around the pixel (one render texel each way): the
               // anti-aliased edge of a letter took the upscaled scene under it, and FSR 3's sharp grass showed
               // through text edges (frayed letters on the title screen); DLSS's smoother output hid it
               "  vec2 rt = 1.0 / vec2(textureSize(finalTex, 0));\n"
               "  float mm = m;\n"
               "  for (int i = 0; i < 4; i++) {\n"
               "    vec2 q = uv + rt * vec2(i == 0 ? 1.0 : i == 1 ? -1.0 : 0.0, i == 2 ? 1.0 : i == 3 ? -1.0 : 0.0);\n"
               "    vec3 e = abs(texture(finalTex, q).rgb - texture(sceneTex, q).rgb);\n"
               "    mm = max(mm, max(e.r, max(e.g, e.b)));\n"
               "  }\n"
               "  float w = smoothstep(1.5 / 255.0, 0.03, mm);\n"
               "  o = vec4(mm > 1.5 / 255.0 ? mix(clamp(up + d, 0.0, 1.0), fin, w) : up, 1.0);\n"
               "}\n";
    }
    if (!strcmp(name, "post.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "}\n";
    }
    // SMAA 1x (third_party/smaa, runtime/nvn/smaa_data.c): the reference SMAA.hlsl in its GLSL mode, High preset,
    // with an entry point per pass. a = SMAA_RT_METRICS (1 / size, size). The vertex-stage offset helpers run per
    // pixel here. Luma edges (t0 = colour) -> blending weights (t0 = edges, t1 = area, t2 = search) -> neighbourhood
    // blending (t0 = colour, t1 = weights). Vulkan's texture origin is top-left, as SMAA expects (no flips).
    if (!strncmp(name, "smaa_", 5)) {
        *stage = STAGE_FRAGMENT;
        static char *src[3];
        int k = !strcmp(name, "smaa_edge.frag") ? 0 : !strcmp(name, "smaa_weight.frag") ? 1 : !strcmp(name, "smaa_blend.frag") ? 2 : -1;
        if (k < 0) return NULL;
        if (!src[k]) {
            extern const char smaa_hlsl_src[];
            static const char *head =
                "#version 450\n"
                "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
                "#define SMAA_RT_METRICS pc.a\n"
                "#define SMAA_GLSL_4\n"
                "#define SMAA_PRESET_HIGH\n";
            static const char *mains[3] = {
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off[3];\n"
                "  SMAAEdgeDetectionVS(uv, off);\n"
                "  o = vec4(SMAALumaEdgeDetectionPS(uv, off, t0), 0.0, 0.0);\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(set = 0, binding = 2) uniform sampler2D t2;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy, pix;\n"
                "  vec4 off[3];\n"
                "  SMAABlendingWeightCalculationVS(uv, pix, off);\n"
                "  o = SMAABlendingWeightCalculationPS(uv, pix, off, t0, t1, t2, vec4(0.0));\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off;\n"
                "  SMAANeighborhoodBlendingVS(uv, off);\n"
                "  o = vec4(SMAANeighborhoodBlendingPS(uv, off, t0, t1).rgb, 1.0);\n"
                "}\n"};
            size_t n = strlen(head) + strlen(smaa_hlsl_src) + strlen(mains[k]) + 1;
            src[k] = malloc(n);
            snprintf(src[k], n, "%s%s%s", head, smaa_hlsl_src, mains[k]);
        }
        return src[k];
    }
    // AMD FidelityFX Super Resolution 1 (MIT), EASU: edge-adaptive upscale. a = (in / out scale xy, offset zw),
    // b.xy = input size. 12 taps around the source pixel, direction / edge length from luma, Lanczos-2-like
    // weights stretched along the edge, clamped to the nearest 4 (no ringing).
    if (!strcmp(name, "easu.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void setf(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
               "  float dx = lD - lB, dy = lE - lA;\n"
               "  float lx = clamp(abs(dx) / max(max(abs(lD - lC), abs(lC - lB)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  float ly = clamp(abs(dy) / max(max(abs(lE - lC), abs(lC - lA)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  dir += vec2(dx, dy) * w;\n"
               "  len += (lx * lx + ly * ly) * w;\n"
               "}\n"
               "void tapf(inout vec3 ac, inout float aw, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {\n"
               "  vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len2;\n"
               "  float d2 = min(dot(v, v), clp);\n"
               "  float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;\n"
               "  wb *= wb; wa *= wa;\n"
               "  float w = (1.5625 * wb - 0.5625) * wa;\n"
               "  ac += c * w; aw += w;\n"
               "}\n"
               "void main() {\n"
               "  vec2 pp = floor(gl_FragCoord.xy) * pc.a.xy + pc.a.zw;\n"
               "  vec2 fpf = floor(pp); pp -= fpf;\n"
               "  ivec2 fp = ivec2(fpf);\n"
               "  //    b c\n"
               "  //  e f g h\n"
               "  //  i j k l\n"
               "  //    n o\n"
               "  vec3 bC = tap(fp + ivec2(0, -1)), cC = tap(fp + ivec2(1, -1));\n"
               "  vec3 eC = tap(fp + ivec2(-1, 0)), fC = tap(fp), gC = tap(fp + ivec2(1, 0)), hC = tap(fp + ivec2(2, 0));\n"
               "  vec3 iC = tap(fp + ivec2(-1, 1)), jC = tap(fp + ivec2(0, 1)), kC = tap(fp + ivec2(1, 1)), lC = tap(fp + ivec2(2, 1));\n"
               "  vec3 nC = tap(fp + ivec2(0, 2)), oC = tap(fp + ivec2(1, 2));\n"
               "  float bL = lum(bC), cL = lum(cC), eL = lum(eC), fL = lum(fC), gL = lum(gC), hL = lum(hC);\n"
               "  float iL = lum(iC), jL = lum(jC), kL = lum(kC), lL = lum(lC), nL = lum(nC), oL = lum(oC);\n"
               "  vec2 dir = vec2(0.0);\n"
               "  float len = 0.0;\n"
               "  setf(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
               "  setf(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
               "  setf(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
               "  setf(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
               "  float dr = dot(dir, dir);\n"
               "  bool zro = dr < 1.0 / 32768.0;\n"
               "  dir = zro ? vec2(1.0, 0.0) : dir * inversesqrt(dr);\n"
               "  len = len * 0.5; len *= len;\n"
               "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
               "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
               "  float lob = 0.5 - 0.29 * len, clp = 1.0 / lob;\n"
               "  vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));\n"
               "  vec3 ac = vec3(0.0);\n"
               "  float aw = 0.0;\n"
               "  tapf(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);\n"
               "  tapf(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);\n"
               "  tapf(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);\n"
               "  tapf(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);\n"
               "  tapf(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);\n"
               "  tapf(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);\n"
               "  tapf(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);\n"
               "  tapf(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);\n"
               "  tapf(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);\n"
               "  tapf(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);\n"
               "  tapf(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);\n"
               "  tapf(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);\n"
               "  o = vec4(min(mx, max(mn, ac / aw)), 1.0);\n"
               "}\n";
    }
    // FSR 1 RCAS: contrast-adaptive sharpening of the EASU output (same size). a.x = sharpness (exp2(-stops)),
    // b.xy = image size. The lobe is limited so the result stays inside the neighbours' range; noise is damped.
    if (!strcmp(name, "rcas.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void main() {\n"
               "  ivec2 ip = ivec2(gl_FragCoord.xy);\n"
               "  //    b\n"
               "  //  d e f\n"
               "  //    h\n"
               "  vec3 b = tap(ip + ivec2(0, -1)), d = tap(ip + ivec2(-1, 0)), e = tap(ip), f = tap(ip + ivec2(1, 0)), h = tap(ip + ivec2(0, 1));\n"
               "  float bL = lum(b), dL = lum(d), eL = lum(e), fL = lum(f), hL = lum(h);\n"
               "  float nz = 0.25 * (bL + dL + fL + hL) - eL;\n"
               "  float rng = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);\n"
               "  nz = 1.0 - 0.5 * clamp(abs(nz) / max(rng, 1.0 / 65536.0), 0.0, 1.0);\n"
               "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
               "  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));\n"
               "  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));\n"
               "  vec3 lobeRGB = max(-hitMin, hitMax);\n"
               "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.a.x * nz;\n"
               "  o = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1.0);\n"
               "}\n";
    }
    if (!strcmp(name, "clear.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, pc.d.x, 1.0);\n"
               "}\n";
    }
    // clear.frag.<f|i|u><index>: writes the push-constant colour to colour target <index>
    if (!strncmp(name, "clear.frag.", 11)) {
        *stage = STAGE_FRAGMENT;
        static __thread char buf[512];
        char k = name[11];
        int idx = atoi(name + 12);
        const char *ty = k == 'i' ? "ivec4" : k == 'u' ? "uvec4" : "vec4";
        const char *cv = k == 'i' ? "floatBitsToInt(pc.c)" : k == 'u' ? "floatBitsToUint(pc.c)" : "pc.c";
        snprintf(buf, sizeof buf,
                 "#version 450\n"
                 "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
                 "layout(location = %d) out %s o;\n"
                 "void main() { o = %s; }\n", idx, ty, cv);
        return buf;
    }
    if (!strcmp(name, "clear.frag.none")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\nvoid main() {}\n";
    }
    // [XCDE] temporal upscalers' inputs (temporal.c), at render size: the game image (raw bytes), the scene depth and
    // camera motion vectors. m = previous view-projection x inverse(current one) (clip from this frame's NDC);
    // a = (jitter x, y in render pixels, render width, height); b.x = NDC z in [-1, 1] (NVN depth mode), b.y = a
    // previous camera exists. Motion: previous minus current position in UV units (the upscalers scale them to pixels),
    // from the unjittered pixel position; image rows top-down, NDC y up.
    if (!strcmp(name, "tmp_prep.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n"
               "layout(set = 0, binding = 1) uniform sampler2D depthTex;\n"
               "layout(set = 0, binding = 2) uniform sampler2D sceneTex;\n"  // [XCDE] the game image before the HUD
               "layout(set = 0, binding = 3) uniform sampler2D velTex;\n"  // [XCDE] T215: the game's velocity buffer
               "layout(push_constant) uniform P { mat4 m; vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 oColor;\n"
               "layout(location = 1) out vec2 oMv;\n"
               "layout(location = 2) out float oDepth;\n"
               "layout(location = 3) out float oHud;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  ivec2 csz = textureSize(colorTex, 0), dsz = textureSize(depthTex, 0);\n"
               "  ivec2 cp = min(ivec2(vec2(p) * vec2(csz) / pc.a.zw), csz - 1);\n"
               "  vec3 c = texelFetch(colorTex, cp, 0).rgb;\n"
               "  oColor = vec4(c, 1.0);\n"
               "  vec3 dh = abs(c - texelFetch(sceneTex, min(cp, textureSize(sceneTex, 0) - 1), 0).rgb);\n"
               "  oHud = max(dh.r, max(dh.g, dh.b)) > 1.5 / 255.0 ? 1.0 : 0.0;\n"  // (pixels the HUD passes changed)
               "  float d = texelFetch(depthTex, min(ivec2(vec2(p) * vec2(dsz) / pc.a.zw), dsz - 1), 0).r;\n"
               "  oDepth = d;\n"
               "  vec2 uv = (gl_FragCoord.xy - pc.a.xy) / pc.a.zw;\n"
               "  vec4 ndc = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, pc.b.x > 0.5 ? d * 2.0 - 1.0 : d, 1.0);\n"
               "  vec4 q = pc.m * ndc;\n"
               "  vec2 puv = vec2(q.x / q.w * 0.5 + 0.5, 0.5 - q.y / q.w * 0.5);\n"
               "  oMv = pc.b.y > 0.5 && abs(q.w) > 1e-7 ? puv - uv : vec2(0.0);\n"
               // [XCDE] T215: where the scene has geometry, the game's own velocity (per object: characters, animation)
               // RGB10A2: R = sqrt|v.x|, G = sqrt|v.y|, alpha bit 0 = x negative, bit 1 = y positive (UV units, previous
               // minus current, as ours - measured against the camera motion on static scenery: XCDE_TMP_VEL_FIT)
               "  if (pc.b.z > 0.5 && d < 1.0) {\n"
               "    ivec2 vsz = textureSize(velTex, 0);\n"
               "    vec4 v = texelFetch(velTex, min(ivec2(vec2(p) * vec2(vsz) / pc.a.zw), vsz - 1), 0);\n"
               "    int ab = int(v.a * 3.0 + 0.5);\n"
               "    vec2 gv = vec2((ab & 1) != 0 ? -v.r * v.r : v.r * v.r, (ab & 2) != 0 ? v.g * v.g : -v.g * v.g);\n"
               // [XCDE] T228: the game's velocity is mostly the camera's motion (corr 0.993 with ours), stored in 10
               // bits as a square root: our float camera motion is the same thing, exact. Keep ours unless the game's
               // differs by more than its rounding (3 steps of sqrt-coded 10 bits) - a pixel with its own motion.
               "    vec2 step = 6.0 * vec2(v.r, v.g) / 1023.0 + 2.0 / (1023.0 * 1023.0);\n"
               "    oMv = pc.b.w > 0.5 && all(lessThanEqual(abs(gv - oMv), step)) ? oMv : gv;\n"
               "  }\n"
               "}\n";
    }
    return NULL;
}

VkShaderModule vk_builtin_shader(const char *name) {
    static struct { char name[32]; VkShaderModule m; } cache[64];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) return cache[i].m;
    int stage = 0;
    const char *src = builtin_src(name, &stage);
    if (!src) FATAL("no built-in shader %s\n", name);
    VkShaderModule m = vk_compile_glsl(src, stage, name);
    if (!m) FATAL("built-in shader %s failed to compile\n", name);
    if (n < 64) { snprintf(cache[n].name, sizeof cache[n].name, "%s", name); cache[n++].m = m; }
    return m;
}
