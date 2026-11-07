// [XCDE] LINUX-017: native Linux NGX Super Resolution/DLAA. Proprietary SDK
// headers/loader/runtime are pinned in ignored .deps, not copied into this source.
#include "dlss_linux.h"
#include "dlss_ngx_api.h"
#include <dlfcn.h>
static const MonadoNgxApi *api;
static bool valid_api(const MonadoNgxApi *table) {
    if (!table || table->abi_version != 1 || table->size != sizeof(MonadoNgxApi)) return false;
#define CHECK_POINTER(function) if (!table->function) return false;
    MONADO_NGX_FUNCTIONS(CHECK_POINTER)
#undef CHECK_POINTER
    return true;
}
#define NVSDK_NGX_VULKAN_Init_with_ProjectID api->NVSDK_NGX_VULKAN_Init_with_ProjectID
#define NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements api->NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements
#define NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements api->NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements
#define NVSDK_NGX_VULKAN_GetFeatureRequirements api->NVSDK_NGX_VULKAN_GetFeatureRequirements
#define NVSDK_NGX_VULKAN_GetCapabilityParameters api->NVSDK_NGX_VULKAN_GetCapabilityParameters
#define NVSDK_NGX_VULKAN_AllocateParameters api->NVSDK_NGX_VULKAN_AllocateParameters
#define NVSDK_NGX_VULKAN_DestroyParameters api->NVSDK_NGX_VULKAN_DestroyParameters
#define NVSDK_NGX_VULKAN_CreateFeature1 api->NVSDK_NGX_VULKAN_CreateFeature1
#define NVSDK_NGX_VULKAN_CreateFeature api->NVSDK_NGX_VULKAN_CreateFeature
#define NVSDK_NGX_VULKAN_EvaluateFeature_C api->NVSDK_NGX_VULKAN_EvaluateFeature_C
#define NVSDK_NGX_VULKAN_ReleaseFeature api->NVSDK_NGX_VULKAN_ReleaseFeature
#define NVSDK_NGX_VULKAN_Shutdown1 api->NVSDK_NGX_VULKAN_Shutdown1
#define NVSDK_NGX_Parameter_SetUI api->NVSDK_NGX_Parameter_SetUI
#define NVSDK_NGX_Parameter_SetI api->NVSDK_NGX_Parameter_SetI
#define NVSDK_NGX_Parameter_SetF api->NVSDK_NGX_Parameter_SetF
#define NVSDK_NGX_Parameter_SetVoidPointer api->NVSDK_NGX_Parameter_SetVoidPointer
#include <nvsdk_ngx_helpers_vk.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <mutex>
#include <dirent.h>
#include <cerrno>
#include <cstddef>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>

static std::mutex lock;
static bool prepared, initialized, available;
static VkDevice device;
static void *loader;
static NVSDK_NGX_Parameter *caps;
static std::wstring feature_path, data_path;
static const wchar_t *search_paths[1];
static NVSDK_NGX_FeatureCommonInfo common;
static NVSDK_NGX_FeatureDiscoveryInfo discovery;
// Same project identity as the inherited custom-engine Windows integration.
static constexpr const char *project = "5b7c1d8e-6a4f-4e1d-9c3a-2f8e7d6b5a41";

static bool result(NVSDK_NGX_Result r, const char *operation) {
    if (NVSDK_NGX_SUCCEED(r)) return true;  // NGX success is 1, not 0.
    fprintf(stderr, "[dlss] %s failed: 0x%08x\n", operation, (unsigned)r);
    return false;
}

