// [XCDE] LINUX-103: property/roundtrip checks; synthetic data only.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../Monado_Linux/runtime/quality_policy.h"
#include "../../Monado_Linux/runtime/dynamic_resolution_policy.h"
#include "../../Monado_Linux/runtime/gpu_selection.h"
bool bind_settings_load(const char *k,const char *v){(void)k;(void)v;return false;}
void bind_settings_save(FILE *f){(void)f;}
static const char *ini;
static void put(const char *text){FILE *f=fopen(ini,"w");assert(f&&fputs(text,f)>=0&&!fclose(f));}
int main(int argc,char **argv) {
 assert(argc==2);ini=argv[1];setenv("XCDE_INI",ini,1);remove(ini);settings_load();
 assert(g_settings.quality_preset==QUALITY_CUSTOM && g_settings.upscale_sharpness==-1 && !g_settings.input_rumble && !g_settings.taa_off && g_settings.grass_lod==GRASS_ORIGINAL);
 assert(gfx_pack(&g_settings.graphics)==0);
 put("quality_preset=potato\nresolution=2\nupscaler=dlss\nframe_gen=1\nfps=144\nquickstep_slot=1\ncheat_gold=1\ninput_rumble=1\ngpu_uuid=ABCDEF0123456789ABCDEF0123456789\ndynamic_min_height=1080\ndynamic_max_height=720\nupscale_sharpness=0.45\ntaa=0\nshadow_map_resolution=2048\nshadow_mask=full\ntexture_filtering=16x\ngrass_distance=ultra\n");
 settings_load();assert(g_settings.quality_preset==QUALITY_POTATO && g_settings.res_mode==2); // metadata never silently applies
 assert(g_settings.frame_gen && g_settings.fps==144 && g_settings.quickstep_slot && g_settings.cheat_gold);
 assert(g_settings.dynamic_min_height==720 && g_settings.dynamic_max_height==720);
 assert(!strcmp(g_settings.gpu_uuid,"abcdef0123456789abcdef0123456789"));
 assert(g_settings.input_rumble && g_settings.taa_off && fabsf(g_settings.upscale_sharpness-.45f)<.00001f);
 assert(g_settings.graphics.value[GFX_SHADOW_MAPS]==1 && g_settings.graphics.value[GFX_SHADOW_MASK]==1 && g_settings.graphics.value[GFX_FILTERING]==4 && g_settings.grass_lod==GRASS_ULTRA);
 settings_save();settings_load();assert(g_settings.frame_gen && g_settings.quickstep_slot && g_settings.cheat_gold && g_settings.input_rumble);
 assert(!strcmp(g_settings.gpu_uuid,"abcdef0123456789abcdef0123456789"));
 assert(g_settings.graphics.value[GFX_FILTERING]==4 && fabsf(xcde_sharpness(&g_settings)-.45f)<.00001f);
 const char *bad[]={"nan","inf","-0.1","1.1","0.4junk","","9999999999999999999999999"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){char b[256];snprintf(b,sizeof b,"upscale_sharpness=%s\ndynamic_min_height=%s\ninput_rumble=invalid\ntaa=invalid\n",bad[i],bad[i]);put(b);settings_load();assert(g_settings.upscale_sharpness==-1 && g_settings.dynamic_min_height==0 && !g_settings.input_rumble && !g_settings.taa_off);}
 Settings s={0};s.frame_gen=true;s.frame_gen_smooth_motion=true;s.fps=165;s.quickstep_slot=true;s.cheat_gold=true;s.upscaler=UPSCALE_DLSS;
 xcde_quality_defaults(&s);assert(s.frame_gen && s.frame_gen_smooth_motion && s.fps==165 && s.quickstep_slot && s.cheat_gold);
 for(int p=0;p<QUALITY_CUSTOM;p++){xcde_quality_apply(&s,p);assert(s.frame_gen && s.frame_gen_smooth_motion && s.fps==165 && s.quickstep_slot && s.cheat_gold && s.upscaler==UPSCALE_DLSS);assert(s.grass_lod==GRASS_ORIGINAL && gfx_pack(&s.graphics)==0);}
 double lo,hi;s.dynamic_min_height=540;s.dynamic_max_height=720;xcde_dynamic_bounds(&s,600,&lo,&hi);assert(lo==540 && hi==600);xcde_dynamic_bounds(&s,400,&lo,&hi);assert(lo==400 && hi==400);
 s.dynamic_min_height=s.dynamic_max_height=0;xcde_dynamic_bounds(&s,1440,&lo,&hi);assert(lo==720 && hi==1440);assert(fabs(xcde_dynamic_target_ms(&s)-1000./165)<1e-10);
 uint8_t original[0x1220],changed[0x1220];memset(original,0x55,sizeof original);GraphicsOptions o={0};memcpy(changed,original,sizeof changed);gfx_overlay(changed,gfx_pack(&o));assert(!memcmp(original,changed,sizeof original));
 for(int i=0;i<GFX_COUNT;i++)for(int v=0;v<gfx_count(i);v++){GraphicsOptions one={0};assert(gfx_load_key(&one,gfx_keys[i],gfx_choice(i,v)));assert(gfx_value(gfx_pack(&one),i)==v);assert(gfx_load_key(&one,gfx_keys[i],"invalid")&&one.value[i]==0);}
 for(int i=GFX_CLOUD;i<=GFX_FUR_BLUR;i++)o.value[i]=1;gfx_overlay(changed,gfx_pack(&o));assert(changed[0x3c]==0 && changed[0x17c]==0 && changed[0x19c]==0);
 assert(gfx_count(-1)==0 && gfx_count(GFX_COUNT)==0 && gfx_value(UINT64_MAX,-1)==0 && gfx_value(UINT64_MAX,99)==0);
 remove(ini);settings_load();assert(gfx_pack(&g_settings.graphics)==0 && !g_settings.input_rumble && g_settings.grass_lod==GRASS_ORIGINAL);
 puts("Graphics quality defaults/parser/roundtrip/preset isolation/bounds/overlay contracts PASS");
}
