// [XCDE] LINUX-020: checked native FidelityFX recording; no presenter, queue thread, or guest-clock changes.
#include "framegen_linux.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <new>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

// The inherited public headers use a Windows export annotation on declarations.
// Their structures/function signatures are unchanged; this TU calls only loaded pointers.
#define __declspec(x)
#include "ffx_api/ffx_framegeneration.h"
#include "ffx_api/vk/ffx_api_vk.h"
#undef __declspec

static constexpr const char *sdk_commit = "c6efa6bf7f2027b3ec94f28578bb5965eabb9e55";
// Official pinned provider GetId(): FG tag plus FFX_SDK_MAKE_VERSION(1, 1, 3).
static constexpr uint64_t provider_id = UINT64_C(0xf600000000401003);
static constexpr unsigned max_contexts = 8;
struct FgContext {
    ffxContext handle = nullptr;
    ffxCreateBackendVKDesc backend{};
    ffxOverrideVersion version{};
    ffxCreateContextDescFrameGeneration description{};
    uint64_t last_seq = 0;
    FgContext *next = nullptr;
};
struct FgLinux {
    void *module = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr proc = nullptr;
    uint64_t provider_version = 0, completed = 0, last_frame = 0, last_seq = 0;
    uint32_t rw = 0, rh = 0;
    bool requested = false, faulted = false, history = false;
    FgContext *active = nullptr, *retired = nullptr;
    FgLinux *proc_next = nullptr;
    FgLinuxStats counters{};
    char error[256]{};
};
// SDK 1.1.4 requests promoted Vulkan entry points under their KHR names. Keep
// the caller's resolver/proxies, then try the core name. The registry is keyed
// by the actual device, not a process-global "current" context or thread-local
// creation shortcut, and remains live until every SDK context is destroyed.
static std::mutex proc_lock;
static FgLinux *proc_registry;
static PFN_vkVoidFunction VKAPI_PTR provider_proc(VkDevice device, const char *name) {
    if (!device || !name) return nullptr;
    PFN_vkGetDeviceProcAddr resolve = nullptr;
    {
        std::lock_guard<std::mutex> guard(proc_lock);
        for (FgLinux *entry = proc_registry; entry; entry = entry->proc_next)
            if (entry->device == device) { resolve = entry->proc; break; }
    }
    if (!resolve) return nullptr;
    PFN_vkVoidFunction result = resolve(device, name);
    if (result) return result;
    size_t length = strlen(name);
    if (length > 3 && length < 128 && !strcmp(name + length - 3, "KHR")) {
        char core[128]; memcpy(core, name, length - 3); core[length - 3] = 0;
        result = resolve(device, core);
    }
    return result;
}
static bool register_proc(FgLinux *fg) {
    std::lock_guard<std::mutex> guard(proc_lock);
    for (FgLinux *entry = proc_registry; entry; entry = entry->proc_next)
        if (entry->device == fg->device && entry->proc != fg->proc) return false;
    fg->proc_next = proc_registry; proc_registry = fg; return true;
}
static void unregister_proc(FgLinux *fg) {
    std::lock_guard<std::mutex> guard(proc_lock);
    for (FgLinux **entry = &proc_registry; *entry; entry = &(*entry)->proc_next)
        if (*entry == fg) { *entry = fg->proc_next; return; }
}
static void copy_error(char *out, size_t size, const char *text) { if (out && size) snprintf(out, size, "%s", text); }
static FgLinuxResult fail(FgLinux *fg, const char *message) {
    if (!fg->faulted) fg->counters.failures++;
    fg->faulted = true; fg->history = false;
    copy_error(fg->error, sizeof fg->error, message);
    return FG_LINUX_ERROR;
}
static bool destroy_context(FgLinux *fg, FgContext *context) {
    if (context->handle && fg->destroy(&context->handle, nullptr) != FFX_API_RETURN_OK) {
        fail(fg, "FidelityFX context destruction failed; context and module retained");
        return false;
    }
    delete context; return true;
}