// Decode UTF-8 without changing the process/thread locale (paths may be non-ASCII).
static bool wide(const char *s, std::wstring &out) {
    out.clear();
    const auto *p = reinterpret_cast<const unsigned char *>(s);
    while (*p) {
        uint32_t c = *p++; unsigned n = 0; uint32_t minimum = 0;
        if (c < 0x80) {}
        else if (c >= 0xc2 && c <= 0xdf) { c &= 31; n = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { c &= 15; n = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { c &= 7; n = 3; minimum = 0x10000; }
        else return false;
        for (unsigned i = 0; i < n; i++) { if ((*p & 0xc0) != 0x80) return false; c = (c << 6) | (*p++ & 63); }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
        out.push_back(static_cast<wchar_t>(c));
    }
    return true;
}

static std::string json_string(const std::string &input) {
    std::string out = "\"";
    for (unsigned char c : input) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32) { char escape[7]; snprintf(escape, sizeof escape, "\\u%04x", c); out += escape; }
        else out += c;
    }
    return out + '"';
}
// NVIDIA 595.99.02 README/ngx.html documents this per-process config. Keep
// models private and explicitly disable updater launches; IP denial is separate.
static bool private_config(const char *root) {
    std::string models = std::string(root) + "/models";
    if (mkdir(models.c_str(), 0700) && errno != EEXIST) return false;
    struct stat st{};
    if (lstat(models.c_str(), &st) || !S_ISDIR(st.st_mode)) return false;
    std::string path = std::string(root) + "/nvidia-ngx-conf.json";
    std::string text = "{\"file_format_version\":\"1.0.0\",\"allow_ngx_updater\":false,\"ngx_models_path\":" + json_string(models) + "}\n";
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        size_t done = 0;
        while (done < text.size()) {
            ssize_t n = write(fd, text.data() + done, text.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(fd); return false; }
            done += (size_t)n;
        }
        if (close(fd)) return false;
    } else {
        if (errno != EEXIST) return false;
        fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return false;
        bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == (off_t)text.size();
        std::string old(text.size(), '\0');
        size_t done = 0;
        while (ok && done < old.size()) {
            ssize_t n = read(fd, &old[done], old.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = false; break; }
            done += (size_t)n;
        }
        close(fd);
        if (!ok || old != text) return false;  // preserve unfamiliar user data
    }
    return !setenv("__NGX_CONF_FILE", path.c_str(), 1);
}

// NGX's public interface has no documented offline switch. This offline game's
// provider therefore refuses inherited IP sockets and installs a process-wide,
// inherited seccomp policy BEFORE any SDK call. Unix sockets/driver IPC remain.
// io_uring creation is denied too: async socket creation must not bypass policy.
static bool offline() {
    DIR *fds = opendir("/proc/self/fd");
    if (!fds) return false;
    bool safe = true;
    while (dirent *e = readdir(fds)) {
        char *end; long fd = strtol(e->d_name, &end, 10);
        if (*end || fd < 0 || fd > INT32_MAX) continue;
        sockaddr_storage addr{}; socklen_t size = sizeof addr;
        if (!getsockname((int)fd, reinterpret_cast<sockaddr *>(&addr), &size) &&
            (addr.ss_family == AF_INET || addr.ss_family == AF_INET6)) safe = false;
    }
    closedir(fds);
    if (!safe) { fprintf(stderr, "[dlss] inherited IP socket: provider refused\n"); return false; }
    sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x40000000, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_io_uring_setup, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 5),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET6, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog filter{static_cast<unsigned short>(sizeof code / sizeof code[0]), code};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &filter)) {
        fprintf(stderr, "[dlss] cannot enforce offline policy: %s\n", strerror(errno)); return false;
    }
    fprintf(stderr, "[dlss] offline policy active (IP sockets and io_uring denied)\n");
    return true;
}

static void log_message(const char *message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature) {
    if (level != NVSDK_NGX_LOGGING_LEVEL_VERBOSE || getenv("XCDE_TMP_LOG")) fprintf(stderr, "[ngx] %s\n", message);
}

