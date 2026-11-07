// [XCDE] Temporal upscaling / anti-aliasing at the present (temporal.h). AMD FSR 3.1 through the FidelityFX API
// (amd_fidelityfx_vk.dll next to the exe, FidelityFX SDK 1.1.4 - the last with Vulkan; loaded at run time, so the exe
// runs without it). NVIDIA DLSS: dlss.cpp (Windows Streamline / Linux NGX).
//
// Per frame: exec.c jitters the draws into the scene depth by a Halton (2, 3) sub-pixel offset and finds the game's
// camera. Here one fullscreen pass at render size copies the game image (raw bytes: gamma-encoded LDR) and the scene
// depth into images the upscaler can read, and writes camera motion vectors (each pixel reprojected with the previous
// frame's view-projection). The upscaler then accumulates the jittered frames into the window-size image.
// Motion combines camera reprojection with the game's object vectors when available. T213 strips the HUD before
// temporal reconstruction and composites it afterwards (exec.c).
//   XCDE_TMP_DEBUG=1  the upscaler's debug view (FSR: motion vectors, depth, reactiveness...)
//   XCDE_TMP_LOG=1    a line a second: camera found, sizes, jitter
// [XCDE] LINUX-017: portable Vulkan inputs for DLSS; DLL-dependent FSR3/frame generation stay Windows-only.
#include <SDL3/SDL_timer.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../../diag/diag.h"
#include "../../settings.h"
#include "temporal.h"
#include "dlss.h"
#if defined(__linux__) && defined(XCDE_LINUX_FRAMEGEN)
static void linux_fg_reset(void);
#endif
#ifdef _WIN32
#include <windows.h>
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_upscale.h"
// (ffx_api/vk/ffx_api_vk.h is C++-only: its one structure used here, as declared there)
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK 0x0000003u
struct ffxCreateBackendVKDesc {
    ffxCreateContextDescHeader header;
    VkDevice vkDevice;
    VkPhysicalDevice vkPhysicalDevice;
    PFN_vkGetDeviceProcAddr vkDeviceProcAddr;
};

#endif

bool diag_deterministic(void);

// ---------------------------------------------------------------- FidelityFX loader
#ifdef _WIN32
static PfnFfxCreateContext ffx_create;
static PfnFfxDestroyContext ffx_destroy;
static PfnFfxDispatch ffx_dispatch;
static PfnFfxQuery ffx_query;
static PfnFfxConfigure ffx_configure;
static int ffx_state;  // 0 not tried, 1 loaded, -1 unavailable

static bool ffx_load(void) {
    if (ffx_state) return ffx_state > 0;
    ffx_state = -1;
    HMODULE m = LoadLibraryA("amd_fidelityfx_vk.dll");
    if (!m) { LOG("[tmp] amd_fidelityfx_vk.dll not found: FSR 3 unavailable\n"); return false; }
    ffx_create = (PfnFfxCreateContext)(void *)GetProcAddress(m, "ffxCreateContext");
    ffx_destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(m, "ffxDestroyContext");
    ffx_dispatch = (PfnFfxDispatch)(void *)GetProcAddress(m, "ffxDispatch");
    ffx_query = (PfnFfxQuery)(void *)GetProcAddress(m, "ffxQuery");
    ffx_configure = (PfnFfxConfigure)(void *)GetProcAddress(m, "ffxConfigure");
    if (!ffx_create || !ffx_destroy || !ffx_dispatch || !ffx_query || !ffx_configure) { LOG("[tmp] amd_fidelityfx_vk.dll: entry points missing\n"); return false; }
    ffx_state = 1;
    LOG("[tmp] FidelityFX API loaded (FSR 3.1)\n");
    return true;
}
#else
static bool ffx_load(void) { return false; }
#endif

// [XCDE] LINUX-017: history belongs to one successful, consecutive provider evaluation.
static TmpCamera previous;
static uint32_t hist_rw, hist_rh, hist_ow, hist_oh;
static int hist_upscaler, hist_quality, hist_provider;
static uint64_t hist_frame, frames, t_last;
static int active_provider = -1, active_upscaler = -1, active_quality = -1;
static uint32_t jit_index, jit_phases = 8;
static uint64_t snap_frame = ~0ull;
static void history_invalidate(void) { previous.ok = false; t_last = 0; }

static int env_int(const char *k) { const char *e = getenv(k); return e ? atoi(e) : 0; }

bool tmp_enabled(void) {
    int provider = 0;
    if (!env_int("XCDE_TMP_OFF") && (!diag_deterministic() || env_int("XCDE_TMP_DETERMINISTIC"))) {
        if (g_settings.upscaler == UPSCALE_FSR3) provider = ffx_load();
        else if (g_settings.upscaler == UPSCALE_DLSS) provider = dlss_available() ? 2 : ffx_load();
    }
    if (provider != active_provider || g_settings.upscaler != active_upscaler || g_settings.upscale_quality != active_quality) {
        history_invalidate();
        snap_frame = ~0ull;
        jit_index = 0;
        jit_phases = 8;
        if (g_settings.upscaler == UPSCALE_DLSS && provider == 1) LOG("[tmp] DLSS unavailable: FSR 3 instead\n");
        active_provider = provider;
        active_upscaler = g_settings.upscaler;
        active_quality = g_settings.upscale_quality;
    }
    if (!provider) history_invalidate();
    return provider != 0;
}
static bool use_dlss(void) { return g_settings.upscaler == UPSCALE_DLSS && dlss_available(); }

