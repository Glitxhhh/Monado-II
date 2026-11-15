// [XCDE] LINUX-103: actual output-scale controller with authored timings.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../../runtime/settings.h"
#include "../../runtime/nvn/output_scale.h"

Settings g_settings;
static double now;
static double width=2560.,height=1440.;
static OutScale frame(double dt, double gpu) {
    now += dt / 1000.;
    OutScale result;
    output_scale(width,height,true,now,gpu,&result);
    return result;
}
static double actual_height(OutScale s) { return s.sy * 720.; }
static void bounds(double low,double high) {
    g_settings.dynamic_min_height=(int)low; g_settings.dynamic_max_height=(int)high;
}
static void setup(int fps) {
    memset(&g_settings,0,sizeof g_settings);
    g_settings.aspect=ASPECT_ORIGINAL;g_settings.res_mode=RES_DYNAMIC;
    g_settings.upscaler=UPSCALE_BILINEAR;g_settings.upscale_quality=UQ_QUALITY;g_settings.fps=fps;
    output_scale_set_base(1280,720);
#ifndef MONADO_DRS_LEGACY
    output_scale_reset_dynamic();
#endif
    now=1.;width=2560.;height=1440.;
}
static OutScale run(unsigned n,double dt,double gpu) {
    OutScale s={0};for(unsigned i=0;i<n;i++)s=frame(dt,gpu);return s;
}
static void bounded_height(void) {
    setup(120);bounds(540,720);
    assert(fabs(actual_height(frame(8.333,7.))-720.)<.00001);
    OutScale s=run(1200,20.,19.);
    assert(fabs(actual_height(s)-540.)<.00001);
    s=run(1800,5.,1.);assert(fabs(actual_height(s)-720.)<.00001);
    // Endpoints need not be exact5% steps. Both must remain reachable.
    bounds(550,905);s=frame(8.,6.);assert(actual_height(s)>=550 && actual_height(s)<=905);
    s=run(1200,20.,19.);assert(fabs(actual_height(s)-550.)<.00001);
    s=run(1800,5.,1.);assert(fabs(actual_height(s)-905.)<.00001);
}
static void native_budget(void) {
    setup(30);bounds(540,720);OutScale s=run(300,1000./30.,25.);
    assert(fabs(actual_height(s)-720.)<.00001); //25ms fits30native, not60.
    g_settings.fps=120;s=run(600,10.,8.5);assert(actual_height(s)<720.);
    // GPU headroom means a CPU/present bottleneck must not lower resolution.
    setup(120);bounds(540,720);s=run(300,40.,1.);assert(fabs(actual_height(s)-720.)<.00001);
}
static void native_cap(void) {
    setup(180);bounds(540,720);
#ifndef MONADO_DRS_LEGACY
    output_scale_set_native_fps(90.f);
#endif
    OutScale s=run(300,1000./90.,8.);
    assert(g_settings.fps==180 && fabs(actual_height(s)-720.)<.00001);
#ifndef MONADO_DRS_LEGACY
    output_scale_set_native_fps(0.f);
#endif
    s=run(600,1000./180.,8.);assert(actual_height(s)<720.);
}
static void resizing(void) {
    setup(120);bounds(540,720);OutScale s=frame(8.,7.);assert(actual_height(s)==720.);
    width=640.;height=360.;s=frame(8.,7.);assert(actual_height(s)==360.);
    width=2560.;height=1440.;s=frame(8.,7.);assert(actual_height(s)>=540. && actual_height(s)<=720.);
    g_settings.aspect=ASPECT_32_9;width=1920.;height=1080.;
    s=frame(8.,7.);assert(fabs(s.vh-540.)<.00001 && fabs(actual_height(s)-540.)<.00001);
    assert(fabs((1280*s.sx)/(720*s.sy)-32./9.)<.00001);
    // A target-budget change discards old slow samples/cooldown.
    g_settings.fps=30;s=run(300,1000./30.,25.);assert(fabs(actual_height(s)-540.)<.00001);
}
static void legacy(void) {
    setup(120);bounds(0,0);OutScale s=frame(8.,7.);assert(actual_height(s)==1440.);
    s=run(1200,20.,19.);assert(actual_height(s)==720.);
    s=run(6000,5.,1.);assert(actual_height(s)==1440.);
    // Legacy automatic mode keeps its existing60FPS target despite selected120.
    setup(120);s=run(300,10.,8.5);assert(actual_height(s)==1440.);
}
static void native_aa(void) {
    const int providers[]={UPSCALE_DLSS,UPSCALE_FSR3,UPSCALE_FSR4_LITE};
    const int resolutions[]={RES_DYNAMIC,1,2,RES_Q50};
    for(unsigned p=0;p<sizeof providers/sizeof providers[0];p++)for(unsigned q=0;q<4;q++) {
        setup(120);g_settings.aspect=ASPECT_21_9;
        g_settings.upscaler=providers[p];g_settings.upscale_quality=UQ_NATIVE;g_settings.res_mode=resolutions[q];
        width=1304.;height=744.;bounds(360,540);
        OutScale s=run(90,20.,19.);
        assert(lroundf((float)(1280*s.sx))==lroundf((float)s.vw));
        assert(lroundf((float)(720*s.sy))==lroundf((float)s.vh));
    }
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"bounds"))bounded_height();
    else if(!strcmp(argv[1],"budget"))native_budget();
    else if(!strcmp(argv[1],"cap"))native_cap();
    else if(!strcmp(argv[1],"resize"))resizing();
    else if(!strcmp(argv[1],"legacy"))legacy();
    else if(!strcmp(argv[1],"native-aa"))native_aa();
    else assert(!"unknown scenario");
    puts("Dynamic resolution output integration: PASS");return 0;
}
