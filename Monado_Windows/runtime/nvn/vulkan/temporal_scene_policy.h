// Authored Windows camera/jitter contract. Scene-depth and no-depth world draw
// design reference: Monado-Reborn (MIT), 3428bb323043477d522c4bb00dfa9fbcc4f13e86,
// runtime/nvn/vulkan/exec.c:5913-5981. Keep accepted world-space history math.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

static inline bool tmp_scene_depth_eligible(uint32_t width,uint32_t height,uint32_t base_w,uint32_t base_h) {
    if(!width||!height||!base_w||!base_h||width>16384||height>16384||base_w>16384||base_h>16384)return false;
    // A scene-sized depth attachment, excluding square shadows, tiny copies and
    // half-sized reflections. Host render scaling does not change this guest grid.
    return width<=base_w&&height<=base_h&&(uint64_t)width*2>base_w&&(uint64_t)height*2>base_h&&
        (uint64_t)width*base_h*100>=(uint64_t)height*base_w*99&&
        (uint64_t)width*base_h*100<=(uint64_t)height*base_w*101;
}

typedef struct TmpJitterGrid {
    uint32_t verified_w,verified_h;
    float uv[2];
} TmpJitterGrid;
static inline void tmp_jitter_grid_begin(TmpJitterGrid *grid,const float sample[2]) {
    grid->uv[0]=grid->uv[1]=0;
    if(!grid->verified_w||!grid->verified_h||!isfinite(sample[0])||!isfinite(sample[1]))return;
    grid->uv[0]=sample[0]/grid->verified_w;grid->uv[1]=sample[1]/grid->verified_h;
}
static inline void tmp_jitter_grid_offset(const TmpJitterGrid *grid,uint32_t width,uint32_t height,float offset[2]) {
    offset[0]=grid->uv[0]*width;offset[1]=grid->uv[1]*height;
}
static inline void tmp_jitter_grid_verify(TmpJitterGrid *grid,uint32_t width,uint32_t height) {
    // The actual final color extent is learned at presentation. Never guess it
    // from an output resolution or the original Switch/NVN dimensions.
    grid->verified_w=width&&width<=16384?width:0;
    grid->verified_h=height&&height<=16384?height:0;
}