// ---------------------------------------------------------------- jitter
static float halton(uint32_t i, uint32_t b) {
    float f = 1, r = 0;
    for (; i; i /= b) { f /= (float)b; r += f * (float)(i % b); }
    return r;
}
void tmp_jitter(float j[2]) {
    jit_index = jit_index % jit_phases + 1;
    j[0] = halton(jit_index, 2) - 0.5f;
    j[1] = halton(jit_index, 3) - 0.5f;
}

// ---------------------------------------------------------------- images
typedef struct { TexBackend b; VkImageView v; } Img;
static void img_make(Img *d, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    if (d->b.image && d->b.width == w && d->b.height == h && d->b.format == f) return;
    if (d->b.image) {
        vk_views_forget(&d->b);
        vk_defer_image(d->b.image, d->b.mem);
        memset(d, 0, sizeof *d);
    }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->b.image, &d->b.mem, NULL));
    d->b.format = f; d->b.width = w; d->b.height = h; d->b.depth = 1; d->b.layers = 1; d->b.levels = 1;
    d->b.samples = 1; d->b.aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->b.base_type = VK_IMAGE_VIEW_TYPE_2D;
    vk_image_init_layout(vk_cmd(), d->b.image, d->b.aspect);
    static const VkComponentMapping id = {0};
    d->v = vk_image_view(&d->b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}
// (render size) 0 colour, 1 motion, 2 depth, 4 the game image before the HUD, 5 HUD mask; (window size) 3 output
static Img img[6];
enum { F_COLOR = VK_FORMAT_R8G8B8A8_UNORM, F_MV = VK_FORMAT_R16G16_SFLOAT, F_DEPTH = VK_FORMAT_R32_SFLOAT, F_MASK = VK_FORMAT_R8_UNORM };