extern "C" bool ngx_linux_init(const char *feature_dir, const char *data_dir) {
    std::lock_guard<std::mutex> guard(lock);
    if (prepared || initialized || !feature_dir || !data_dir) return false;
    char *f = realpath(feature_dir, nullptr), *d = realpath(data_dir, nullptr);
    bool valid = f && d;
    struct stat st{};
    if (valid) {
        std::string lib = std::string(f) + "/libnvidia-ngx-dlss.so.310.9.1";
        std::string module = std::string(f) + "/libmonado-ngx.so";
        valid = !stat(module.c_str(), &st) && S_ISREG(st.st_mode) && !stat(lib.c_str(), &st) && S_ISREG(st.st_mode) && !stat(d, &st) && S_ISDIR(st.st_mode);
        if (valid) valid = wide(f, feature_path) && wide(d, data_path) && private_config(d);
    }
    std::string module = f ? std::string(f) + "/libmonado-ngx.so" : "";
    free(f); free(d);
    if (!valid) { fprintf(stderr, "[dlss] local SR runtime/private data directory unavailable\n"); return false; }
    if (!offline()) return false;
    loader = dlopen(module.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!loader) { fprintf(stderr, "[dlss] native loader module: %s\n", dlerror()); return false; }
    auto get_api = reinterpret_cast<const MonadoNgxApi *(*)()>(dlsym(loader, "monado_ngx_api"));
    if (!get_api || !valid_api(api = get_api())) {
        fprintf(stderr, "[dlss] loader ABI unavailable\n"); dlclose(loader); loader = nullptr; api = nullptr; return false;
    }
    search_paths[0] = feature_path.c_str();
    common = {};
    common.PathListInfo.Path = search_paths;
    common.PathListInfo.Length = 1;
    common.LoggingInfo.LoggingCallback = log_message;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    common.LoggingInfo.DisableOtherLoggingSinks = true;
    discovery = {};
    discovery.SDKVersion = NVSDK_NGX_Version_API;
    discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc = {project, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "monado-linux"};
    discovery.ApplicationDataPath = data_path.c_str();
    discovery.FeatureInfo = &common;
    prepared = true;
    return true;
}

// Keep returned extension strings stable even if NGX reuses its own query buffer.
static std::vector<std::string> instance_exts, device_exts;
extern "C" int ngx_linux_instance_exts(const char **out, int max) {
    std::lock_guard<std::mutex> guard(lock);
    if (!prepared || max < 0) return 0;
    uint32_t n = 0; VkExtensionProperties *exts = nullptr;
    if (!result(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery, &n, &exts), "instance requirements") || n > (unsigned)max) {
        prepared = false; return 0;
    }
    uint32_t supported = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &supported, nullptr) != VK_SUCCESS) { prepared = false; return 0; }
    std::vector<VkExtensionProperties> have(supported);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &supported, have.data()) != VK_SUCCESS) { prepared = false; return 0; }
    instance_exts.clear();
    for (uint32_t i = 0; i < n; i++) {
        if (std::none_of(have.begin(), have.end(), [&](const auto &e) { return !strcmp(e.extensionName, exts[i].extensionName); })) {
            fprintf(stderr, "[dlss] missing instance extension %s\n", exts[i].extensionName); prepared = false; return 0;
        }
        instance_exts.emplace_back(exts[i].extensionName);
    }
    for (uint32_t i = 0; i < n; i++) out[i] = instance_exts[i].c_str();
    return (int)n;
}

extern "C" bool ngx_linux_device_exts(VkInstance inst, VkPhysicalDevice phys, const char **out, uint32_t *n, uint32_t max) {
    std::lock_guard<std::mutex> guard(lock);
    if (!prepared || !n || *n > max) return false;
    NVSDK_NGX_FeatureRequirement requirement{};
    if (!result(NVSDK_NGX_VULKAN_GetFeatureRequirements(inst, phys, &discovery, &requirement), "feature requirements") || requirement.FeatureSupported) {
        fprintf(stderr, "[dlss] SR unsupported (mask 0x%x)\n", (unsigned)requirement.FeatureSupported); prepared = false; return false;
    }
    uint32_t count = 0; VkExtensionProperties *exts = nullptr;
    if (!result(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(inst, phys, &discovery, &count, &exts), "device requirements")) { prepared = false; return false; }
    uint32_t supported = 0;
    if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &supported, nullptr) != VK_SUCCESS) { prepared = false; return false; }
    std::vector<VkExtensionProperties> have(supported);
    if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &supported, have.data()) != VK_SUCCESS) { prepared = false; return false; }
    device_exts.clear();
    for (uint32_t i = 0; i < count; i++) {
        if (std::none_of(have.begin(), have.end(), [&](const auto &e) { return !strcmp(e.extensionName, exts[i].extensionName); })) { prepared = false; return false; }
        bool duplicate = false;
        for (uint32_t j = 0; j < *n; j++) duplicate |= !strcmp(out[j], exts[i].extensionName);
        if (!duplicate) device_exts.emplace_back(exts[i].extensionName);
    }
    if (*n + device_exts.size() > max) { prepared = false; return false; }
    for (const auto &ext : device_exts) out[(*n)++] = ext.c_str();
    return true;
}

