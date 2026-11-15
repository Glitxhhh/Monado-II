// [XCDE] LINUX-103: actual launcher persistence, no SDL initialization.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cassert>
#define main launcher_main_under_test
#include "../../Monado_Linux/tools/launcher/launcher.cpp"
#undef main
static void put(const char *s){FILE *f=fopen(path("xcde.ini").c_str(),"w");assert(f&&fputs(s,f)>=0&&!fclose(f));}
int main(int argc,char **argv){
 assert(argc==2 && mkdir(argv[1],0700)==0);g_root=argv[1];load();assert(!g.input_rumble && g.grass_lod==GRASS_ORIGINAL && g.upscale_sharpness==-1);
 put("upscaler=dlss\nframe_gen_smooth_motion=1\nquickstep_slot=1\ncheat_gold=1\nquality_preset=potato\nresolution=2\ndynamic_min_height=900\ndynamic_max_height=720\nupscale_sharpness=0.72\ninput_rumble=true\ngpu_uuid=ABCDEF0123456789ABCDEF0123456789\ngrass_distance=high\nshadow_map_resolution=2048\ntexture_filtering=16x\ncustom_key=keep\n");load();
 assert(!strcmp(g.gpu_uuid,"abcdef0123456789abcdef0123456789"));
 assert(g.input_rumble && g.upscale_sharpness>.71f && g.upscale_sharpness<.73f && g.grass_lod==GRASS_HIGH);
 assert(g.res==2 && g.quality_preset==QUALITY_POTATO && g.dynamic_min_height==720 && g.dynamic_max_height==720);
 assert(g.frame_gen_smooth_motion && g.quickstep_slot && g.cheat_enabled[5]);assert(g.graphics.value[GFX_SHADOW_MAPS]==1 && g.graphics.value[GFX_FILTERING]==4);
 assert(!g_extra.count("texture_filtering") && !g_extra.count("input_rumble"));assert(save());load();assert(g_extra.at("custom_key")=="keep" && g.frame_gen_smooth_motion && g.quickstep_slot && g.input_rumble);
 put("upscale_sharpness=nan\ninput_rumble=yes\ngrass_distance=invalid\ntexture_filtering=invalid\n");load();assert(g.upscale_sharpness==-1 && !g.input_rumble && g.grass_lod==GRASS_ORIGINAL && gfx_pack(&g.graphics)==0);
 assert(!SDL_WasInit(0));puts("Graphics launcher defaults/strict parsing/preservation/roundtrip PASS");
}
