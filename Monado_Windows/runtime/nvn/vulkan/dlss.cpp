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
#include <stdarg.h>
#include <atomic>
#include <mutex>
#include <windows.h>
#include "../../platform/windows_optional_library.h"
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_hooks.h>
#include <sl_helpers_vk.h>

extern "C" {
#include "temporal.h"
#include "dlss.h"
#include "temporal_camera.h"
#include "../../settings.h"

}
#define DLOG(...) fprintf(stderr, __VA_ARGS__)

static HMODULE sl_mod;
static bool sl_ok, sl_initialized;
static std::atomic<bool> dlss_ok{false}, sl_surface_ready{false};
static std::mutex status_mutex;
static char status_text[512]="DLSS not initialized; checked when the game starts.";
static uint32_t sl_frame;
static PFun_slFreeResources *p_slFreeResources;
static PFun_slShutdown *p_slShutdown;
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
static PFN_vkGetInstanceProcAddr p_proxyInstanceProcAddr;
static const char *const proxy_names[]={"vkCreateSwapchainKHR","vkDestroySwapchainKHR","vkGetSwapchainImagesKHR","vkAcquireNextImageKHR","vkQueuePresentKHR","vkDeviceWaitIdle"};
static PFN_vkVoidFunction device_proxies[sizeof proxy_names/sizeof *proxy_names];
static sl::FeatureRequirements reqs;

static void dlss_note(const char *format,...) {
    std::lock_guard<std::mutex> lock(status_mutex);
    va_list arguments;va_start(arguments,format);vsnprintf(status_text,sizeof status_text,format,arguments);va_end(arguments);
    DLOG("[dlss] %s\n",status_text);
}
extern "C" const char *dlss_status(void) {
    static thread_local char text[sizeof status_text];
    std::lock_guard<std::mutex> lock(status_mutex);memcpy(text,status_text,sizeof text);return text;
}
// Early initialization owns no GPU work. Release partially initialized SDK state
// before falling back, including optional modules whose exports are incomplete.
static bool dlss_early_failure(const char *reason, int code=0) {
    dlss_note("%s (SDK %d); DLSS unavailable.",reason,code);
    if(sl_initialized&&p_slShutdown)p_slShutdown();
    sl_initialized=sl_ok=false;dlss_ok=false;sl_surface_ready=false;
    if(sl_mod){FreeLibrary(sl_mod);sl_mod=nullptr;}
    p_proxyDeviceProcAddr=nullptr;p_proxyInstanceProcAddr=nullptr;
    memset(device_proxies,0,sizeof device_proxies);
    memset(&reqs,0,sizeof reqs);return false;
}

static void sl_log(sl::LogType type, const char *msg) {
    if (type != sl::LogType::eInfo) DLOG("[dlss] %s", msg);
}

