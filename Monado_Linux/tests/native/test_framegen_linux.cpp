// [XCDE] LINUX-020: authored offscreen frame-generation input/output fixture.
// No SDL, window, game data, guest execution or human profile. A resource probe
// alone is explicitly not an interpolation or presentation qualification.
#include <vulkan/vulkan.h>
extern "C" {
#include "../../runtime/platform/linux_state_guard.h"
}
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#ifdef FG_LINUX_PROVIDER
#include "../../runtime/nvn/vulkan/framegen_linux.h"
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define VK_OK(x) do { VkResult result_ = (x); if (result_ != VK_SUCCESS) { fprintf(stderr, "FAIL line %d: %s -> %d\n", __LINE__, #x, result_); exit(1); } } while (0)

static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static VkCommandPool pool;
static VkCommandBuffer command;
static VkFence fence;
static uint32_t queue_family;
static uint64_t recording_seq = 1, completed_seq;
static uint32_t validation_errors;

static uint64_t hash(const unsigned char *data, size_t size) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) { h ^= data[i]; h *= 1099511628211ull; }
    return h;
}
static uint64_t rgb_hash(const unsigned char *data,size_t size) {
    uint64_t h=14695981039346656037ull;for(size_t p=0;p<size;p+=4)for(unsigned c=0;c<3;c++){h^=data[p+c];h*=1099511628211ull;}return h;
}
static void private_state(const char *state) {
    char canonical[PATH_MAX], error[256];
    CHECK(state && realpath(state, canonical) && !strcmp(state, canonical));
    CHECK(chdir(canonical) == 0);
    const char *names[] = {"HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "MESA_SHADER_CACHE_DIR", "__GL_SHADER_DISK_CACHE_PATH", "TMPDIR"};
    const char *directories[] = {"home", "cache", "config", "data", "mesa", "nvidia", "tmp"};
    for (unsigned i = 0; i < std::size(names); ++i) {
        std::string path = std::string(canonical) + "/" + directories[i];
        CHECK(mkdir(path.c_str(), 0700) == 0 || errno == EEXIST);
        struct stat st{}; CHECK(lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid());
        CHECK(setenv(names[i], path.c_str(), 1) == 0);
    }
    CHECK(rt_linux_state_guard(canonical, error, sizeof error));
    printf("STATE guarded=%s; authored images only; no SDL/window/game/input/saves\n", canonical);
}
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        validation_errors++;
        fprintf(stderr, "VALIDATION_ERROR %s\n", data->pMessage ? data->pMessage : "no text");
    }
    return VK_FALSE;
}
static VkDebugUtilsMessengerEXT debug_messenger;
static void create_device(bool headless_probe) {
    uint32_t count{}; VK_OK(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> iexts(count); VK_OK(vkEnumerateInstanceExtensionProperties(nullptr, &count, iexts.data()));
    auto has = [&](const char *name) { return std::any_of(iexts.begin(), iexts.end(), [&](const auto &e) { return !strcmp(e.extensionName, name); }); };
    const bool headless = has(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME), debug = has(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    VK_OK(vkEnumerateInstanceLayerProperties(&count, nullptr)); std::vector<VkLayerProperties> layers(count);
    VK_OK(vkEnumerateInstanceLayerProperties(&count, layers.data()));
    const bool validation = std::any_of(layers.begin(), layers.end(), [](const auto &l) { return !strcmp(l.layerName, "VK_LAYER_KHRONOS_validation"); });
    const char *validation_layer = "VK_LAYER_KHRONOS_validation";
    std::vector<const char *> instance_extensions;
    if (debug) instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (headless_probe && headless && has(VK_KHR_SURFACE_EXTENSION_NAME)) {
        instance_extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        instance_extensions.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
    }
    VkApplicationInfo app{}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; app.pApplicationName = "Monado authored FG component"; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ic{}; ic.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ic.pApplicationInfo = &app;
    ic.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.size()); ic.ppEnabledExtensionNames = instance_extensions.data();
    if (validation) { ic.enabledLayerCount = 1; ic.ppEnabledLayerNames = &validation_layer; }
    VK_OK(vkCreateInstance(&ic, nullptr, &instance));
    if (debug && validation) {
        VkDebugUtilsMessengerCreateInfoEXT di{}; di.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        di.pfnUserCallback = debug_message;
        auto make = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        CHECK(make); VK_OK(make(instance, &di, nullptr, &debug_messenger));
    }
    VK_OK(vkEnumeratePhysicalDevices(instance, &count, nullptr)); CHECK(count && count < 64);
    std::vector<VkPhysicalDevice> devices(count); VK_OK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    int score = -1; VkPhysicalDeviceProperties selected{};
    for (auto candidate : devices) {
        VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(candidate, &p);
        int rank = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 10 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 5 : 0;
        if (p.vendorID == 0x10de) rank += 20;
        if (rank > score) { physical = candidate; selected = p; score = rank; }
    }
    CHECK(physical && selected.apiVersion >= VK_API_VERSION_1_3);
    VkPhysicalDeviceVulkan13Features f13{}; f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{}; f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES; f12.pNext = &f13;
    VkPhysicalDeviceVulkan11Features f11{}; f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES; f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 available{}; available.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; available.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(physical, &available);
    printf("CAPABILITIES {\"gpu\":\"%s\",\"vendor\":%u,\"device\":%u,\"driver\":%u,\"api\":%u,\"headless_surface_advertised\":%s,\"validation_layer\":%s,\"buffer_device_address\":%s,\"float16\":%s,\"int16\":%s,\"subgroup_size_control\":%s}\n",
        selected.deviceName, selected.vendorID, selected.deviceID, selected.driverVersion, selected.apiVersion,
        headless ? "true" : "false", validation ? "true" : "false", f12.bufferDeviceAddress ? "true" : "false",
        f12.shaderFloat16 ? "true" : "false", available.features.shaderInt16 ? "true" : "false", f13.subgroupSizeControl ? "true" : "false");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr); std::vector<VkQueueFamilyProperties> qprops(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, qprops.data()); queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) if ((qprops[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { queue_family = i; break; }
    CHECK(queue_family != UINT32_MAX);
    if (headless_probe) {
        VkResult result = VK_ERROR_EXTENSION_NOT_PRESENT; VkSurfaceKHR surface{}; VkBool32 supported = VK_FALSE;
        auto make = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
        if (headless && make) {
            VkHeadlessSurfaceCreateInfoEXT si{}; si.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
            result = make(instance, &si, nullptr, &surface);
            if (result == VK_SUCCESS) {
                VK_OK(vkGetPhysicalDeviceSurfaceSupportKHR(physical, queue_family, surface, &supported));
                uint32_t formats{}, modes{}; VkSurfaceCapabilitiesKHR caps{};
                VkResult cr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
                VkResult fr = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formats, nullptr);
                VkResult mr = vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modes, nullptr);
                printf("HEADLESS {\"create_result\":%d,\"selected_gpu_present_support\":%s,\"queue_family\":%u,\"capabilities_result\":%d,\"formats_result\":%d,\"modes_result\":%d,\"format_count\":%u,\"mode_count\":%u,\"usage\":%u,\"minimum_images\":%u,\"maximum_images\":%u}\n",
                    result,supported?"true":"false",queue_family,cr,fr,mr,formats,modes,caps.supportedUsageFlags,caps.minImageCount,caps.maxImageCount);
                vkDestroySurfaceKHR(instance,surface,nullptr);
            }
        }
        if (result != VK_SUCCESS) printf("HEADLESS {\"create_result\":%d,\"selected_gpu_present_support\":false}\n",result);
    }
    VkPhysicalDeviceVulkan13Features enable13{}; enable13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enable13.synchronization2 = f13.synchronization2; enable13.subgroupSizeControl = f13.subgroupSizeControl; enable13.computeFullSubgroups = f13.computeFullSubgroups;
    VkPhysicalDeviceVulkan12Features enable12{}; enable12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES; enable12.pNext = &enable13;
    enable12.shaderFloat16 = f12.shaderFloat16; enable12.bufferDeviceAddress = f12.bufferDeviceAddress;
    enable12.storageBuffer8BitAccess = f12.storageBuffer8BitAccess;
    enable12.scalarBlockLayout = f12.scalarBlockLayout; enable12.uniformBufferStandardLayout = f12.uniformBufferStandardLayout;
    VkPhysicalDeviceVulkan11Features enable11{}; enable11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES; enable11.pNext = &enable12;
    enable11.storageBuffer16BitAccess = f11.storageBuffer16BitAccess;
    enable11.uniformAndStorageBuffer16BitAccess = f11.uniformAndStorageBuffer16BitAccess;
    enable11.storagePushConstant16 = f11.storagePushConstant16; enable11.storageInputOutput16 = f11.storageInputOutput16;
    VkPhysicalDeviceFeatures features{}; features.shaderInt16 = available.features.shaderInt16; features.shaderInt64 = available.features.shaderInt64;
    features.shaderStorageImageExtendedFormats = available.features.shaderStorageImageExtendedFormats;
    features.shaderStorageImageReadWithoutFormat = available.features.shaderStorageImageReadWithoutFormat;
    features.shaderStorageImageWriteWithoutFormat = available.features.shaderStorageImageWriteWithoutFormat;
    const float priority = 1;
    VkDeviceQueueCreateInfo qi{}; qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qi.queueFamilyIndex = queue_family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo dc{}; dc.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; dc.pNext = &enable11;
    dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &qi; dc.pEnabledFeatures = &features;
    VK_OK(vkCreateDevice(physical, &dc, nullptr, &device)); vkGetDeviceQueue(device, queue_family, 0, &queue);
    printf("PROCS {\"buffer_requirements_khr\":%s,\"buffer_requirements_core\":%s,\"image_requirements_khr\":%s,\"image_requirements_core\":%s}\n",
        vkGetDeviceProcAddr(device,"vkGetBufferMemoryRequirements2KHR")?"true":"false",vkGetDeviceProcAddr(device,"vkGetBufferMemoryRequirements2")?"true":"false",
        vkGetDeviceProcAddr(device,"vkGetImageMemoryRequirements2KHR")?"true":"false",vkGetDeviceProcAddr(device,"vkGetImageMemoryRequirements2")?"true":"false");
    VkCommandPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pi.queueFamilyIndex = queue_family; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_OK(vkCreateCommandPool(device, &pi, nullptr, &pool));
    VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VK_OK(vkAllocateCommandBuffers(device, &ai, &command));
    VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; VK_OK(vkCreateFence(device, &fi, nullptr, &fence));
}
static void begin() {
    VK_OK(vkResetCommandBuffer(command, 0)); VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; VK_OK(vkBeginCommandBuffer(command, &bi));
}
static void finish() {
    VK_OK(vkEndCommandBuffer(command)); VK_OK(vkResetFences(device, 1, &fence));
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &command;
    VK_OK(vkQueueSubmit(queue, 1, &si, fence)); VK_OK(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull));
    completed_seq = recording_seq++;
}
static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
    VkPhysicalDeviceMemoryProperties p{}; vkGetPhysicalDeviceMemoryProperties(physical, &p); uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i) if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & required) == required) {
        found = i; if ((p.memoryTypes[i].propertyFlags & preferred) == preferred) break;
    }
    CHECK(found != UINT32_MAX); return found;
}
struct Buffer {
    VkBuffer buffer{}; VkDeviceMemory memory{}; unsigned char *host{}; bool coherent{};
    explicit Buffer(size_t size) {
        VkBufferCreateInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VK_OK(vkCreateBuffer(device, &bi, nullptr, &buffer)); VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device, buffer, &requirements);
        uint32_t type = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(physical, &mp); coherent = mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = requirements.size; ai.memoryTypeIndex = type;
        VK_OK(vkAllocateMemory(device, &ai, nullptr, &memory)); VK_OK(vkBindBufferMemory(device, buffer, memory, 0));
        void *mapped{}; VK_OK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped)); host = static_cast<unsigned char *>(mapped);
    }
    ~Buffer() { vkUnmapMemory(device, memory); vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr); }
    void sync(bool upload) {
        if (coherent) return; VkMappedMemoryRange range{}; range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; range.memory = memory; range.size = VK_WHOLE_SIZE;
        if (upload) VK_OK(vkFlushMappedMemoryRanges(device, 1, &range)); else VK_OK(vkInvalidateMappedMemoryRanges(device, 1, &range));
    }
};
struct Image {
    VkImage image{}; VkImageView view{}; VkDeviceMemory memory{}; VkFormat format; uint32_t width, height; bool initialized{};
    Image(VkFormat f, uint32_t w, uint32_t h) : format(f), width(w), height(h) {
        VkFormatProperties p{}; vkGetPhysicalDeviceFormatProperties(physical, format, &p);
        CHECK((p.optimalTilingFeatures & (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) == (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT));
        VkImageCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format;
        ci.extent = {w, h, 1}; ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VK_OK(vkCreateImage(device, &ci, nullptr, &image)); VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(device, image, &requirements);
        VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = requirements.size; ai.memoryTypeIndex = memory_type(requirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_OK(vkAllocateMemory(device, &ai, nullptr, &memory)); VK_OK(vkBindImageMemory(device, image, memory, 0));
        VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_OK(vkCreateImageView(device, &vi, nullptr, &view));
    }
    Image(const Image &) = delete;
    ~Image() { vkDestroyImageView(device, view, nullptr); vkDestroyImage(device, image, nullptr); vkFreeMemory(device, memory, nullptr); }
    void dependency(VkAccessFlags destination) {
        VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = initialized ? VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT : 0;
        b.dstAccessMask = destination; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, initialized ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b); initialized = true;
    }
};
static uint16_t half(float f) {
    uint32_t value{}; memcpy(&value, &f, sizeof value); unsigned sign = value >> 16 & 0x8000, exponent = value >> 23 & 0xff, mantissa = value & 0x7fffff;
    if (!exponent || exponent < 113) return static_cast<uint16_t>(sign);
    CHECK(exponent < 143); return static_cast<uint16_t>(sign | (exponent - 112) << 10 | (mantissa + 0x1000) >> 13);
}
struct Scene {
    uint32_t w, h; size_t bytes, scene_offset, motion_offset, depth_offset, read_offset;
    Image present, hudless, motion, depth, generated; Buffer staging;
    Scene(uint32_t width, uint32_t height,VkFormat color=VK_FORMAT_R8G8B8A8_UNORM) : w(width), h(height), bytes(size_t(w)*h*4), scene_offset(bytes), motion_offset(bytes*2), depth_offset(bytes*3), read_offset(bytes*4),
        present(color,w,h), hudless(color,w,h), motion(VK_FORMAT_R16G16_SFLOAT,w,h), depth(VK_FORMAT_R32_SFLOAT,w,h), generated(color,w,h), staging(bytes*9) {}
    void pattern(unsigned frame, bool still) {
        const uint32_t box_x = w/4 + (still ? 0 : frame*8), box_y = h/3, side = std::min(w,h)/4;
        CHECK(box_x + side < w);
        auto *mv = reinterpret_cast<uint16_t *>(staging.host + motion_offset); auto *z = reinterpret_cast<float *>(staging.host + depth_offset);
        for (uint32_t y=0;y<h;y++) for (uint32_t x=0;x<w;x++) {
            const size_t p=size_t(y)*w+x; bool object=x>=box_x && x<box_x+side && y>=box_y && y<box_y+side;
            unsigned char color[4] = {static_cast<unsigned char>(object?240:24+(x/8%2)*6),static_cast<unsigned char>(object?36:36+(y/8%2)*6),static_cast<unsigned char>(object?20:96),255};
            if(present.format==VK_FORMAT_B8G8R8A8_UNORM)std::swap(color[0],color[2]);
            memcpy(staging.host+4*p,color,4); memcpy(staging.host+scene_offset+4*p,color,4);
            if (x>=8 && x<88 && y>=8 && y<28) { staging.host[4*p]=present.format==VK_FORMAT_B8G8R8A8_UNORM?32:16;staging.host[4*p+1]=240;staging.host[4*p+2]=present.format==VK_FORMAT_B8G8R8A8_UNORM?16:32; }
            mv[2*p]=half(object && frame && !still ? -8.f/w : 0); mv[2*p+1]=0; z[p]=object?.5f:.9f;
        }
        staging.sync(true);
    }
    void upload() {
        Image *images[] = {&present,&hudless,&motion,&depth}; size_t offsets[] = {0,scene_offset,motion_offset,depth_offset};
        for (unsigned i=0;i<4;i++) {
            images[i]->dependency(VK_ACCESS_TRANSFER_WRITE_BIT); VkBufferImageCopy copy{}; copy.bufferOffset=offsets[i]; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={w,h,1};
            vkCmdCopyBufferToImage(command,staging.buffer,images[i]->image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
            images[i]->dependency(VK_ACCESS_SHADER_READ_BIT);
        }
        generated.dependency(VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue sentinel{}; sentinel.float32[0]=sentinel.float32[2]=1;sentinel.float32[3]=1;VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(command,generated.image,VK_IMAGE_LAYOUT_GENERAL,&sentinel,1,&range);generated.dependency(VK_ACCESS_SHADER_WRITE_BIT);
    }
    void read(Image &image, unsigned slot=0) {
        CHECK(slot<5);image.dependency(VK_ACCESS_TRANSFER_READ_BIT);VkBufferImageCopy copy{};copy.bufferOffset=read_offset+slot*bytes;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={w,h,1};
        vkCmdCopyImageToBuffer(command,image.image,VK_IMAGE_LAYOUT_GENERAL,staging.buffer,1,&copy);
        VkBufferMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.buffer=staging.buffer;b.offset=read_offset;b.size=bytes;
        b.size=bytes*5;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&b,0,nullptr);
    }
    void read_generated_and_inputs() {
        read(generated);read(present,1);read(hudless,2);read(motion,3);read(depth,4);
    }
    void check_resource(unsigned frame) {
        staging.sync(false);CHECK(!memcmp(staging.host,staging.host+read_offset,bytes*4));
        printf("RESOURCE {\"width\":%u,\"height\":%u,\"format\":%d,\"frame\":%u,\"hash\":\"%016llx\",\"bytes\":%zu}\n",w,h,present.format,frame,(unsigned long long)hash(staging.host+read_offset,bytes),bytes);
    }
};
static void resource_probe() {
    uint64_t prior{};
    for (auto format : {VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM})for (auto extent : {std::array<uint32_t,2>{512,288},std::array<uint32_t,2>{640,360}}) {
        Scene scene(extent[0],extent[1],format);
        for (unsigned frame=0;frame<3;frame++) {
            scene.pattern(frame,false);begin();scene.upload();scene.read(scene.present,0);scene.read(scene.hudless,1);scene.read(scene.motion,2);scene.read(scene.depth,3);finish();scene.check_resource(frame);
            uint64_t current=hash(scene.staging.host+scene.read_offset,scene.bytes);CHECK(current!=prior);prior=current;
        }
    }
    printf("PASS authored resource upload/readback/resize; NOT frame interpolation or WSI/pacing qualification\n");
}
#ifdef FG_LINUX_PROVIDER
static FgLinuxImage fg_image(const Image &image) { return {image.image,image.format,image.width,image.height}; }
static FgLinuxFrame fg_frame(Scene &scene,uint64_t frame,bool reset=false) {
    FgLinuxFrame f{};f.commands=command;f.scene=fg_image(scene.present);f.hudless=fg_image(scene.hudless);
    f.depth=fg_image(scene.depth);f.motion=fg_image(scene.motion);f.output=fg_image(scene.generated);
    f.motion_scale[0]=static_cast<float>(scene.w);f.motion_scale[1]=static_cast<float>(scene.h);
    f.dt_ms=1000.f/30;f.camera_near=.1f;f.camera_far=1000;f.camera_fov=.7853982f;f.meters=1;
    f.real_frame_id=frame;f.recording_seq=recording_seq;f.completed_seq=completed_seq;f.reset=reset;return f;
}
struct Metric {uint64_t hash,rgb_hash;double centroid;size_t red,changed,rgb_changed,hud_bad;};
static void write_pixels(const char *scenario,uint64_t frame,const char *kind,const unsigned char *pixels,size_t bytes) {
    char name[160];CHECK(snprintf(name,sizeof name,"%s-%llu-%s.rgba",scenario,(unsigned long long)frame,kind)<static_cast<int>(sizeof name));
    FILE *f=fopen(name,"wb");CHECK(f);CHECK(fwrite(pixels,1,bytes,f)==bytes);CHECK(!fclose(f));
}
static Metric metric(Scene &scene,const char *name,uint64_t frame,FgLinuxResult result) {
    scene.staging.sync(false);const unsigned char *pixels=scene.staging.host+scene.read_offset;
    // Interpolation must not replace or mutate the caller's real scene, HUD-less color, depth or motion.
    CHECK(!memcmp(scene.staging.host,scene.staging.host+scene.read_offset+scene.bytes,scene.bytes*4));
    Metric m{};m.hash=hash(pixels,scene.bytes);m.rgb_hash=rgb_hash(pixels,scene.bytes);double red_x=0;
    for(uint32_t y=0;y<scene.h;y++)for(uint32_t x=0;x<scene.w;x++) {
        const size_t p=size_t(y)*scene.w+x;const unsigned char *color=pixels+4*p;
        if(memcmp(color,scene.staging.host+4*p,4))m.changed++;
        if(memcmp(color,scene.staging.host+4*p,3))m.rgb_changed++;
        unsigned ri=scene.present.format==VK_FORMAT_B8G8R8A8_UNORM?2:0,bi=2-ri;
        if(color[ri]>180 && color[1]<100 && color[bi]<100) {m.red++;red_x+=x;}
        if(x>=10 && x<86 && y>=10 && y<26)
            for(unsigned c=0;c<4;c++)if(abs(int(color[c])-int(scene.staging.host[4*p+c]))>2) {m.hud_bad++;break;}
    }
    m.centroid=m.red?red_x/m.red:-1;
    write_pixels(name,frame,"output",pixels,scene.bytes);write_pixels(name,frame,"real",scene.staging.host,scene.bytes);
    printf("FRAMEGEN {\"scenario\":\"%s\",\"frame\":%llu,\"result\":%d,\"width\":%u,\"height\":%u,\"hash\":\"%016llx\",\"rgb_hash\":\"%016llx\",\"red_pixels\":%zu,\"red_centroid_x\":%.5f,\"changed_from_real\":%zu,\"rgb_changed_from_real\":%zu,\"hud_bad_pixels\":%zu}\n",
        name,(unsigned long long)frame,result,scene.w,scene.h,(unsigned long long)m.hash,(unsigned long long)m.rgb_hash,m.red,m.centroid,m.changed,m.rgb_changed,m.hud_bad);
    return m;
}
static FgLinux *open_provider(const char *module) {
    char error[512]{};FgLinux *fg=fg_linux_open(module,physical,device,vkGetDeviceProcAddr,error,sizeof error);
    if(!fg)fprintf(stderr,"PROVIDER_OPEN %s\n",error);CHECK(fg && fg_linux_available(fg));
    printf("PROVIDER version=%llu\n",(unsigned long long)fg_linux_provider_version(fg));fg_linux_set_enabled(fg,true);return fg;
}
static FgLinuxResult dispatch(FgLinux *fg,const FgLinuxFrame &f) {
    FgLinuxResult result=fg_linux_dispatch(fg,&f);
    if(result==FG_LINUX_ERROR)fprintf(stderr,"PROVIDER_DISPATCH %s\n",fg_linux_error(fg));
    CHECK(result!=FG_LINUX_ERROR);return result;
}
static void sequence(const char *module,bool still,VkFormat format=VK_FORMAT_R8G8B8A8_UNORM) {
    FgLinux *fg=open_provider(module);Scene scene(512,288,format);uint64_t previous_hash{};unsigned generated=0;
    for(unsigned frame=0;frame<6;frame++) {
        scene.pattern(frame,still);begin();scene.upload();FgLinuxFrame f=fg_frame(scene,frame+1,frame==0);
        f.depth_infinite=still || format==VK_FORMAT_B8G8R8A8_UNORM;
        if(format==VK_FORMAT_B8G8R8A8_UNORM)f.dt_ms=1000.f/165;
        FgLinuxResult result=dispatch(fg,f);scene.read_generated_and_inputs();finish();fg_linux_collect(fg,completed_seq);
        Metric m=metric(scene,still?"static-hud":format==VK_FORMAT_B8G8R8A8_UNORM?"moving-bgra-hud":"moving-hud",frame,result);
        CHECK(result==(frame?FG_LINUX_GENERATED:FG_LINUX_WARMUP));
        if(result==FG_LINUX_GENERATED) {
            generated++;CHECK(m.red>100 && m.hud_bad==0);
            if(still) {
                // Opaque color regions should not drift in a zero-motion sequence.
                CHECK(fabs(m.centroid-(scene.w/4+(std::min(scene.w,scene.h)/4-1)*.5))<1 && !m.rgb_changed);
                if(frame>1)CHECK(m.rgb_hash==previous_hash);
            } else {
                double side=std::min(scene.w,scene.h)/4;
                double current=scene.w/4+frame*8+(side-1)*.5,prior=current-8;
                CHECK(m.centroid>prior+.4 && m.centroid<current-.4 && fabs(m.centroid-(current+prior)*.5)<3);
                CHECK(m.rgb_changed>100 && m.rgb_hash!=rgb_hash(scene.staging.host,scene.bytes));
                CHECK(m.rgb_hash!=previous_hash);
            }
        }
        previous_hash=m.rgb_hash;
    }
    FgLinuxStats stats{};fg_linux_stats(fg,&stats);CHECK(stats.real_frames==6 && stats.generated_recordings==generated && stats.failures==0);
    CHECK(fg_linux_close(fg,completed_seq));
}
static void history_sequence(const char *module) {
    FgLinux *fg=open_provider(module);Scene scene(512,288);
    const uint64_t ids[]={10,11,14,15,16,17,18,19};
    for(unsigned i=0;i<std::size(ids);i++) {
        if(i==4)fg_linux_reset(fg);
        if(i==6) {fg_linux_set_enabled(fg,false);fg_linux_set_enabled(fg,true);}
        scene.pattern(i,false);begin();scene.upload();auto f=fg_frame(scene,ids[i]);FgLinuxResult result=dispatch(fg,f);
        scene.read_generated_and_inputs();finish();fg_linux_collect(fg,completed_seq);metric(scene,"reset-gap-reenable",ids[i],result);
        CHECK(result==((i==0 || i==2 || i==4 || i==6)?FG_LINUX_WARMUP:FG_LINUX_GENERATED));
    }
    FgLinuxStats stats{};fg_linux_stats(fg,&stats);CHECK(stats.resets==4 && stats.failures==0 && stats.generated_recordings==4);
    CHECK(fg_linux_close(fg,completed_seq));
}
static void recorded_resize(const char *module) {
    FgLinux *fg=open_provider(module);Scene a(512,288),b(640,360);a.pattern(0,false);b.pattern(0,false);
    begin();a.upload();auto af=fg_frame(a,100);CHECK(dispatch(fg,af)==FG_LINUX_WARMUP);a.read(a.generated);
    b.upload();auto bf=fg_frame(b,101);CHECK(dispatch(fg,bf)==FG_LINUX_WARMUP);b.read(b.generated);
    FgLinuxStats stats{};fg_linux_stats(fg,&stats);CHECK(stats.live_contexts==2);
    CHECK(!fg_linux_close(fg,completed_seq)); // Recorded resources cannot retire before this submission completes.
    finish();fg_linux_collect(fg,completed_seq);fg_linux_stats(fg,&stats);CHECK(stats.live_contexts==1);
    CHECK(fg_linux_close(fg,completed_seq));
    printf("PASS recorded resize before submission; old context retained until actual fence completion\n");
}
static void provider_tests(const char *module) {
    char error[256]{};CHECK(!fg_linux_open("/tmp/not-an-installed-framegen-provider.so",physical,device,vkGetDeviceProcAddr,error,sizeof error));CHECK(*error);
    sequence(module,false);sequence(module,true);sequence(module,false,VK_FORMAT_B8G8R8A8_UNORM);history_sequence(module);recorded_resize(module);
    printf("PASS actual SDK offscreen interpolation/static HUD/reset/gap/re-enable/recorded-resize; physical WSI/pacing remains unqualified\n");
}
static void unavailable_provider_test() {
    char error[256]{};CHECK(!fg_linux_open("/tmp/not-an-installed-framegen-provider.so",physical,device,vkGetDeviceProcAddr,error,sizeof error));CHECK(*error);
    CHECK(!fg_linux_available(nullptr));CHECK(fg_linux_dispatch(nullptr,nullptr)==FG_LINUX_DISABLED);CHECK(fg_linux_close(nullptr,completed_seq));
    printf("PASS actual wrapper missing-module/disabled contracts on a real Vulkan device; no SDK code executed\n");
}
#endif
static void shutdown() {
    VK_OK(vkDeviceWaitIdle(device));vkDestroyFence(device,fence,nullptr);vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);
    if(debug_messenger) {auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));CHECK(destroy);destroy(instance,debug_messenger,nullptr);}
    vkDestroyInstance(instance,nullptr);CHECK(!validation_errors);
}
int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IOLBF,0);const char *state{},*module{};bool probe{},headless{};
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--state-root") && i+1<argc)state=argv[++i];
        else if(!strcmp(argv[i],"--resource-probe"))probe=true;
        else if(!strcmp(argv[i],"--headless-wsi-probe"))headless=true;
        else if(!strcmp(argv[i],"--sdk-library") && i+1<argc)module=argv[++i];
        else CHECK(!"unknown argument");
    }
    CHECK(state && probe);private_state(state);create_device(headless);resource_probe();
#ifdef FG_LINUX_PROVIDER
    if(module)provider_tests(module);else unavailable_provider_test();
#else
    CHECK(!module);
#endif
    shutdown();return 0;
}
