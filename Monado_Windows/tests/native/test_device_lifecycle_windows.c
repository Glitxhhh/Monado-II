// Execute the actual production lifecycle with owned queued work and borrowed
// resource sentinels. No game, window, provider DLL or human profile is used.
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); abort(); } } while (0)
static struct { void *dev; } g_vk;
static _Atomic float g_framegen_cap_fps;
static int texture, *borrowed, texture_freed, gpu_idle, dynamic_samples, fps_samples, provider_history;
static void *g_last_presented;
static pthread_t queued;
static _Atomic bool queued_completed;
static bool queued_pending, acquire_ok;
static bool fps_game_output, fps_repeated_output;
static int begins, ends, resets, texture_resets;
static char order[64];static int order_n;
static void note(char event){CHECK(order_n<(int)sizeof(order)-1);order[order_n++]=event;order[order_n]=0;}
static void *owned_work(void *unused) {
    (void)unused;CHECK(borrowed==&texture&&!texture_freed);
    dynamic_samples=41;fps_samples=37;provider_history=1;
    atomic_store(&queued_completed,true);return NULL;
}
void backend_drain(void) { note('D');if(queued_pending){CHECK(!pthread_join(queued,NULL));queued_pending=false;}CHECK(atomic_load(&queued_completed)); }
static void sc_wait_idle(void *device){CHECK(device&&atomic_load(&queued_completed));note('I');gpu_idle=1;}
static void tmp_fg_drain(void){CHECK(atomic_load(&queued_completed));note('G');}
static void output_scale_reset_dynamic(void){if(g_vk.dev)CHECK(gpu_idle);dynamic_samples=0;note('R');}
static void fps_overlay_reset(void){fps_samples=0;note('F');}
void vk_exec_reset(void){CHECK(gpu_idle&&borrowed==&texture&&!texture_freed);note('E');borrowed=NULL;provider_history=0;resets++;}
void vk_textures_reset(void){CHECK(gpu_idle&&!borrowed&&!provider_history);note('T');texture_freed=1;texture_resets++;}
static bool vk_frame_begin(const float rgba[4]){CHECK(rgba[0]==0&&rgba[3]==1&&!fps_samples&&!fps_game_output&&!fps_repeated_output&&atomic_load(&g_framegen_cap_fps)==0);note('B');begins++;return acquire_ok;}
static void vk_frame_end(void *image){CHECK(!image);note('P');ends++;}
static void *vk_cmd(void){return &texture;}
static void tmp_fg_idle(void *command){CHECK(command==&texture);note('N');}
#include "../../runtime/nvn/vulkan/device_lifecycle_windows.inc"
int main(void){
    g_vk.dev=&texture;borrowed=&texture;g_last_presented=&texture;atomic_store(&g_framegen_cap_fps,90);
    fps_game_output=fps_repeated_output=true;
    queued_pending=true;CHECK(!pthread_create(&queued,NULL,owned_work,NULL));
    backend_reset();
    CHECK(!strcmp(order,"DGIRFET"));CHECK(!dynamic_samples&&!fps_samples&&!provider_history&&!g_last_presented);
    CHECK(resets==1&&texture_resets==1&&texture_freed&&!borrowed&&!fps_game_output&&!fps_repeated_output&&atomic_load(&g_framegen_cap_fps)==0);
    order_n=0;order[0]=0;g_vk.dev=NULL;dynamic_samples=4;fps_samples=3;atomic_store(&g_framegen_cap_fps,90);
    backend_reset();CHECK(!strcmp(order,"RF")&&!dynamic_samples&&!fps_samples&&atomic_load(&g_framegen_cap_fps)==0&&resets==1);
    g_vk.dev=&texture;order_n=0;order[0]=0;fps_samples=8;atomic_store(&g_framegen_cap_fps,75);acquire_ok=true;fps_game_output=fps_repeated_output=true;
    backend_clear_window();CHECK(!strcmp(order,"FBNP")&&begins==1&&ends==1&&!fps_samples&&atomic_load(&g_framegen_cap_fps)==0);
    order_n=0;order[0]=0;fps_samples=8;atomic_store(&g_framegen_cap_fps,75);acquire_ok=false;
    backend_clear_window();CHECK(!strcmp(order,"FB")&&begins==2&&ends==1);
    g_vk.dev=NULL;order_n=0;order[0]=0;fps_samples=8;atomic_store(&g_framegen_cap_fps,75);
    backend_clear_window();CHECK(!strcmp(order,"F")&&begins==2&&ends==1&&!fps_samples&&atomic_load(&g_framegen_cap_fps)==0);
    puts("native production lifecycle: queued ownership/reset/borrowed textures/non-game clear PASS");return 0;
}