// [XCDE] T213: HUD without ghosting. exec.c calls this when the first pass of the frame that draws into the presented
// image ends (the scene composite; the HUD passes follow): a copy of the image before the HUD. The inputs pass marks
// the pixels the HUD changed; the upscalers get that as their reactive mask (FSR) / "bias current colour" hint (DLSS):
// no history there, so text and icons leave no trails. Any HUD, menu or scene: it is measured every frame.
uint64_t renderer_present_count(void);
// [XCDE] T215: the game's velocity buffer (exec.c finds it at its readers' gTVelocity unit)
static TexBackend *game_vel;
static uint64_t game_vel_frame = ~0ull;
// GPU idle, guest execution stopped: retain device-local scratch images, drop every guest/history reference.
void tmp_reset(void) {
#if defined(__linux__) && defined(XCDE_LINUX_FRAMEGEN)
    linux_fg_reset();
#endif
    memset(&previous, 0, sizeof previous);
    hist_rw = hist_rh = hist_ow = hist_oh = 0;
    hist_upscaler = hist_quality = hist_provider = 0;
    hist_frame = frames = t_last = 0;
    active_provider = active_upscaler = active_quality = -1;
    game_vel = NULL;
    game_vel_frame = snap_frame = ~0ull;
    jit_index = 0;
    jit_phases = 8;
    dlss_reset();
}
void tmp_set_velocity(TexBackend *v) { game_vel = v; game_vel_frame = renderer_present_count(); }
void tmp_forget_texture(TexBackend *b) {
    if (b && b == game_vel) { game_vel = NULL; game_vel_frame = ~0ull; }
}
// [XCDE] T228: the readers run only with the game's own AA / motion blur on (launcher "Antialiasing y desenfoque de
// movimiento del juego"); with it off (the user's setting) the buffer is still written by the G-buffer pass but was
// never found, and DLSS / FSR got camera-only motion for characters and grass (MV captures, meadow frame 3500:
// identical with and without the game's velocity). Once found, the buffer counts for a frame in which the game renders
// into it (exec.c begin_pass). XCDE_TMP_VEL_KEEP=0: readers only, as before.
void tmp_note_target(TexBackend *b) {
    static int keep = -1;
    if (keep < 0) { const char *e = getenv("XCDE_TMP_VEL_KEEP"); keep = !(e && *e == '0'); }
    if (keep && b && b == game_vel) game_vel_frame = renderer_present_count();
}
// XCDE_TMP_VEL_FIT=<frame>: read back the game's velocity and our camera motion vectors of that frame and fit
// game = a * ours + b per channel over the pixels (static scenery: ours are right there) - the encoding
static float vk_half_to_float(uint16_t h) {
    uint32_t sg = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 31u, m = h & 1023u, f;
    if (!e) { float d = (float)m * 5.9604644775390625e-8f; return sg ? -d : d; }
    f = e == 31 ? sg | 0x7F800000u | (m << 13) : sg | ((e + 112u) << 23) | (m << 13);
    float r; memcpy(&r, &f, 4); return r;
}
void renderer_flush(void);
static void vel_fit(VkCommandBuffer cb, uint32_t rw, uint32_t rh) {
    static long at = -2;
    if (at == -2) at = getenv("XCDE_TMP_VEL_FIT") ? atol(getenv("XCDE_TMP_VEL_FIT")) : -1;
    if (at < 0 || (long)renderer_present_count() != at) return;
    if (!game_vel || game_vel_frame + 1 < renderer_present_count()) { LOG("[velfit] no game velocity this frame\n"); return; }
    uint32_t gw = game_vel->width, gh = game_vel->height;
    uint64_t gsz = (uint64_t)gw * gh * 4, msz = (uint64_t)rw * rh * 4;
    VkSpan rb = vk_readback(gsz + msz);
    vk_full_barrier(cb);
    VkBufferImageCopy c1 = {.bufferOffset = rb.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {gw, gh, 1}};
    vkCmdCopyImageToBuffer(cb, game_vel->image, VK_IMAGE_LAYOUT_GENERAL, rb.buf, 1, &c1);
    VkBufferImageCopy c2 = {.bufferOffset = rb.off + gsz, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {rw, rh, 1}};
    vkCmdCopyImageToBuffer(cb, img[1].b.image, VK_IMAGE_LAYOUT_GENERAL, rb.buf, 1, &c2);
    vk_full_barrier(cb);
    renderer_flush();
    const uint32_t *gv = (const uint32_t *)rb.ptr;
    const uint16_t *mv = (const uint16_t *)((const uint8_t *)rb.ptr + gsz);
    double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, sgx = 0, sgy = 0, sgxx = 0, sgyy = 0, smx = 0, smy = 0, smxx = 0, smyy = 0;
    double sgmx = 0, sgmy = 0, cb_[4] = {0}, ca[4] = {0};
    for (uint32_t y = 0; y < rh; y += 2)
        for (uint32_t x = 0; x < rw; x += 2) {
            uint32_t g = gv[(uint64_t)(y * gh / rh) * gw + (x * gw / rw)];
            double r = (g & 1023) / 1023.0, gg = ((g >> 10) & 1023) / 1023.0, b = ((g >> 20) & 1023) / 1023.0;
            unsigned a = g >> 30;
            double mx = vk_half_to_float(mv[((uint64_t)y * rw + x) * 2]), my = vk_half_to_float(mv[((uint64_t)y * rw + x) * 2 + 1]);
            n++; sgx += r; sgy += gg; sgxx += r * r; sgyy += gg * gg; smx += mx; smy += my; smxx += mx * mx; smyy += my * my;
            sgmx += r * mx; sgmy += gg * my; cb_[(int)(b * 3.999)]++; ca[a]++;
            (void)sx; (void)sy; (void)sxx; (void)sxy;
        }
    {   // models: |mv.x| vs R (linear / square root), sign(mv.x) vs alpha bits; samples
        double n2 = 0, s1 = 0, s2 = 0, s11 = 0, s22 = 0, s12 = 0, q1 = 0, q11 = 0, q12 = 0, agree[2] = {0}, cnt = 0;
        int ns = 0;
        for (uint32_t y = 0; y < rh; y += 3)
            for (uint32_t x = 0; x < rw; x += 3) {
                uint32_t g = gv[(uint64_t)(y * gh / rh) * gw + (x * gw / rw)];
                double r = (g & 1023) / 1023.0, gg = ((g >> 10) & 1023) / 1023.0, b = ((g >> 20) & 1023) / 1023.0;
                unsigned al = g >> 30;
                double mx = vk_half_to_float(mv[((uint64_t)y * rw + x) * 2]), my = vk_half_to_float(mv[((uint64_t)y * rw + x) * 2 + 1]);
                if (fabs(mx) < 2e-4) continue;
                double ax_ = fabs(mx), sq = sqrt(ax_);
                n2++; s1 += ax_; s2 += r; s11 += ax_ * ax_; s22 += r * r; s12 += ax_ * r; q1 += sq; q11 += sq * sq; q12 += sq * r;
                cnt++; agree[0] += ((al & 1) != 0) == (mx < 0); agree[1] += ((al & 2) != 0) == (mx < 0);
                if (ns < 16 && (x % 97) == 0) { ns++; LOG("[velfit]  px %u,%u: R %.4f G %.4f B %.4f A %u | mv %.6f %.6f\n", x, y, r, gg, b, al, mx, my); }
            }
        double cl = (n2 * s12 - s1 * s2) / sqrt((n2 * s11 - s1 * s1) * (n2 * s22 - s2 * s2) + 1e-30);
        double cq = (n2 * q12 - q1 * s2) / sqrt((n2 * q11 - q1 * q1) * (n2 * s22 - s2 * s2) + 1e-30);
        double sl = (n2 * s12 - s1 * s2) / (n2 * s11 - s1 * s1 + 1e-30), of = (s2 - sl * s1) / n2;
        LOG("[velfit] |mv.x| vs R: linear corr %.3f (R = %.4f * |mv| + %.4f), sqrt corr %.3f; sign(mv.x)<0 == A bit0 %.1f%%, == A bit1 %.1f%% (%.0f px)\n",
            cl, sl, of, cq, 100 * agree[0] / (cnt + 1e-9), 100 * agree[1] / (cnt + 1e-9), cnt);
    }
    {   // pixels the camera doesn't move but the game's velocity does (objects moving on their own: characters)
        int nd = 0, ns = 0;
        for (uint32_t y = 0; y < rh; y += 2)
            for (uint32_t x = 0; x < rw; x += 2) {
                uint32_t g = gv[(uint64_t)(y * gh / rh) * gw + (x * gw / rw)];
                double r = (g & 1023) / 1023.0, gg = ((g >> 10) & 1023) / 1023.0;
                unsigned al = g >> 30;
                double mx = vk_half_to_float(mv[((uint64_t)y * rw + x) * 2]), my = vk_half_to_float(mv[((uint64_t)y * rw + x) * 2 + 1]);
                if (fabs(mx) > 1e-5 || fabs(my) > 1e-5 || (r < 0.002 && gg < 0.002)) continue;
                nd++;
                if (ns < 24 && (nd % 37) == 1) {
                    ns++;
                    LOG("[velfit] dyn px %u,%u: R %.4f G %.4f B %.4f A %u -> v %.6f %.6f (ours %.6f %.6f)\n", x, y, r, gg,
                        ((g >> 20) & 1023) / 1023.0, al, (al & 1) ? -r * r : r * r, (al & 2) ? gg * gg : -gg * gg, mx, my);
                }
            }
        LOG("[velfit] %d pixels moving by the game's velocity where the camera's is 0\n", nd);
    }
    double ax = (n * sgmx - sgx * smx) / (n * smxx - smx * smx + 1e-30), bx = (sgx - ax * smx) / n;
    double ay = (n * sgmy - sgy * smy) / (n * smyy - smy * smy + 1e-30), by = (sgy - ay * smy) / n;
    double rx = (n * sgmx - sgx * smx) / sqrt((n * smxx - smx * smx) * (n * sgxx - sgx * sgx) + 1e-30);
    double ry = (n * sgmy - sgy * smy) / sqrt((n * smyy - smy * smy) * (n * sgyy - sgy * sgy) + 1e-30);
    LOG("[velfit] frame %ld game %ux%u ours %ux%u: R = %.5f * mv.x + %.5f (corr %.3f) | G = %.5f * mv.y + %.5f (corr %.3f) |"
        " mean R %.4f G %.4f, mean mv %.6f %.6f | B quartiles %.0f %.0f %.0f %.0f | A %.0f %.0f %.0f %.0f\n",
        at, gw, gh, rw, rh, ax, bx, rx, ay, by, ry, sgx / n, sgy / n, smx / n, smy / n, cb_[0], cb_[1], cb_[2], cb_[3], ca[0], ca[1], ca[2], ca[3]);
}
bool tmp_hud_started(void) { return snap_frame == renderer_present_count(); }  // (the HUD passes of this frame have begun)
// the image before the HUD of this frame (render size), or VK_NULL_HANDLE
VkImageView tmp_hud_scene_view(uint32_t w, uint32_t h) {
    if (env_int("XCDE_TMP_NO_HUD_MASK") || snap_frame != renderer_present_count() || !img[4].b.image || img[4].b.width != w ||
        img[4].b.height != h)
        return VK_NULL_HANDLE;
    return img[4].v;
}
void tmp_hud_snapshot(VkCommandBuffer cb, TexBackend *b) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) || snap_frame == renderer_present_count()) return;
    img_make(&img[4], b->format, b->width, b->height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    vk_full_barrier(cb);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .extent = {b->width, b->height, 1}};
    vkCmdCopyImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, img[4].b.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vk_full_barrier(cb);
    snap_frame = renderer_present_count();
    if (env_int("XCDE_TMP_LOG")) { static int nl; if (nl++ < 5) LOG("[tmp] HUD snapshot frame %llu: %ux%u\n", (unsigned long long)snap_frame, b->width, b->height); }
}