extern "C" void ngx_linux_device_created(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t) {
    std::lock_guard<std::mutex> guard(lock);
    if (!prepared || initialized) return;
    if (!result(NVSDK_NGX_VULKAN_Init_with_ProjectID(project, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "monado-linux", data_path.c_str(), inst, phys, dev,
        vkGetInstanceProcAddr, vkGetDeviceProcAddr, &common, NVSDK_NGX_Version_API), "initialize")) return;
    initialized = true; device = dev;
    if (!result(NVSDK_NGX_VULKAN_GetCapabilityParameters(&caps), "capabilities")) return;
    int supported = 0, driver = 0;
    caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
    caps->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &driver);
    available = supported != 0 && !driver;
    fprintf(stderr, "[dlss] native SR/DLAA %s (driver update required=%d)\n", available ? "available" : "unavailable", driver);
}
extern "C" bool ngx_linux_available(void) { std::lock_guard<std::mutex> guard(lock); return available; }
static NVSDK_NGX_PerfQuality_Value quality_mode(int quality) {
    static const NVSDK_NGX_PerfQuality_Value modes[] = {NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
        NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    return modes[quality];
}
extern "C" bool ngx_linux_optimal(uint32_t ow, uint32_t oh, int quality, uint32_t *rw, uint32_t *rh) {
    std::lock_guard<std::mutex> guard(lock);
    if (!available || !rw || !rh || quality < 0 || quality > 4 || ow < 32 || oh < 32) return false;
    void *callback = nullptr;
    if (NVSDK_NGX_FAILED(caps->Get(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback)) || !callback) return false;
    caps->Set(NVSDK_NGX_Parameter_Width, ow);
    caps->Set(NVSDK_NGX_Parameter_Height, oh);
    caps->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int)quality_mode(quality));
    caps->Set(NVSDK_NGX_Parameter_RTXValue, 0);
    auto optimal = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(callback);
    return result(optimal(caps), "optimal settings") && NVSDK_NGX_SUCCEED(caps->Get(NVSDK_NGX_Parameter_OutWidth, rw)) &&
        NVSDK_NGX_SUCCEED(caps->Get(NVSDK_NGX_Parameter_OutHeight, rh)) && *rw && *rh;
}