// Before the Vulkan instance: load Streamline with the DLSS plugin (only when DLSS is the chosen upscaler).
extern "C" bool dlss_init_early(void) {
    if (g_settings.upscaler != UPSCALE_DLSS || getenv("XCDE_TMP_OFF") || getenv("XCDE_PRECOMPILING")) {dlss_note("DLSS not requested at startup; select DLSS and restart the game to check it.");return false;}
    if(sl_mod)return sl_ok;
    static wchar_t dir[4096];
    DWORD n = GetModuleFileNameW(NULL, dir, 4096);
    if(!n || n>=4096){dlss_note("The runtime executable path is unavailable or too long; DLSS unavailable.");return false;}
    // Streamline appends a forward slash when locating its plugins. Extended
    // namespace paths treat that slash literally; give the SDK the ordinary
    // DOS/UNC spelling while retaining checked extended mapping in our loader.
    if(!monado_windows_module_path(dir,4096,dir)){dlss_note("The runtime executable path is not an ordinary module path; DLSS unavailable.");return false;}
    n=(DWORD)wcslen(dir);DWORD module_length=n;
    while (n && dir[n - 1] != L'\\' && dir[n - 1] != L'/') n--;
    dir[n] = 0;
    // The official SDK can fail inside plugin/signature handling beyond its
    // MAX_PATH boundary, even though our own native loader supports long paths.
    // Include its added separator, longest selected SDK DLL basename and NUL.
    if(module_length+1>=MAX_PATH || n+1+sizeof(L"NvLowLatencyVk.dll")/sizeof(wchar_t)>=MAX_PATH) {
        dlss_note("DLSS unavailable: this installation path exceeds the NVIDIA SDK path limit; use a shorter installation folder.");return false;
    }
    wchar_t path[4096 + 32];
    swprintf(path, 4096 + 32, L"%lssl.interposer.dll", dir);
    char load_reason[512];
    sl_mod = monado_optional_library_load(path,load_reason,sizeof load_reason);
    if (!sl_mod) { dlss_note("sl.interposer.dll unavailable: %s",load_reason); return false; }
#define GET(f) p_##f = (PFun_##f *)(void *)GetProcAddress(sl_mod, #f)
    GET(slFreeResources); GET(slShutdown);
    GET(slInit); GET(slGetFeatureRequirements); GET(slSetVulkanInfo); GET(slIsFeatureSupported); GET(slGetFeatureFunction);
    GET(slGetNewFrameToken); GET(slSetConstants); GET(slSetTagForFrame); GET(slEvaluateFeature);
#undef GET
    p_proxyDeviceProcAddr = (PFN_vkGetDeviceProcAddr)(void *)GetProcAddress(sl_mod, "vkGetDeviceProcAddr");
    p_proxyInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(sl_mod, "vkGetInstanceProcAddr");
    if (!p_slInit || !p_slGetFeatureRequirements || !p_slSetVulkanInfo || !p_slIsFeatureSupported || !p_slGetFeatureFunction || !p_slEvaluateFeature || !p_proxyDeviceProcAddr || !p_proxyInstanceProcAddr || !p_slGetNewFrameToken || !p_slSetConstants || !p_slSetTagForFrame || !p_slFreeResources || !p_slShutdown)
        return dlss_early_failure("Streamline is missing required entry points");
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
    if (r != sl::Result::eOk) return dlss_early_failure("Streamline initialization failed",(int)r);
    sl_initialized=true;
    r = p_slGetFeatureRequirements(sl::kFeatureDLSS, reqs);
    if (r != sl::Result::eOk) return dlss_early_failure("The DLSS plugin did not load",(int)r);
    if(reqs.vkNumInstanceExtensions>128||reqs.vkNumDeviceExtensions>128||reqs.vkNumFeatures12>4096||reqs.vkNumFeatures13>4096||
       (reqs.vkNumInstanceExtensions&&!reqs.vkInstanceExtensions)||(reqs.vkNumDeviceExtensions&&!reqs.vkDeviceExtensions)||
       (reqs.vkNumFeatures12&&!reqs.vkFeatures12)||(reqs.vkNumFeatures13&&!reqs.vkFeatures13))
        return dlss_early_failure("Streamline returned invalid Vulkan requirements");
    sl_ok = true;
    dlss_note("Streamline loaded; selected GPU, Vulkan queues and surface still require validation.");
    DLOG("[dlss] Streamline loaded: %u instance / %u device extensions, %u + %u extra queues\n", reqs.vkNumInstanceExtensions,
         reqs.vkNumDeviceExtensions, reqs.vkNumGraphicsQueuesRequired, reqs.vkNumComputeQueuesRequired);
    return true;
}

extern "C" int dlss_instance_exts(const char **out, int max) {
    if(sl_ok&&(max<0||reqs.vkNumInstanceExtensions>(uint32_t)max)){sl_ok=false;dlss_note("Too many optional DLSS instance extensions; using fallback.");return 0;}
    if(sl_ok&&reqs.vkNumInstanceExtensions){
        uint32_t count=0;VkResult result=vkEnumerateInstanceExtensionProperties(nullptr,&count,nullptr);
        auto available=count&&count<=4096?(VkExtensionProperties*)calloc(count,sizeof(VkExtensionProperties)):nullptr;
        bool supported=result==VK_SUCCESS&&available&&vkEnumerateInstanceExtensionProperties(nullptr,&count,available)==VK_SUCCESS;
        for(uint32_t i=0;supported&&i<reqs.vkNumInstanceExtensions;i++){
            bool found=false;for(uint32_t j=0;j<count;j++)found|=!strcmp(reqs.vkInstanceExtensions[i],available[j].extensionName);
            if(!found)supported=false;
        }
        free(available);
        if(!supported){sl_ok=false;dlss_note("The Vulkan loader lacks required DLSS instance extensions; using fallback.");return 0;}
    }
    int n = 0;
    for (uint32_t i = 0; sl_ok && i < reqs.vkNumInstanceExtensions && n < max; i++) out[n++] = reqs.vkInstanceExtensions[i];
    return n;
}

