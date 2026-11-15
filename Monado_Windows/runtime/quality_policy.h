// [XCDE] LINUX-103: explicit, shared quality presets; loading never reapplies one.
#pragma once
#include "settings.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { QUALITY_BALANCED, QUALITY_PERFORMANCE, QUALITY_POTATO, QUALITY_CUSTOM, QUALITY_COUNT };
static inline int xcde_quality_parse(const char *name) {
    return !strcmp(name,"balanced") ? QUALITY_BALANCED : !strcmp(name,"performance") ? QUALITY_PERFORMANCE :
           !strcmp(name,"potato") ? QUALITY_POTATO : QUALITY_CUSTOM;
}
static inline const char *xcde_quality_name(int p) {
    static const char *const names[]={"balanced","performance","potato","custom"};
    return names[p>=0 && p<QUALITY_COUNT ? p : QUALITY_CUSTOM];
}
static inline int xcde_quality_height(const char *text) {
    if(!*text)return 0;
    for(const char *p=text;*p;p++)if(*p<'0'||*p>'9')return 0;
    errno=0; char *end;long value=strtol(text,&end,10);
    return errno || *end || value<0 || value>4320 ? 0 : (int)value;
}
static inline float xcde_quality_sharpness(const char *text) {
    if(!strcmp(text,"default"))return -1.f;
    errno=0;char *end;float value=strtof(text,&end);
    return errno || end==text || *end || !isfinite(value) || value<0.f || value>1.f ? -1.f : value;
}
static inline void xcde_quality_bounds(Settings *s) {
    if(s->dynamic_min_height<0 || s->dynamic_min_height>4320)s->dynamic_min_height=0;
    if(s->dynamic_max_height<0 || s->dynamic_max_height>4320)s->dynamic_max_height=0;
    if(s->dynamic_max_height && s->dynamic_min_height>s->dynamic_max_height)s->dynamic_min_height=s->dynamic_max_height;
}
static inline void xcde_quality_defaults(Settings *s) {
    s->quality_preset=QUALITY_CUSTOM;
    s->dynamic_min_height=s->dynamic_max_height=0;
    s->upscale_sharpness=-1.f; s->taa_off=false; s->input_rumble=false;
    s->grass_lod=GRASS_ORIGINAL; memset(&s->graphics,0,sizeof s->graphics);
}
static inline void xcde_quality_apply(Settings *s,int preset) {
    if(preset<QUALITY_BALANCED || preset>=QUALITY_CUSTOM)return;
    s->quality_preset=preset;
    s->res_mode=preset==QUALITY_BALANCED ? RES_MATCH : RES_DYNAMIC;
    s->dynamic_min_height=preset==QUALITY_POTATO ? 360 : preset==QUALITY_PERFORMANCE ? 540 : 0;
    s->dynamic_max_height=preset==QUALITY_POTATO ? 540 : preset==QUALITY_PERFORMANCE ? 720 : 0;
    s->dof_off=preset!=QUALITY_BALANCED;
    // Presets do not switch GPU/upscaler, disable game effects, alter FG/FPS,
    // input, saves, cheats or optional gameplay features.
}
static inline int xcde_quality_load(Settings *s,const char *key,const char *value) {
    if(!strcmp(key,"quality_preset"))s->quality_preset=xcde_quality_parse(value);
    else if(!strcmp(key,"dynamic_min_height"))s->dynamic_min_height=xcde_quality_height(value);
    else if(!strcmp(key,"dynamic_max_height"))s->dynamic_max_height=xcde_quality_height(value);
    else if(!strcmp(key,"upscale_sharpness"))s->upscale_sharpness=xcde_quality_sharpness(value);
    else if(!strcmp(key,"taa"))s->taa_off=!strcmp(value,"0");
    else if(!strcmp(key,"input_rumble"))s->input_rumble=!strcmp(value,"1") || !strcmp(value,"true");
    else if(!strcmp(key,"grass_distance"))s->grass_lod=grass_lod_parse(value);
    else return gfx_load_key(&s->graphics,key,value);
    return 1;
}
static inline void xcde_quality_save(FILE *f,const Settings *s) {
    fprintf(f,"quality_preset = %s\ndynamic_min_height = %d\ndynamic_max_height = %d\n",xcde_quality_name(s->quality_preset),s->dynamic_min_height,s->dynamic_max_height);
    if(s->upscale_sharpness<0.f || !isfinite(s->upscale_sharpness))fprintf(f,"upscale_sharpness = default\n");
    else fprintf(f,"upscale_sharpness = %.3f\n",xcde_sharpness(s));
    fprintf(f,"taa = %d\ninput_rumble = %d\ngrass_distance = %s\n",!s->taa_off,s->input_rumble,grass_lod_ini[grass_lod_valid(s->grass_lod)]);
    gfx_save(f,&s->graphics);
}
