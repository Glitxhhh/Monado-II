// [XCDE] LINUX-017: compile the actual authored temporal GLSL; no Vulkan instance or shader cache.
#include <assert.h>
#include <stdio.h>
#include "../../runtime/nvn/vulkan/shaders.c"

// builtin_src also has an unrelated SMAA branch. This fixture never requests it.
const char smaa_hlsl_src[] = "";

int main(void) {
    const char *names[] = {"post.vert", "tmp_prep.frag", "tmp_hud.frag"};
    const int counts[] = {0, 4, 3};
    shaderc_compiler_t comp = shaderc_compiler_initialize();
    assert(comp);
    unsigned cases = 0;
    for (int optimized = 0; optimized < 2; optimized++) {
        shaderc_compile_options_t o = shaderc_compile_options_initialize();
        assert(o);
        shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        shaderc_compile_options_set_optimization_level(o, optimized ? shaderc_optimization_level_performance : shaderc_optimization_level_zero);
        for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
            int stage = -1;
            const char *src = builtin_src(names[i], &stage);
            assert(src && stage == (i == 0 ? STAGE_VERTEX : STAGE_FRAGMENT));
            shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, src, strlen(src), kind_of(stage), names[i], "main", o);
            assert(r);
            if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
                fprintf(stderr, "%s: %s\n", names[i], shaderc_result_get_error_message(r));
                return 1;
            }
            size_t bytes = shaderc_result_get_length(r);
            const uint32_t *spv = (const uint32_t *)shaderc_result_get_bytes(r);
            assert(bytes >= 5 * sizeof *spv && bytes % sizeof *spv == 0 && spv[0] == 0x07230203);
            VkBindingInfo b[8];
            int n = vk_reflect(spv, bytes / sizeof *spv, b, 8);
            assert(n == counts[i]);
            for (int k = 0; k < n; k++) {
                assert(b[k].set == 0 && b[k].binding < (uint32_t)n && b[k].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                for (int j = 0; j < k; j++) assert(b[j].binding != b[k].binding);
            }
            printf("%s optimization=%d: %zu SPIR-V bytes, %d expected sampled bindings\n", names[i], optimized, bytes, n);
            shaderc_result_release(r);
            cases++;
        }
        const char *malformed = "#version 450\nvoid main( {\n";
        shaderc_compilation_result_t bad = shaderc_compile_into_spv(comp, malformed, strlen(malformed),
            shaderc_fragment_shader, "negative.frag", "main", o);
        assert(bad && shaderc_result_get_compilation_status(bad) != shaderc_compilation_status_success);
        shaderc_result_release(bad); cases++;
        shaderc_compile_options_release(o);
    }
    shaderc_compiler_release(comp);
    printf("authored temporal GLSL: %u cases passed; no GPU/game/cache\n", cases);
    return 0;
}
