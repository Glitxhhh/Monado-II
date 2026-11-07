// [XCDE] LINUX-020: native interpolation records only; device.c owns ordinary presentation.
#include "framegen_linux.h"
#include "../../paths.h"

static FgLinux *linux_fg;
static bool linux_fg_tried, linux_fg_pending, linux_fg_hudless;
static bool linux_fg_generated, linux_fg_error_logged, linux_fg_rect_valid;
static int32_t linux_fg_rect[4];
static Img linux_fg_real, linux_fg_output, linux_fg_hud;
static FgLinuxFrame linux_fg_frame;
static int linux_fg_quality;

static bool linux_fg_on(void) {
    extern bool xcde_cutscene_30(void);
    return g_settings.frame_gen || (g_settings.cutscene_mode == 2 && xcde_cutscene_30());
}
bool tmp_frame_gen_wanted(void) {
    return (g_settings.frame_gen || g_settings.cutscene_mode == 2) && g_settings.upscaler == UPSCALE_DLSS &&
        !diag_deterministic() && !env_int("XCDE_TMP_OFF") && !getenv("XCDE_PRECOMPILING");
}
static bool linux_fg_load(void) {
    if (linux_fg_tried) return linux_fg != NULL;
    linux_fg_tried = true;
    char path[4096], error[256];
    if (!rt_resource("libmonado-fidelityfx.so", NULL, path, sizeof path)) {
        LOG("[fg] native provider absent; ordinary presentation retained\n"); return false;
    }
    linux_fg = fg_linux_open(path, g_vk.phys, g_vk.dev, vkGetDeviceProcAddr, error, sizeof error);
    if (!linux_fg) { LOG("[fg] native provider unavailable: %s\n", error); return false; }
    LOG("[fg] native FidelityFX interpolation available; rendered FPS remains the game rate\n");
    return true;
}
bool tmp_fg_supported(void) { return linux_fg_load(); }
bool tmp_fg_active(void) { return linux_fg_generated; }
static void linux_fg_reset(void) {
    linux_fg_pending = linux_fg_hudless = linux_fg_generated = linux_fg_rect_valid = false;
    if (linux_fg) { fg_linux_reset(linux_fg); fg_linux_collect(linux_fg, vk_completed_seq()); }
}
void tmp_fg_invalidate(void) { linux_fg_reset(); }
void tmp_fg_idle(VkCommandBuffer cb) { (void)cb; linux_fg_reset(); }
bool tmp_fg_install(VkSwapchainKHR *sc, const VkSwapchainCreateInfoKHR *ci, PFN_vkCreateSwapchainKHR *create,
    PFN_vkDestroySwapchainKHR *destroy, PFN_vkGetSwapchainImagesKHR *images,
    PFN_vkAcquireNextImageKHR *acquire, PFN_vkQueuePresentKHR *present) {
    (void)sc; (void)ci; (void)create; (void)destroy; (void)images; (void)acquire; (void)present;
    return false; // Linux keeps the real swapchain; no proxy installation is claimed.
}
static FgLinuxImage linux_fg_image(const Img *im) {
    return (FgLinuxImage){im->b.image, im->b.format, im->b.width, im->b.height};
}
static void linux_fg_prepare(const TmpCamera *cam, const float jitter[2], uint32_t rw, uint32_t rh, float dt, bool reset) {
    linux_fg_pending = linux_fg_hudless = false;
    linux_fg_generated = false;
    if (!tmp_frame_gen_wanted() || !linux_fg_on()) {
        if (linux_fg) fg_linux_set_enabled(linux_fg, false);
        linux_fg_error_logged = false;
        linux_fg_reset(); return;
    }
    if (!isfinite(dt) || dt < 1.f || dt > 100.f) { linux_fg_reset(); return; }
    if (!linux_fg_load()) return;
    linux_fg_frame = (FgLinuxFrame){0};
    linux_fg_frame.depth = linux_fg_image(&img[2]);
    linux_fg_frame.motion = linux_fg_image(&img[1]);
    linux_fg_frame.jitter[0] = jitter[0]; linux_fg_frame.jitter[1] = jitter[1];
    linux_fg_frame.motion_scale[0] = (float)rw; linux_fg_frame.motion_scale[1] = (float)rh;
    linux_fg_frame.dt_ms = dt;
    linux_fg_frame.camera_near = cam->proj[11] != 0 ? fabsf(cam->proj[11]) * .5f : .1f;
    linux_fg_frame.camera_far = 1e6f;
    linux_fg_frame.camera_fov = cam->proj[5] != 0 ? 2.f * atanf(1.f / fabsf(cam->proj[5])) : .785f;
    linux_fg_frame.meters = 1.f;
    linux_fg_frame.depth_infinite = true;
    linux_fg_frame.reset = reset;
    linux_fg_frame.real_frame_id = renderer_present_count();
    linux_fg_quality = g_settings.upscale_quality;
    linux_fg_pending = true;
}
void tmp_fg_set_rect(float x, float y, float w, float h) {
    if (!linux_fg_pending) return;
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) || w <= 0 || h <= 0) { linux_fg_reset(); return; }
    float left = fmaxf(0.f, floorf(x)), top = fmaxf(0.f, floorf(y));
    float right = fminf((float)g_vk.sc_extent.width, ceilf(x + w)), bottom = fminf((float)g_vk.sc_extent.height, ceilf(y + h));
    if (right <= left || bottom <= top) { linux_fg_reset(); return; }
    linux_fg_frame.rect_x = (int32_t)left; linux_fg_frame.rect_y = (int32_t)top;
    linux_fg_frame.rect_width = (uint32_t)(right - left); linux_fg_frame.rect_height = (uint32_t)(bottom - top);
    if (!linux_fg_rect_valid || linux_fg_rect[0] != linux_fg_frame.rect_x || linux_fg_rect[1] != linux_fg_frame.rect_y ||
        linux_fg_rect[2] != (int32_t)linux_fg_frame.rect_width || linux_fg_rect[3] != (int32_t)linux_fg_frame.rect_height)
        linux_fg_frame.reset = true;
}
VkImageView tmp_fg_hudless_target(void) {
    linux_fg_hudless = false;
    if (!linux_fg_pending || !linux_fg_on() || env_int("XCDE_FG_NO_HUDLESS")) return VK_NULL_HANDLE;
    img_make(&linux_fg_hud, g_vk.sc_format, g_vk.sc_extent.width, g_vk.sc_extent.height,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    linux_fg_hudless = true;
    return linux_fg_hud.v;
}
// scene is GENERAL, after every game/host HUD draw. On failure it remains the ordinary real image.
bool tmp_fg_record_present(VkCommandBuffer cb, VkImage scene, VkImage *base, float *dt_ms) {
    if (!linux_fg_pending || !linux_fg_on() || !tmp_frame_gen_wanted() || linux_fg_quality != g_settings.upscale_quality ||
        linux_fg_frame.real_frame_id != renderer_present_count()) {
        if (linux_fg && (!linux_fg_on() || !tmp_frame_gen_wanted())) fg_linux_set_enabled(linux_fg, false);
        linux_fg_reset(); return false;
    }
    linux_fg_pending = false;
    fg_linux_set_enabled(linux_fg, true);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    img_make(&linux_fg_real, g_vk.sc_format, g_vk.sc_extent.width, g_vk.sc_extent.height, usage);
    img_make(&linux_fg_output, g_vk.sc_format, g_vk.sc_extent.width, g_vk.sc_extent.height, usage);
    vk_full_barrier(cb);
    VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent = {g_vk.sc_extent.width, g_vk.sc_extent.height, 1}};
    vkCmdCopyImage(cb, scene, VK_IMAGE_LAYOUT_GENERAL, linux_fg_real.b.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    vk_full_barrier(cb);
    linux_fg_frame.commands = cb;
    linux_fg_frame.scene = linux_fg_image(&linux_fg_real);
    linux_fg_frame.output = linux_fg_image(&linux_fg_output);
    linux_fg_frame.hudless = linux_fg_hudless ? linux_fg_image(&linux_fg_hud) : (FgLinuxImage){0};
    linux_fg_frame.recording_seq = vk_pending_seq();
    linux_fg_frame.completed_seq = vk_completed_seq();
    FgLinuxResult result = fg_linux_dispatch(linux_fg, &linux_fg_frame);
    vk_full_barrier(cb);
    if (result != FG_LINUX_GENERATED) {
        if (result == FG_LINUX_ERROR && !linux_fg_error_logged) {
            LOG("[fg] generation disabled: %s; ordinary frame retained\n", fg_linux_error(linux_fg)); linux_fg_error_logged = true;
        }
        if (result == FG_LINUX_WARMUP) {
            linux_fg_rect_valid = true;
            linux_fg_rect[0] = linux_fg_frame.rect_x; linux_fg_rect[1] = linux_fg_frame.rect_y;
            linux_fg_rect[2] = (int32_t)linux_fg_frame.rect_width; linux_fg_rect[3] = (int32_t)linux_fg_frame.rect_height;
        }
        return false;
    }
    vkCmdCopyImage(cb, linux_fg_output.b.image, VK_IMAGE_LAYOUT_GENERAL, scene, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    vk_full_barrier(cb);
    *base = linux_fg_real.b.image; *dt_ms = linux_fg_frame.dt_ms;
    linux_fg_generated = linux_fg_rect_valid = true;
    linux_fg_rect[0] = linux_fg_frame.rect_x; linux_fg_rect[1] = linux_fg_frame.rect_y;
    linux_fg_rect[2] = (int32_t)linux_fg_frame.rect_width; linux_fg_rect[3] = (int32_t)linux_fg_frame.rect_height;
    return true;
}