extern "C" FgLinux *fg_linux_open(const char *path, VkPhysicalDevice physical, VkDevice device,
                                  PFN_vkGetDeviceProcAddr proc, char *error, size_t size) {
    copy_error(error, size, "");
    if (!path || path[0] != '/' || !physical || !device || !proc) {
        copy_error(error, size, "Frame-generation provider requires an explicit module and valid Vulkan device"); return nullptr;
    }
    char canonical[4096]; struct stat before{}, after{};
    if (!realpath(path, canonical) || strcmp(path, canonical)) {
        copy_error(error, size, "Frame-generation module path must be canonical"); return nullptr;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_nlink != 1) {
        if (fd >= 0) close(fd);
        copy_error(error, size, "Frame-generation module must be an ordinary single-link file"); return nullptr;
    }
    void *module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    bool same = stat(path, &after) == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
                before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
    close(fd);
    if (!module || !same) {
        if (module) dlclose(module);
        copy_error(error, size, "Cannot load the verified frame-generation module"); return nullptr;
    }
    auto abi = reinterpret_cast<uint32_t (*)()>(dlsym(module, "monado_ffx_abi_version"));
    auto commit = reinterpret_cast<const char *(*)()>(dlsym(module, "monado_ffx_sdk_commit"));
    FgLinux *fg = new (std::nothrow) FgLinux;
    if (!fg) { dlclose(module); copy_error(error, size, "Cannot allocate frame-generation provider state"); return nullptr; }
    fg->module = module; fg->physical = physical; fg->device = device; fg->proc = proc;
#define LOAD(member, name) fg->member = reinterpret_cast<decltype(fg->member)>(dlsym(module, name))
    LOAD(create, "ffxCreateContext"); LOAD(destroy, "ffxDestroyContext"); LOAD(configure, "ffxConfigure");
    LOAD(query, "ffxQuery"); LOAD(dispatch, "ffxDispatch");
#undef LOAD
    if (!abi || !commit || abi() != 1 || !commit() || strcmp(commit(), sdk_commit) ||
        !fg->create || !fg->destroy || !fg->configure || !fg->query || !fg->dispatch) {
        dlclose(module); delete fg; copy_error(error, size, "Frame-generation module ABI/source identity or required exports mismatch"); return nullptr;
    }
    uint64_t count = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    versions.outputCount = &count; versions.device = reinterpret_cast<void *>(device);
    if (fg->query(nullptr, &versions.header) != FFX_API_RETURN_OK || !count || count > 16) {
        dlclose(module); delete fg; copy_error(error, size, "Frame-generation provider version query failed"); return nullptr;
    }
    uint64_t ids[16]{}; const char *names[16]{};
    versions.versionIds = ids; versions.versionNames = names;
    if (fg->query(nullptr, &versions.header) != FFX_API_RETURN_OK || !count || count > 16 ||
        ids[0] != provider_id || !names[0] || strcmp(names[0], "1.1.3")) {
        dlclose(module); delete fg; copy_error(error, size, "Frame-generation provider returned an invalid version"); return nullptr;
    }
    // The module and the selected effect provider are pinned independently.
    fg->provider_version = ids[0];
    if (!register_proc(fg)) {
        dlclose(module); delete fg; copy_error(error, size, "Conflicting frame-generation Vulkan resolver for the same device"); return nullptr;
    }
    return fg;
}
extern "C" bool fg_linux_available(const FgLinux *fg) { return fg && fg->module && fg->provider_version; }
extern "C" void fg_linux_set_enabled(FgLinux *fg, bool enabled) {
    if (!fg) return;
    if (enabled && !fg->requested) { fg->faulted = false; fg->error[0] = 0; fg->history = false; }
    if (!enabled) fg->history = false;
    fg->requested = enabled;  // A failure stays latched until an explicit off/on transition.
}
extern "C" void fg_linux_reset(FgLinux *fg) { if (fg) fg->history = false; }
extern "C" void fg_linux_collect(FgLinux *fg, uint64_t completed) {
    if (!fg || completed < fg->completed) return;
    fg->completed = completed;
    for (FgContext **link = &fg->retired; *link;) {
        FgContext *context = *link;
        if (context->last_seq <= completed) {
            FgContext *next = context->next;
            if (!destroy_context(fg, context)) return;
            *link = next; fg->counters.live_contexts--;
        } else link = &context->next;
    }
}
static bool color_format(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}
static bool valid_image(const FgLinuxImage &image) {
    return image.image && image.width && image.height && image.width <= 16384 && image.height <= 16384;
}
static FfxApiResource resource(const FgLinuxImage &image, uint32_t usage) {
    FfxApiResource result{};
    if (!image.image) return result;
    result.resource = reinterpret_cast<void *>(image.image);
    result.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    result.description.format = ffxApiGetSurfaceFormatVK(image.format);
    result.description.width = image.width; result.description.height = image.height;
    result.description.depth = result.description.mipCount = 1; result.description.usage = usage;
    result.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;  // Caller contract: GENERAL layout.
    return result;
}
static bool context_for(FgLinux *fg, const FgLinuxFrame &frame) {
    const uint32_t width = frame.output.width, height = frame.output.height;
    const uint32_t render_w = std::max(width, frame.depth.width), render_h = std::max(height, frame.depth.height);
    uint32_t flags = frame.depth_infinite ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0;
    if (frame.depth_inverted) flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    if (frame.scene.format == VK_FORMAT_R16G16B16A16_SFLOAT) flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
    FgContext *old = fg->active;
    if (old && old->description.displaySize.width == width && old->description.displaySize.height == height &&
        old->description.maxRenderSize.width == render_w && old->description.maxRenderSize.height == render_h &&
        old->description.flags == flags && old->description.backBufferFormat == ffxApiGetSurfaceFormatVK(frame.output.format)) return true;
    if (fg->counters.live_contexts >= max_contexts) { fail(fg, "Frame-generation context retirement is waiting for GPU completion"); return false; }
    FgContext *context = new (std::nothrow) FgContext;
    if (!context) { fail(fg, "Cannot allocate frame-generation context descriptors"); return false; }
    context->backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    context->backend.vkDevice = fg->device; context->backend.vkPhysicalDevice = fg->physical; context->backend.vkDeviceProcAddr = provider_proc;
    context->version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    context->version.header.pNext = &context->backend.header; context->version.versionId = fg->provider_version;
    context->description.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    context->description.header.pNext = &context->version.header;
    context->description.flags = flags; context->description.displaySize = {width, height};
    context->description.maxRenderSize = {render_w, render_h};
    context->description.backBufferFormat = ffxApiGetSurfaceFormatVK(frame.output.format);
    if (fg->create(&context->handle, &context->description.header, nullptr) != FFX_API_RETURN_OK || !context->handle) {
        if (context->handle) {
            context->next = fg->retired; fg->retired = context; fg->counters.live_contexts++;
        } else delete context;
        fail(fg, "FidelityFX frame-generation context creation failed"); return false;
    }
    fg->counters.live_contexts++; fg->active = context; fg->history = false;
    if (old) { old->next = fg->retired; fg->retired = old; }
    fg_linux_collect(fg, fg->completed);
    return !fg->faulted;
}
extern "C" FgLinuxResult fg_linux_dispatch(FgLinux *fg, const FgLinuxFrame *input) {
    if (!fg || !fg->requested) return FG_LINUX_DISABLED;
    if (fg->faulted) return FG_LINUX_ERROR;
    if (!input) return fail(fg, "Missing frame-generation inputs");
    const FgLinuxFrame &f = *input;
    if (!f.commands || !f.recording_seq || f.completed_seq >= f.recording_seq || f.completed_seq < fg->completed ||
        f.recording_seq < fg->last_seq || !valid_image(f.scene) || !valid_image(f.depth) || !valid_image(f.motion) || !valid_image(f.output) ||
        !color_format(f.scene.format) || f.scene.format != f.output.format ||
        f.scene.width != f.output.width || f.scene.height != f.output.height ||
        f.depth.format != VK_FORMAT_R32_SFLOAT || (f.motion.format != VK_FORMAT_R16G16_SFLOAT && f.motion.format != VK_FORMAT_R32G32_SFLOAT) ||
        f.depth.width != f.motion.width || f.depth.height != f.motion.height ||
        f.depth.width > f.output.width + 1 || f.depth.height > f.output.height + 1 ||
        f.output.image == f.scene.image || f.output.image == f.depth.image || f.output.image == f.motion.image ||
        (f.hudless.image && (!valid_image(f.hudless) || f.hudless.width != f.scene.width || f.hudless.height != f.scene.height || f.hudless.format != f.scene.format)) ||
        !std::isfinite(f.dt_ms) || f.dt_ms <= 0 || f.dt_ms > 1000 ||
        !std::isfinite(f.camera_near) || f.camera_near <= 0 || !std::isfinite(f.camera_far) || f.camera_far <= f.camera_near ||
        !std::isfinite(f.camera_fov) || f.camera_fov <= 0 || f.camera_fov >= 3.141593f || !std::isfinite(f.meters) || f.meters <= 0)
        return fail(fg, "Invalid frame-generation resource, camera, timing or timeline input");
    for (unsigned i = 0; i < 2; ++i)
        if (!std::isfinite(f.jitter[i]) || !std::isfinite(f.motion_scale[i])) return fail(fg, "Nonfinite frame-generation motion/jitter input");
    uint32_t rect_w = f.rect_width ? f.rect_width : f.scene.width, rect_h = f.rect_height ? f.rect_height : f.scene.height;
    if (f.rect_x < 0 || f.rect_y < 0 || uint64_t(f.rect_x) + rect_w > f.scene.width || uint64_t(f.rect_y) + rect_h > f.scene.height)
        return fail(fg, "Invalid frame-generation rectangle");
    if (fg->history && f.real_frame_id == fg->last_frame) return fail(fg, "Frame generation was requested twice for the same real frame");
    fg_linux_collect(fg, f.completed_seq);
    if (fg->faulted) return FG_LINUX_ERROR;
    if (!context_for(fg, f)) return FG_LINUX_ERROR;
    bool reset = f.reset || !fg->history || fg->last_frame == UINT64_MAX || f.real_frame_id != fg->last_frame + 1 ||
                 fg->rw != f.depth.width || fg->rh != f.depth.height;
    ffxConfigureDescFrameGeneration config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.frameGenerationEnabled = true; config.allowAsyncWorkloads = false;
    config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    config.HUDLessColor = resource(f.hudless, FFX_API_RESOURCE_USAGE_READ_ONLY);
    config.generationRect = {f.rect_x, f.rect_y, static_cast<int32_t>(rect_w), static_cast<int32_t>(rect_h)};
    config.frameID = f.real_frame_id;
    // Even failed SDK recording may have emitted commands referring to its context.
    fg->active->last_seq = std::max(fg->active->last_seq, f.recording_seq);
    fg->last_seq = f.recording_seq;
    if (fg->configure(&fg->active->handle, &config.header) != FFX_API_RETURN_OK) return fail(fg, "FidelityFX offscreen configuration failed");
    ffxDispatchDescFrameGenerationPrepare prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepare.commandList = reinterpret_cast<void *>(f.commands); prepare.frameID = f.real_frame_id;
    prepare.renderSize = {f.depth.width, f.depth.height};
    prepare.jitterOffset = {f.jitter[0], f.jitter[1]}; prepare.motionVectorScale = {f.motion_scale[0], f.motion_scale[1]};
    prepare.frameTimeDelta = f.dt_ms; prepare.cameraNear = f.camera_near; prepare.cameraFar = f.camera_far;
    prepare.cameraFovAngleVertical = f.camera_fov; prepare.viewSpaceToMetersFactor = f.meters;
    prepare.depth = resource(f.depth, FFX_API_RESOURCE_USAGE_READ_ONLY); prepare.motionVectors = resource(f.motion, FFX_API_RESOURCE_USAGE_READ_ONLY);
    if (fg->dispatch(&fg->active->handle, &prepare.header) != FFX_API_RETURN_OK) return fail(fg, "FidelityFX interpolation preparation failed");
    ffxDispatchDescFrameGeneration generate{};
    generate.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    generate.commandList = reinterpret_cast<void *>(f.commands); generate.presentColor = resource(f.scene, FFX_API_RESOURCE_USAGE_READ_ONLY);
    generate.outputs[0] = resource(f.output, FFX_API_RESOURCE_USAGE_UAV); generate.numGeneratedFrames = 1;
    generate.reset = reset; generate.frameID = f.real_frame_id; generate.generationRect = config.generationRect;
    generate.backbufferTransferFunction = f.scene.format == VK_FORMAT_R16G16B16A16_SFLOAT ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    generate.minMaxLuminance[0] = 0; generate.minMaxLuminance[1] = 1000;
    if (fg->dispatch(&fg->active->handle, &generate.header) != FFX_API_RETURN_OK || generate.numGeneratedFrames != 1)
        return fail(fg, "FidelityFX interpolation dispatch failed or returned an unexpected output count");
    fg->history = true; fg->last_frame = f.real_frame_id; fg->rw = f.depth.width; fg->rh = f.depth.height;
    fg->counters.real_frames++;
    if (reset) { fg->counters.resets++; return FG_LINUX_WARMUP; }
    fg->counters.generated_recordings++; return FG_LINUX_GENERATED;
}
extern "C" bool fg_linux_close(FgLinux *fg, uint64_t completed) {
    if (!fg) return true;
    fg->requested = false; fg->history = false;
    fg_linux_collect(fg, completed);
    if (fg->retired || (fg->active && fg->active->last_seq > completed)) {
        copy_error(fg->error, sizeof fg->error, "Frame-generation shutdown is waiting for GPU completion"); return false;
    }
    if (fg->active) {
        if (!destroy_context(fg, fg->active)) return false;
        fg->active = nullptr; fg->counters.live_contexts--;
    }
    unregister_proc(fg);
    if (fg->module) dlclose(fg->module);
    delete fg; return true;
}
extern "C" void fg_linux_stats(const FgLinux *fg, FgLinuxStats *out) {
    if (!out) return;
    *out = fg ? fg->counters : FgLinuxStats{};
    if (fg) { out->enabled = fg->requested && !fg->faulted; out->history_valid = fg->history; }
}
extern "C" const char *fg_linux_error(const FgLinux *fg) { return fg ? fg->error : "Frame-generation provider is unavailable"; }
extern "C" uint64_t fg_linux_provider_version(const FgLinux *fg) { return fg ? fg->provider_version : 0; }
