// Actual production dispatch/ring/reset with explicit component API mocks.
// No Vulkan command or GPU operation is executed by these API boundaries.
#include <assert.h>
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include "nvn/vulkan/temporal.h"
#include "settings.h"
typedef struct {TexBackend b;VkImageView v;} Img;
enum {F_LINEAR=VK_FORMAT_R16G16B16A16_SFLOAT,F_MV=VK_FORMAT_R16G16_SFLOAT,F_DEPTH=VK_FORMAT_R32_SFLOAT};
VkG g_vk;Settings g_settings;
static uint64_t pending=10,completed=9;static unsigned waits;
uint64_t vk_pending_seq(void){return pending;}
uint64_t vk_completed_seq(void){return completed;}
void vk_wait_seq(uint64_t value){assert(value<pending);completed=value;waits++;}
#include "../../runtime/nvn/vulkan/temporal_fsr4_windows.inc"
static unsigned creates,builds,records,destroys;static bool build_ok=true,record_ok=true;static MonadoFsr4Frame captured;
static void *create(VkPhysicalDevice p,VkDevice d,const char *path){assert(p==(void*)1&&d==(void*)2&&!strcmp(path,"authored"));creates++;return (void*)3;}
static void destroy(void *p){assert(p==(void*)3);destroys++;}
static int build(void *p,uint32_t w,uint32_t h,uint32_t ultra){assert(p==(void*)3&&w==320&&h==180&&ultra<=1);builds++;return build_ok;}
static int record(void *p,const MonadoFsr4Frame *frame){assert(p==(void*)3&&frame&&frame->cmd==(void*)4);records++;captured=*frame;return record_ok;}
static const char *error(void *p){assert(p==(void*)3);return "authored API boundary failure";}
int main(void){
 const MonadoFsr4Api api={2,create,destroy,build,record,error};fsr4_api=&api;fsr4_load_state=1;strcpy(fsr4_asset_dir,"authored");
 g_vk.fsr4_lite=true;g_vk.phys=(void*)1;g_vk.dev=(void*)2;g_settings.upscaler=UPSCALE_FSR4_LITE;g_settings.upscale_sharpness=0;
 Img color={.b={.image=(void*)11,.format=F_LINEAR,.width=160,.height=90},.v=(void*)21};
 Img motion={.b={.image=(void*)12,.format=F_MV,.width=160,.height=90},.v=(void*)22};
 Img depth={.b={.image=(void*)13,.format=F_DEPTH,.width=160,.height=90},.v=(void*)23};
 Img output={.b={.image=(void*)14,.format=F_LINEAR,.width=320,.height=180},.v=(void*)24};float jitter[2]={.25f,-.25f};
 assert(fsr4_evaluate((void*)4,&color,&motion,&depth,&output,jitter,false));
 assert(creates==1&&builds==1&&records==1&&captured.reset&&captured.sharpness==0&&captured.color.image==color.b.image&&captured.motion.view==motion.v);
 assert(fsr4_evaluate((void*)4,&color,&motion,&depth,&output,jitter,false)&&!captured.reset&&builds==1);
 jitter[0]=NAN;assert(!fsr4_evaluate((void*)4,&color,&motion,&depth,&output,jitter,false));jitter[0]=0;
 motion.b.width=159;assert(!fsr4_evaluate((void*)4,&color,&motion,&depth,&output,jitter,false));motion.b.width=160;
 for(unsigned i=2;i<8;i++)assert(fsr4_evaluate((void*)4,&color,&motion,&depth,&output,jitter,false));
 assert(!fsr4_evaluate((void*)4,&color,&motion,&depth,&output,jitter,false)&&!destroys); // Same unsubmitted ring slot.
 pending=11;completed=9;assert(fsr4_evaluate((void*)4,&color,&motion,&depth,&output,jitter,false)&&waits==1);
 record_ok=false;assert(!fsr4_evaluate((void*)4,&color,&motion,&depth,&output,jitter,false));
 assert(fsr4_failed&&fsr4_context==(void*)3&&!destroys&&fsr4_ring[1]==11);
 fsr4_reset();assert(destroys==1&&!fsr4_context&&!fsr4_failed);
 build_ok=false;assert(!fsr4_evaluate((void*)4,&color,&motion,&depth,&output,jitter,false));assert(fsr4_context==(void*)3&&destroys==1);
 fsr4_reset();assert(destroys==2);puts("PASS actual native FSR4 dispatch API, finite/size inputs, zero sharpening, ring retirement, failure retention and GPU-idle reset");return 0;
}