// ---------------------------------------------------------------- the inputs pass
static VkPipelineLayout prep_layout;
static VkDescriptorSetLayout prep_set_layout;
static VkPipeline prep_pipe;
static void prep_pipeline(void) {
    if (prep_pipe) return;
    VkDescriptorSetLayoutBinding b[4];
    for (uint32_t i = 0; i < 4; i++)
        b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &prep_set_layout));
    VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96};
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &prep_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &prep_layout));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("tmp_prep.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba[4] = {{.colorWriteMask = 0xF}, {.colorWriteMask = 0x3}, {.colorWriteMask = 0x1},
                                                  {.colorWriteMask = 0x1}};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 4,
                                              .pAttachments = cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat fmts[4] = {F_COLOR, F_MV, F_DEPTH, F_MASK};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 4,
                                        .pColorAttachmentFormats = fmts};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = prep_layout};
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &prep_pipe));
}

// 4x4 helpers on the game's row matrices (double precision)
static void mat_from_rows(const float *r, double m[16]) { for (int i = 0; i < 16; i++) m[i] = r[i]; }
static void mat_mul(const double a[16], const double b[16], double o[16]) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
            o[i * 4 + j] = s;
        }
}
static bool mat_inv(const double m[16], double o[16]) {  // Gauss-Jordan
    for (int i = 0; i < 16; i++) if (!isfinite(m[i])) return false;
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return false;
        if (p != c) for (int j = 0; j < 8; j++) { double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        double d = a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) { double f = a[r][c]; for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        o[i * 4 + j] = a[i][j + 4];
        if (!isfinite(o[i * 4 + j])) return false;
    }
    return true;
}

