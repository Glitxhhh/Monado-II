// [XCDE] LINUX-017: actual headless NGX SR/DLAA, entirely synthetic images.
// No window, guest code, game input, renderer-success stubs or human state.
#include "../../runtime/nvn/vulkan/dlss_linux.h"
extern "C" {
#include "../../runtime/platform/linux_state_guard.h"
}
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <linux/io_uring.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define VK_OK(x) do { VkResult result_ = (x); if (result_ != VK_SUCCESS) { fprintf(stderr, "FAIL line %d: %s -> %d\n", __LINE__, #x, result_); exit(1); } } while (0)

static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static VkCommandPool pool;
static VkCommandBuffer command;
static VkFence fence;
static uint64_t recording_seq = 1, completed_seq;

static void network_policy() {
    auto denied = [](int family, int type) {
        errno = 0;
        int fd = socket(family, type | SOCK_CLOEXEC, 0), error = errno;
        if (fd >= 0) close(fd);
        CHECK(fd == -1 && error == EPERM);
        printf("NETWORK_PROBE {\"tid\":%ld,\"family\":%d,\"type\":%d,\"errno\":%d}\n",
            syscall(SYS_gettid), family, type | SOCK_CLOEXEC, error);
    };
    denied(AF_INET, SOCK_DGRAM); denied(AF_INET, SOCK_STREAM);
    denied(AF_INET6, SOCK_DGRAM); denied(AF_INET6, SOCK_STREAM);
    int local = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); CHECK(local >= 0); CHECK(close(local) == 0);
    std::thread child([&] { denied(AF_INET, SOCK_DGRAM); }); child.join();
    io_uring_params params{};
    errno = 0; long ring = syscall(SYS_io_uring_setup, 1, &params); int error = errno;
    if (ring >= 0) close(static_cast<int>(ring));
    CHECK(ring == -1 && error == EPERM);
    printf("NETWORK_POLICY IPv4/IPv6 UDP/TCP and new-thread IP denied EPERM; Unix allowed; io_uring denied; no sends\n");
}

static void private_state(const char *state) {
    char canonical[PATH_MAX], error[256];
    CHECK(state && realpath(state, canonical) && !strcmp(state, canonical));
    CHECK(chdir(canonical) == 0);
    const char *names[] = {"HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "MESA_SHADER_CACHE_DIR", "__GL_SHADER_DISK_CACHE_PATH", "TMPDIR"};
    const char *directories[] = {"home", "cache", "config", "data", "mesa", "nvidia", "tmp"};
    for (unsigned i = 0; i < std::size(names); ++i) {
        std::string path = std::string(canonical) + "/" + directories[i];
        CHECK(mkdir(path.c_str(), 0700) == 0);
        CHECK(setenv(names[i], path.c_str(), 1) == 0);
    }
    if (!rt_linux_state_guard(canonical, error, sizeof error)) {
        fprintf(stderr, "state guard: %s\n", error); exit(1);
    }
    printf("STATE guarded=%s; no game input or human profile\n", canonical);
}

static void begin() {
    VK_OK(vkResetCommandBuffer(command, 0));
    VkCommandBufferBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_OK(vkBeginCommandBuffer(command, &info));
}

static void barrier(VkPipelineStageFlags source, VkAccessFlags source_access,
                    VkPipelineStageFlags dest, VkAccessFlags dest_access) {
    VkMemoryBarrier memory{};
    memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memory.srcAccessMask = source_access;
    memory.dstAccessMask = dest_access;
    vkCmdPipelineBarrier(command, source, dest, 0, 1, &memory, 0, nullptr, 0, nullptr);
}

static void finish() {
    VK_OK(vkEndCommandBuffer(command));
    VK_OK(vkResetFences(device, 1, &fence));
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VK_OK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_OK(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull));
    completed_seq = recording_seq++;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & required) == required) {
            found = i;
            if ((props.memoryTypes[i].propertyFlags & preferred) == preferred) break;
        }
    }
    CHECK(found != UINT32_MAX);
    return found;
}

