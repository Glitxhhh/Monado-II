// [XCDE] LINUX-103: execute production sampler creation/cache with deterministic Vulkan mocks.
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define vkCreateSampler fixture_vkCreateSampler
#define vkDestroySampler fixture_vkDestroySampler
#include "../../Monado_Linux/runtime/nvn/vulkan/textures.c"
#undef vkCreateSampler
#undef vkDestroySampler
VkG g_vk;
static unsigned choice,created,destroyed;
static bool alive[2048];
static VkSamplerCreateInfo latest;
int xcde_graphics_option(int index){assert(index==GFX_FILTERING);return choice;}
void rt_fatal_trace(void){abort();}
void vk_device_fault_report(void){abort();}
VKAPI_ATTR VkResult VKAPI_CALL fixture_vkCreateSampler(VkDevice dev,const VkSamplerCreateInfo *info,const VkAllocationCallbacks *allocator,VkSampler *sampler){
 (void)allocator;assert(dev==g_vk.dev && info && created+1<2048);created++;alive[created]=true;*sampler=(VkSampler)(uintptr_t)created;latest=*info;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL fixture_vkDestroySampler(VkDevice dev,VkSampler sampler,const VkAllocationCallbacks *allocator){
 (void)allocator;assert(dev==g_vk.dev);unsigned id=(unsigned)(uintptr_t)sampler;assert(id>0 && id<=created && alive[id]);alive[id]=false;destroyed++;
}
static NvnSamplerDesc descriptor(void){NvnSamplerDesc d={0};d.min_filter=5;d.mag_filter=1;d.lod_max=1000;d.max_aniso=1;d.wrap[0]=d.wrap[1]=d.wrap[2]=1;return d;}
int main(void){
 g_vk.dev=(VkDevice)(uintptr_t)1;g_vk.sampler_anisotropy=true;g_vk.props.limits.maxSamplerAnisotropy=8;g_vk.props.limits.maxSamplerAllocationCount=2048;
 NvnSamplerDesc d=descriptor();choice=0;VkSampler native=vk_sampler_filtered(&d,true,false,false,4);assert(native && d.backend==(void*)native && created==1 && !latest.anisotropyEnable);
 assert(vk_sampler_filtered(&d,true,false,false,4)==native && created==1);
 choice=1;VkSampler two=vk_sampler_filtered(&d,true,false,false,4);assert(two!=native && created==2 && latest.anisotropyEnable && latest.maxAnisotropy==2);
 assert(d.backend==(void*)native);NvnSamplerDesc same=descriptor();assert(vk_sampler_filtered(&same,true,false,false,4)==two && same.backend==NULL && created==2);
 choice=4;VkSampler eight=vk_sampler_filtered(&d,true,false,false,4);assert(eight!=two && created==3 && latest.maxAnisotropy==8);choice=3;assert(vk_sampler_filtered(&d,true,false,false,4)==eight && created==3);
 choice=2;assert(vk_sampler_filtered(&d,false,false,false,4)==native);assert(vk_sampler_filtered(&d,true,true,false,4)==native);assert(vk_sampler_filtered(&d,true,false,true,4)==native);assert(vk_sampler_filtered(&d,true,false,false,1)==native);
 assert(created==3 && destroyed==0 && alive[(unsigned)(uintptr_t)two]);
 // Release the ephemeral guest descriptor; interned handles remain valid and shared.
 memset(&same,0xff,sizeof same);same=descriptor();choice=1;assert(vk_sampler_filtered(&same,true,false,false,4)==two && created==3);
 vk_sampler_variants_reset();assert(destroyed==2 && !alive[(unsigned)(uintptr_t)two] && !alive[(unsigned)(uintptr_t)eight] && alive[(unsigned)(uintptr_t)native]);
 vk_sampler_variants_reset();assert(destroyed==2); // idempotent, native belongs to its original owner
 choice=1;for(unsigned i=0;i<256;i++){NvnSamplerDesc variant=descriptor();variant.lod_bias=(float)i/16;VkSampler s=vk_sampler_filtered(&variant,true,false,false,4);assert(s && !variant.backend);}
 unsigned cache_created=created;NvnSamplerDesc overflow=descriptor();overflow.lod_bias=100;VkSampler fallback=vk_sampler_filtered(&overflow,true,false,false,4);assert(fallback && overflow.backend==(void*)fallback && created==cache_created+1);
 assert(vk_sampler_filtered(&overflow,true,false,false,4)==fallback && created==cache_created+1);
 unsigned old_destroyed=destroyed;vk_sampler_variants_reset();assert(destroyed==old_destroyed+256 && alive[(unsigned)(uintptr_t)native] && alive[(unsigned)(uintptr_t)fallback]);
 // Small limits reserve native-device headroom; exact intern hits remain usable.
 g_vk.props.limits.maxSamplerAllocationCount=8;NvnSamplerDesc first=descriptor(),second=descriptor();second.lod_bias=.25f;
 VkSampler retained=vk_sampler_filtered(&first,true,false,false,4);assert(!first.backend);VkSampler bounded=vk_sampler_filtered(&second,true,false,false,4);assert(second.backend==(void*)bounded);assert(vk_sampler_filtered(&first,true,false,false,4)==retained);
 vk_sampler_variants_reset();assert(!alive[(unsigned)(uintptr_t)retained] && alive[(unsigned)(uintptr_t)bounded]);
 // Mock cleanup of native-owned handles, separate from override lifetime.
 for(unsigned i=1;i<=created;i++)if(alive[i])fixture_vkDestroySampler(g_vk.dev,(VkSampler)(uintptr_t)i,NULL);
 assert(destroyed==created);puts("Production sampler Original/cache/live choice/semantic sharing/bounds/reset ownership PASS");
}
