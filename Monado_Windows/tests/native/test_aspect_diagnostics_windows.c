// Accepted bounded normal-log aspect transitions, using the production helper.
#include "../../runtime/nvn/output_scale.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);abort();} } while(0)
static struct {int aspect;} g_settings;
static struct {struct {uint32_t width,height;} sc_extent;} g_vk;
static bool enabled;
static bool xcde_aspect_enabled(void){return enabled;}
static unsigned logged,suppressed;
static char last[512];
static void log_note(const char *format,...){va_list args;va_start(args,format);vsnprintf(last,sizeof last,format,args);va_end(args);logged++;if(strstr(last,"suppressed"))suppressed++;}
#define LOG(...) log_note(__VA_ARGS__)
#include "../../runtime/nvn/vulkan/aspect_output_diagnostics.inc"
int main(void){
    OutScale scale={1,1,1.3,10,20,2560,1440};g_settings.aspect=2;g_vk.sc_extent.width=2560;g_vk.sc_extent.height=1440;
    aspect_output_note(&scale);CHECK(!logged);
    enabled=true;aspect_output_note(&scale);CHECK(logged==1&&strstr(last,"mode 2 drawable 2560x1440")&&strstr(last,"1.300000"));
    aspect_output_note(&scale);CHECK(logged==1);
    scale.sx=1.5;aspect_output_note(&scale);CHECK(logged==2);
    g_vk.sc_extent.width=3440;aspect_output_note(&scale);CHECK(logged==3);
    g_settings.aspect=3;aspect_output_note(&scale);CHECK(logged==4);
    for(int i=0;i<100;i++){scale.shape+=0.01;aspect_output_note(&scale);}
    CHECK(logged==33&&suppressed==1);
    puts("native aspect transition diagnostics: eligibility/change/bounds PASS");return 0;
}