struct Image {
    DlssImage desc{};
    VkDeviceMemory memory{};
    bool initialized{};
    Image(VkFormat format, uint32_t width, uint32_t height) {
        desc.format = format; desc.width = width; desc.height = height;
        VkFormatProperties props{}; vkGetPhysicalDeviceFormatProperties(physical, format, &props);
        CHECK((props.optimalTilingFeatures & (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) ==
              (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT));
        VkImageCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        create.imageType = VK_IMAGE_TYPE_2D; create.format = format;
        create.extent = {width, height, 1}; create.mipLevels = 1; create.arrayLayers = 1;
        create.samples = VK_SAMPLE_COUNT_1_BIT; create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_OK(vkCreateImage(device, &create, nullptr, &desc.image));
        VkMemoryRequirements required{}; vkGetImageMemoryRequirements(device, desc.image, &required);
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; allocate.allocationSize = required.size;
        allocate.memoryTypeIndex = memory_type(required.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_OK(vkAllocateMemory(device, &allocate, nullptr, &memory));
        VK_OK(vkBindImageMemory(device, desc.image, memory, 0));
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = desc.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_OK(vkCreateImageView(device, &view, nullptr, &desc.view));
    }
    Image(const Image &) = delete;
    ~Image() { vkDestroyImageView(device, desc.view, nullptr); vkDestroyImage(device, desc.image, nullptr); vkFreeMemory(device, memory, nullptr); }
    void general() {
        if (initialized) return;
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = desc.image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        initialized = true;
    }
};

struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    unsigned char *host{};
    bool coherent{};
    Buffer(size_t size) {
        VkBufferCreateInfo create{}; create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        create.size = size; create.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_OK(vkCreateBuffer(device, &create, nullptr, &buffer));
        VkMemoryRequirements required{}; vkGetBufferMemoryRequirements(device, buffer, &required);
        uint32_t type = memory_type(required.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkPhysicalDeviceMemoryProperties props{}; vkGetPhysicalDeviceMemoryProperties(physical, &props);
        coherent = !!(props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo allocate{}; allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = required.size; allocate.memoryTypeIndex = type;
        VK_OK(vkAllocateMemory(device, &allocate, nullptr, &memory));
        VK_OK(vkBindBufferMemory(device, buffer, memory, 0));
        void *mapped{}; VK_OK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
        host = static_cast<unsigned char *>(mapped);
    }
    Buffer(const Buffer &) = delete;
    ~Buffer() { vkUnmapMemory(device, memory); vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr); }
    void sync(bool invalidate) {
        if (coherent) return;
        VkMappedMemoryRange range{}; range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = memory; range.size = VK_WHOLE_SIZE;
        if (invalidate) { VK_OK(vkInvalidateMappedMemoryRanges(device, 1, &range)); }
        else { VK_OK(vkFlushMappedMemoryRanges(device, 1, &range)); }
    }
};

// All patterns, depth and half-float motion are authored synthetic test data.
// Convert finite small floats to IEEE binary16 without relying on CPU F16C.
static uint16_t half(float value) {
    uint32_t bits; memcpy(&bits, &value, sizeof bits);
    const uint32_t sign = (bits >> 16) & 0x8000;
    int exponent = static_cast<int>((bits >> 23) & 255) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffff;
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa = (mantissa | 0x800000) >> (1 - exponent);
        return static_cast<uint16_t>(sign | ((mantissa + 0x1000) >> 13));
    }
    CHECK(exponent < 31);
    uint32_t rounded = (mantissa + 0x1000) >> 13;
    if (rounded == 0x400) { rounded = 0; ++exponent; }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | rounded);
}

struct Metrics { std::array<double, 4> mean{}; uint64_t hash{}; size_t changed{}; };

