// Execute actual production cache with authored Vulkan allocation boundaries.
#include "nvn/nvn.h"
#include "nvn/vulkan/sampler_filter_policy.h"
#include "graphics_options.h"
#include <assert.h>
#include <stdio.h>
#undef LOG
#define LOG(...) ((void)0)
typedef uintptr_t VkSampler;
typedef uintptr_t VkDevice;
static struct {
    VkDevice dev;bool sampler_anisotropy;
    struct {struct {float maxSamplerAnisotropy;unsigned maxSamplerAllocationCount;}limits;}props;
} g_vk;
static unsigned choice,created,destroyed;
static bool alive[2048];
static float latest_anisotropy;
int xcde_graphics_option(int index){assert(index==GFX_FILTERING);return choice;}
static VkSampler vk_sampler(NvnSamplerDesc *d){
    if(d->backend)return(VkSampler)d->backend;
    assert(created+1<2048);alive[++created]=true;latest_anisotropy=d->max_aniso;
    d->backend=(void*)(uintptr_t)created;return created;
}
static void vkDestroySampler(VkDevice device,VkSampler sampler,const void *allocator){
    (void)allocator;assert(device==g_vk.dev && sampler && sampler<=created && alive[sampler]);
    alive[sampler]=false;destroyed++;
}
#include "sampler_cache_source.h"
static NvnSamplerDesc descriptor(void){
    NvnSamplerDesc d={0};d.min_filter=5;d.mag_filter=1;d.lod_max=1000;d.max_aniso=1;
    d.wrap[0]=d.wrap[1]=d.wrap[2]=1;return d;
}
int main(void){
    g_vk.dev=1;g_vk.sampler_anisotropy=true;
    g_vk.props.limits.maxSamplerAnisotropy=8;g_vk.props.limits.maxSamplerAllocationCount=2048;
    NvnSamplerDesc d=descriptor();choice=0;
    VkSampler native=vk_sampler_filtered(&d,true,false,false,4);
    assert(native && d.backend==(void*)native && created==1 && latest_anisotropy==1);
    choice=1;VkSampler two=vk_sampler_filtered(&d,true,false,false,4);
    assert(two!=native && created==2 && latest_anisotropy==2 && d.backend==(void*)native);
    NvnSamplerDesc same=descriptor();assert(vk_sampler_filtered(&same,true,false,false,4)==two && !same.backend && created==2);
    choice=4;VkSampler eight=vk_sampler_filtered(&d,true,false,false,4);
    assert(eight!=two && latest_anisotropy==8);choice=3;
    assert(vk_sampler_filtered(&d,true,false,false,4)==eight && created==3);
    choice=2;
    assert(vk_sampler_filtered(&d,false,false,false,4)==native);
    assert(vk_sampler_filtered(&d,true,true,false,4)==native);
    assert(vk_sampler_filtered(&d,true,false,true,4)==native);
    assert(vk_sampler_filtered(&d,true,false,false,1)==native);
    g_vk.sampler_anisotropy=false;assert(vk_sampler_filtered(&d,true,false,false,4)==native);
    g_vk.sampler_anisotropy=true;assert(created==3 && !destroyed && alive[two]);
    vk_sampler_variants_reset();assert(destroyed==2 && !alive[two] && !alive[eight] && alive[native]);
    vk_sampler_variants_reset();assert(destroyed==2);
    choice=1;
    for(unsigned i=0;i<256;i++){
        NvnSamplerDesc v=descriptor();v.lod_bias=(float)i/16;
        assert(vk_sampler_filtered(&v,true,false,false,4) && !v.backend);
    }
    unsigned old=created;NvnSamplerDesc overflow=descriptor();overflow.lod_bias=100;
    VkSampler fallback=vk_sampler_filtered(&overflow,true,false,false,4);
    assert(fallback && overflow.backend==(void*)fallback && created==old+1);
    assert(vk_sampler_filtered(&overflow,true,false,false,4)==fallback && created==old+1);
    old=destroyed;vk_sampler_variants_reset();assert(destroyed==old+256 && alive[native] && alive[fallback]);
    g_vk.props.limits.maxSamplerAllocationCount=8;
    NvnSamplerDesc first=descriptor(),second=descriptor();second.lod_bias=.25f;
    VkSampler retained=vk_sampler_filtered(&first,true,false,false,4);
    VkSampler bounded=vk_sampler_filtered(&second,true,false,false,4);
    assert(!first.backend && second.backend==(void*)bounded);
    assert(vk_sampler_filtered(&first,true,false,false,4)==retained);
    vk_sampler_variants_reset();assert(!alive[retained] && alive[bounded]);
    for(unsigned i=1;i<=created;i++)if(alive[i])vkDestroySampler(g_vk.dev,i,NULL);
    assert(destroyed==created);puts("production sampler cache bounds/ownership/native feature contracts: PASS");
}
