// [XCDE] LINUX-097: real parser/save contracts; no game or user profile.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../Monado_Linux/runtime/settings.h"
#include "../../Monado_Linux/runtime/input_bind.h"
bool bind_settings_load(const char *k,const char *v){(void)k;(void)v;return false;}
void bind_settings_save(FILE *f){(void)f;}
static const char *ini;
static void put(const char *text){FILE *f=fopen(ini,"w");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static void expect(int fsr,int smooth,int match,int cut){assert(g_settings.frame_gen==fsr);assert(g_settings.frame_gen_smooth_motion==smooth);assert(g_settings.frame_gen_match_display==match);assert(g_settings.cutscene_mode==cut);}
int main(int argc,char **argv){
 assert(argc==2);ini=argv[1];assert(!setenv("XCDE_INI",ini,1));
 remove(ini);settings_load();expect(0,0,0,0);
 put("upscaler = dlss\nframe_gen = 1\nfps = 120\n");settings_load();expect(1,0,0,0);assert(g_settings.fps==120);
 put("upscaler = dlss\nframe_gen_smooth_motion = true\nframe_gen_match_display = 1\nfps = 120\n");settings_load();expect(0,1,1,0);assert(g_settings.fps==120);
 settings_save();settings_load();expect(0,1,1,0);assert(g_settings.fps==120);
 put("upscaler = dlss\nframe_gen_smooth_motion = 1\nframe_gen = 1\nframe_gen_match_display = true\n");settings_load();expect(1,0,1,0);
 put("upscaler = dlss\nframe_gen = 1\nframe_gen_smooth_motion = 1\n");settings_load();expect(1,0,0,0);
 put("upscaler = dlss\ncutscenes = 30fg\nframe_gen_smooth_motion = 1\n");settings_load();expect(0,0,0,2);
 const char *bad[]={"2","yes","TRUE","1junk","-1","false","","1 #comment","true ;comment"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){char text[256];snprintf(text,sizeof text,"upscaler = dlss\nframe_gen_smooth_motion = %s\nframe_gen_match_display = %s\n",bad[i],bad[i]);put(text);settings_load();expect(0,0,0,0);}
 put("frame_gen_smooth_motion = 1\nframe_gen_smooth_motion = invalid\n");settings_load();expect(0,0,0,0);
 // Values retain their full trimmed text; inline comments are not supported.
 put("cutscenes = 30fg #comment\nframe_gen_smooth_motion = 1\n");settings_load();expect(0,1,0,0);
 put("frame_gen = 1 #comment\nframe_gen_smooth_motion = 1\nupscaler = dlss\n");settings_load();expect(1,0,0,0);
 put("frame_gen_smooth_motion = 1\nframe_gen_match_display = 1\n");settings_load();expect(0,1,1,0);
 remove(ini);settings_load();expect(0,0,0,0);
 puts("Frame generation runtime settings: legacy/defaults/exclusive backends/strict opt-ins/native cap/roundtrip PASS");return 0;
}