struct Feature {
    NVSDK_NGX_Handle *handle{};
    NVSDK_NGX_Parameter *params{};
    uint32_t rw{}, rh{}, ow{}, oh{};
    int quality{};
    uint64_t last_seq{};
};
static Feature current;
static std::vector<Feature> retired;
static void release(Feature &f) {
    if (f.handle) result(NVSDK_NGX_VULKAN_ReleaseFeature(f.handle), "release");
    if (f.params) result(NVSDK_NGX_VULKAN_DestroyParameters(f.params), "destroy parameters");
    f = {};
}
static NVSDK_NGX_Resource_VK resource(const DlssImage &im, bool write) {
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return NVSDK_NGX_Create_ImageView_Resource_VK(im.view, im.image, range, im.format, im.width, im.height, write);
}
extern "C" bool ngx_linux_evaluate(VkCommandBuffer cb, const DlssFrame *f) {
    std::lock_guard<std::mutex> guard(lock);
    if (!available || !cb || !f || f->quality < 0 || f->quality > 4 || !std::isfinite(f->jitter[0]) || !std::isfinite(f->jitter[1]) ||
        !f->recording_seq || f->completed_seq >= f->recording_seq) return false;
    const auto &c = f->color, &m = f->motion, &d = f->depth, &o = f->output;
    if (!c.image || !c.view || !m.image || !m.view || !d.image || !d.view || !o.image || !o.view || !c.width || !c.height ||
        m.width != c.width || m.height != c.height || d.width != c.width || d.height != c.height || o.width < 32 || o.height < 32 ||
        c.width > o.width || c.height > o.height || o.width > 16384 || o.height > 16384 ||
        (f->quality == 0 && (c.width != o.width || c.height != o.height)) || c.format != VK_FORMAT_R8G8B8A8_UNORM || o.format != VK_FORMAT_R8G8B8A8_UNORM ||
        m.format != VK_FORMAT_R16G16_SFLOAT || d.format != VK_FORMAT_R32_SFLOAT) return false;
    if (f->recording_seq < current.last_seq) return false;
    for (auto i = retired.begin(); i != retired.end();) {
        if (i->last_seq <= f->completed_seq) { release(*i); i = retired.erase(i); } else ++i;
    }
    bool recreated = !current.handle || current.rw != c.width || current.rh != c.height || current.ow != o.width || current.oh != o.height || current.quality != f->quality;
    if (recreated) {
        if (current.params || current.handle) { retired.push_back(current); current = {}; }
        // Bound transient resources if a caller records unlimited resizes without submitting.
        if (retired.size() >= 16) { fprintf(stderr, "[dlss] pending feature retirement limit; spatial fallback\n"); return false; }
        if (!result(NVSDK_NGX_VULKAN_AllocateParameters(&current.params), "allocate parameters")) return false;
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = c.width; create.Feature.InHeight = c.height;
        create.Feature.InTargetWidth = o.width; create.Feature.InTargetHeight = o.height;
        create.Feature.InPerfQualityValue = quality_mode(f->quality);
        // LDR gamma input, conventional depth, low-resolution unjittered motion.
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        current.last_seq = f->recording_seq;
        if (!result(NGX_VULKAN_CREATE_DLSS_EXT1(device, cb, 1, 1, &current.handle, current.params, &create), "create SR/DLAA")) {
            retired.push_back(current); current = {}; return false;
        }
        current.rw = c.width; current.rh = c.height; current.ow = o.width; current.oh = o.height; current.quality = f->quality;
        fprintf(stderr, "[dlss] SR/DLAA created: %ux%u -> %ux%u, quality=%d, requested-runtime=310.9.1\n", c.width, c.height, o.width, o.height, f->quality);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    // Direct NGX has no resource-layout field. Its sampled inputs use read-only
    // layout; restore the renderer's GENERAL invariant even after failed evaluation.
    VkImageMemoryBarrier layouts[3]{};
    const DlssImage *inputs[] = {&c, &m, &d};
    for (unsigned i = 0; i < 3; i++) {
        layouts[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        layouts[i].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        layouts[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        layouts[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        layouts[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        layouts[i].srcQueueFamilyIndex = layouts[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        layouts[i].image = inputs[i]->image;
        layouts[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3, layouts);
    auto color = resource(c, false), motion = resource(m, false), depth = resource(d, false), output = resource(o, true);
    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &color; eval.Feature.pInOutput = &output;
    eval.pInDepth = &depth; eval.pInMotionVectors = &motion;
    eval.InJitterOffsetX = f->jitter[0]; eval.InJitterOffsetY = f->jitter[1];
    eval.InRenderSubrectDimensions = {c.width, c.height};
    eval.InReset = f->reset || recreated;
    // tmp_prep stores previous-minus-current UV offsets; NGX expects pixels.
    eval.InMVScaleX = (float)c.width; eval.InMVScaleY = (float)c.height;
    eval.InPreExposure = eval.InExposureScale = 1.0f;
    current.last_seq = f->recording_seq;
    bool ok = result(NGX_VULKAN_EVALUATE_DLSS_EXT(cb, current.handle, current.params, &eval), "evaluate SR/DLAA");
    for (auto &layout : layouts) {
        std::swap(layout.oldLayout, layout.newLayout);
        layout.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        layout.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
    VkMemoryBarrier output_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &output_barrier, 0, nullptr, 3, layouts);
    if (!ok) { retired.push_back(current); current = {}; }
    return ok;
}
static void reset_locked() {
    release(current);
    for (auto &f : retired) release(f);
    retired.clear();
}
extern "C" void ngx_linux_reset(void) { std::lock_guard<std::mutex> guard(lock); reset_locked(); }
extern "C" void ngx_linux_shutdown(void) {
    std::lock_guard<std::mutex> guard(lock);
    reset_locked();
    if (caps) { result(NVSDK_NGX_VULKAN_DestroyParameters(caps), "destroy capabilities"); caps = nullptr; }
    if (initialized) result(NVSDK_NGX_VULKAN_Shutdown1(device), "shutdown");
    available = initialized = prepared = false; device = VK_NULL_HANDLE;
    if (loader) dlclose(loader);
    loader = nullptr; api = nullptr;
}
