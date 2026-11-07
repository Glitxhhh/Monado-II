// [XCDE] LINUX-018: authored GLSL -> actual SPIR-V cache -> Vulkan pipeline
// -> offscreen pixels, cold and fresh-process warm. No window/game/guest data.
#define _GNU_SOURCE
#include "../../runtime/nvn/vulkan/shaders.c"
#include "../../runtime/nvn/vulkan/pipeline_cache_io.h"
#include "../../runtime/platform/linux_state_guard.h"
#include <limits.h>

const char smaa_hlsl_src[] = ""; // unrelated builtin is not requested
VkG g_vk;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
#define VK_OK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "FAIL %d: %s -> %d\n", __LINE__, #x, r_); exit(1); } } while (0)

static const char vertex[] = "#version 450\nvoid main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
static const char fragment[] = "#version 450\nlayout(location=0) out vec4 color;void main(){color=vec4(0.25,0.5,0.75,1.0);}\n";

static void state_guard(const char *state) {
    char canonical[PATH_MAX], path[PATH_MAX], error[256];
    CHECK(realpath(state, canonical) && !strcmp(state, canonical) && chdir(state) == 0);
    const char *names[] = {"HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME", "MESA_SHADER_CACHE_DIR", "__GL_SHADER_DISK_CACHE_PATH", "TMPDIR"};
    const char *dirs[] = {"home", "cache", "config", "mesa", "nvidia", "tmp"};
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        CHECK(snprintf(path, sizeof path, "%s/%s", state, dirs[i]) < (int)sizeof path);
        CHECK(!mkdir(path, 0700) || errno == EEXIST); CHECK(!setenv(names[i], path, 1));
    }
    CHECK(!mkdir("cache/spirv", 0700) || errno == EEXIST);
    CHECK(!mkdir("runtime", 0700) || errno == EEXIST);
    CHECK(!setenv("XDG_RUNTIME_DIR", "/tmp/runtime", 1));
    CHECK(rt_linux_state_guard(state, error, sizeof error));
}