// [XCDE] LINUX-017: reject invalid cameras before recording GPU work. Every reset uses zero motion vectors.
static bool history_prepare(const TmpCamera *cam, const float jitter[2], uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh,
                            int provider, uint64_t frame, double reproj[16], bool *have_prev) {
    *have_prev = false;
    memset(reproj, 0, 16 * sizeof *reproj);
    if (!cam || !cam->ok || !jitter || !isfinite(jitter[0]) || !isfinite(jitter[1]) ||
        !rw || !rh || !ow || !oh || ow > 16384 || oh > 16384 || rw > ow + 1 || rh > oh + 1) return false;
    for (int i = 0; i < 12; i++) if (!isfinite(cam->view[i])) return false;
    for (int i = 0; i < 16; i++) if (!isfinite(cam->proj[i]) || !isfinite(cam->viewproj[i])) return false;
    double cur[16], inv[16];
    mat_from_rows(cam->viewproj, cur);
    if (!mat_inv(cur, inv)) return false;
    bool keep = previous.ok && hist_frame + 1 == frame && hist_rw == rw && hist_rh == rh && hist_ow == ow && hist_oh == oh &&
                hist_upscaler == g_settings.upscaler && hist_quality == g_settings.upscale_quality && hist_provider == provider;
    if (keep) {
        double delta = 0;
        for (int i = 0; i < 12; i++) delta += fabs((double)cam->view[i] - previous.view[i]);
        keep = delta <= 1.5;  // existing cut heuristic: ~30 degrees or a big translation in one frame
    }
    if (keep) {
        double pv[16];
        mat_from_rows(previous.viewproj, pv);
        mat_mul(pv, inv, reproj);
        for (int i = 0; i < 16; i++) if (!isfinite(reproj[i])) return false;
        *have_prev = true;
    }
    return true;
}
static void history_commit(const TmpCamera *cam, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh,
                           int provider, uint64_t frame, bool ok) {
    if (!ok) { history_invalidate(); return; }
    previous = *cam;
    hist_rw = rw; hist_rh = rh; hist_ow = ow; hist_oh = oh;
    hist_upscaler = g_settings.upscaler; hist_quality = g_settings.upscale_quality; hist_provider = provider;
    hist_frame = frame;
}

static void prep(VkCommandBuffer cb, VkImageView color_view, VkImageView scene_view, VkImageView vel_view, TexBackend *depth,
                 const double reproj[16], bool have_prev, const float jit[2], bool dm1, uint32_t rw, uint32_t rh) {
    prep_pipeline();
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca[4];
    for (int i = 0; i < 4; i++)
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = img[i < 3 ? i : 5].v,
                                            .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                            .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {rw, rh}}, .layerCount = 1,
                          .colorAttachmentCount = 4, .pColorAttachments = ca};
    vkCmdBeginRendering(cb, &ri);
    static const VkComponentMapping id = {0};
    VkImageView dv = vk_image_view(depth, VK_IMAGE_VIEW_TYPE_2D, depth->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkDescriptorSet ds = vk_alloc_set(prep_set_layout);
    VkDescriptorImageInfo ii[4] = {{vk_default_sampler(false), color_view, VK_IMAGE_LAYOUT_GENERAL},
                                   {vk_default_sampler(false), dv, VK_IMAGE_LAYOUT_GENERAL},
                                   {vk_default_sampler(false), scene_view, VK_IMAGE_LAYOUT_GENERAL},
                                   {vk_default_sampler(false), vel_view ? vel_view : color_view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[4];
    for (int i = 0; i < 4; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[i]};
    vkUpdateDescriptorSets(g_vk.dev, 4, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, prep_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, prep_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)rw, (float)rh, 0, 1};
    VkRect2D sc = {{0, 0}, {rw, rh}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pc[24];
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) pc[c * 4 + r] = (float)reproj[r * 4 + c];  // (GLSL: column-major)
    pc[16] = jit[0]; pc[17] = jit[1]; pc[18] = (float)rw; pc[19] = (float)rh;
    static int precise = -1;  // [XCDE] T228: our exact camera motion where the game's velocity says the same (tmp_prep.frag)
    if (precise < 0) { const char *e = getenv("XCDE_TMP_VEL_PRECISE"); precise = !(e && *e == '0'); }
    pc[20] = dm1 ? 1.0f : 0.0f; pc[21] = have_prev ? 1.0f : 0.0f; pc[22] = vel_view ? 1.0f : 0.0f; pc[23] = precise ? 1.0f : 0.0f;
    vkCmdPushConstants(cb, prep_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
}

// ---------------------------------------------------------------- FSR 3.1
#ifdef _WIN32
static ffxContext fsr_ctx;
static uint32_t fsr_w, fsr_h;
static void fsr_message(uint32_t type, const wchar_t *msg) {
    char buf[512];
    size_t n = 0;
    for (; msg && msg[n] && n < sizeof buf - 1; n++) buf[n] = (char)(msg[n] < 128 ? msg[n] : '?');
    buf[n] = 0;
    LOG("[tmp] FSR %s: %s\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", buf);
}
// The FidelityFX backend asks for some functions by their extension names; the device has them as core functions.
static PFN_vkVoidFunction VKAPI_PTR ffx_proc(VkDevice d, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(d, name);
    if (f) return f;
    char core[128];
    size_t n = strlen(name);
    if (n > 3 && n < sizeof core && !strcmp(name + n - 3, "KHR")) {  // vkGetBufferMemoryRequirements2KHR -> ...2
        memcpy(core, name, n - 3);
        core[n - 3] = 0;
        f = vkGetDeviceProcAddr(d, core);
    }
    return f;
}
static bool fsr_context(uint32_t ow, uint32_t oh) {
    if (fsr_ctx && fsr_w == ow && fsr_h == oh) return true;
    if (fsr_ctx) { vkDeviceWaitIdle(g_vk.dev); ffx_destroy(&fsr_ctx, NULL); fsr_ctx = NULL; }
    struct ffxCreateBackendVKDesc be = {.header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK},
                                        .vkDevice = g_vk.dev, .vkPhysicalDevice = g_vk.phys, .vkDeviceProcAddr = ffx_proc};
    struct ffxCreateContextDescUpscale cd = {.header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, .pNext = &be.header},
                                             .flags = FFX_UPSCALE_ENABLE_DEPTH_INFINITE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE |
                                                      FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION,
                                             .maxRenderSize = {ow, oh}, .maxUpscaleSize = {ow, oh}, .fpMessage = fsr_message};
    if (env_int("XCDE_TMP_DEBUG")) cd.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    ffxReturnCode_t r = ffx_create(&fsr_ctx, &cd.header, NULL);
    if (r != FFX_API_RETURN_OK) { LOG("[tmp] FSR 3 context failed: %u\n", r); fsr_ctx = NULL; ffx_state = -1; return false; }
    fsr_w = ow; fsr_h = oh;
    struct ffxQueryGetProviderVersion pv = {.header = {.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION}};
    ffx_query(&fsr_ctx, &pv.header);
    LOG("[tmp] FSR context %ux%u (%s)\n", ow, oh, pv.versionName ? pv.versionName : "?");
    return true;
}
static struct FfxApiResource res(Img *d, uint32_t ffx_fmt, uint32_t usage) {
    struct FfxApiResource r;
    memset(&r, 0, sizeof r);
    r.resource = (void *)d->b.image;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffx_fmt;
    r.description.width = d->b.width;
    r.description.height = d->b.height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.usage = usage;
    r.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;  // (every image is in GENERAL)
    return r;
}

