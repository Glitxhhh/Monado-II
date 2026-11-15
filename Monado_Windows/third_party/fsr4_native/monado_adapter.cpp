// [XCDE] T-FSR4-LITE: native component adapter. No game code or assets.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "fsr411.h"
#include "fsr4_api.h"
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <cmath>
namespace {
void *create(VkPhysicalDevice p, VkDevice d, const char *assets) {
    if (!p || !d || !assets || !*assets) return nullptr;
    try {
        Fsr411::Features f; f.model = "int8_lite";
        auto ctx = std::make_unique<Fsr411::Upscaler>(p, d, assets, f);
        if (std::strcmp(ctx->ModelName(), "INT8 Lite")) {
            std::fprintf(stderr, "[fsr4] Lite assets unavailable; refusing another model\n"); return nullptr;
        }
        return ctx.release();
    } catch (const std::exception& e) { std::fprintf(stderr,"[fsr4] %s\n",e.what()); return nullptr; }
}
void destroy(void *p) { delete static_cast<Fsr411::Upscaler *>(p); }
int build(void *p, uint32_t w, uint32_t h, uint32_t ultra) {
    if (!p || !w || !h || w > 3840 || h > 2160 || ultra > 1) return 0;
    try { return static_cast<Fsr411::Upscaler *>(p)->Build(ultra != 0, w, h); }
    catch (...) { return 0; }
}
Fsr411::Image image(const MonadoFsr4Image& i) { return {i.image, i.view, i.width, i.height}; }
int record(void *p, const MonadoFsr4Frame *in) {
    if (!p || !in || !in->cmd || in->ultra > 1 || in->reset > 1 ||
        !std::isfinite(in->sharpness) || in->sharpness < 0 || in->sharpness > 1 ||
        !std::isfinite(in->jitter[0]) || !std::isfinite(in->jitter[1])) return 0;
    const MonadoFsr4Image images[] = {in->color, in->depth, in->motion, in->output};
    for (const auto& im : images) if (!im.image || !im.view || !im.width || !im.height) return 0;
    if (in->depth.width != in->color.width || in->depth.height != in->color.height ||
        in->motion.width != in->color.width || in->motion.height != in->color.height) return 0;
    try {
        Fsr411::Frame f;
        f.cmdbuf = in->cmd; f.color = image(in->color); f.depth = image(in->depth);
        f.motion = image(in->motion); f.output = image(in->output);
        f.render_width = in->color.width; f.render_height = in->color.height;
        // Monado's preparation pass writes normalized previous-minus-current UV motion.
        f.motion_scale[0] = float(f.render_width); f.motion_scale[1] = float(f.render_height);
        f.jitter[0] = in->jitter[0]; f.jitter[1] = in->jitter[1];
        f.reset = in->reset != 0; f.ultra_performance = in->ultra != 0;
        f.auto_exposure = true; f.sharpen = in->sharpness > 0.0f; f.sharpness = in->sharpness;
        return static_cast<Fsr411::Upscaler *>(p)->Record(f);
    } catch (...) { return 0; }
}
const char *error(void *p) { return p ? static_cast<Fsr411::Upscaler *>(p)->Error().c_str() : "null FSR4 context"; }
const MonadoFsr4Api api{MONADO_FSR4_ABI,create,destroy,build,record,error};
}
#ifdef _WIN32
#define MONADO_FSR4_EXPORT __declspec(dllexport)
#else
#define MONADO_FSR4_EXPORT __attribute__((visibility("default")))
#endif
extern "C" MONADO_FSR4_EXPORT const MonadoFsr4Api *monado_fsr4_get_api(uint32_t abi) {
    return abi == MONADO_FSR4_ABI ? &api : nullptr;
}
