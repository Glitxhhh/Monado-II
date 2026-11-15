// [XCDE] LINUX-103: actual quality UI callbacks, no native window/game.
#include "imgui.h"
#include <cassert>
#include <cstring>
namespace ImGui {
static bool QualityTestCombo(const char *,int *,const char *const[],int,int=-1);
static bool QualityTestHeader(const char *,ImGuiTreeNodeFlags=0){return true;}
}
#define Combo QualityTestCombo
#define CollapsingHeader QualityTestHeader
#include "../../Monado_Linux/runtime/ui/quality_controls_linux.h"
#undef Combo
#undef CollapsingHeader
static int desired_preset=-1,desired_bloom=-1,desired_grass=-1,seen_graphics;
namespace ImGui {
static bool QualityTestCombo(const char *label,int *selected,const char *const items[],int count,int) {
 assert(items && count>0);
 if(!strcmp(label,"Quality preset") && desired_preset>=0){*selected=desired_preset;return true;}
 if(!strcmp(label,"Bloom") && desired_bloom>=0){assert(count==3);*selected=desired_bloom;return true;}
 if(!strcmp(label,"Grass distance (restart)") && desired_grass>=0){assert(count==GRASS_PRESET_COUNT);*selected=desired_grass;return true;}
 if(strstr(label,"Effects resolution"))assert(!"retired control exposed");
 if(!strcmp(label,"Shadow maps (restart)")){assert(count==2 && strstr(items[1],"2048"));seen_graphics++;}
 return false;
}
}
static bool frame(Settings *s) {
 ImGui::NewFrame();ImGui::SetNextWindowSize(ImVec2(1000,900));ImGui::Begin("Quality fixture");
 bool changed=monado_quality_controls_linux(s,true);ImGui::End();ImGui::Render();return changed;
}
int main(){
 ImGui::CreateContext();ImGuiIO &io=ImGui::GetIO();io.DisplaySize=ImVec2(1024,1024);io.DeltaTime=1.f/60;io.IniFilename=nullptr;
 unsigned char *pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);assert(pixels&&w>0&&h>0);
 Settings s{};xcde_quality_defaults(&s);s.upscaler=UPSCALE_DLSS;s.fps=165;s.frame_gen_smooth_motion=true;s.quickstep_slot=true;s.cheat_gold=true;
 assert(!frame(&s));assert(s.quality_preset==QUALITY_CUSTOM && s.upscale_sharpness==-1);
 desired_preset=QUALITY_PERFORMANCE;assert(frame(&s));desired_preset=-1;
 assert(s.res_mode==RES_DYNAMIC && s.dynamic_min_height==540 && s.dynamic_max_height==720 && s.dof_off);
 assert(s.fps==165 && s.upscaler==UPSCALE_DLSS && s.frame_gen_smooth_motion && s.quickstep_slot && s.cheat_gold);
 desired_bloom=1;desired_grass=GRASS_HIGH;assert(frame(&s));desired_bloom=desired_grass=-1;
 assert(s.graphics.value[GFX_BLOOM]==1 && s.grass_lod==GRASS_HIGH && s.quality_preset==QUALITY_CUSTOM && seen_graphics>0);
 ImGui::DestroyContext();puts("Graphics actual UI preset/extra controls/original defaults/feature preservation PASS");
}