#include "temporal_fg.inc"
#elif defined(XCDE_LINUX_FRAMEGEN)
#include "temporal_fg_linux.inc"
#else
// Builds without a native interpolation dependency keep ordinary presentation.
bool tmp_fg_active(void) { return false; }
bool tmp_frame_gen_wanted(void) { return false; }
bool tmp_fg_install(VkSwapchainKHR *sc, const VkSwapchainCreateInfoKHR *ci, PFN_vkCreateSwapchainKHR *create,
    PFN_vkDestroySwapchainKHR *destroy, PFN_vkGetSwapchainImagesKHR *images,
    PFN_vkAcquireNextImageKHR *acquire, PFN_vkQueuePresentKHR *present) {
    (void)sc; (void)ci; (void)create; (void)destroy; (void)images; (void)acquire; (void)present;
    return false;
}
void tmp_fg_idle(VkCommandBuffer cb) { (void)cb; }
VkImageView tmp_fg_hudless_target(void) { return VK_NULL_HANDLE; }
#ifdef __linux__
bool tmp_fg_supported(void) { return false; }
void tmp_fg_invalidate(void) {}
void tmp_fg_set_rect(float x, float y, float w, float h) { (void)x; (void)y; (void)w; (void)h; }
bool tmp_fg_record_present(VkCommandBuffer cb, VkImage scene, VkImage *base, float *dt_ms) {
    (void)cb; (void)scene; (void)base; (void)dt_ms; return false;
}
#endif
#endif