// Adapter/surface/queue handling adapts Monado-Reborn 3428bb323043477d522c4bb00dfa9fbcc4f13e86,
// runtime/nvn/vulkan/dlss.cpp (original MIT project attribution retained).
// An optional provider must never make the selected ordinary Vulkan device fail.
extern "C" void dlss_validate_adapter(VkPhysicalDevice phys,uint32_t api,uint32_t qfam) {
    if(!sl_ok)return;
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(phys,&properties);
    bool supported=api>=VK_API_VERSION_1_3&&properties.vendorID==0x10de;
    uint32_t count=0;VkResult result=vkEnumerateDeviceExtensionProperties(phys,nullptr,&count,nullptr);
    auto extensions=count&&count<=4096?(VkExtensionProperties*)calloc(count,sizeof(VkExtensionProperties)):nullptr;
    if(result!=VK_SUCCESS||!extensions||vkEnumerateDeviceExtensionProperties(phys,nullptr,&count,extensions)!=VK_SUCCESS)supported=false;
    for(uint32_t i=0;supported&&i<reqs.vkNumDeviceExtensions;i++) {
        bool found=false;for(uint32_t j=0;j<count;j++)found|=!strcmp(reqs.vkDeviceExtensions[i],extensions[j].extensionName);
        if(!found)supported=false;
    }
    free(extensions);
    if(supported) {
        VkPhysicalDeviceVulkan13Features h13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features h12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};h12.pNext=&h13;
        VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};have.pNext=&h12;vkGetPhysicalDeviceFeatures2(phys,&have);
        auto need12=sl::getVkPhysicalDeviceVulkan12Features(reqs.vkNumFeatures12,reqs.vkFeatures12);
        auto need13=sl::getVkPhysicalDeviceVulkan13Features(reqs.vkNumFeatures13,reqs.vkFeatures13);
        const VkBool32 *h=&h12.samplerMirrorClampToEdge,*need=&need12.samplerMirrorClampToEdge;
        for(size_t i=0;i<(offsetof(VkPhysicalDeviceVulkan12Features,subgroupBroadcastDynamicId)+sizeof(VkBool32)-offsetof(VkPhysicalDeviceVulkan12Features,samplerMirrorClampToEdge))/sizeof(VkBool32);i++)if(need[i]&&!h[i])supported=false;
        h=&h13.robustImageAccess;need=&need13.robustImageAccess;
        for(size_t i=0;i<(offsetof(VkPhysicalDeviceVulkan13Features,maintenance4)+sizeof(VkBool32)-offsetof(VkPhysicalDeviceVulkan13Features,robustImageAccess))/sizeof(VkBool32);i++)if(need[i]&&!h[i])supported=false;
        uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(phys,&families,nullptr);
        auto queues=families&&families<=4096?(VkQueueFamilyProperties*)calloc(families,sizeof(VkQueueFamilyProperties)):nullptr;
        if(queues)vkGetPhysicalDeviceQueueFamilyProperties(phys,&families,queues);
        uint64_t required=1ull+reqs.vkNumGraphicsQueuesRequired+reqs.vkNumComputeQueuesRequired;
        if(!queues||qfam>=families||required>queues[qfam].queueCount||required>16||
           (reqs.vkNumGraphicsQueuesRequired&&!(queues[qfam].queueFlags&VK_QUEUE_GRAPHICS_BIT))||
           (reqs.vkNumComputeQueuesRequired&&!(queues[qfam].queueFlags&VK_QUEUE_COMPUTE_BIT)))supported=false;
        free(queues);
    }
    if(!supported){sl_ok=false;dlss_ok=false;dlss_note("The selected GPU/driver lacks required DLSS Vulkan extensions, features or queues; using fallback.");}
}

