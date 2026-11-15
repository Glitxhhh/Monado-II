// Checked native heap-import lifecycle, shared with the authored native fixture.
// Included only by the render executor: guest-thread fence scanning must not
// flush command pools or retire a live import.
#include "host_import_memory_windows.h"

static uint64_t gmem_wanted_size(void) {
    uint64_t size = kernel_heap_size();
    if (!size || size > HEAP_MAX) FATAL("[vk] invalid guest heap size for GPU import: %llu\n", (unsigned long long)size);
    size = (size + 0xFFFF) & ~0xFFFFull;
    if (size > HEAP_MAX || size > g_vk.props.limits.maxStorageBufferRange)
        FATAL("[vk] guest heap %llu exceeds the supported global-memory descriptor range %u\n",
              (unsigned long long)size, g_vk.props.limits.maxStorageBufferRange);
    return size;
}

static void gmem_import(uint64_t size) {
    if (gmem.buffer || gmem.memory) FATAL("[vk] attempted to overlap a live guest heap import\n");
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &properties);
    uint64_t alignment = host.minImportedHostPointerAlignment;
    if (!alignment || (alignment & (alignment - 1)) || alignment > HEAP_MAX ||
        size > UINT64_MAX - (alignment - 1)) FATAL("[vk] invalid host-import alignment/extent\n");
    uint64_t accessible = (size + alignment - 1) & ~(alignment - 1);
    if (accessible > HEAP_MAX) FATAL("[vk] aligned host import exceeds the guest reservation\n");
    char error[256];
    if (!windows_host_import_commit((void *)(uintptr_t)HEAP_BASE, (size_t)accessible, error, sizeof error))
        FATAL("[vk] guest heap cannot be imported: %s\n", error);
    VkResult result = vk_host_import_buffer(g_vk.phys, g_vk.dev, (void *)(uintptr_t)HEAP_BASE,
        accessible, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        &gmem, error, sizeof error);
    if (result != VK_SUCCESS) FATAL("[vk] guest heap import failed: %s; refusing incorrect global memory\n", error);
    LOG("[vk] guest heap imported: %llu MB, allocation %llu MB, coherent memory type %u\n",
        (unsigned long long)(gmem.size >> 20), (unsigned long long)(gmem.allocation_size >> 20), gmem.memory_type);
}

static uint64_t gmem_tried;
static VkDescriptorBufferInfo gmem_info(void) {
    if (getenv("LARECOMP_VK_NO_GMEM") || !g_vk.host_import)
        FATAL("[vk] shader global memory requires VK_EXT_external_memory_host; disabled/unsupported import cannot return dummy data\n");
    uint64_t want = gmem_wanted_size();
    if (!gmem.buffer) gmem_import(want);
    // Flushing here would invalidate descriptor/command state held by the draw.
    if (want > gmem.size) FATAL("[vk] guest heap grew during a recorded command; cannot safely replace the import here\n");
    return (VkDescriptorBufferInfo){gmem.buffer, 0, gmem.size};
}

static void gmem_prepare_command(void) {
    if (gmem.buffer) {
        uint64_t want = gmem_wanted_size();
        if (want > gmem.size) {
            // Retire BOTH recorded and submitted work before unpinning pages.
            renderer_flush();
            vk_host_import_destroy(g_vk.dev, &gmem);
            gmem_import(want);
        }
    }
}

static void gmem_reset(void) {
    // In-place reboot calls this at GPU idle, before guest pages are reset.
    vk_host_import_destroy(g_vk.dev, &gmem);
    gmem_tried = 0;
}