// ---------------------------------------------------------------- per frame
// [XCDE] T228 research: XCDE_TMP_MV_SHOT=<frame>[,<frame>...] saves the motion vectors handed to DLSS / FSR (img[1],
// render size) as build/mv_<frame>.png: red |x|, green |y| (x 60: 1/60 of the screen a frame = full), blue the signs (x<0: 80, y<0: 160); (unused:
// they differ from the camera-only motion of a static world (the game's per-object velocity, T215). Copied during that
// frame, written on the next one (after its submission is done).
static float half_f(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? m / 1024.0f / 16384.0f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * ldexpf(1.0f, (int)e - 15);
    return s ? -v : v;
}
static void mv_shot(VkCommandBuffer cb, uint32_t rw, uint32_t rh, const double reproj[16], bool have_prev) {
    static VkSpan pend;
    static uint64_t pend_seq, pend_frame;
    static VkBuffer sbuf; static VmaAllocation smem; static void *sptr; static uint64_t ssize;
    static uint32_t pw, ph;
    static double prj[16];
    if (pend.ptr) {  // the copy of an earlier frame: write it
        vk_wait_seq(pend_seq);
        vmaInvalidateAllocation(g_vk.vma, smem, 0, VK_WHOLE_SIZE);
        uint8_t *px = malloc((size_t)pw * ph * 4);
        const uint16_t *src = pend.ptr;
        float *dep = (float *)((uint8_t *)pend.ptr + (size_t)pw * ph * 4);
        for (uint32_t y = 0; y < ph; y++)
            for (uint32_t x = 0; x < pw; x++) {
                size_t i = (size_t)y * pw + x;
                float mx = half_f(src[i * 2]), my = half_f(src[i * 2 + 1]);
                // camera-only motion of this pixel (the prep's static-world reprojection: NDC -> previous clip)
                double nx = (x + 0.5) / pw * 2 - 1, ny = (y + 0.5) / ph * 2 - 1, nz = dep[i];
                double cx = prj[0] * nx + prj[1] * ny + prj[2] * nz + prj[3], cy = prj[4] * nx + prj[5] * ny + prj[6] * nz + prj[7];
                double cw = prj[12] * nx + prj[13] * ny + prj[14] * nz + prj[15];
                double camx = 0, camy = 0;
                if (cw != 0) { camx = ((cx / cw) - nx) * 0.5; camy = ((cy / cw) - ny) * 0.5; }
                double dx = fabs(fabs(mx) - fabs(camx)) + fabs(fabs(my) - fabs(camy));
                px[i * 4 + 0] = (uint8_t)fminf(255, fabsf(mx) * 60 * 255);
                px[i * 4 + 1] = (uint8_t)fminf(255, fabsf(my) * 60 * 255);
                px[i * 4 + 2] = (uint8_t)((mx < 0 ? 1 : 0) | (my < 0 ? 2 : 0)) * 80;
                px[i * 4 + 3] = 255;
            }
        char path[64];
        snprintf(path, sizeof path, "build/mv_%llu.png", (unsigned long long)pend_frame);
        vk_save_png(path, px, pw, ph);
        free(px);
        pend.ptr = NULL;
    }
    const char *e = getenv("XCDE_TMP_MV_SHOT");
    if (!e || !have_prev) return;
    uint64_t f = renderer_present_count();
    bool want = false;
    for (const char *p = e; *p;) { char *q; uint64_t v = strtoull(p, &q, 10); if (q == p) break; want |= v == f; p = *q ? q + 1 : q; }
    { static uint64_t lastf; if (f / 500 != lastf / 500) { lastf = f; LOG("[tmp] mv shot check: frame %llu %s\n", (unsigned long long)f, want ? "(armed)" : ""); } }
    if (!want) return;
    pw = rw; ph = rh;
    memcpy(prj, reproj, sizeof prj);
    {   // (a buffer of its own: the shared readback ring is reused before the next frame reads it)
        uint64_t need = (uint64_t)rw * rh * 8;  // (motion R16G16 + depth R32)
        if (need > ssize) {
            if (sbuf) vmaDestroyBuffer(g_vk.vma, sbuf, smem);
            VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = need, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                          .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
            VmaAllocationInfo inf;
            if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &sbuf, &smem, &inf) != VK_SUCCESS) return;
            sptr = inf.pMappedData; ssize = need;
        }
        pend = (VkSpan){sbuf, 0, sptr};
    }
    VkBufferImageCopy c[2] = {{.bufferOffset = pend.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {rw, rh, 1}},
                              {.bufferOffset = pend.off + (uint64_t)rw * rh * 4, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {rw, rh, 1}}};
    vk_full_barrier(cb);
    vkCmdCopyImageToBuffer(cb, img[1].b.image, VK_IMAGE_LAYOUT_GENERAL, pend.buf, 1, &c[0]);
    vkCmdCopyImageToBuffer(cb, img[2].b.image, VK_IMAGE_LAYOUT_GENERAL, pend.buf, 1, &c[1]);
    vk_full_barrier(cb);
    pend_seq = vk_pending_seq();
    pend_frame = f;
}

