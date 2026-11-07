// [XCDE] LINUX-006: real settings parsing and explicit unavailable vendor APIs.
// Synthetic local config only; no game input, GPU or user configuration.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/nvn/vulkan/temporal.h"
#include "../../runtime/ui/graphics_options.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
// Unrelated binding parser/saver is not exercised by these graphics-only configs.
bool bind_settings_load(const char *key, const char *val) { (void)key; (void)val; abort(); }
void bind_settings_save(FILE *f) { (void)f; abort(); }
bool dlss_init_early(void);
bool dlss_available(void);
int dlss_instance_exts(const char **out, int max);
void dlss_device_requirements(const char **ext, uint32_t *n, uint32_t max,
    VkPhysicalDeviceVulkan12Features *f12, VkPhysicalDeviceVulkan13Features *f13,
    uint32_t *extra_gfx, uint32_t *extra_compute);
PFN_vkVoidFunction dlss_proxy(VkDevice dev, const char *name);
bool dlss_evaluate(VkCommandBuffer cb, const TexBackend *color, const TexBackend *mv,
    const TexBackend *depth, const TexBackend *out, const TexBackend *hud, const float jitter[2],
    bool reset, const TmpCamera *cam, const double reproj[16]);
bool tmp_frame_gen_wanted(void);
bool tmp_fg_active(void);
bool tmp_fg_install(VkSwapchainKHR *sc, const VkSwapchainCreateInfoKHR *ci, PFN_vkCreateSwapchainKHR *create,
    PFN_vkDestroySwapchainKHR *destroy, PFN_vkGetSwapchainImagesKHR *images,
    PFN_vkAcquireNextImageKHR *acquire, PFN_vkQueuePresentKHR *present);

int main(void) {
    CHECK(MENU_UPSCALER_COUNT == 2);
    CHECK(menu_upscale_values[0] == UPSCALE_BILINEAR && menu_upscale_values[1] == UPSCALE_FSR1);
    CHECK(UPSCALE_FSR3 == 3 && UPSCALE_DLSS == 4); // parser/file enum compatibility
    CHECK(!dlss_init_early() && !dlss_available());
    const char *ext[2] = {"sentinel-0", "sentinel-1"};
    CHECK(dlss_instance_exts(ext, 2) == 0 && !strcmp(ext[0], "sentinel-0"));
    uint32_t n = 2, graphics = 99, compute = 98;
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .timelineSemaphore = VK_TRUE};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceVulkan12Features before12 = f12;
    VkPhysicalDeviceVulkan13Features before13 = f13;
    dlss_device_requirements(ext, &n, 2, &f12, &f13, &graphics, &compute);
    CHECK(n == 2 && graphics == 0 && compute == 0);
    CHECK(!memcmp(&f12, &before12, sizeof f12) && !memcmp(&f13, &before13, sizeof f13));
    CHECK(dlss_proxy(VK_NULL_HANDLE, "vkQueuePresentKHR") == NULL);
    CHECK(!dlss_evaluate(VK_NULL_HANDLE, NULL, NULL, NULL, NULL, NULL, NULL, false, NULL, NULL));
    CHECK(!tmp_enabled() && !tmp_frame_gen_wanted() && !tmp_fg_active());
    float jitter[2] = {1, -1}; tmp_jitter(jitter);
    CHECK(jitter[0] == 0 && jitter[1] == 0 && !tmp_hud_started());
    CHECK(tmp_upscale(VK_NULL_HANDLE, NULL, VK_NULL_HANDLE, NULL, NULL, jitter, false, 1280, 720) == VK_NULL_HANDLE);
    VkSwapchainKHR swapchain = (VkSwapchainKHR)(uintptr_t)0x1234;
    CHECK(!tmp_fg_install(&swapchain, NULL, NULL, NULL, NULL, NULL, NULL));
    CHECK(swapchain == (VkSwapchainKHR)(uintptr_t)0x1234);
    const char *modes[] = {"bilinear", "fsr", "fsr3", "dlss"};
    const int effective[] = {UPSCALE_BILINEAR, UPSCALE_FSR1, UPSCALE_FSR1, UPSCALE_FSR1};
    for (int i = 0; i < 4; i++) {
        char text[256], after[256] = {0};
        snprintf(text, sizeof text, "upscaler = %s\nresolution = match\nupscale_quality = ultra\nframe_gen = 1\ncutscenes = 30fg\nantialias = smaa\n", modes[i]);
        FILE *f = fopen("graphics-test.ini", "wx"); CHECK(f != NULL);
        CHECK(fputs(text, f) >= 0 && fclose(f) == 0);
        CHECK(setenv("XCDE_INI", "graphics-test.ini", 1) == 0);
        settings_load();
        CHECK(g_settings.upscaler == effective[i] && !g_settings.frame_gen && g_settings.cutscene_mode == 1);
        CHECK(g_settings.res_mode == RES_MATCH && g_settings.antialias == AA_SMAA && g_settings.upscale_quality == UQ_ULTRA);
        f = fopen("graphics-test.ini", "r"); CHECK(f != NULL);
        CHECK(fread(after, 1, sizeof after - 1, f) == strlen(text)); CHECK(fclose(f) == 0);
        CHECK(!strcmp(text, after)); // load/fallback must not overwrite imported settings
        CHECK(remove("graphics-test.ini") == 0);
    }
    puts("Linux optional vendor capabilities/settings: OK");
    return 0;
}