// The device's extensions (appended to ext[*n]) and features (OR-ed into f12 / f13); extra queues it asks for.
extern "C" void dlss_device_requirements(VkInstance inst, VkPhysicalDevice phys, const char **ext, uint32_t *n, uint32_t max, VkPhysicalDeviceVulkan12Features *f12,
                                         VkPhysicalDeviceVulkan13Features *f13, uint32_t *extra_gfx, uint32_t *extra_compute) {
    *extra_gfx = *extra_compute = 0;
    if (!sl_ok) return;
    uint32_t additional=0;
    for(uint32_t i=0;i<reqs.vkNumDeviceExtensions;i++) {
        bool found=false;for(uint32_t j=0;j<*n;j++)found|=!strcmp(ext[j],reqs.vkDeviceExtensions[i]);
        if(!found)additional++;
    }
    if(*n>max||additional>max-*n){sl_ok=false;dlss_note("Too many optional DLSS device extensions; using fallback.");return;}
    for (uint32_t i = 0; i < reqs.vkNumDeviceExtensions && *n < max; i++) {
        bool dup = false;
        for (uint32_t j = 0; j < *n; j++) dup |= !strcmp(ext[j], reqs.vkDeviceExtensions[i]);
        if (!dup) ext[(*n)++] = reqs.vkDeviceExtensions[i];
    }
    VkPhysicalDeviceVulkan12Features a = sl::getVkPhysicalDeviceVulkan12Features(reqs.vkNumFeatures12, reqs.vkFeatures12);
    VkPhysicalDeviceVulkan13Features b = sl::getVkPhysicalDeviceVulkan13Features(reqs.vkNumFeatures13, reqs.vkFeatures13);
    // (OR the VkBool32 fields: everything between pNext and the end of each structure)
    VkBool32 *d = &f12->samplerMirrorClampToEdge, *s = &a.samplerMirrorClampToEdge;
    for (size_t i = 0; i < (offsetof(VkPhysicalDeviceVulkan12Features, subgroupBroadcastDynamicId)+sizeof(VkBool32) - offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge)) / sizeof(VkBool32); i++) d[i] |= s[i];
    d = &f13->robustImageAccess; s = &b.robustImageAccess;
    for (size_t i = 0; i < (offsetof(VkPhysicalDeviceVulkan13Features, maintenance4)+sizeof(VkBool32) - offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess)) / sizeof(VkBool32); i++) d[i] |= s[i];
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
    info.graphicsQueueIndex = reqs.vkNumGraphicsQueuesRequired?1:0;
    info.computeQueueFamily = qfam;
    info.computeQueueIndex = reqs.vkNumComputeQueuesRequired?1+reqs.vkNumGraphicsQueuesRequired:0;
    sl::Result r = p_slSetVulkanInfo(info);
    if (r != sl::Result::eOk) { dlss_note("Streamline Vulkan device setup failed (SDK %d).",(int)r); sl_ok = false; return; }
    sl::AdapterInfo ai{};
    VkPhysicalDevice pd = phys;
    ai.vkPhysicalDevice = pd;
    r = p_slIsFeatureSupported(sl::kFeatureDLSS, ai);
    if (r != sl::Result::eOk) { dlss_note("DLSS is unsupported on the selected GPU/driver (SDK %d).",(int)r);sl_ok=false;return; }
    void *f = nullptr;
    if (p_slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", f) != sl::Result::eOk || !f) {
        dlss_note("The DLSS plugin is missing slDLSSSetOptions.");
        return;
    }
    p_slDLSSSetOptions = (PFun_slDLSSSetOptions *)f;
    for(size_t i=0;i<sizeof proxy_names/sizeof *proxy_names;i++) {
        device_proxies[i]=p_proxyDeviceProcAddr(dev,proxy_names[i]);
        if(!device_proxies[i]){sl_ok=false;dlss_note("The required Streamline device proxy %s is unavailable; using fully native presentation.",proxy_names[i]);memset(device_proxies,0,sizeof device_proxies);return;}
    }
    dlss_ok = true;
    dlss_note("DLSS device initialized; Streamline presentation surface still requires validation.");
}

