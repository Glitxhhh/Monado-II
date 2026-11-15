// [XCDE] LINUX-103: actual runtime overlays, latched options and bounded storage.
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include "../../Monado_Linux/runtime/hle.h"
#include "../../Monado_Linux/runtime/settings.h"
#include "../../Monado_Linux/runtime/hle/foliage_distance.h"
Settings g_settings;
void plat_commit(uint64_t a,uint64_t n){(void)a;(void)n;}
bool plat_reset_guest(uint64_t lo,uint64_t hi){return mmap((void*)lo,hi-lo,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==(void*)lo;}
void rt_fatal_trace(void){abort();}
extern ResetFn __start_resets[],__stop_resets[];
static void reset(void){for(ResetFn *f=__start_resets;f<__stop_resets;f++)(*f)();}
int main(void){
 const uint64_t view=0x7280000000ull;assert(mmap((void*)view,0x2000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)view);
 uint8_t *p=(uint8_t*)view;memset(p,0x55,0x1220);g_settings.grass_lod=GRASS_ORIGINAL;
 GraphicsOptions o={0};xcde_graphics_publish(&o);xcde_graphics_finish(view);for(int i=0;i<0x1220;i++)assert(p[i]==0x55);
 o.value[GFX_BLOOM]=1;xcde_graphics_publish(&o);xcde_graphics_begin(view);xcde_graphics_finish(view);assert(p[0x3c]==0);
 memset(&o,0,sizeof o);xcde_graphics_publish(&o);xcde_graphics_begin(view);xcde_graphics_finish(view);assert(p[0x3c]==0x55); // Original truly restores
 o.value[GFX_BLOOM]=1;xcde_graphics_publish(&o);xcde_graphics_finish(view);p[0x3c]=7;xcde_graphics_publish(&(GraphicsOptions){0});xcde_graphics_begin(view);assert(p[0x3c]==7); // native change supersedes overlay
 xcde_graphics_copy(view);xcde_graphics_copy(0);xcde_graphics_copy(UINT64_MAX);
 o.value[GFX_SHADOW_MAPS]=1;xcde_graphics_publish(&o);assert(xcde_shadow_map_size()==2048);o.value[GFX_SHADOW_MAPS]=0;xcde_graphics_publish(&o);assert(xcde_shadow_map_size()==2048);reset();assert(xcde_shadow_map_size()==1024);
 assert(xcde_foliage_pool_layout(0)==0x3a5a20 && xcde_foliage_pool_layout(1)==0x2000 && xcde_foliage_pool_layout(5)==0x380000);assert(xcde_foliage_pool_alloc(1)==0);assert(!xcde_foliage_pool_free(view));
 assert(xcde_foliage_distance(2,80)==80 && xcde_foliage_shading_distance(2,80)==80);
 g_settings.grass_lod=GRASS_ULTRA;assert(xcde_foliage_distance(0,50)==50);reset();assert(xcde_foliage_distance(0,50)==150);assert(xcde_foliage_distance(7000,8000)==8000);
 const uint64_t base=HLE_HEAP+HLE_HEAP_SIZE,span=0x10000000;
 assert(mmap((void*)base,span,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE|MAP_NORESERVE,-1,0)==(void*)base);
 assert(xcde_foliage_pool_layout(0)<0x02000000 && xcde_foliage_pool_layout(1)==XCDE_FOLIAGE_BITMAP_BYTES);
 uint64_t slots[8];for(unsigned i=0;i<8;i++){slots[i]=xcde_foliage_pool_alloc(i+1);assert(slots[i]==base+i*0x02000000);*(char*)slots[i]=1;}
 for(unsigned i=0;i<8;i++){assert(xcde_foliage_pool_free(slots[i]));assert(*(char*)slots[i]==0);}assert(xcde_foliage_pool_alloc(99)==base);assert(xcde_foliage_pool_free(base));
 assert(!xcde_foliage_pool_free(base+1) && !xcde_foliage_pool_free(base+span));
 reset();g_settings.grass_lod=GRASS_LOW;assert(xcde_foliage_distance(0,50)==25 && xcde_foliage_pool_alloc(1)==0);
 munmap((void*)base,span);munmap((void*)view,0x2000);
 puts("Graphics actual overlay restoration/startup latch/grass Original allocator/private pool lifecycle PASS");
}
