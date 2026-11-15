// [XCDE] LINUX-103: actual native FSR4 glue negative/fallback/lifetime tests.
#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../runtime/nvn/vulkan/vk.h"
#include "../../runtime/nvn/vulkan/temporal.h"
#include "../../runtime/nvn/vulkan/fsr4_api.h"
#include "../../runtime/paths.h"
#include "../../runtime/settings.h"
VkG g_vk;Settings g_settings;
typedef struct {TexBackend b;VkImageView v;}Img;
enum {F_LINEAR=VK_FORMAT_R16G16B16A16_SFLOAT,F_MV=VK_FORMAT_R16G16_SFLOAT,F_DEPTH=VK_FORMAT_R32_SFLOAT};
static bool module_present=true,open_ok=true,abi_ok=true,create_ok=true,build_ok=true,record_ok=true;
static unsigned opened,closed,created,built,recorded,destroyed,waited;static uint64_t pending=10,completed;
static MonadoFsr4Frame latest;static bool ffx_available=true,dlss_ready;
void rt_fatal_trace(void){abort();}
uint64_t vk_pending_seq(void){return pending;}uint64_t vk_completed_seq(void){return completed;}
void vk_wait_seq(uint64_t value){assert(value<pending);waited++;completed=value;}
bool rt_resource(const char *name,const char *relative,char *out,size_t size){assert(!strcmp(name,"libmonado-fsr4.so") && !relative);if(!module_present)return false;assert(snprintf(out,size,"/fixture/libmonado-fsr4.so")<(int)size);return true;}
static void *fixture_create(VkPhysicalDevice physical,VkDevice device,const char *path){assert(physical && device && path);created++;return create_ok?(void*)1:NULL;}
static void fixture_destroy(void *context){assert(context==(void*)1);destroyed++;}
static int fixture_build(void *context,uint32_t w,uint32_t h,uint32_t ultra){assert(context==(void*)1 && w && h && ultra<2);built++;return build_ok;}
static int fixture_record(void *context,const MonadoFsr4Frame *frame){assert(context==(void*)1 && frame);recorded++;latest=*frame;return record_ok;}
static const char *fixture_error(void *context){assert(context==(void*)1);return "authored failure";}
static const MonadoFsr4Api api={MONADO_FSR4_ABI,fixture_create,fixture_destroy,fixture_build,fixture_record,fixture_error};
static const MonadoFsr4Api *fixture_get(uint32_t abi){assert(abi==2);return abi_ok?&api:NULL;}
static void *fixture_open(const char *path,int flags){assert(!strcmp(path,"/fixture/libmonado-fsr4.so") && flags==(RTLD_NOW|RTLD_LOCAL));opened++;return open_ok?(void*)2:NULL;}
static void *fixture_symbol(void *module,const char *name){assert(module==(void*)2 && !strcmp(name,"monado_fsr4_get_api"));return(void*)fixture_get;}
static int fixture_close(void *module){assert(module==(void*)2);closed++;return 0;}
#define dlopen fixture_open
#define dlsym fixture_symbol
#define dlclose fixture_close
#include "../../runtime/nvn/vulkan/temporal_fsr4_linux.inc"
#undef dlopen
#undef dlsym
#undef dlclose
static int active_provider=-1,active_upscaler=-1,active_quality=-1;
static unsigned jit_index,jit_phases;static uint64_t snap_frame;static unsigned invalidated;
static void history_invalidate(void){invalidated++;}
static bool diag_deterministic(void){return false;}
static int env_int(const char *name){(void)name;return 0;}
static bool ffx_load(void){return ffx_available;}
static bool dlss_available(void){return dlss_ready;}
// GENERATED_TEMPORAL_SELECTION
static void assets(const char *root){
 assert(!mkdir(root,0700));char path[4096];snprintf(path,sizeof path,"%s/lite",root);assert(!mkdir(path,0700));
 const char *sets[]={"t1080_m0","t1080_m1","t2160_m0","t2160_m1"},*files[]={"spd.spv","prepass.spv","pass0_post.spv","pass1.spv","pass1_post.spv","pass1_postb.spv","pass2.spv","pass2_post.spv","pass3.spv","pass3_post.spv","postpass.spv","rcas.spv","initializer.bin"};
 for(unsigned set=0;set<4;set++){snprintf(path,sizeof path,"%s/lite/%s",root,sets[set]);assert(!mkdir(path,0700));for(unsigned file=0;file<13;file++){snprintf(path,sizeof path,"%s/lite/%s/%s",root,sets[set],files[file]);FILE*f=fopen(path,"wb");assert(f);if(file==12){assert(!fseek(f,27391,SEEK_SET));assert(fputc(0,f)!=EOF);}else{uint32_t words[5]={0x07230203,0x00010600,0,1,0};assert(fwrite(words,1,sizeof words,f)==sizeof words);}assert(!fclose(f));}}
}
int main(int argc,char**argv){assert(argc==2);char root[4096],path[4096],other[4096];assert(snprintf(root,sizeof root,"%s/models",argv[1])<(int)sizeof root);
#ifndef XCDE_LINUX_FSR4
 assert(!tmp_fsr4_set_asset_dir(root) && !tmp_fsr4_supported());g_settings.upscaler=UPSCALE_FSR4_LITE;assert(tmp_enabled() && active_provider==1 && tmp_linear_output());ffx_available=false;assert(!tmp_enabled() && !tmp_linear_output());puts("PASS FSR4 compiled-out setter/availability and FSR3/spatial fallback");return 0;
#else
 assert(!tmp_fsr4_set_asset_dir("relative"));assets(root);assert(tmp_fsr4_set_asset_dir(root));
 snprintf(path,sizeof path,"%s/lite/t1080_m0/spd.spv",root);snprintf(other,sizeof other,"%s/original.spv",argv[1]);assert(!rename(path,other));assert(!tmp_fsr4_set_asset_dir(root));assert(!symlink(other,path));assert(!tmp_fsr4_set_asset_dir(root));assert(!unlink(path));assert(!rename(other,path));assert(tmp_fsr4_set_asset_dir(root));
 snprintf(path,sizeof path,"%s/model-link",argv[1]);assert(!symlink(root,path));assert(!tmp_fsr4_set_asset_dir(path));assert(tmp_fsr4_set_asset_dir(root));
 g_vk.phys=(VkPhysicalDevice)1;g_vk.dev=(VkDevice)2;g_vk.sc_extent=(VkExtent2D){320,180};g_settings.upscaler=UPSCALE_FSR4_LITE;g_settings.upscale_sharpness=.9f;
 g_vk.fsr4_lite=false;g_vk.fsr4_reason="authored missing capability";assert(tmp_enabled() && active_provider==1 && tmp_linear_output() && !opened);assert(strstr(tmp_fsr4_status(),"capability"));
 g_vk.fsr4_lite=true;module_present=false;assert(tmp_enabled() && active_provider==1);assert(strstr(tmp_fsr4_status(),"absent"));fsr4_load_state=0;module_present=true;open_ok=false;assert(!tmp_fsr4_supported());fsr4_load_state=0;open_ok=true;abi_ok=false;assert(!tmp_fsr4_supported() && closed==1);fsr4_load_state=0;abi_ok=true;
 assert(tmp_enabled() && active_provider==3 && tmp_linear_output());
 Img color={.b={.image=(VkImage)10,.width=160,.height=90,.format=F_LINEAR},.v=(VkImageView)11},motion={.b={.image=(VkImage)12,.width=160,.height=90,.format=F_MV},.v=(VkImageView)13},depth={.b={.image=(VkImage)14,.width=160,.height=90,.format=F_DEPTH},.v=(VkImageView)15},output={.b={.image=(VkImage)16,.width=320,.height=180,.format=F_LINEAR},.v=(VkImageView)17};float jitter[2]={.25f,-.25f};
 for(unsigned i=0;i<8;i++)assert(fsr4_evaluate((VkCommandBuffer)18,&color,&motion,&depth,&output,jitter,i==0));assert(created==1 && built==1 && recorded==8 && !waited && !destroyed);assert(latest.color.image==color.b.image && latest.output.view==output.v && latest.jitter[1]==-.25f && latest.sharpness==.9f && !latest.ultra);
 assert(!fsr4_evaluate((VkCommandBuffer)18,&color,&motion,&depth,&output,jitter,false) && recorded==8 && !fsr4_failed && !destroyed);pending=11;assert(fsr4_evaluate((VkCommandBuffer)18,&color,&motion,&depth,&output,jitter,false) && waited==1 && recorded==9);
 output.b.width=640;pending=12;assert(fsr4_evaluate((VkCommandBuffer)18,&color,&motion,&depth,&output,jitter,false) && built==2 && latest.reset && waited==2);
 g_settings.upscale_quality=UQ_ULTRA;pending=13;assert(fsr4_evaluate((VkCommandBuffer)18,&color,&motion,&depth,&output,jitter,false) && built==3 && latest.ultra && latest.reset);
 g_settings.upscale_sharpness=0;completed=pending;assert(fsr4_evaluate((VkCommandBuffer)18,&color,&motion,&depth,&output,jitter,false) && latest.sharpness==0);
 // Malformed inputs are rejected before any backend command or resource change.
 unsigned before=recorded;motion.b.width--;assert(!fsr4_evaluate((VkCommandBuffer)18,&color,&motion,&depth,&output,jitter,false) && recorded==before);motion.b.width++;jitter[0]=NAN;assert(!fsr4_evaluate((VkCommandBuffer)18,&color,&motion,&depth,&output,jitter,false));jitter[0]=.25f;
 output.b.width=3841;assert(!fsr4_evaluate((VkCommandBuffer)18,&color,&motion,&depth,&output,jitter,false));assert(temporal_provider(160,90,3841,180)==1 && tmp_linear_output());output.b.width=640;assert(temporal_provider(160,90,640,180)==3);
 assert(temporal_provider(127,60,320,180)==1);assert(temporal_provider(128,36,320,180)==3);
 record_ok=false;pending++;completed=pending-1;assert(!fsr4_evaluate((VkCommandBuffer)18,&color,&motion,&depth,&output,jitter,false) && fsr4_failed && !destroyed);assert(tmp_enabled() && active_provider==1);ffx_available=false;assert(!tmp_enabled() && active_provider==0 && !tmp_linear_output());
 completed=pending;fsr4_reset();assert(destroyed==1 && !fsr4_failed && !fsr4_context);ffx_available=true;record_ok=true;create_ok=false;assert(!fsr4_evaluate((VkCommandBuffer)18,&color,&motion,&depth,&output,jitter,true) && fsr4_failed);fsr4_reset();create_ok=true;build_ok=false;assert(!fsr4_evaluate((VkCommandBuffer)18,&color,&motion,&depth,&output,jitter,true) && fsr4_failed && destroyed==1);fsr4_reset();assert(destroyed==2);
 g_settings.upscaler=UPSCALE_DLSS;dlss_ready=true;assert(tmp_enabled() && active_provider==2 && !tmp_linear_output());g_settings.upscaler=UPSCALE_FSR3;assert(tmp_enabled() && active_provider==1 && tmp_linear_output());
 puts("PASS actual FSR4 glue:52-file ordinary paths, missing features/module/ABI, provider/fallback/linear identity,8-slot completion, resize/ultra/reset/sharpness, malformed inputs, failure lifetime");return 0;
#endif
}
