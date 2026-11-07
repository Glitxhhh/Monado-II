// [XCDE] NVIDIA DLSS Super Resolution through Streamline 2.x (third_party/streamline, MIT; sl.interposer.dll,
// sl.common.dll, sl.dlss.dll and nvngx_dlss.dll next to the exe), for temporal.c. Streamline's manual hooking mode:
// the Vulkan API is called directly; before the instance / device are created the DLSS plugin's extensions,
// features and queues are added (device.c), the device is then handed to Streamline (slSetVulkanInfo), and the
// swapchain calls it intercepts go through its proxies (dlss_proxy). Loaded at run time: without the DLLs, without an
// RTX GPU or with another upscaler chosen nothing of this runs (temporal.c falls back to FSR 3).
// Per frame (dlss_evaluate): constants (camera, jitter), DLSS options (quality mode, output size), the tagged inputs
// (colour, depth, motion vectors at render size; output at window size) and the evaluation into the command buffer.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <vulkan/vulkan.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_hooks.h>
#include <sl_helpers_vk.h>

extern "C" {
#include "temporal.h"
#include "../../settings.h"

}
#define DLOG(...) fprintf(stderr, __VA_ARGS__)

static HMODULE sl_mod;
static bool sl_ok, dlss_ok;
static PFun_slInit *p_slInit;
static PFun_slGetFeatureRequirements *p_slGetFeatureRequirements;
static PFun_slSetVulkanInfo *p_slSetVulkanInfo;
static PFun_slIsFeatureSupported *p_slIsFeatureSupported;
static PFun_slGetFeatureFunction *p_slGetFeatureFunction;
static PFun_slGetNewFrameToken *p_slGetNewFrameToken;
static PFun_slSetConstants *p_slSetConstants;
static PFun_slSetTagForFrame *p_slSetTagForFrame;
static PFun_slEvaluateFeature *p_slEvaluateFeature;
static PFun_slDLSSSetOptions *p_slDLSSSetOptions;
static PFN_vkGetDeviceProcAddr p_proxyDeviceProcAddr;
static sl::FeatureRequirements reqs;

static void sl_log(sl::LogType type, const char *msg) {
    if (type != sl::LogType::eInfo) DLOG("[dlss] %s", msg);
}

// Before the Vulkan instance: load Streamline with the DLSS plugin (only when DLSS is the chosen upscaler).
extern "C" bool dlss_init_early(void) {
    if (g_settings.upscaler != UPSCALE_DLSS || getenv("XCDE_TMP_OFF")) return false;
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    while (n && dir[n - 1] != L'\\' && dir[n - 1] != L'/') n--;
    dir[n] = 0;
    wchar_t path[MAX_PATH + 32];
    swprintf(path, MAX_PATH + 32, L"%lssl.interposer.dll", dir);
    sl_mod = LoadLibraryW(path);
    if (!sl_mod) { DLOG("[dlss] sl.interposer.dll not found: DLSS unavailable\n"); return false; }
#define GET(f) p_##f = (PFun_##f *)(void *)GetProcAddress(sl_mod, #f)
    GET(slInit); GET(slGetFeatureRequirements); GET(slSetVulkanInfo); GET(slIsFeatureSupported); GET(slGetFeatureFunction);
    GET(slGetNewFrameToken); GET(slSetConstants); GET(slSetTagForFrame); GET(slEvaluateFeature);
#undef GET
    p_proxyDeviceProcAddr = (PFN_vkGetDeviceProcAddr)(void *)GetProcAddress(sl_mod, "vkGetDeviceProcAddr");
    if (!p_slInit || !p_slGetFeatureRequirements || !p_slSetVulkanInfo || !p_slEvaluateFeature || !p_proxyDeviceProcAddr) {
        DLOG("[dlss] sl.interposer.dll: entry points missing\n");
        return false;
    }
    static const wchar_t *paths[1];
    paths[0] = dir;
    static sl::Feature features[] = {sl::kFeatureDLSS};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = getenv("XCDE_TMP_LOG") ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.logMessageCallback = sl_log;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = nullptr;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 1;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "xcde";
    pref.projectId = "5b7c1d8e-6a4f-4e1d-9c3a-2f8e7d6b5a41";
    pref.renderAPI = sl::RenderAPI::eVulkan;
    sl::Result r = p_slInit(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) { DLOG("[dlss] slInit failed: %d\n", (int)r); return false; }
    r = p_slGetFeatureRequirements(sl::kFeatureDLSS, reqs);
    if (r != sl::Result::eOk) { DLOG("[dlss] DLSS plugin not loaded: %d\n", (int)r); return false; }
    sl_ok = true;
    DLOG("[dlss] Streamline loaded: %u instance / %u device extensions, %u + %u extra queues\n", reqs.vkNumInstanceExtensions,
         reqs.vkNumDeviceExtensions, reqs.vkNumGraphicsQueuesRequired, reqs.vkNumComputeQueuesRequired);
    return true;
}