static bool create_context(bool warm) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Monado authored precompile/cache probe", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VK_OK(vkCreateInstance(&instance, NULL, &g_vk.inst));
    uint32_t count = 0; VK_OK(vkEnumeratePhysicalDevices(g_vk.inst, &count, NULL)); CHECK(count && count < 64);
    VkPhysicalDevice devices[64]; VK_OK(vkEnumeratePhysicalDevices(g_vk.inst, &count, devices));
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(devices[i], &p);
        printf("DEVICE %s vendor=%x device=%x\n", p.deviceName, p.vendorID, p.deviceID);
        if (p.vendorID == 0x10de && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) g_vk.phys = devices[i];
    }
    CHECK(g_vk.phys);
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p); g_vk.props = p.properties;
    printf("SELECTED %s driver=%s %s api=%u.%u.%u\n", p.properties.deviceName, driver.driverName, driver.driverInfo,
        VK_API_VERSION_MAJOR(p.properties.apiVersion), VK_API_VERSION_MINOR(p.properties.apiVersion), VK_API_VERSION_PATCH(p.properties.apiVersion));
    uint32_t layers = 0; VK_OK(vkEnumerateInstanceLayerProperties(&layers, NULL));
    VkLayerProperties *lp = calloc(layers, sizeof *lp); CHECK(lp || !layers);
    VK_OK(vkEnumerateInstanceLayerProperties(&layers, lp));
    bool validation = false; for (uint32_t i = 0; i < layers; ++i) validation |= !strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation");
    free(lp); printf("VALIDATION available=%d enabled=0; no validation-pass claim\n", validation);
    uint32_t ext_count = 0; VK_OK(vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ext_count, NULL));
    VkExtensionProperties *extensions = calloc(ext_count, sizeof *extensions); CHECK(extensions || !ext_count);
    VK_OK(vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ext_count, extensions));
    bool feedback = false;
    for (uint32_t i = 0; i < ext_count; ++i) feedback |= !strcmp(extensions[i].extensionName, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
    free(extensions);
    uint32_t families = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &families, NULL);
    VkQueueFamilyProperties *family = calloc(families, sizeof *family); CHECK(family);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &families, family);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < families; ++i) if (family[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_vk.qfam = i; break; }
    free(family); CHECK(g_vk.qfam != UINT32_MAX);
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_vk.qfam, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE};
    const char *enabled[] = {VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME};
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue, .enabledExtensionCount = feedback, .ppEnabledExtensionNames = enabled};
    VK_OK(vkCreateDevice(g_vk.phys, &device, NULL, &g_vk.dev)); vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    void *data = NULL; size_t bytes = 0;
    if (warm) {
        FILE *f = fopen("cache/vk_pipelines.bin", "rb"); CHECK(f && !fseek(f, 0, SEEK_END));
        long n = ftell(f); CHECK(n >= (long)sizeof(VkPipelineCacheHeaderVersionOne)); bytes = (size_t)n;
        CHECK(!fseek(f, 0, SEEK_SET)); data = malloc(bytes); CHECK(data);
        CHECK(fread(data, 1, bytes, f) == bytes && !fclose(f));
        VkPipelineCacheHeaderVersionOne *header = data;
        CHECK(header->headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && header->vendorID == g_vk.props.vendorID &&
              header->deviceID == g_vk.props.deviceID && !memcmp(header->pipelineCacheUUID, g_vk.props.pipelineCacheUUID, VK_UUID_SIZE));
    }
    VkPipelineCacheCreateInfo cache = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = bytes, .pInitialData = data};
    VK_OK(vkCreatePipelineCache(g_vk.dev, &cache, NULL, &g_vk.pcache)); free(data);
    printf("CACHE_INPUT bytes=%zu warm=%d\n", bytes, warm);
    return feedback;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags prefer, bool *coherent) {
    VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &memory);
    uint32_t chosen = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & need) == need) {
        chosen = i; if ((memory.memoryTypes[i].propertyFlags & prefer) == prefer) break;
    }
    CHECK(chosen != UINT32_MAX);
    if (coherent) *coherent = !!(memory.memoryTypes[chosen].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    return chosen;
}

