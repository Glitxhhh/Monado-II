// [XCDE] LINUX-097: actual launcher config and normalization, no SDL init.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cstdio>
#include <cstdlib>
#define main launcher_main_under_test
#include "../../Monado_Linux/tools/launcher/launcher.cpp"
#undef main
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);abort();}}while(0)
static void put(const char *s){FILE *f=fopen(path("xcde.ini").c_str(),"w");CHECK(f);CHECK(fputs(s,f)>=0);CHECK(!fclose(f));}
static void expect(bool fsr,bool smooth,bool match,bool cut){CHECK(g.frame_gen==fsr);CHECK(g.frame_gen_smooth_motion==smooth);CHECK(g.frame_gen_match_display==match);CHECK(g.cut_fg==cut);}
int main(int argc,char **argv){
 CHECK(argc==2&&mkdir(argv[1],0700)==0);g_root=argv[1];
 // Exercise the production argument-value parser without retaining main's
 // full renderer dependencies or initializing SDL.
 CHECK(launcher_linux::smooth_motion_hint("1")&&launcher_linux::smooth_motion_layer_available);
 CHECK(launcher_linux::smooth_motion_hint("0")&&!launcher_linux::smooth_motion_layer_available);
 CHECK(!launcher_linux::smooth_motion_hint("invalid")&&!launcher_linux::smooth_motion_layer_available);
 CHECK(!launcher_linux::smooth_motion_hint(nullptr));
 CHECK(!SDL_WasInit(0)&&!SDL_GetCurrentVideoDriver());load();expect(0,0,0,0);
 put("upscaler = dlss\nframe_gen = 1\nfps = 120\n");load();expect(1,0,0,0);
 put("frame_gen_smooth_motion = true\nframe_gen_match_display = 1\nfps = 120\ncustom_key = retained\n");load();expect(0,1,1,0);CHECK(!g_extra.count("frame_gen_smooth_motion")&&!g_extra.count("frame_gen_match_display"));
 CHECK(save());load();expect(0,1,1,0);CHECK(g_extra.at("custom_key")=="retained");
 put("frame_gen_smooth_motion = 1\nframe_gen = 1\n");load();expect(1,0,0,0);
 put("cutscenes = 30fg\nframe_gen_smooth_motion = 1\n");load();expect(0,0,0,1);
 put("frame_gen_smooth_motion = invalid\nframe_gen_match_display = 2\n");load();expect(0,0,0,0);
 // Match the runtime's full-value comparison, including no inline comments.
 put("frame_gen_smooth_motion = 1 #comment\nframe_gen_match_display = true ;comment\n");load();expect(0,0,0,0);
 put("cutscenes = 30fg #comment\nframe_gen_smooth_motion = 1\n");load();expect(0,1,0,0);
 put("frame_gen = 1 #comment\nframe_gen_smooth_motion = 1\n");load();expect(1,0,0,0);
 put("frame_gen_smooth_motion = 1\n");load();expect(0,1,0,0);launcher_linux::smooth_motion_layer_available=false;linux_graphics_normalize();expect(0,0,0,0);
 put("frame_gen_smooth_motion = 1\n");load();launcher_linux::smooth_motion_layer_available=true;linux_graphics_normalize();expect(0,1,0,0);
 // SR availability is distinct from compiled FG: an honest legacy no-DLSS
 // payload may include FSR3 SR but omit the frame-generation companions.
 launcher_linux::game=std::string(argv[1])+"/xcde-linux";g_fsr3_available=true;
 CHECK(!linux_framegen_packaged());
 put("upscaler = fsr3\nframe_gen = 1\ncutscenes = 30fg\n");load();linux_graphics_normalize();
 CHECK(g.upscaler==2&&g_fsr3_available);expect(0,0,0,0);
 remove(path("xcde.ini").c_str());load();expect(0,0,0,0);
 CHECK(!SDL_WasInit(0));puts("Frame generation launcher settings: defaults/legacy/exclusivity/component availability/extras/roundtrip PASS");
}