extern "C" int dlss_instance_exts(const char **out, int max) {
    int n = 0;
    for (uint32_t i = 0; sl_ok && i < reqs.vkNumInstanceExtensions && n < max; i++) out[n++] = reqs.vkInstanceExtensions[i];
    return n;
}

// The device's extensions (appended to ext[*n]) and features (OR-ed into f12 / f13); extra queues it asks for.
extern "C" void dlss_device_requirements(const char **ext, uint32_t *n, uint32_t max, VkPhysicalDeviceVulkan12Features *f12,
                                         VkPhysicalDeviceVulkan13Features *f13, uint32_t *extra_gfx, uint32_t *extra_compute) {
    *extra_gfx = *extra_compute = 0;
    if (!sl_ok) return;
    for (uint32_t i = 0; i < reqs.vkNumDeviceExtensions && *n < max; i++) {
        bool dup = false;
        for (uint32_t j = 0; j < *n; j++) dup |= !strcmp(ext[j], reqs.vkDeviceExtensions[i]);
        if (!dup) ext[(*n)++] = reqs.vkDeviceExtensions[i];
    }
    VkPhysicalDeviceVulkan12Features a = sl::getVkPhysicalDeviceVulkan12Features(reqs.vkNumFeatures12, reqs.vkFeatures12);
    VkPhysicalDeviceVulkan13Features b = sl::getVkPhysicalDeviceVulkan13Features(reqs.vkNumFeatures13, reqs.vkFeatures13);
    // (OR the VkBool32 fields: everything between pNext and the end of each structure)
    VkBool32 *d = &f12->samplerMirrorClampToEdge, *s = &a.samplerMirrorClampToEdge;
    for (size_t i = 0; i < (sizeof a - offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge)) / sizeof(VkBool32); i++) d[i] |= s[i];
    d = &f13->robustImageAccess; s = &b.robustImageAccess;
    for (size_t i = 0; i < (sizeof b - offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess)) / sizeof(VkBool32); i++) d[i] |= s[i];
    *extra_gfx = reqs.vkNumGraphicsQueuesRequired;
    *extra_compute = reqs.vkNumComputeQueuesRequired;
}

// After vkCreateDevice: the device for Streamline (its queues start at index 1 of the host's family).
extern "C" void dlss_device_created(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t qfam) {
    if (!sl_ok) return;
    sl::VulkanInfo info{};
    info.device = dev;
    info.instance = inst;
    info.physicalDevice = phys;
    info.graphicsQueueFamily = qfam;
    info.graphicsQueueIndex = 1;
    info.computeQueueFamily = qfam;
    info.computeQueueIndex = 1 + reqs.vkNumGraphicsQueuesRequired;
    sl::Result r = p_slSetVulkanInfo(info);
    if (r != sl::Result::eOk) { DLOG("[dlss] slSetVulkanInfo failed: %d\n", (int)r); sl_ok = false; return; }
    sl::AdapterInfo ai{};
    VkPhysicalDevice pd = phys;
    ai.vkPhysicalDevice = pd;
    r = p_slIsFeatureSupported(sl::kFeatureDLSS, ai);
    if (r != sl::Result::eOk) { DLOG("[dlss] DLSS not supported on this GPU / driver: %d\n", (int)r); return; }
    void *f = nullptr;
    if (p_slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", f) != sl::Result::eOk || !f) {
        DLOG("[dlss] slDLSSSetOptions missing\n");
        return;
    }
    p_slDLSSSetOptions = (PFun_slDLSSSetOptions *)f;
    dlss_ok = true;
    DLOG("[dlss] DLSS Super Resolution ready\n");
}

// Streamline's proxies for the swapchain calls it intercepts (NULL: call Vulkan directly).
extern "C" PFN_vkVoidFunction dlss_proxy(VkDevice dev, const char *name) {
    if (!sl_ok || !p_proxyDeviceProcAddr) return nullptr;
    return p_proxyDeviceProcAddr(dev, name);
}

extern "C" bool dlss_available(void) { return dlss_ok; }