int main(int argc, char **argv) {
    CHECK(argc == 3 && (!strcmp(argv[1], "cold") || !strcmp(argv[1], "warm")));
    bool warm = !strcmp(argv[1], "warm"); state_guard(argv[2]); bool feedback = create_context(warm);
    VkShaderModule vs = vk_compile_glsl(vertex, STAGE_VERTEX, "authored-precompile.vert");
    VkShaderModule fs = vk_compile_glsl(fragment, STAGE_FRAGMENT, "authored-precompile.frag"); CHECK(vs && fs);
    CHECK(warm ? vk_spirv_hits == 2 && vk_spirv_misses == 0 : vk_spirv_misses == 2 && vk_spirv_hits == 0);
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout; VK_OK(vkCreatePipelineLayout(g_vk.dev, &layout_info, NULL, &layout));
    VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0, 0, 32, 32, 0, 1}; VkRect2D scissor = {{0, 0}, {32, 32}};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo samples = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &attachment};
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
    VkPipelineCreationFeedback pipeline_feedback = {0};
    VkPipelineCreationFeedbackCreateInfo creation_feedback = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
        .pNext = &rendering, .pPipelineCreationFeedback = &pipeline_feedback};
    VkGraphicsPipelineCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = feedback ? (void *)&creation_feedback : &rendering,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
        .pRasterizationState = &raster, .pMultisampleState = &samples, .pColorBlendState = &blend, .layout = layout};
    VkPipeline pipeline; VK_OK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pipeline_info, NULL, &pipeline));
    printf("PIPELINE feedback_available=%d flags=%u duration_ns=%llu; driver disk-cache hit is not inferred from timing\n",
        feedback, pipeline_feedback.flags, (unsigned long long)pipeline_feedback.duration);
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {32, 32, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage image; VK_OK(vkCreateImage(g_vk.dev, &image_info, NULL, &image));
    VkMemoryRequirements requirement; vkGetImageMemoryRequirements(g_vk.dev, image, &requirement);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirement.size,
        .memoryTypeIndex = memory_type(requirement.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL)};
    VkDeviceMemory image_memory; VK_OK(vkAllocateMemory(g_vk.dev, &allocation, NULL, &image_memory)); VK_OK(vkBindImageMemory(g_vk.dev, image, image_memory, 0));
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view; VK_OK(vkCreateImageView(g_vk.dev, &view_info, NULL, &view));
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 32 * 32 * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer; VK_OK(vkCreateBuffer(g_vk.dev, &buffer_info, NULL, &buffer)); vkGetBufferMemoryRequirements(g_vk.dev, buffer, &requirement);
    bool coherent = false;
    allocation.allocationSize = requirement.size;
    allocation.memoryTypeIndex = memory_type(requirement.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &coherent);
    VkDeviceMemory buffer_memory; VK_OK(vkAllocateMemory(g_vk.dev, &allocation, NULL, &buffer_memory)); VK_OK(vkBindBufferMemory(g_vk.dev, buffer, buffer_memory, 0));
    unsigned char *pixels; VK_OK(vkMapMemory(g_vk.dev, buffer_memory, 0, VK_WHOLE_SIZE, 0, (void **)&pixels));
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_vk.qfam};
    VkCommandPool pool; VK_OK(vkCreateCommandPool(g_vk.dev, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command; VK_OK(vkAllocateCommandBuffers(g_vk.dev, &command_info, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_OK(vkBeginCommandBuffer(command, &begin));
    VkImageMemoryBarrier transition = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &transition);
    VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.color.float32 = {1, 0, 1, 1}};
    VkRenderingInfo render = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {32, 32}}, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &color};
    vkCmdBeginRendering(command, &render); vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(command, 3, 1, 0, 0); vkCmdEndRendering(command);
    transition.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; transition.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    transition.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; transition.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &transition);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {32, 32, 1}};
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    VK_OK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; VK_OK(vkCreateFence(g_vk.dev, &fence_info, NULL, &fence));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    VK_OK(vkQueueSubmit(g_vk.queue, 1, &submit, fence)); VK_OK(vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, 10000000000ull));
    if (!coherent) {
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = buffer_memory, .size = VK_WHOLE_SIZE};
        VK_OK(vkInvalidateMappedMemoryRanges(g_vk.dev, 1, &range));
    }
    const int expected[] = {64, 128, 191, 255}; uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < 32 * 32 * 4; ++i) { CHECK(abs((int)pixels[i] - expected[i % 4]) <= 1); hash = (hash ^ pixels[i]) * 1099511628211ull; }
    CHECK(vk_spirv_cache_flush() && !vk_spirv_cache_errors());
    CHECK(vk_pipeline_cache_write_file(g_vk.dev, g_vk.pcache, "cache/vk_pipelines.bin"));
    printf("PASS authored-pipeline warm=%d hits=%llu misses=%llu pixels=1024 fnv64=%016llx\n", warm,
        (unsigned long long)vk_spirv_hits, (unsigned long long)vk_spirv_misses, (unsigned long long)hash);
    vkDestroyFence(g_vk.dev, fence, NULL); vkDestroyCommandPool(g_vk.dev, pool, NULL);
    vkUnmapMemory(g_vk.dev, buffer_memory); vkDestroyBuffer(g_vk.dev, buffer, NULL); vkFreeMemory(g_vk.dev, buffer_memory, NULL);
    vkDestroyImageView(g_vk.dev, view, NULL); vkDestroyImage(g_vk.dev, image, NULL); vkFreeMemory(g_vk.dev, image_memory, NULL);
    vkDestroyPipeline(g_vk.dev, pipeline, NULL); vkDestroyPipelineLayout(g_vk.dev, layout, NULL);
    vkDestroyShaderModule(g_vk.dev, vs, NULL); vkDestroyShaderModule(g_vk.dev, fs, NULL);
    vkDestroyPipelineCache(g_vk.dev, g_vk.pcache, NULL); vkDestroyDevice(g_vk.dev, NULL); vkDestroyInstance(g_vk.inst, NULL);
    return 0;
}
