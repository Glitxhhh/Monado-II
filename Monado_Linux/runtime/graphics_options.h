// [XCDE] LINUX-103: adapted from Monado-Reborn contributors (MIT),
// commit 3cc9eeb46dc879fe36929d13758754c429b8377b; see NOTICE.md.
// [XCDE] T-GRAPHICS-SCALABILITY: shared typed controls and persistence.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { GFX_SHADOWS, GFX_SHADOW_MASK, GFX_EFFECTS_RES, GFX_CLOUD, GFX_AO,
       GFX_REFLECTIONS, GFX_BLOOM, GFX_GODRAYS, GFX_SHAFTS, GFX_MOTION_BLUR,
       GFX_FUR_BLUR, GFX_FILTERING, GFX_SHADOW_MAPS, GFX_PROP_LOD, GFX_BLOOM_STABILITY, GFX_COUNT };
typedef struct { int value[GFX_COUNT]; } GraphicsOptions;
static const char *const gfx_keys[GFX_COUNT] = {"shadows", "shadow_mask", "effects_resolution", "cloud_shadows", "ambient_occlusion",
    "reflections", "bloom", "godrays", "light_shafts", "motion_blur", "fur_blur", "texture_filtering", "shadow_map_resolution", "prop_lod", "bloom_stability"};
static const char *const gfx_labels[GFX_COUNT] = {"Shadows", "Shadow mask (restart)", "Effects resolution (restart)", "Cloud shadows", "Ambient occlusion",
    "Screen-space reflections", "Bloom", "God rays", "Light shafts", "Motion blur", "Fur blur", "Texture filtering", "Shadow maps (restart)", "Tree / rock LOD (restart)", "Bloom stability (experimental)"};
// Retired effects-resolution Full (2026-11-12): it set TransReduction=off, which the game never shipped. The
// occlusion boxes then test a full-size depth whose only content is a 640x360 quarter: field objects popped in and
// out on camera pans (prologue). Only Original remains; the slot stays so indices and live packing are unchanged.
static inline int gfx_count(int i) { if(i<0 || i>=GFX_COUNT)return 0; return i==GFX_EFFECTS_RES ? 1 : i==GFX_PROP_LOD ? 4 : i==GFX_FILTERING ? 5 : i==GFX_SHADOWS || i==GFX_SHADOW_MASK || i==GFX_SHADOW_MAPS ? 2 : 3; }
// Retired 4096 choice (index 2) exceeded the native render-pool layout.
// Normalize persisted/in-memory legacy choices to the supported 2048 mode.
static inline int gfx_normalize(int i,int v) {
    if(i==GFX_SHADOW_MAPS && v==2)return 1;
    return v>=0 && v<gfx_count(i) ? v : 0;
}
static inline const char *gfx_choice(int i,int v) {
    static const char *const tri[]={"original","off","on"}, *const full[]={"original","full"}, *const af[]={"original","2x","4x","8x","16x"};
    v=gfx_normalize(i,v);
    if(i==GFX_BLOOM_STABILITY) { static const char *const x[]={"original","full","stable"};return x[v]; }
    if(i==GFX_SHADOW_MAPS) { static const char *const x[]={"original","2048"};return x[v]; }
    if(i==GFX_PROP_LOD) { static const char *const x[]={"original","low","high","ultra"};return x[v]; }
    return i==GFX_FILTERING ? af[v] : i==GFX_SHADOW_MASK || i==GFX_EFFECTS_RES ? full[v] : tri[v];
}
static inline const char *gfx_display(int i,int v) {
    static const char *const tri[]={"Original","Off","On"}, *const full[]={"Original","Full resolution"}, *const af[]={"Original","2x","4x","8x","16x"};
    v=gfx_normalize(i,v);
    if(i==GFX_BLOOM_STABILITY) { static const char *const x[]={"Original","Full footprint","Stable (Karis)"};return x[v]; }
    if(i==GFX_SHADOW_MAPS) { static const char *const x[]={"Original (1024 px)","High (2048 px)"};return x[v]; }
    if(i==GFX_PROP_LOD) { static const char *const x[]={"Original (1x)","Low (0.5x)","High (1.5x)","Ultra (2x)"};return x[v]; }
    return i==GFX_FILTERING ? af[v] : i==GFX_SHADOW_MASK || i==GFX_EFFECTS_RES ? full[v] : tri[v];
}
static inline int gfx_load_key(GraphicsOptions *o,const char *key,const char *value) {
    for(int i=0;i<GFX_COUNT;i++) if(!strcmp(key,gfx_keys[i])) {
        o->value[i]=0;
        if(i==GFX_SHADOW_MAPS && !strcmp(value,"4096")) {
            o->value[i]=1;
            fprintf(stderr,"[graphics] 4096px shadow maps exceed the supported native render-pool layout; using 2048px\n");
            return 1;
        }
        if(i==GFX_EFFECTS_RES && !strcmp(value,"full")) {
            fprintf(stderr,"[graphics] full effects resolution is no longer offered (it broke the game's occlusion culling); using original\n");
            return 1;
        }
        for(int v=0;v<gfx_count(i);v++) if(!strcmp(value,gfx_choice(i,v)))o->value[i]=v;
        return 1;
    }
    return 0;
}
static inline void gfx_save(FILE *f,const GraphicsOptions *o) {
    for(int i=0;i<GFX_COUNT;i++)fprintf(f,"%s = %s\n",gfx_keys[i],gfx_choice(i,o->value[i]));
}
static inline uint64_t gfx_pack(const GraphicsOptions *o) {
    uint64_t b=0;
    for(int i=0;i<GFX_COUNT;i++) { int v=o->value[i]; v=gfx_normalize(i,v); b|=(uint64_t)v<<(3*i); }
    return b;
}
static inline int gfx_value(uint64_t b,int i) { return i>=0 && i<GFX_COUNT ? gfx_normalize(i,(int)((b>>(3*i))&7)) : 0; }
// Only the render frame's working copy is changed; the authored config/mod stays intact.
static inline void gfx_overlay(uint8_t *p,uint64_t b) {
    static const unsigned offsets[]={0,0,0,0x3ad,0x17c,0x492,0x3c,0xec,0x520,0x204,0x53d};
    for(int i=GFX_CLOUD;i<=GFX_FUR_BLUR;i++) {
        int v=gfx_value(b,i); if(v==1 || v==2)p[offsets[i]]=(uint8_t)(v==2);
    }
    if(gfx_value(b,GFX_AO)==1)p[0x19c]=0; // disable classic AO as well, preserving it for Original.
    if(gfx_value(b,GFX_SHADOWS)==1) {
        const float zero=0;
        const unsigned f[]={0x368,0x47c,0x2b0,0x2b4};
        for(unsigned i=0;i<sizeof(f)/sizeof(*f);i++)memcpy(p+f[i],&zero,4);
        p[0x3ad]=0; // Shadows Off also suppresses cloud-shadow contribution.
    }
}
#ifdef __cplusplus
extern "C" {
#endif
void xcde_graphics_publish(const GraphicsOptions *o);
int xcde_graphics_option(int index); // atomic current choice for renderer threads
void xcde_graphics_copy(uint64_t destination);
void xcde_graphics_forget(uint64_t destination);
void xcde_graphics_begin(uint64_t destination);
void xcde_graphics_finish(uint64_t destination);
uint32_t xcde_shadow_map_size(void);
void xcde_prop_lod(uint64_t data,uint32_t size);
#ifdef __cplusplus
}
#endif