static sl::Resource tex(const TexBackend *b, VkImageUsageFlags usage) {
    static const VkComponentMapping id = {};
    VkImageView v = vk_image_view((TexBackend *)b, VK_IMAGE_VIEW_TYPE_2D, b->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    sl::Resource r(sl::ResourceType::eTex2d, (void *)b->image, nullptr, (void *)v, (uint32_t)VK_IMAGE_LAYOUT_GENERAL);
    r.width = b->width;
    r.height = b->height;
    r.nativeFormat = (uint32_t)b->format;
    r.mipLevels = 1;
    r.arrayLayers = 1;
    r.flags = 0;
    r.usage = usage;
    return r;
}

static void rows_to_sl(const double m[16], sl::float4x4 &o) {  // column-vector matrix -> Streamline's row-vector one
    for (int r = 0; r < 4; r++) o.row[r] = sl::float4((float)m[0 * 4 + r], (float)m[1 * 4 + r], (float)m[2 * 4 + r], (float)m[3 * 4 + r]);
}
static bool inv4(const double m[16], double o[16]) {
    double a[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return false;
        if (p != c) for (int j = 0; j < 8; j++) { double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        double d = a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int r = 0; r < 4; r++) if (r != c) { double f = a[r][c]; for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) o[i * 4 + j] = a[i][j + 4];
    return true;
}

// reproj: the previous clip position from this frame's (column vectors), as temporal.c computed it.
extern "C" bool dlss_evaluate(VkCommandBuffer cb, const TexBackend *color, const TexBackend *mv, const TexBackend *depth,
                              const TexBackend *out, const TexBackend *hud, const float jitter[2], bool reset, const TmpCamera *cam,
                              const double reproj[16]) {
    if (!dlss_ok) return false;
    static uint32_t frame;
    sl::FrameToken *token = nullptr;
    uint32_t fi = frame++;
    if (p_slGetNewFrameToken(token, &fi) != sl::Result::eOk || !token) return false;
    sl::ViewportHandle vp(0);
    sl::Constants c{};
    double proj[16], iproj[16];
    for (int i = 0; i < 16; i++) proj[i] = cam->proj[i];
    if (!inv4(proj, iproj)) return false;
    rows_to_sl(proj, c.cameraViewToClip);
    rows_to_sl(iproj, c.clipToCameraView);
    double idm[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, back[16];
    rows_to_sl(idm, c.clipToLensClip);
    rows_to_sl(reproj, c.clipToPrevClip);
    if (!inv4(reproj, back)) memcpy(back, idm, sizeof back);
    rows_to_sl(back, c.prevClipToClip);
    c.jitterOffset = sl::float2(jitter[0], jitter[1]);
    c.mvecScale = sl::float2(1.0f, 1.0f);  // (motion vectors in UV units)
    c.cameraPinholeOffset = sl::float2(0, 0);
    c.cameraPos = sl::float3(0, 0, 0);
    c.cameraUp = sl::float3(cam->view[4], cam->view[5], cam->view[6]);
    c.cameraRight = sl::float3(cam->view[0], cam->view[1], cam->view[2]);
    c.cameraFwd = sl::float3(-cam->view[8], -cam->view[9], -cam->view[10]);
    c.cameraNear = fabsf(cam->proj[11]) * 0.5f;
    c.cameraFar = 100000.0f;
    c.cameraFOV = cam->proj[5] != 0 ? 2.0f * atanf(1.0f / fabsf(cam->proj[5])) : 0.785f;
    c.cameraAspectRatio = cam->proj[0] != 0 ? fabsf(cam->proj[5] / cam->proj[0]) : 1.778f;
    c.motionVectorsInvalidValue = 0;
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    if (p_slSetConstants(c, *token, vp) != sl::Result::eOk) return false;
    sl::DLSSOptions o{};
    static const sl::DLSSMode modes[] = {sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eBalanced,
                                         sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eUltraPerformance};
    int q = g_settings.upscale_quality;
    o.mode = modes[q >= 0 && q <= 4 ? q : 1];
    o.outputWidth = out->width;
    o.outputHeight = out->height;
    o.colorBuffersHDR = sl::Boolean::eFalse;  // (the game image: gamma-encoded LDR)
    if (p_slDLSSSetOptions(vp, o) != sl::Result::eOk) return false;
    sl::Resource rc = tex(color, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    sl::Resource rd = tex(depth, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    sl::Resource rm = tex(mv, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    sl::Resource ro = tex(out, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    sl::Extent ein{0, 0, color->width, color->height}, eout{0, 0, out->width, out->height};
    sl::ResourceTag tags[5] = {sl::ResourceTag(&rc, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &ein),
                               sl::ResourceTag(&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &ein),
                               sl::ResourceTag(&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &ein),
                               sl::ResourceTag(&ro, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &eout),
                               sl::ResourceTag()};
    uint32_t ntags = 4;
    sl::Resource rh;
    if (hud) {  // [XCDE] T213: the HUD's pixels (temporal.c tmp_hud_snapshot): DLSS takes the current colour there (no trails)
        rh = tex(hud, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        tags[ntags++] = sl::ResourceTag(&rh, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eValidUntilPresent, &ein);
    }
    if (p_slSetTagForFrame(*token, vp, tags, ntags, (sl::CommandBuffer *)cb) != sl::Result::eOk) return false;
    const sl::BaseStructure *inputs[] = {&vp};
    sl::Result r = p_slEvaluateFeature(sl::kFeatureDLSS, *token, inputs, 1, (sl::CommandBuffer *)cb);
    if (r != sl::Result::eOk) { static int n; if (n++ < 5) DLOG("[dlss] evaluate failed: %d\n", (int)r); return false; }
    return true;
}