struct Scene {
    Image color, motion, depth, output;
    const size_t pixels, output_bytes, motion_offset, depth_offset, read_offset;
    Buffer staging;
    Scene(uint32_t width, uint32_t height, uint32_t ow, uint32_t oh)
        : color(VK_FORMAT_R8G8B8A8_UNORM, width, height), motion(VK_FORMAT_R16G16_SFLOAT, width, height),
          depth(VK_FORMAT_R32_SFLOAT, width, height), output(VK_FORMAT_R8G8B8A8_UNORM, ow, oh),
          pixels(static_cast<size_t>(width) * height), output_bytes(static_cast<size_t>(ow) * oh * 4),
          motion_offset(pixels * 4), depth_offset(pixels * 8), read_offset(pixels * 12), staging(read_offset + output_bytes) {}
    void pattern(unsigned frame, unsigned variant) {
        auto *mv = reinterpret_cast<uint16_t *>(staging.host + motion_offset);
        auto *z = reinterpret_cast<float *>(staging.host + depth_offset);
        for (uint32_t y = 0; y < color.desc.height; ++y) for (uint32_t x = 0; x < color.desc.width; ++x) {
            size_t p = static_cast<size_t>(y) * color.desc.width + x;
            bool rectangle = x >= color.desc.width / 4 + frame && x < color.desc.width / 2 + frame && y > color.desc.height / 4 && y < color.desc.height * 3 / 4;
            unsigned checker = ((x / 16) ^ (y / 16)) & 1;
            staging.host[p * 4 + 0] = static_cast<unsigned char>((variant ? 155 : 25) + (x * 35 / color.desc.width) + checker * 12 + (rectangle ? 35 : 0));
            staging.host[p * 4 + 1] = static_cast<unsigned char>((variant ? 30 : 145) + y * 40 / color.desc.height + (rectangle ? 20 : 0));
            staging.host[p * 4 + 2] = static_cast<unsigned char>(45 + checker * 50 + (rectangle ? 25 : 0));
            staging.host[p * 4 + 3] = 255;
            // NGX provider consumes normalized UV motion; +1 pixel current-to-previous is negative x.
            mv[p * 2] = half(rectangle && frame ? -1.0f / color.desc.width : 0.0f);
            mv[p * 2 + 1] = 0;
            z[p] = rectangle ? 0.3f : 0.8f;
        }
        staging.sync(false);
    }
    void upload() {
        color.general(); motion.general(); depth.general(); output.general();
        barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        Image *images[] = {&color, &motion, &depth};
        size_t offsets[] = {0, motion_offset, depth_offset};
        for (unsigned i = 0; i < 3; ++i) {
            VkBufferImageCopy copy{}; copy.bufferOffset = offsets[i];
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {color.desc.width, color.desc.height, 1};
            vkCmdCopyBufferToImage(command, staging.buffer, images[i]->desc.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        }
        VkClearColorValue sentinel{{0.87f, 0.13f, 0.67f, 0.42f}};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(command, output.desc.image, VK_IMAGE_LAYOUT_GENERAL, &sentinel, 1, &range);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    }
    void evaluate(int quality, unsigned frame, bool reset) {
        DlssFrame input{};
        input.color = color.desc; input.motion = motion.desc; input.depth = depth.desc; input.output = output.desc;
        // Authored small alternating subpixel jitter in render-pixel units.
        input.jitter[0] = frame & 1 ? 0.25f : -0.25f;
        input.jitter[1] = frame & 2 ? 0.25f : -0.25f;
        input.reset = reset; input.quality = quality;
        input.recording_seq = recording_seq; input.completed_seq = completed_seq;
        CHECK(ngx_linux_evaluate(command, &input));
    }
    void reject_contracts(int quality) {
        DlssFrame input{};
        input.color = color.desc; input.motion = motion.desc; input.depth = depth.desc; input.output = output.desc;
        input.quality = quality;
        input.recording_seq = recording_seq; input.completed_seq = completed_seq;
        auto bad = input; bad.color.format = VK_FORMAT_R32_SFLOAT; CHECK(!ngx_linux_evaluate(command, &bad));
        bad = input; bad.motion.width++; CHECK(!ngx_linux_evaluate(command, &bad));
        bad = input; bad.depth.height++; CHECK(!ngx_linux_evaluate(command, &bad));
        bad = input; bad.output.view = VK_NULL_HANDLE; CHECK(!ngx_linux_evaluate(command, &bad));
        bad = input; bad.quality = 5; CHECK(!ngx_linux_evaluate(command, &bad));
        bad = input; bad.recording_seq = 0; CHECK(!ngx_linux_evaluate(command, &bad));
        bad = input; bad.completed_seq = bad.recording_seq; CHECK(!ngx_linux_evaluate(command, &bad));
        bad = input; bad.jitter[0] = NAN; CHECK(!ngx_linux_evaluate(command, &bad));
        printf("NEGATIVE format,dimensions,view,quality,sequence,nonfinite-jitter safely rejected\n");
    }
    void copy_output() {
        barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{}; copy.bufferOffset = read_offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {output.desc.width, output.desc.height, 1};
        vkCmdCopyImageToBuffer(command, output.desc.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &copy);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    }
    Metrics metrics(const char *name, int quality, unsigned frame, bool save) {
        staging.sync(true);
        const unsigned char *bytes = staging.host + read_offset;
        Metrics result{}; result.hash = 14695981039346656037ull;
        unsigned minima[4] = {255, 255, 255, 255}, maxima[4] = {0, 0, 0, 0};
        size_t count = output_bytes / 4;
        for (size_t p = 0; p < count; ++p) {
            bool sentinel = true;
            const int clear[] = {222, 33, 171, 107};
            for (unsigned c = 0; c < 4; ++c) {
                unsigned value = bytes[p * 4 + c];
                result.mean[c] += value;
                minima[c] = std::min(minima[c], value); maxima[c] = std::max(maxima[c], value);
                sentinel &= std::abs(static_cast<int>(value) - clear[c]) <= 1;
                result.hash = (result.hash ^ value) * 1099511628211ull;
            }
            result.changed += !sentinel;
        }
        for (double &mean : result.mean) mean /= count;
        CHECK(result.changed > count * 9 / 10);
        CHECK(maxima[0] - minima[0] > 12 && maxima[1] - minima[1] > 12 && maxima[2] - minima[2] > 12);
        printf("METRIC {\"case\":\"%s\",\"quality\":%d,\"frame\":%u,\"width\":%u,\"height\":%u,\"changed_pixels\":%zu,\"pixels\":%zu,\"mean\":[%.5f,%.5f,%.5f,%.5f],\"min\":[%u,%u,%u,%u],\"max\":[%u,%u,%u,%u],\"fnv64\":\"%016llx\"}\n",
            name, quality, frame, output.desc.width, output.desc.height, result.changed, count,
            result.mean[0], result.mean[1], result.mean[2], result.mean[3], minima[0], minima[1], minima[2], minima[3],
            maxima[0], maxima[1], maxima[2], maxima[3], static_cast<unsigned long long>(result.hash));
        if (save) {
            char path[128]; CHECK(snprintf(path, sizeof path, "%s-q%d-f%u.rgba", name, quality, frame) < static_cast<int>(sizeof path));
            FILE *file = fopen(path, "wb"); CHECK(file);
            CHECK(fwrite(bytes, 1, output_bytes, file) == output_bytes); CHECK(fclose(file) == 0);
        }
        return result;
    }
};

static void create_device() {
    const char *extensions[64]{};
    int instance_count = ngx_linux_instance_exts(extensions, 64);
    CHECK(instance_count >= 0 && instance_count <= 64);
    VkApplicationInfo app{}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Monado synthetic DLSS component"; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo create{}; create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &app; create.enabledExtensionCount = static_cast<uint32_t>(instance_count); create.ppEnabledExtensionNames = extensions;
    VK_OK(vkCreateInstance(&create, nullptr, &instance));
    uint32_t count{}; VK_OK(vkEnumeratePhysicalDevices(instance, &count, nullptr)); CHECK(count > 0 && count < 64);
    std::vector<VkPhysicalDevice> devices(count); VK_OK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    for (auto candidate : devices) {
        VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(candidate, &props);
        printf("DEVICE name=%s vendor=%#x device=%#x\n", props.deviceName, props.vendorID, props.deviceID);
        if (props.vendorID == 0x10de && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) physical = candidate;
    }
    CHECK(physical != VK_NULL_HANDLE);
    VkPhysicalDeviceDriverProperties driver{}; driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 props{}; props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; props.pNext = &driver;
    vkGetPhysicalDeviceProperties2(physical, &props);
    printf("SELECTED name=%s driver=%s info=%s api=%u.%u.%u\n", props.properties.deviceName, driver.driverName, driver.driverInfo,
        VK_API_VERSION_MAJOR(props.properties.apiVersion), VK_API_VERSION_MINOR(props.properties.apiVersion), VK_API_VERSION_PATCH(props.properties.apiVersion));
    uint32_t layers{}; VK_OK(vkEnumerateInstanceLayerProperties(&layers, nullptr));
    std::vector<VkLayerProperties> layer(layers); VK_OK(vkEnumerateInstanceLayerProperties(&layers, layer.data()));
    bool validation{}; for (const auto &entry : layer) validation |= !strcmp(entry.layerName, "VK_LAYER_KHRONOS_validation");
    printf("VALIDATION available=%d enabled=0; no validation-pass claim\n", validation);
    uint32_t extension_count{};
    CHECK(ngx_linux_device_exts(instance, physical, extensions, &extension_count, 64));
    CHECK(extension_count <= 64);
    for (uint32_t i = 0; i < extension_count; ++i) printf("EXTENSION %s\n", extensions[i]);
    VkPhysicalDeviceVulkan13Features f13{}; f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{}; f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES; f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 supported{}; supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; supported.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(physical, &supported);
    CHECK(supported.features.shaderInt64 && f12.shaderFloat16 && f12.bufferDeviceAddress && f13.synchronization2);
    // Enable the same four explicit DLSS-related features as the production integration.
    VkPhysicalDeviceFeatures features{}; features.shaderInt64 = VK_TRUE;
    f12 = {}; f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES; f12.pNext = &f13;
    f12.shaderFloat16 = VK_TRUE; f12.bufferDeviceAddress = VK_TRUE;
    f13 = {}; f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES; f13.synchronization2 = VK_TRUE;
    uint32_t families{}; vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family(families); vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, family.data());
    uint32_t qfam = UINT32_MAX;
    for (uint32_t i = 0; i < families; ++i) if ((family[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { qfam = i; break; }
    CHECK(qfam != UINT32_MAX);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create{}; queue_create.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create.queueFamilyIndex = qfam; queue_create.queueCount = 1; queue_create.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_create{}; device_create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create.pNext = &f12; device_create.pEnabledFeatures = &features;
    device_create.queueCreateInfoCount = 1; device_create.pQueueCreateInfos = &queue_create;
    device_create.enabledExtensionCount = extension_count; device_create.ppEnabledExtensionNames = extensions;
    VK_OK(vkCreateDevice(physical, &device_create, nullptr, &device)); vkGetDeviceQueue(device, qfam, 0, &queue);
    ngx_linux_device_created(instance, physical, device, qfam); CHECK(ngx_linux_available());
    VkCommandPoolCreateInfo pool_create{}; pool_create.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_create.queueFamilyIndex = qfam; pool_create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_OK(vkCreateCommandPool(device, &pool_create, nullptr, &pool));
    VkCommandBufferAllocateInfo command_create{}; command_create.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_create.commandPool = pool; command_create.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; command_create.commandBufferCount = 1;
    VK_OK(vkAllocateCommandBuffers(device, &command_create, &command));
    VkFenceCreateInfo fence_create{}; fence_create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VK_OK(vkCreateFence(device, &fence_create, nullptr, &fence));
    printf("CAPABILITY available=1 features=shaderInt64,shaderFloat16,bufferDeviceAddress,synchronization2\n");
}

static std::array<uint32_t, 2> optimal(uint32_t ow, uint32_t oh, int quality) {
    uint32_t width{}, height{}; CHECK(ngx_linux_optimal(ow, oh, quality, &width, &height));
    CHECK(width > 0 && height > 0 && width <= ow && height <= oh);
    if (!quality) CHECK(width == ow && height == oh);
    printf("OPTIMAL quality=%d input=%ux%u output=%ux%u\n", quality, width, height, ow, oh);
    return {width, height};
}

static void sequence(const char *name, int quality, uint32_t ow, uint32_t oh, unsigned frames) {
    auto input = optimal(ow, oh, quality);
    Scene scene(input[0], input[1], ow, oh);
    Metrics cold{}, changed{};
    for (unsigned frame = 0; frame < frames; ++frame) {
        unsigned variant = frame >= frames / 2;
        scene.pattern(frame % (frames / 2), variant);
        begin(); scene.upload();
        if (frame == 0) scene.reject_contracts(quality);
        scene.evaluate(quality, frame, frame == 0 || frame == frames / 2); scene.copy_output(); finish();
        Metrics result = scene.metrics(name, quality, frame, frame == 0 || frame == frames - 1);
        if (frame == 0) cold = result;
        if (frame == frames - 1) changed = result;
    }
    // This independent color discontinuity proves more than a changed output sentinel.
    CHECK(changed.mean[0] > cold.mean[0] + 40.0);
    CHECK(changed.mean[1] + 40.0 < cold.mean[1]);
    CHECK(changed.hash != cold.hash);
    VK_OK(vkDeviceWaitIdle(device)); ngx_linux_reset();
}

static void recorded_lifetime() {
    // Two quality/size handles are recorded before either can complete. Old handle
    // release must use completed_seq, not a waitIdle on unsubmitted commands.
    auto a = optimal(960, 540, 0), b = optimal(1280, 720, 1);
    Scene native(a[0], a[1], 960, 540), scaled(b[0], b[1], 1280, 720);
    native.pattern(0, 0); scaled.pattern(0, 1);
    begin(); native.upload(); native.evaluate(0, 0, true); native.copy_output();
    scaled.upload(); scaled.evaluate(1, 0, true); scaled.copy_output(); finish();
    auto ma = native.metrics("recorded-native", 0, 0, true);
    auto mb = scaled.metrics("recorded-scaled", 1, 0, true);
    CHECK(mb.mean[0] > ma.mean[0] + 40.0);
    // Evaluate once with updated completed_seq to exercise deferred collection.
    scaled.pattern(1, 1); begin(); scaled.upload(); scaled.evaluate(1, 1, false); scaled.copy_output(); finish();
    scaled.metrics("recorded-scaled", 1, 1, false);
    VK_OK(vkDeviceWaitIdle(device)); ngx_linux_reset();
    printf("LIFETIME recorded-mode-resize-before-submit PASS\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char *state{}, *feature{}; bool absent{}, capability{}, ascii_alias{};
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--state-root") && i + 1 < argc) state = argv[++i];
        else if (!strcmp(argv[i], "--feature-dir") && i + 1 < argc) feature = argv[++i];
        else if (!strcmp(argv[i], "--absent-runtime")) absent = true;
        else if (!strcmp(argv[i], "--capability-only")) capability = true;
        else if (!strcmp(argv[i], "--ascii-data-alias")) ascii_alias = true;
        else CHECK(!"unknown argument");
    }
    CHECK(state && feature); private_state(state);
    CHECK(!ngx_linux_available());
    CHECK(!ngx_linux_evaluate(VK_NULL_HANDLE, nullptr));
    std::string data = ascii_alias ? "/tmp/ngx-data" : std::string(state) + "/ngx";
    if (ascii_alias) {
        struct stat info{}; CHECK(lstat(data.c_str(), &info) == 0 && S_ISDIR(info.st_mode));
    } else { CHECK(mkdir(data.c_str(), 0700) == 0); }
    if (absent) {
        CHECK(!ngx_linux_init(feature, data.c_str()));
        CHECK(!ngx_linux_available());
        uint32_t width{}, height{}; CHECK(!ngx_linux_optimal(1280, 720, 1, &width, &height));
        CHECK(!ngx_linux_evaluate(VK_NULL_HANDLE, nullptr));
        ngx_linux_reset(); ngx_linux_shutdown();
        printf("PASS absent-runtime safe unavailable; no Vulkan instance/device created\n"); return 0;
    }
    CHECK(ngx_linux_init(feature, data.c_str())); network_policy(); create_device();
    uint32_t width{}, height{};
    CHECK(!ngx_linux_optimal(0, 720, 1, &width, &height));
    CHECK(!ngx_linux_optimal(1280, 720, -1, &width, &height));
    for (int quality = 0; quality < 5; ++quality) optimal(1280, 720, quality);
    begin(); DlssFrame invalid{}; CHECK(!ngx_linux_evaluate(command, &invalid)); finish();
    printf("NEGATIVE invalid-dimensions,quality,resources safely rejected\n");
    if (!capability) {
        sequence("dlaa", 0, 1280, 720, 8);
        sequence("quality", 1, 1280, 720, 12);
        for (int quality = 2; quality < 5; ++quality) sequence("mode", quality, 1280, 720, 4);
        sequence("resize", 1, 960, 540, 4);
        recorded_lifetime();
        sequence("recreate", 0, 960, 540, 4);
    }
    VK_OK(vkDeviceWaitIdle(device)); ngx_linux_reset(); ngx_linux_shutdown();
    CHECK(!ngx_linux_available());
    vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);
    printf("PASS %s; synthetic component only, gameplay temporal quality untested\n", capability ? "capability/init/contracts" : "DLAA, SR modes, reset, resize, deferred lifetime and readback");
    return 0;
}
