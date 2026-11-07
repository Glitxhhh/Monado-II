// [XCDE] LINUX-020: actual provider wrapper with authored C-API boundary mock; no Vulkan device/SDK/game/UI.
#include <assert.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/nvn/vulkan/framegen_linux.h"
#define __declspec(x)
#include "../../third_party/ffx_api/ffx_framegeneration.h"
#undef __declspec

typedef struct { unsigned creates, destroys, configs, prepares, generates, reset; int stage; } MockInfo;
static const char *commit = "c6efa6bf7f2027b3ec94f28578bb5965eabb9e55";
#ifdef FRAMEGEN_MOCK_MODULE
static MockInfo info;
typedef struct { const struct ffxCreateContextDescFrameGeneration *description; uint64_t prepared; } MockContext;
uint32_t monado_ffx_abi_version(void) {
#ifdef FRAMEGEN_BAD_ABI
    return 2;
#else
    return 1;
#endif
}
const char *monado_ffx_sdk_commit(void) {
#ifdef FRAMEGEN_BAD_COMMIT
    return "0000000000000000000000000000000000000000";
#else
    return commit;
#endif
}
void monado_test_stage(int stage) { info.stage = stage; }
void monado_test_info(MockInfo *out) { *out = info; }
ffxReturnCode_t ffxCreateContext(ffxContext *context, ffxCreateContextDescHeader *header, const ffxAllocationCallbacks *callbacks) {
    assert(!callbacks && header->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
    if(info.stage==10) {
        struct Backend { ffxCreateContextDescHeader header;VkDevice device;VkPhysicalDevice physical;PFN_vkGetDeviceProcAddr proc; };
        const struct Backend *backend=(const void *)header->pNext->pNext;
        assert(backend->header.type==3 && backend->device && backend->physical && backend->proc);
        assert(backend->proc(backend->device,"vkGetBufferMemoryRequirements2KHR"));
        assert(!backend->proc(backend->device,"vkNoSuchFunctionKHR"));
    }
    MockContext *c = calloc(1, sizeof *c); assert(c); c->description = (void *)header; *context = c; info.creates++;
    return info.stage == 1 ? FFX_API_RETURN_ERROR : FFX_API_RETURN_OK;
}
ffxReturnCode_t ffxDestroyContext(ffxContext *context, const ffxAllocationCallbacks *callbacks) {
    assert(!callbacks && context && *context);
    if (info.stage == 5) return FFX_API_RETURN_ERROR;
    free(*context); *context = NULL; info.destroys++; return FFX_API_RETURN_OK;
}
ffxReturnCode_t ffxQuery(ffxContext *context, ffxQueryDescHeader *header) {
    assert(!context && header->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS);
    struct ffxQueryDescGetVersions *v = (void *)header;
    if (info.stage == 4) return FFX_API_RETURN_NO_PROVIDER;
    if (v->versionIds) v->versionIds[0] = info.stage == 8 ? 0x010101u : UINT64_C(0xf600000000401003);
    if (v->versionNames) v->versionNames[0] = info.stage == 9 ? "wrong-version" : "1.1.3";
    *v->outputCount = 1; return FFX_API_RETURN_OK;
}
ffxReturnCode_t ffxConfigure(ffxContext *context, const ffxConfigureDescHeader *header) {
    assert(context && *context && header->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION);
    MockContext *c = *context;
    // The create descriptors must still be alive, not a pointer to a prior stack frame.
    assert(c->description->header.type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
    assert(c->description->header.pNext->type == FFX_API_DESC_TYPE_OVERRIDE_VERSION);
    const struct ffxConfigureDescFrameGeneration *config = (const void *)header;
    assert(config->frameGenerationEnabled && !config->allowAsyncWorkloads && !config->swapChain);
    assert(config->flags == FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY);
    info.configs++; return info.stage == 6 ? FFX_API_RETURN_ERROR : FFX_API_RETURN_OK;
}
#ifndef FRAMEGEN_MISSING_DISPATCH
ffxReturnCode_t ffxDispatch(ffxContext *context, const ffxDispatchDescHeader *header) {
    assert(context && *context); MockContext *c = *context;
    if (header->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE) {
        const struct ffxDispatchDescFrameGenerationPrepare *p = (const void *)header;
        assert(p->commandList && p->depth.resource && p->motionVectors.resource);
        c->prepared = p->frameID; info.prepares++;
        return info.stage == 2 ? FFX_API_RETURN_ERROR : FFX_API_RETURN_OK;
    }
    assert(header->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION);
    struct ffxDispatchDescFrameGeneration *d = (void *)header;
    assert(d->frameID == c->prepared && d->commandList && d->presentColor.resource && d->outputs[0].resource && d->numGeneratedFrames == 1);
    info.generates++; info.reset = d->reset;
    if (info.stage == 7) d->numGeneratedFrames = 0;
    return info.stage == 3 ? FFX_API_RETURN_ERROR : FFX_API_RETURN_OK;
}
#endif
#else
static VkDevice device = (VkDevice)(uintptr_t)0x11;
static VkPhysicalDevice physical = (VkPhysicalDevice)(uintptr_t)0x22;
static unsigned core_lookups,extension_lookups;
static void VKAPI_CALL fake_core(void) {}
static void VKAPI_CALL fake_extension(void) {}
static PFN_vkVoidFunction VKAPI_PTR fake_proc(VkDevice d, const char *name) {
    assert(d==device);
    if(!strcmp(name,"vkGetBufferMemoryRequirements2")){core_lookups++;return fake_core;}
    return NULL;
}
static PFN_vkVoidFunction VKAPI_PTR extension_proc(VkDevice d, const char *name) {
    assert(d==(VkDevice)(uintptr_t)0x44);
    if(!strcmp(name,"vkGetBufferMemoryRequirements2KHR")){extension_lookups++;return fake_extension;}
    if(!strcmp(name,"vkGetBufferMemoryRequirements2")){assert(!"extension function must be preferred to core alias");}
    return NULL;
}
static FgLinuxFrame frame(void) {
    FgLinuxFrame f = {0};
    f.commands = (VkCommandBuffer)(uintptr_t)0x33;
    f.scene = (FgLinuxImage){(VkImage)(uintptr_t)0x101,VK_FORMAT_R8G8B8A8_UNORM,64,32};
    f.output = (FgLinuxImage){(VkImage)(uintptr_t)0x102,VK_FORMAT_R8G8B8A8_UNORM,64,32};
    f.depth = (FgLinuxImage){(VkImage)(uintptr_t)0x103,VK_FORMAT_R32_SFLOAT,32,16};
    f.motion = (FgLinuxImage){(VkImage)(uintptr_t)0x104,VK_FORMAT_R16G16_SFLOAT,32,16};
    f.motion_scale[0] = 32; f.motion_scale[1] = 16;
    f.dt_ms = 16.667f; f.camera_near = .1f; f.camera_far = 10000; f.camera_fov = .8f; f.meters = 1;
    f.real_frame_id = f.recording_seq = 1; f.depth_infinite = true;
    return f;
}
static unsigned cases;
static FgLinux *provider(const char *path) {
    char error[256]; FgLinux *fg = fg_linux_open(path, physical, device, fake_proc, error, sizeof error);
    if (!fg) fprintf(stderr,"provider error: %s\n",error);
    assert(fg && fg_linux_available(fg) && fg_linux_provider_version(fg) == UINT64_C(0xf600000000401003)); return fg;
}
int main(int argc, char **argv) {
    assert(argc == 5);
    char error[256];
    assert(!fg_linux_open("relative.so", physical, device, fake_proc,error,sizeof error)); cases++;
    assert(!fg_linux_open(argv[1],physical,VK_NULL_HANDLE,fake_proc,error,sizeof error)); cases++;
    for (int i = 2; i < 5; i++) { assert(!fg_linux_open(argv[i],physical,device,fake_proc,error,sizeof error)); cases++; }
    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); assert(module);
    void (*stage)(int) = (void (*)(int))dlsym(module,"monado_test_stage");
    void (*get_info)(MockInfo*) = (void (*)(MockInfo*))dlsym(module,"monado_test_info"); assert(stage && get_info);
    stage(4); assert(!fg_linux_open(argv[1],physical,device,fake_proc,error,sizeof error)); stage(0); cases++;
    stage(8); assert(!fg_linux_open(argv[1],physical,device,fake_proc,error,sizeof error)); stage(0); cases++;
    stage(9); assert(!fg_linux_open(argv[1],physical,device,fake_proc,error,sizeof error)); stage(0); cases++;
    FgLinux *first=provider(argv[1]),*second=provider(argv[1]);
    assert(!fg_linux_open(argv[1],physical,device,extension_proc,error,sizeof error));cases++;
    stage(10);FgLinuxFrame alias_frame=frame();fg_linux_set_enabled(first,true);assert(fg_linux_dispatch(first,&alias_frame)==FG_LINUX_WARMUP);
    assert(core_lookups==1);assert(fg_linux_close(first,1));cases++;
    fg_linux_set_enabled(second,true);assert(fg_linux_dispatch(second,&alias_frame)==FG_LINUX_WARMUP);assert(core_lookups==2);assert(fg_linux_close(second,1));cases++;
    first=fg_linux_open(argv[1],physical,(VkDevice)(uintptr_t)0x44,extension_proc,error,sizeof error);assert(first);
    fg_linux_set_enabled(first,true);assert(fg_linux_dispatch(first,&alias_frame)==FG_LINUX_WARMUP);
    assert(extension_lookups==1 && core_lookups==2);assert(fg_linux_close(first,1));stage(0);cases++;
    FgLinux *fg = provider(argv[1]); FgLinuxFrame f = frame(); FgLinuxStats stats;
    assert(fg_linux_dispatch(fg,&f) == FG_LINUX_DISABLED); fg_linux_stats(fg,&stats); assert(!stats.real_frames); cases++;
    fg_linux_set_enabled(fg,true); assert(fg_linux_dispatch(fg,&f)==FG_LINUX_WARMUP); cases++;
    MockInfo info; get_info(&info); assert(info.reset);
    f.real_frame_id = 2; f.recording_seq = 2; f.completed_seq = 1;
    assert(fg_linux_dispatch(fg,&f)==FG_LINUX_GENERATED); get_info(&info); assert(!info.reset); cases++;
    assert(fg_linux_dispatch(fg,&f)==FG_LINUX_ERROR); fg_linux_stats(fg,&stats); assert(!stats.enabled && stats.failures==1); cases++;
    fg_linux_set_enabled(fg,true); assert(fg_linux_dispatch(fg,&f)==FG_LINUX_ERROR); cases++;
    fg_linux_set_enabled(fg,false); fg_linux_set_enabled(fg,true); f.real_frame_id=8; f.recording_seq=3; f.completed_seq=2;
    assert(fg_linux_dispatch(fg,&f)==FG_LINUX_WARMUP); cases++;
    fg_linux_reset(fg); f.real_frame_id=9;f.recording_seq=4; f.completed_seq=3;
    assert(fg_linux_dispatch(fg,&f)==FG_LINUX_WARMUP); cases++;
    assert(!fg_linux_close(fg,3)); assert(fg_linux_close(fg,4)); cases++;
    fg=provider(argv[1]); fg_linux_set_enabled(fg,true); f=frame(); assert(fg_linux_dispatch(fg,&f)==FG_LINUX_WARMUP);
    f.scene.width=f.output.width=80; f.real_frame_id=2; f.recording_seq=2;
    assert(fg_linux_dispatch(fg,&f)==FG_LINUX_WARMUP); fg_linux_stats(fg,&stats); assert(stats.live_contexts==2); cases++;
    fg_linux_collect(fg,0); fg_linux_stats(fg,&stats);assert(stats.live_contexts==2);
    fg_linux_collect(fg,1);fg_linux_stats(fg,&stats);assert(stats.live_contexts==1);assert(fg_linux_close(fg,2));cases++;
    fg=provider(argv[1]);fg_linux_set_enabled(fg,true);f=frame();assert(fg_linux_dispatch(fg,&f)==FG_LINUX_WARMUP);
    f.scene.width=f.output.width=80;f.real_frame_id=2;f.recording_seq=2;
    assert(fg_linux_dispatch(fg,&f)==FG_LINUX_WARMUP);
    MockInfo before;get_info(&before);stage(5);f.real_frame_id=3;f.recording_seq=3;f.completed_seq=1;
    assert(fg_linux_dispatch(fg,&f)==FG_LINUX_ERROR);get_info(&info);assert(info.generates==before.generates);
    stage(0);assert(fg_linux_close(fg,2));cases++;
    for(int failure=1;failure<=7;failure++) {
        if(failure==4 || failure==5)continue;
        fg=provider(argv[1]);fg_linux_set_enabled(fg,true);f=frame();stage(failure);
        assert(fg_linux_dispatch(fg,&f)==FG_LINUX_ERROR);stage(0);assert(fg_linux_close(fg,1));cases++;
    }
    fg=provider(argv[1]);fg_linux_set_enabled(fg,true);f=frame();assert(fg_linux_dispatch(fg,&f)==FG_LINUX_WARMUP);
    stage(5);assert(!fg_linux_close(fg,1));stage(0);assert(fg_linux_close(fg,1));cases++;
    for(unsigned invalid=0;invalid<12;invalid++) {
        fg=provider(argv[1]);fg_linux_set_enabled(fg,true);f=frame();
        switch(invalid){case 0:f.commands=VK_NULL_HANDLE;break;case 1:f.dt_ms=NAN;break;case 2:f.dt_ms=1001;break;
        case 3:f.camera_near=0;break;case 4:f.camera_fov=4;break;case 5:f.jitter[1]=INFINITY;break;
        case 6:f.output.image=f.scene.image;break;case 7:f.motion.width++;break;case 8:f.output.format=VK_FORMAT_R32_SFLOAT;break;
        case 9:f.rect_x=-1;break;case 10:f.completed_seq=f.recording_seq;break;case 11:f.depth.width=200;break;}
        assert(fg_linux_dispatch(fg,&f)==FG_LINUX_ERROR);assert(fg_linux_close(fg,0));cases++;
    }
    get_info(&info);assert(info.creates==info.destroys);dlclose(module);
    printf("frame-generation actual-wrapper policy: %u cases passed; authored API mock, no GPU/SDK\n",cases);
    return 0;
}
#endif