VkImageView tmp_upscale(VkCommandBuffer cb, TexBackend *color, VkImageView color_view, TexBackend *depth, const TmpCamera *cam,
                        const float jitter[2], bool dm1, uint32_t ow, uint32_t oh) {
    double reproj[16];
    bool have_prev;
    uint32_t rw = color ? color->width : 0, rh = color ? color->height : 0;
    int provider = use_dlss() ? 2 : 1;
    uint64_t frame = renderer_present_count();
    if (!color || !color->image || !color_view || !depth || !depth->image || !(depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ||
        !history_prepare(cam, jitter, rw, rh, ow, oh, provider, frame, reproj, &have_prev)) {
        history_invalidate();
        tmp_fg_idle(cb);
        return VK_NULL_HANDLE;
    }
    bool reset = !have_prev;
    jit_phases = (uint32_t)ceil(8.0 * ((double)ow / rw) * ((double)oh / rh));
    if (jit_phases < 8) jit_phases = 8;
    img_make(&img[0], F_COLOR, rw, rh, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    img_make(&img[1], F_MV, rw, rh, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    img_make(&img[2], F_DEPTH, rw, rh, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    img_make(&img[5], F_MASK, rw, rh, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    bool snap = snap_frame == renderer_present_count() && img[4].b.image && img[4].b.width == color->width &&
                img[4].b.height == color->height && !env_int("XCDE_TMP_NO_HUD_MASK");
    img_make(&img[3], F_COLOR, ow, oh, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    // [XCDE] T213: the upscalers get the image before the HUD; the HUD goes back on top after them (exec.c
    // post_process, tmp_hud.frag). The upscalers undo the jitter on all they get: the HUD, never jittered, shook
    // (user: "it vibrates") - with the HUD in their input no mask can fix that.
    // [XCDE] T215: the game's velocity (per-object motion) when its buffer was seen this frame; XCDE_TMP_GAME_VEL=0: camera only
    VkImageView vel_view = VK_NULL_HANDLE;
    static int use_vel = -1;
    if (use_vel < 0) { const char *e = getenv("XCDE_TMP_GAME_VEL"); use_vel = !(e && *e == '0'); }
    if (use_vel && have_prev && game_vel && game_vel_frame == renderer_present_count() && game_vel->image &&
        game_vel->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
        static const VkComponentMapping id = {0};
        vel_view = vk_image_view(game_vel, VK_IMAGE_VIEW_TYPE_2D, game_vel->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    {   static int nl;  // (XCDE_TMP_LOG: whether the game's velocity was used, and why not)
        if (env_int("XCDE_TMP_LOG") && (nl < 6 ? (frames % 60) == 0 : (frames % 600) == 0)) {
            nl++;
            LOG("[tmp] game velocity %s (seen frame %llu, now %llu, fmt %d, prev %d)\n", vel_view ? "used" : "NOT used",
                (unsigned long long)game_vel_frame, (unsigned long long)renderer_present_count(),
                game_vel && game_vel_frame == renderer_present_count() ? (int)game_vel->format : -1, have_prev);
        }
    }
    if (!getenv("XCDE_TMP_VEL_FIT")) {
        prep(cb, snap ? img[4].v : color_view, snap ? img[4].v : color_view, vel_view, depth, reproj, have_prev, jitter, dm1, rw, rh);
    } else {
        prep(cb, snap ? img[4].v : color_view, snap ? img[4].v : color_view, VK_NULL_HANDLE, depth, reproj, have_prev, jitter, dm1, rw, rh);
    }
    vel_fit(cb, rw, rh);  // [XCDE] T215 (research)
    mv_shot(cb, rw, rh, reproj, have_prev);  // [XCDE] T228 (research)
    uint64_t now = SDL_GetTicksNS();
    float dt = t_last ? (float)((now - t_last) / 1e6) : 16.7f;
    t_last = now;
    bool ok = false;
    if (provider == 2) {
        ok = dlss_evaluate(cb, &img[0].b, &img[1].b, &img[2].b, &img[3].b, NULL, jitter, reset, cam, reproj, vk_pending_seq(), vk_completed_seq());
    }
#ifdef _WIN32
    else if (ffx_load() && fsr_context(ow, oh)) {
        struct ffxDispatchDescUpscale d;
        memset(&d, 0, sizeof d);
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = (void *)cb;
        d.color = res(&img[0], FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, FFX_API_RESOURCE_USAGE_READ_ONLY);
        d.motionVectors = res(&img[1], FFX_API_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY);
        d.depth = res(&img[2], FFX_API_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY);
        d.output = res(&img[3], FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, FFX_API_RESOURCE_USAGE_UAV);

        d.jitterOffset.x = jitter[0];
        d.jitterOffset.y = jitter[1];
        d.motionVectorScale.x = (float)rw;
        d.motionVectorScale.y = (float)rh;
        d.renderSize.width = rw; d.renderSize.height = rh;
        d.upscaleSize.width = ow; d.upscaleSize.height = oh;
        d.enableSharpening = true;
        d.sharpness = 0.3f;
        d.frameTimeDelta = dt;
        d.preExposure = 1.0f;
        d.reset = reset;
        float p11 = cam->proj[5];
        d.cameraNear = cam->proj[11] != 0 ? fabsf(cam->proj[11]) * 0.5f : 0.1f;  // (GL infinite projection: row 2 w = -2 near)
        d.cameraFar = 1e6f;
        d.cameraFovAngleVertical = p11 != 0 ? 2.0f * atanf(1.0f / fabsf(p11)) : 0.785f;
        d.viewSpaceToMetersFactor = 1.0f;
        d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB | (env_int("XCDE_TMP_DEBUG") ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0);
        ffxReturnCode_t r = ffx_dispatch(&fsr_ctx, &d.header);
        ok = r == FFX_API_RETURN_OK;
        if (!ok) { static int n; if (n++ < 5) LOG("[tmp] FSR dispatch failed: %u\n", r); }
    }
#endif
    vk_full_barrier(cb);
#ifdef _WIN32
    fg_frame(cb, ok, cam, jitter, rw, rh, dt, reset);
#elif defined(XCDE_LINUX_FRAMEGEN)
    if (ok) linux_fg_prepare(cam, jitter, rw, rh, dt, reset);
    else linux_fg_reset();
#else
    (void)dt;
#endif
    if (env_int("XCDE_TMP_LOG") && (frames++ % 60) == 0)
        LOG("[tmp] %ux%u -> %ux%u jitter %.3f %.3f (%u phases) %s%s near %.3f fov %.1f proj11 %.3f\n", rw, rh, ow, oh, jitter[0],
            jitter[1], jit_phases, reset ? "reset " : "", ok ? "ok" : "FAILED", cam->proj[11] * 0.5, 2 * atan(1.0 / cam->proj[5]) * 57.2958,
            cam->proj[5]);
    history_commit(cam, rw, rh, ow, oh, provider, frame, ok);
    return ok ? img[3].v : VK_NULL_HANDLE;
}
