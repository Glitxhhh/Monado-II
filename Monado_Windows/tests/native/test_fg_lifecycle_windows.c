// Real production SDK lifecycle functions; SDK results controlled separately
// from real vendor/GPU qualification. Native descriptor ABI is verified here.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "nvn/vulkan/temporal.h"
#include "ffx_api/ffx_framegeneration.h"
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK 3u
#define FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK 0x40007u
struct ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK{ffxDispatchDescHeader header;};
struct ffxCreateBackendVKDesc{ffxCreateContextDescHeader header;VkDevice vkDevice;VkPhysicalDevice vkPhysicalDevice;PFN_vkGetDeviceProcAddr vkDeviceProcAddr;};
VkG g_vk;
static ffxContext fgsc_ctx,fg_ctx;
static uint32_t fg_w,fg_h;
static bool pending,disabled,idle;
static ffxReturnCode_t wait_result,configure_result,destroy_result,create_result;
static char order[128];static unsigned length;
static jmp_buf fatal_jump;static unsigned fatals;
#define LOG(...) ((void)0)
#define FATAL(...) do{fatals++;longjmp(fatal_jump,1);}while(0)
#define VK_CHECK(call) assert((call)==VK_SUCCESS)
static void note(char c){assert(length<sizeof order-1);order[length++]=c;order[length]=0;}
static ffxReturnCode_t ffx_dispatch(ffxContext *ctx,const ffxDispatchDescHeader *desc){
    assert(ctx==&fgsc_ctx&&*ctx==(void*)2&&desc->type==0x40007u&&!desc->pNext);note('W');
    if(wait_result==FFX_API_RETURN_OK)pending=false;return wait_result;
}
static ffxReturnCode_t ffx_configure(ffxContext *ctx,const ffxConfigureDescHeader *header){
    const struct ffxConfigureDescFrameGeneration *desc=(const void*)header;
    assert(ctx==&fg_ctx&&*ctx==(void*)1&&!pending&&desc->header.type==FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION&&
        !desc->frameGenerationEnabled&&desc->swapChain==(void*)g_vk.swapchain);note('C');
    if(configure_result==FFX_API_RETURN_OK)disabled=true;return configure_result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device){assert(device==g_vk.dev&&disabled&&!pending);note('I');idle=true;return VK_SUCCESS;}
static ffxReturnCode_t ffx_destroy(ffxContext *ctx,void *allocator){assert(ctx==&fg_ctx&&!pending&&disabled&&idle&&!allocator);note('D');return destroy_result;}
static PFN_vkVoidFunction VKAPI_PTR ffx_proc(VkDevice device,const char *name){(void)device;(void)name;return NULL;}
static ffxReturnCode_t ffx_create(ffxContext *ctx,const ffxCreateContextDescHeader *header,void *allocator){
    const struct ffxCreateContextDescFrameGeneration *desc=(const void*)header;
    const struct ffxCreateBackendVKDesc *backend=(const void*)header->pNext;
    assert(ctx==&fg_ctx&&!*ctx&&!allocator&&desc->displaySize.width==1920&&desc->displaySize.height==1080&&
        desc->maxRenderSize.width==1920&&desc->maxRenderSize.height==1080&&backend&&backend->header.type==3&&backend->vkDeviceProcAddr==ffx_proc);
    note('N');if(!create_result)*ctx=(void*)3;return create_result;
}
static void fg_reset(void){note('R');}
#include "fg_lifecycle_source.h"
static void reset(void){length=0;order[0]=0;pending=true;disabled=idle=false;wait_result=configure_result=destroy_result=create_result=0;fgsc_ctx=(void*)2;fg_ctx=(void*)1;fg_w=1280;fg_h=720;}
int main(void){
    g_vk.dev=(VkDevice)(uintptr_t)4;g_vk.swapchain=(VkSwapchainKHR)(uintptr_t)5;
    reset();fgsc_ctx=NULL;tmp_fg_drain();assert(!length);fgsc_ctx=(void*)2;tmp_fg_drain();assert(!strcmp(order,"W")&&!pending);
    reset();wait_result=FFX_API_RETURN_ERROR;if(!setjmp(fatal_jump)){tmp_fg_drain();assert(!"failed SDK drain must be fatal");}assert(!strcmp(order,"W")&&pending);
    reset();assert(fg_context(1280,720)&&!length&&fg_ctx==(void*)1);
    reset();assert(fg_context(1920,1080)&&!strcmp(order,"WCIDRN")&&fg_ctx==(void*)3&&fg_w==1920&&fg_h==1080);
    reset();configure_result=FFX_API_RETURN_ERROR;if(!setjmp(fatal_jump)){fg_context(1920,1080);assert(!"failed disable must be fatal");}assert(!strcmp(order,"WC")&&!idle&&fg_ctx==(void*)1);
    reset();destroy_result=FFX_API_RETURN_ERROR;if(!setjmp(fatal_jump)){fg_context(1920,1080);assert(!"failed destroy must be fatal");}assert(!strcmp(order,"WCID"));
    reset();create_result=FFX_API_RETURN_ERROR;assert(!fg_context(1920,1080)&&!strcmp(order,"WCIDRN")&&!fg_ctx&&disabled&&!pending);
    reset();fg_ctx=NULL;pending=false;assert(fg_context(1920,1080)&&!strcmp(order,"RN"));
    assert(fatals==3);puts("PASS native SDK presentation drain/context ownership and failure boundaries");return 0;
}
