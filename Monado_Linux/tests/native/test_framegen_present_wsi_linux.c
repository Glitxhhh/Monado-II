// [XCDE] LINUX-099: authored hidden Vulkan presentation-worker lifetime fixture.
// MIT; no game/assets/guest execution. CPU tests establish deterministic overlap;
// this fixture exercises actual driver semaphore/timeline/swapchain ownership.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../../runtime/platform/linux_state_guard.h"
#include "../../runtime/nvn/vulkan/framegen_present_linux.h"
#include "../../runtime/nvn/framegen_policy.h"
#define OK(x) do { VkResult result=(x); if(result!=VK_SUCCESS){fprintf(stderr,"FAIL line%d result%d %s\n",__LINE__,result,#x);abort();} } while(0)
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned present_calls;
static uint64_t clock_now(void){return fg_pacing_now();}
static VkResult gpu_ready(const FgPresentJob *job){VkSemaphoreWaitInfo wi={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,.semaphoreCount=1,.pSemaphores=&job->timeline,.pValues=&job->sequence};return vkWaitSemaphores(job->device,&wi,UINT64_C(10000000000));}
static VkResult present_one(const FgPresentJob *job,unsigned which){
 assert(which<2);VkPresentInfoKHR pi={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&job->done[which],.swapchainCount=1,.pSwapchains=&job->swapchain,.pImageIndices=&job->image[which]};
 if(job->shared_queue)pthread_mutex_lock(&queue_lock);VkResult result=vkQueuePresentKHR(job->queue,&pi);if(job->shared_queue)pthread_mutex_unlock(&queue_lock);
 atomic_fetch_add(&present_calls,1);return result;
}
static unsigned accepted_pairs, pixels_verified;
static uint64_t previous_call;
static void accept(const FgPresentResult *r){
 if(!r->valid)return;
 for(unsigned i=0;i<2;i++){
  assert(r->result[i]==VK_SUCCESS||r->result[i]==VK_SUBOPTIMAL_KHR);assert(r->call_ns[i]>=previous_call);assert(r->meter_ns[i]&&r->meter_ns[i]<r->call_ns[i]);previous_call=r->call_ns[i];
 }
 assert(r->job.image[0]!=r->job.image[1]);accepted_pairs++;
}
static void drain(FgPresenter *p){FgPresentResult r={0};if(fg_presenter_collect(p,true,&r))accept(&r);}
typedef struct Slot{VkCommandPool pool;VkCommandBuffer cb;VkSemaphore acquire[2];uint64_t sequence;VkBuffer readback;VkDeviceMemory memory;unsigned char *mapped;VkDeviceSize bytes;bool pending,bgra;unsigned pair,epoch;uint32_t width,height;}Slot;
static void verify_pixels(Slot *slot){
 if(!slot->pending)return;
 for(unsigned k=0;k<2;k++)for(unsigned sample=0;sample<3;sample++){
  size_t pixel=sample==0?0:sample==1?(size_t)(slot->height/2)*slot->width+slot->width/2:(size_t)slot->width*slot->height-1;
  unsigned char *v=slot->mapped+k*slot->bytes+pixel*4;unsigned red=(slot->pair&1)*255,blue=(slot->epoch&1)*255;
  assert(v[0]==(slot->bgra?blue:red)&&v[1]==k*255&&v[2]==(slot->bgra?red:blue)&&v[3]==255);pixels_verified++;
 }
 slot->pending=false;
}
static void make_readback(VkDevice dev,VkPhysicalDevice phys,Slot *slot,VkExtent2D extent,VkFormat format){
 slot->width=extent.width;slot->height=extent.height;slot->bytes=(VkDeviceSize)extent.width*extent.height*4;slot->bgra=format==VK_FORMAT_B8G8R8A8_UNORM||format==VK_FORMAT_B8G8R8A8_SRGB;
 assert(slot->bgra||format==VK_FORMAT_R8G8B8A8_UNORM||format==VK_FORMAT_R8G8B8A8_SRGB);
 VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=slot->bytes*2,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};OK(vkCreateBuffer(dev,&bi,0,&slot->readback));VkMemoryRequirements req;vkGetBufferMemoryRequirements(dev,slot->readback,&req);VkPhysicalDeviceMemoryProperties props;vkGetPhysicalDeviceMemoryProperties(phys,&props);uint32_t type=~0u;
 for(unsigned i=0;i<props.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=i;break;}assert(type!=~0u);
 VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};OK(vkAllocateMemory(dev,&allocation,0,&slot->memory));OK(vkBindBufferMemory(dev,slot->readback,slot->memory,0));OK(vkMapMemory(dev,slot->memory,0,VK_WHOLE_SIZE,0,(void**)&slot->mapped));
}
int main(int argc,char **argv){
 assert(argc==3);bool shared=!strncmp(argv[2],"shared",6), low=strstr(argv[2],"-low")!=NULL, middle=strstr(argv[2],"-mid")!=NULL;assert(shared||!strncmp(argv[2],"dedicated",9));
 char state[4096],path[4096],error[512];assert(realpath(argv[1],state)&&!strcmp(state,argv[1])&&!chdir(state));
 const char *vars[]={"HOME","XDG_CACHE_HOME","XDG_CONFIG_HOME","MESA_SHADER_CACHE_DIR","__GL_SHADER_DISK_CACHE_PATH","TMPDIR"};const char *dirs[]={"home","cache","config","mesa","nvidia","tmp"};
 for(unsigned i=0;i<6;i++){assert(snprintf(path,sizeof path,"%s/%s",state,dirs[i])<(int)sizeof path);assert(!mkdir(path,0700)||errno==EEXIST);assert(!setenv(vars[i],path,1));}
 assert(rt_linux_state_guard(state,error,sizeof error));assert(!getenv("NVPRESENT_ENABLE_SMOOTH_MOTION"));
 if(!SDL_Init(SDL_INIT_VIDEO)){fprintf(stderr,"SDL init %s\n",SDL_GetError());return 2;}
 SDL_Window *window=SDL_CreateWindow("Monado authored hidden worker fixture",640,360,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);assert(window);
 Uint32 ec=0;const char *const *extensions=SDL_Vulkan_GetInstanceExtensions(&ec);assert(extensions&&ec);
 VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Monado worker ownership fixture",.apiVersion=VK_API_VERSION_1_3};VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,.enabledExtensionCount=ec,.ppEnabledExtensionNames=extensions};VkInstance inst;OK(vkCreateInstance(&ii,0,&inst));VkSurfaceKHR surface;assert(SDL_Vulkan_CreateSurface(window,inst,0,&surface));
 uint32_t dc=0;OK(vkEnumeratePhysicalDevices(inst,&dc,0));assert(dc&&dc<16);VkPhysicalDevice devices[16],phys=0;OK(vkEnumeratePhysicalDevices(inst,&dc,devices));
 for(unsigned i=0;i<dc;i++){VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(devices[i],&properties);if(properties.vendorID==0x10de){phys=devices[i];fprintf(stderr,"GPU %s API%u\n",properties.deviceName,properties.apiVersion);}}assert(phys);
 uint32_t qc=0;vkGetPhysicalDeviceQueueFamilyProperties(phys,&qc,0);assert(qc&&qc<64);VkQueueFamilyProperties props[64];vkGetPhysicalDeviceQueueFamilyProperties(phys,&qc,props);uint32_t family=~0u;
 for(unsigned i=0;i<qc;i++){VkBool32 yes=0;OK(vkGetPhysicalDeviceSurfaceSupportKHR(phys,i,surface,&yes));if(yes&&(props[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){family=i;break;}}assert(family!=~0u);if(!shared&&props[family].queueCount<2){fprintf(stderr,"dedicated queue unavailable\n");return 77;}
 float priorities[2]={1,1};VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=shared?1:2,.pQueuePriorities=priorities};
 VkPhysicalDeviceVulkan12Features f12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.timelineSemaphore=VK_TRUE};const char *swapchain_extension=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
 VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f12,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.enabledExtensionCount=1,.ppEnabledExtensionNames=&swapchain_extension};VkDevice dev;OK(vkCreateDevice(phys,&di,0,&dev));VkQueue render,present;vkGetDeviceQueue(dev,family,0,&render);vkGetDeviceQueue(dev,family,shared?0:1,&present);
 VkSemaphoreTypeCreateInfo type={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE};VkSemaphoreCreateInfo ti={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,.pNext=&type};VkSemaphore timeline;OK(vkCreateSemaphore(dev,&ti,0,&timeline));
 Slot slots[2]={0};VkSemaphoreCreateInfo binary={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
 for(unsigned i=0;i<2;i++){VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};OK(vkCreateCommandPool(dev,&pool,0,&slots[i].pool));VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=slots[i].pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};OK(vkAllocateCommandBuffers(dev,&ai,&slots[i].cb));for(unsigned j=0;j<2;j++)OK(vkCreateSemaphore(dev,&binary,0,&slots[i].acquire[j]));}
 FgPresenter worker=FG_PRESENTER_INITIALIZER;FgPresentOps ops={clock_now,SDL_GetTicksNS,fg_pacing_wait,gpu_ready,present_one};uint64_t seq=0;unsigned produced_before_previous_real=0;
 for(unsigned epoch=0;epoch<4;epoch++){
  if(epoch){SDL_SetWindowSize(window,640+epoch*32,360+epoch*18);SDL_PumpEvents();}
  VkSurfaceCapabilitiesKHR caps;OK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys,surface,&caps));uint32_t fc=0;OK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys,surface,&fc,0));assert(fc&&fc<128);VkSurfaceFormatKHR formats[128];OK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys,surface,&fc,formats));VkSurfaceFormatKHR format=formats[0];for(unsigned i=0;i<fc;i++)if(formats[i].format==VK_FORMAT_B8G8R8A8_UNORM)format=formats[i];
  fprintf(stderr,"CAPS min%u max%u\n",caps.minImageCount,caps.maxImageCount);uint32_t count=low?caps.minImageCount+1:middle?caps.minImageCount+2:fg_policy_image_count(caps.minImageCount,caps.maxImageCount,true);if(caps.maxImageCount&&count>caps.maxImageCount)count=caps.maxImageCount;assert(count>=3&&count<=16);
  VkExtent2D extent=caps.currentExtent.width==~0u?(VkExtent2D){640+epoch*32,360+epoch*18}:caps.currentExtent;
  VkPresentModeKHR mode=epoch&1?VK_PRESENT_MODE_FIFO_KHR:VK_PRESENT_MODE_MAILBOX_KHR;uint32_t mc=0;OK(vkGetPhysicalDeviceSurfacePresentModesKHR(phys,surface,&mc,0));assert(mc<32);VkPresentModeKHR modes[32];OK(vkGetPhysicalDeviceSurfacePresentModesKHR(phys,surface,&mc,modes));bool supported=false;for(unsigned i=0;i<mc;i++)supported|=modes[i]==mode;assert(supported);
  VkSwapchainCreateInfoKHR sci={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=surface,.minImageCount=count,.imageFormat=format.format,.imageColorSpace=format.colorSpace,.imageExtent=extent,.imageArrayLayers=1,.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,.preTransform=caps.currentTransform,.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,.presentMode=mode,.clipped=VK_TRUE};VkSwapchainKHR sc;OK(vkCreateSwapchainKHR(dev,&sci,0,&sc));OK(vkGetSwapchainImagesKHR(dev,sc,&count,0));assert(count<=16);VkImage images[16];OK(vkGetSwapchainImagesKHR(dev,sc,&count,images));VkSemaphore done[16];for(unsigned i=0;i<count;i++)OK(vkCreateSemaphore(dev,&binary,0,&done[i]));
  for(unsigned i=0;i<2;i++)make_readback(dev,phys,&slots[i],extent,format.format);
  assert(fg_presenter_start(&worker,&ops));fprintf(stderr,"EPOCH%u mode%d images%u extent%ux%u shared%d\n",epoch,mode,count,extent.width,extent.height,shared);
  uint32_t budget=fg_policy_acquire_budget(count,caps.minImageCount);
  for(unsigned pair=0;pair<24;pair++){
   if(budget<3)drain(&worker);
   Slot *slot=&slots[pair%2];if(slot->sequence){FgPresentJob old={.device=dev,.timeline=timeline,.sequence=slot->sequence};OK(gpu_ready(&old));verify_pixels(slot);}OK(vkResetCommandPool(dev,slot->pool,0));
   uint32_t image[2];for(unsigned k=0;k<2;k++){
    VkResult acquired=vkAcquireNextImageKHR(dev,sc,k?0:UINT64_C(10000000000),slot->acquire[k],0,&image[k]);
    if(k&&(acquired==VK_NOT_READY||acquired==VK_TIMEOUT)){drain(&worker);acquired=vkAcquireNextImageKHR(dev,sc,UINT64_C(10000000000),slot->acquire[k],0,&image[k]);}
    OK(acquired);
   }assert(image[0]!=image[1]);if(pair<2)fprintf(stderr,"ACQUIRED epoch%u pair%u images%u,%u\n",epoch,pair,image[0],image[1]);
   VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};OK(vkBeginCommandBuffer(slot->cb,&begin));
   for(unsigned k=0;k<2;k++){VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=images[image[k]],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};vkCmdPipelineBarrier(slot->cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,0,0,0,1,&barrier);VkClearColorValue color={.float32={(float)(pair&1),(float)k,(float)(epoch&1),1.f}};vkCmdClearColorImage(slot->cb,images[image[k]],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;vkCmdPipelineBarrier(slot->cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,0,0,0,1,&barrier);VkBufferImageCopy copy={.bufferOffset=k*slot->bytes,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={extent.width,extent.height,1}};vkCmdCopyImageToBuffer(slot->cb,images[image[k]],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,slot->readback,1,&copy);barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.dstAccessMask=0;vkCmdPipelineBarrier(slot->cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,0,0,0,1,&barrier);}
   VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=slot->readback,.size=VK_WHOLE_SIZE};vkCmdPipelineBarrier(slot->cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,0,1,&host,0,0);
   OK(vkEndCommandBuffer(slot->cb));VkSemaphore signals[3]={done[image[0]],done[image[1]],timeline};uint64_t values[3]={0,0,++seq},waits[2]={0,0};VkTimelineSemaphoreSubmitInfo tsi={.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,.waitSemaphoreValueCount=2,.pWaitSemaphoreValues=waits,.signalSemaphoreValueCount=3,.pSignalSemaphoreValues=values};VkPipelineStageFlags stages[2]={VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT};VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.pNext=&tsi,.waitSemaphoreCount=2,.pWaitSemaphores=slot->acquire,.pWaitDstStageMask=stages,.commandBufferCount=1,.pCommandBuffers=&slot->cb,.signalSemaphoreCount=3,.pSignalSemaphores=signals};pthread_mutex_lock(&queue_lock);OK(vkQueueSubmit(render,1,&submit,0));pthread_mutex_unlock(&queue_lock);slot->sequence=seq;slot->pending=true;slot->pair=pair;slot->epoch=epoch;uint64_t produced=clock_now();
   FgPresentJob job={.device=dev,.queue=present,.swapchain=sc,.timeline=timeline,.done={done[image[0]],done[image[1]]},.image={image[0],image[1]},.sequence=seq,.interval_ms=16.666667f,.present=vkQueuePresentKHR,.shared_queue=shared};FgPresentResult previous={0};assert(fg_presenter_enqueue(&worker,&job,&previous));if(previous.valid&&produced<previous.call_ns[1])produced_before_previous_real++;accept(&previous);
  }
  drain(&worker);fg_presenter_reset(&worker);fg_presenter_stop(&worker);assert(!worker.started&&!worker.busy);OK(vkDeviceWaitIdle(dev));for(unsigned i=0;i<2;i++){verify_pixels(&slots[i]);vkUnmapMemory(dev,slots[i].memory);vkDestroyBuffer(dev,slots[i].readback,0);vkFreeMemory(dev,slots[i].memory,0);}
  for(unsigned i=0;i<count;i++)vkDestroySemaphore(dev,done[i],0);vkDestroySwapchainKHR(dev,sc,0);
 }
 assert(accepted_pairs==96&&atomic_load(&present_calls)==192&&pixels_verified==576);fprintf(stderr,"COMPLETE pairs%u presents%u producer_submit_before_prior_real%u pixels576 shared%d no_game1 visible0\n",accepted_pairs,atomic_load(&present_calls),produced_before_previous_real,shared);
 OK(vkDeviceWaitIdle(dev));for(unsigned i=0;i<2;i++){for(unsigned j=0;j<2;j++)vkDestroySemaphore(dev,slots[i].acquire[j],0);vkDestroyCommandPool(dev,slots[i].pool,0);}vkDestroySemaphore(dev,timeline,0);vkDestroyDevice(dev,0);vkDestroySurfaceKHR(inst,surface,0);vkDestroyInstance(inst,0);SDL_DestroyWindow(window);SDL_Quit();return 0;
}
