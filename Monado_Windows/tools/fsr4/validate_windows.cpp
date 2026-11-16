// [XCDE] Native Windows adaptation of accepted MIT authored Vulkan FSR4 readback fixtures.
// No game/window/input/profile. Missing capabilities/assets are failures, never qualification.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>
#include "../../runtime/platform/windows_optional_library.h"
#include "../../runtime/nvn/vulkan/fsr4_api.h"
static VkResult fsr4_fixture_device(VkPhysicalDevice physical,const VkDeviceCreateInfo*ci,const VkAllocationCallbacks*a,VkDevice*out){
    VkPhysicalDeviceVulkan13Features q13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features q12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};q12.pNext=&q13;
    VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};q.pNext=&q12;vkGetPhysicalDeviceFeatures2(physical,&q);
    VkPhysicalDeviceFloatControlsProperties fc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};sg.pNext=&fc;
    VkPhysicalDevicePushDescriptorPropertiesKHR pd{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};pd.pNext=&sg;
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};props.pNext=&pd;vkGetPhysicalDeviceProperties2(physical,&props);
    if(!q12.shaderFloat16||!q13.shaderIntegerDotProduct||!q.features.shaderInt16||!q.features.shaderStorageImageWriteWithoutFormat||
       !fc.shaderDenormPreserveFloat16||!(sg.supportedStages&VK_SHADER_STAGE_COMPUTE_BIT)||!(sg.supportedOperations&VK_SUBGROUP_FEATURE_QUAD_BIT)||
       pd.maxPushDescriptors<11||props.properties.limits.maxComputeWorkGroupInvocations<256||props.properties.limits.maxComputeWorkGroupSize[0]<256){
        std::fprintf(stderr,"SKIP FSR4 required native capability unavailable on %s: fp16=%u intdot=%u int16=%u denorm16=%u quad=%u push=%u\n",
          props.properties.deviceName,q12.shaderFloat16,q13.shaderIntegerDotProduct,q.features.shaderInt16,fc.shaderDenormPreserveFloat16,sg.supportedOperations,pd.maxPushDescriptors);std::exit(77);
    }
    uint32_t n=0;vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,nullptr);std::vector<VkExtensionProperties>ext(n);vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,ext.data());
    bool push=false;for(auto&e:ext)push|=!strcmp(e.extensionName,VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);if(!push)std::exit(77);
    VkDeviceCreateInfo copy=*ci;std::vector<const char*>names;
    for(uint32_t i=0;i<ci->enabledExtensionCount;i++)names.push_back(ci->ppEnabledExtensionNames[i]);
    names.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);copy.enabledExtensionCount=(uint32_t)names.size();copy.ppEnabledExtensionNames=names.data();
    for(auto*p=(VkBaseOutStructure*)copy.pNext;p;p=p->pNext)if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
        ((VkPhysicalDeviceVulkan13Features*)p)->shaderIntegerDotProduct=VK_TRUE;
    return vkCreateDevice(physical,&copy,a,out);
}
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
static uint32_t timestamp_bits;
static float timestamp_period;
static uint64_t recording_seq = 1, completed_seq;
static uint32_t validation_errors;

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        validation_errors++;
        fprintf(stderr, "VALIDATION_ERROR %s\n", data->pMessage ? data->pMessage : "no text");
    }
    return VK_FALSE;
}
static VkDebugUtilsMessengerEXT debug_messenger;
static bool renderer_feedback_feature,renderer_feedback_local;
static void create_device(bool headless_probe,bool feedback_fixture=false) {
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
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT feedback_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR feedback_local_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
    std::vector<const char *> extra_extensions;
    if(feedback_fixture){
        uint32_t n=0;VK_OK(vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,nullptr));std::vector<VkExtensionProperties> extensions(n);VK_OK(vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,extensions.data()));
        auto extension=[&](const char *name){return std::any_of(extensions.begin(),extensions.end(),[&](const auto&e){return !strcmp(e.extensionName,name);});};
        if(extension(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME)){
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};query.pNext=&feedback_features;vkGetPhysicalDeviceFeatures2(physical,&query);
            renderer_feedback_feature=feedback_features.attachmentFeedbackLoopLayout;
            if(renderer_feedback_feature)extra_extensions.push_back(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
        }
        if(renderer_feedback_feature && extension(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME)){
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};query.pNext=&feedback_local_features;vkGetPhysicalDeviceFeatures2(physical,&query);
            renderer_feedback_local=feedback_local_features.dynamicRenderingLocalRead;
            if(renderer_feedback_local)extra_extensions.push_back(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
        }
        printf("RENDERER_FEEDBACK feature=%u local_read=%u dynamic_rendering=%u\n",renderer_feedback_feature,renderer_feedback_local,f13.dynamicRendering);
    }

    printf("CAPABILITIES {\"gpu\":\"%s\",\"vendor\":%u,\"device\":%u,\"driver\":%u,\"api\":%u,\"headless_surface_advertised\":%s,\"validation_layer\":%s,\"buffer_device_address\":%s,\"float16\":%s,\"int16\":%s,\"subgroup_size_control\":%s}\n",
        selected.deviceName, selected.vendorID, selected.deviceID, selected.driverVersion, selected.apiVersion,
        headless ? "true" : "false", validation ? "true" : "false", f12.bufferDeviceAddress ? "true" : "false",
        f12.shaderFloat16 ? "true" : "false", available.features.shaderInt16 ? "true" : "false", f13.subgroupSizeControl ? "true" : "false");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr); std::vector<VkQueueFamilyProperties> qprops(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, qprops.data()); queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) if ((qprops[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { queue_family = i; break; }
    CHECK(queue_family != UINT32_MAX);
    timestamp_bits = qprops[queue_family].timestampValidBits;
    timestamp_period = selected.limits.timestampPeriod;
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
    if(feedback_fixture){
        enable13.dynamicRendering=f13.dynamicRendering;
        if(renderer_feedback_feature){feedback_features.pNext=enable13.pNext;enable13.pNext=&feedback_features;}
        if(renderer_feedback_local){feedback_local_features.pNext=enable13.pNext;enable13.pNext=&feedback_local_features;}
        dc.enabledExtensionCount=(uint32_t)extra_extensions.size();dc.ppEnabledExtensionNames=extra_extensions.data();
    }

    VK_OK(fsr4_fixture_device(physical, &dc, nullptr, &device)); vkGetDeviceQueue(device, queue_family, 0, &queue);
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
    VkImage image{}; VkImageView view{}; VkDeviceMemory memory{}; VkFormat format; uint32_t width, height,levels; bool initialized{};
    Image(VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags extra_usage=0,uint32_t mip_levels=1) : format(f), width(w), height(h),levels(mip_levels) {
        VkFormatProperties p{}; vkGetPhysicalDeviceFormatProperties(physical, format, &p);
        CHECK((p.optimalTilingFeatures & (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) == (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT));
        VkImageCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format;
        ci.extent = {w, h, 1}; ci.mipLevels = levels;ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extra_usage;
        VK_OK(vkCreateImage(device, &ci, nullptr, &image)); VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(device, image, &requirements);
        VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = requirements.size; ai.memoryTypeIndex = memory_type(requirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_OK(vkAllocateMemory(device, &ai, nullptr, &memory)); VK_OK(vkBindImageMemory(device, image, memory, 0));
        VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        VK_OK(vkCreateImageView(device, &vi, nullptr, &view));
    }
    Image(const Image &) = delete;
    ~Image() { vkDestroyImageView(device, view, nullptr); vkDestroyImage(device, image, nullptr); vkFreeMemory(device, memory, nullptr); }
    void dependency(VkAccessFlags destination) {
        VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = initialized ? VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT : 0;
        b.dstAccessMask = destination; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        vkCmdPipelineBarrier(command, initialized ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b); initialized = true;
    }
};
static uint16_t half(float f) {
    uint32_t value{}; memcpy(&value, &f, sizeof value); unsigned sign = value >> 16 & 0x8000, exponent = value >> 23 & 0xff, mantissa = value & 0x7fffff;
    if (!exponent || exponent < 113) return static_cast<uint16_t>(sign);
    CHECK(exponent < 143); return static_cast<uint16_t>(sign | (exponent - 112) << 10 | (mantissa + 0x1000) >> 13);
}
static void shutdown() {
    VK_OK(vkDeviceWaitIdle(device));vkDestroyFence(device,fence,nullptr);vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);
    if(debug_messenger) {auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));CHECK(destroy);destroy(instance,debug_messenger,nullptr);}
    vkDestroyInstance(instance,nullptr);CHECK(!validation_errors);
}
static float unhalf4(uint16_t h){unsigned s=h>>15,e=(h>>10)&31,m=h&1023;float f=e==0?std::ldexp((float)m,-24):e==31?INFINITY:std::ldexp(1.f+m/1024.f,(int)e-15);return s?-f:f;}
static MonadoFsr4Image fsr_image(const Image&i){return{i.image,i.view,i.width,i.height};}
static void exercise(const MonadoFsr4Api*api,const char*assets,unsigned rw,unsigned rh,unsigned ow,unsigned oh,bool ultra,unsigned frames){
    void*context=api->create(physical,device,assets);CHECK(context);
    if(!api->build(context,ow,oh,ultra)){fprintf(stderr,"FSR4 build error: %s\n",api->error(context));exit(1);}
    CHECK(!api->build(context,0,oh,ultra));CHECK(!api->record(context,nullptr));
    Image color(VK_FORMAT_R16G16B16A16_SFLOAT,rw,rh),depth(VK_FORMAT_R32_SFLOAT,rw,rh),motion(VK_FORMAT_R16G16_SFLOAT,rw,rh),output(VK_FORMAT_R16G16B16A16_SFLOAT,ow,oh);
    const size_t pixels=size_t(rw)*rh,out_pixels=size_t(ow)*oh,motion_offset=pixels*8,depth_offset=motion_offset+pixels*4,read_offset=(depth_offset+pixels*4+255)&~size_t(255);
    Buffer buffer(read_offset+out_pixels*8);double first=0,last=0;
    for(unsigned frame=0;frame<frames;frame++){
        float jitter=(frame&1)?.25f:-.25f;auto*c=(uint16_t*)buffer.host,*m=(uint16_t*)(buffer.host+motion_offset);auto*z=(float*)(buffer.host+depth_offset);
        for(unsigned y=0;y<rh;y++)for(unsigned x=0;x<rw;x++){
            size_t i=size_t(y)*rw+x;bool object=x>rw/4+frame&&x<rw/2+frame&&y>rh/4&&y<rh*3/4;
            c[i*4]=half((frame>=8?.4f:.1f)+(object?.2f:0));c[i*4+1]=half(.15f+.55f*(.5f+.5f*cos((x+jitter)*.23f)));c[i*4+2]=half(.1f+.4f*y/rh);c[i*4+3]=half(1);
            m[i*2]=half(object&&frame?-1.f/rw:0);m[i*2+1]=0;z[i]=object?.3f:.8f;
        }
        buffer.sync(true);begin();Image*inputs[]={&color,&motion,&depth};const size_t offsets[]={0,motion_offset,depth_offset};
        for(unsigned i=0;i<3;i++){inputs[i]->dependency(VK_ACCESS_TRANSFER_WRITE_BIT);VkBufferImageCopy copy{};copy.bufferOffset=offsets[i];copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={rw,rh,1};vkCmdCopyBufferToImage(command,buffer.buffer,inputs[i]->image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);}
        output.dependency(VK_ACCESS_TRANSFER_WRITE_BIT);VkClearColorValue sentinel{{.95f,.01f,.95f,.5f}};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(command,output.image,VK_IMAGE_LAYOUT_GENERAL,&sentinel,1,&range);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        MonadoFsr4Frame input{};input.cmd=command;input.color=fsr_image(color);input.depth=fsr_image(depth);input.motion=fsr_image(motion);input.output=fsr_image(output);input.jitter[0]=input.jitter[1]=jitter;
        input.reset=frame==0||frame==8;input.ultra=ultra;input.sharpness=frame&1?.3f:0;
        if(!api->record(context,&input)){fprintf(stderr,"FSR4 record error: %s\n",api->error(context));exit(1);}
        output.dependency(VK_ACCESS_TRANSFER_READ_BIT);VkBufferImageCopy copy{};copy.bufferOffset=read_offset;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={ow,oh,1};vkCmdCopyImageToBuffer(command,output.image,VK_IMAGE_LAYOUT_GENERAL,buffer.buffer,1,&copy);finish();buffer.sync(false);
        auto*data=(uint16_t*)(buffer.host+read_offset);size_t changed=0;double red=0;float low=10,high=-10;
        for(size_t i=0;i<out_pixels;i++){float r=unhalf4(data[i*4]),g=unhalf4(data[i*4+1]),b=unhalf4(data[i*4+2]);CHECK(std::isfinite(r)&&std::isfinite(g)&&std::isfinite(b));CHECK(r>=-.1f&&r<=4&&g>=-.1f&&g<=4&&b>=-.1f&&b<=4);red+=r;low=std::min(low,g);high=std::max(high,g);changed+=fabs(r-.95f)>.01||fabs(g-.01f)>.01;}
        red/=out_pixels;fprintf(stderr,"FSR4 diagnostic dims %ux%u->%ux%u ultra=%u frame=%u red=%.6f lowG=%.6f highG=%.6f changed=%zu\n",rw,rh,ow,oh,ultra,frame,red,low,high,changed);CHECK(changed>out_pixels*99/100);CHECK(high-low>.15f);if(frame==0)first=red;last=red;
        printf("FSR4 native frame=%u %ux%u->%ux%u ultra=%u reset=%u sharp=%.1f meanR=%.5f contrastG=%.5f changed=%zu finite=1\n",frame,rw,rh,ow,oh,ultra,input.reset,input.sharpness,red,high-low,changed);
    }
    if(frames>8)CHECK(last>first+.15);VK_OK(vkDeviceWaitIdle(device));api->destroy(context);
}
int wmain(int argc,wchar_t **argv){
    if(argc!=3){fputs("usage: fsr4-validate.exe COMPONENT ABS_ASSETS\n",stderr);return 2;}
    char library[16384],assets[16384],reason[256];
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[1],-1,library,sizeof library,nullptr,nullptr)||
       !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[2],-1,assets,sizeof assets,nullptr,nullptr))return 2;
    HMODULE module=monado_optional_library_load(argv[1],reason,sizeof reason);
    if(!module){fprintf(stderr,"FSR4 component unavailable: %s\n",reason);return 1;}
    auto get=(MonadoFsr4GetApi)GetProcAddress(module,"monado_fsr4_get_api");CHECK(get&&!get(999));
    auto*api=get(MONADO_FSR4_ABI);CHECK(api&&api->abi==2);create_device(false);
    CHECK(!api->create(VK_NULL_HANDLE,device,assets));CHECK(!api->build(nullptr,320,180,0));CHECK(!api->record(nullptr,nullptr));
    exercise(api,assets,320,180,320,180,false,12);exercise(api,assets,160,90,320,180,false,12);
    exercise(api,assets,640,360,1920,1080,true,3);exercise(api,assets,1286,720,1928,1080,false,3);exercise(api,assets,643,360,1928,1080,true,3);
    CHECK(validation_errors==0);shutdown();FreeLibrary(module);
    puts("MONADO_FSR4_GPU_QUALIFIED all4sets frames33 finite detail reset sharpening readback; no gameplay/performance qualification");return 0;
}
