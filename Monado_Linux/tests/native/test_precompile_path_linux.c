// [XCDE] LINUX-004: overlong root must fail before any cache access or GPU work.
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#include "../../runtime/nvn/vulkan/precompile.c"

// GPU/worker entry points are forbidden for these path-only scenarios.
void ui_window_init(void) { abort(); }
int plat_cpu_count(void) { abort(); }
void plat_thread_background(void) { abort(); }
uint64_t vk_spirv_hits, vk_spirv_misses;
bool vk_spirv_cache_flush(void) { abort(); }
uint64_t vk_spirv_cache_errors(void) { abort(); }
bool vk_pipeline_cache_save_checked(void) { abort(); }
size_t vk_precompile_variant_size(void) { abort(); }
VkG g_vk;
int vk_precompile_program(const NvnProgram *program, const ShaderKey *vertex, const ShaderKey *fragment, uint32_t *attributes) {
    (void)program; (void)vertex; (void)fragment; (void)attributes; abort();
}
int vk_precompile_variant(const NvnProgram *program, const void *record, size_t size) {
    (void)program; (void)record; (void)size; abort();
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice device, VkPipelineCache cache, const VkAllocationCallbacks *allocator) {
    (void)device; (void)cache; (void)allocator; abort();
}

int main(void) {
    char root[1024]; memset(root, 'x', sizeof root - 1); root[sizeof root - 1] = 0;
    CHECK(vk_precompile_main(root) == 1);
    CHECK(access("cache", F_OK) != 0);
    puts("Precompile rejects truncated RomFS path before cache/GPU access: OK");
    return 0;
}