// [XCDE] Spanish labels and values for the bilingual launcher / F1 menu.
static const char *const gfx_labels_es[GFX_COUNT] = {"Sombras", "Máscara de sombras (al reiniciar)", "Resolución de efectos (al reiniciar)",
    "Sombras de nubes", "Oclusión ambiental", "Reflejos en pantalla", "Bloom", "Rayos de luz (god rays)", "Haces de luz",
    "Desenfoque de movimiento", "Desenfoque del pelo", "Filtrado de texturas", "Mapas de sombras (al reiniciar)",
    "LOD de árboles / rocas (al reiniciar)", "Estabilidad de bloom (experimental)"};
static inline const char *gfx_label(int i, int en) { return en ? gfx_labels[i] : gfx_labels_es[i]; }
static inline const char *gfx_display_lang(int i, int v, int en) {
    if (en) return gfx_display(i, v);
    static const char *const tri[] = {"Original", "Desactivado", "Activado"}, *const full[] = {"Original", "Resolución completa"};
    v = gfx_normalize(i, v);
    if (i == GFX_SHADOW_MAPS) { static const char *const x[] = {"Original (1024 px)", "Alta (2048 px)"}; return x[v]; }
    if (i == GFX_PROP_LOD) { static const char *const x[] = {"Original (1x)", "Bajo (0,5x)", "Alto (1,5x)", "Ultra (2x)"}; return x[v]; }
    if (i == GFX_FILTERING) return gfx_display(i, v);
    return i == GFX_SHADOW_MASK || i == GFX_EFFECTS_RES ? full[v] : tri[v];
}
// The options this adds next to the game effects the launcher already toggles (SSAO, SSR, bloom, god rays, cloud
// shadows: quality_policy.h effects_off), in display order. The others stay available as xcde.ini keys.
static const int gfx_ui_options[] = {GFX_SHADOWS, GFX_SHADOW_MAPS, GFX_SHADOW_MASK, GFX_PROP_LOD, GFX_FILTERING,
                                     GFX_SHAFTS, GFX_MOTION_BLUR, GFX_FUR_BLUR};