extern "C" bool dlss_create_surface(void *sdl_window,VkInstance instance,VkPhysicalDevice phys,uint32_t qfam,VkSurfaceKHR *surface) {
    if(!dlss_ok||!p_proxyInstanceProcAddr||!surface)return false;
    auto create=(PFN_vkCreateWin32SurfaceKHR)p_proxyInstanceProcAddr(instance,"vkCreateWin32SurfaceKHR");
    auto destroy=(PFN_vkDestroySurfaceKHR)p_proxyInstanceProcAddr(instance,"vkDestroySurfaceKHR");
    HWND hwnd=(HWND)SDL_GetPointerProperty(SDL_GetWindowProperties((SDL_Window*)sdl_window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr);
    if(!create||!destroy||!hwnd){dlss_note("The Streamline Win32 surface hook or window is unavailable; using fallback.");return false;}
    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance=(HINSTANCE)GetWindowLongPtrW(hwnd,GWLP_HINSTANCE);if(!info.hinstance)info.hinstance=GetModuleHandleW(nullptr);info.hwnd=hwnd;
    VkSurfaceKHR hooked=VK_NULL_HANDLE;VkResult result=create(instance,&info,nullptr,&hooked);
    if(result!=VK_SUCCESS||!hooked){dlss_note("Streamline surface creation failed (Vulkan %d); using fallback.",(int)result);return false;}
    VkBool32 present=VK_FALSE;result=vkGetPhysicalDeviceSurfaceSupportKHR(phys,qfam,hooked,&present);
    if(result!=VK_SUCCESS||!present){destroy(instance,hooked,nullptr);dlss_note("The selected queue cannot present the Streamline surface (Vulkan %d); using fallback.",(int)result);return false;}
    *surface=hooked;sl_surface_ready=true;dlss_note("DLSS is ready on the selected GPU; actual frame dispatch is checked separately.");return true;
}

// Streamline's proxies for the swapchain calls it intercepts (NULL: call Vulkan directly).
extern "C" PFN_vkVoidFunction dlss_proxy(VkDevice dev, const char *name) {
    if (!sl_surface_ready || !p_proxyDeviceProcAddr) return nullptr;
    (void)dev;
    for(size_t i=0;i<sizeof proxy_names/sizeof *proxy_names;i++)if(!strcmp(name,proxy_names[i]))return device_proxies[i];
    return nullptr;
}

extern "C" bool dlss_available(void) { return dlss_ok&&sl_surface_ready; }

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
                              const double reproj[16], uint64_t recording_seq, uint64_t completed_seq) {
    (void)recording_seq;(void)completed_seq;
    if (!dlss_available()) return false;
    sl::FrameToken *token = nullptr;
    uint32_t fi = sl_frame++;
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
    double position[3];if(!tmp_camera_position(cam->view,position))return false;
    c.cameraPos = sl::float3((float)position[0],(float)position[1],(float)position[2]);
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
    if (r != sl::Result::eOk) { static int n; if (n++ < 5) dlss_note("DLSS frame dispatch failed (SDK %d); this frame uses fallback.",(int)r);return false; }
    return true;
}

// Call only once the device is idle and no producer can submit provider work.
extern "C" void dlss_reset(void) {
    if(dlss_ok && p_slFreeResources) {
        sl::ViewportHandle viewport(0);
        sl::Result result=p_slFreeResources(sl::kFeatureDLSS,viewport);
        if(result!=sl::Result::eOk){DLOG("[dlss] resource reset failed: %d\n",(int)result);dlss_ok=false;}
    }
    sl_frame=0;
}
extern "C" void dlss_shutdown(void) {
    dlss_reset();
    if(sl_initialized && p_slShutdown) {
        sl::Result result=p_slShutdown();
        if(result!=sl::Result::eOk)DLOG("[dlss] shutdown failed: %d\n",(int)result);
    }
    sl_initialized=sl_ok=false;dlss_ok=false;sl_surface_ready=false;
    p_proxyDeviceProcAddr=nullptr;p_proxyInstanceProcAddr=nullptr;p_slDLSSSetOptions=nullptr;
    memset(device_proxies,0,sizeof device_proxies);
    if(sl_mod){FreeLibrary(sl_mod);sl_mod=nullptr;}
}
