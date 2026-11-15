// Actual production optional device preparation with deterministic Vulkan queries.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nvn/vulkan/vk.h"
#include "nvn/vulkan/fsr4_policy.h"
#undef LOG
#define LOG(...) ((void)0)
VkG g_vk;static unsigned scenario,queries;
bool tmp_fsr4_configured(void){return scenario!=13;}
static void fixture_properties(VkPhysicalDevice physical,VkPhysicalDeviceProperties2 *out){
 assert(physical==(VkPhysicalDevice)1);queries++;
 VkPhysicalDeviceSubgroupProperties *subgroup=out->pNext;assert(subgroup->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES);
 subgroup->supportedStages=scenario==5?0:VK_SHADER_STAGE_COMPUTE_BIT;subgroup->supportedOperations=scenario==6?0:VK_SUBGROUP_FEATURE_QUAD_BIT;
 VkPhysicalDeviceFloatControlsProperties *controls=subgroup->pNext;controls->shaderDenormPreserveFloat16=scenario!=7;
 VkPhysicalDevicePushDescriptorPropertiesKHR *push=controls->pNext;push->maxPushDescriptors=scenario==9?10:11;
}
#define vkGetPhysicalDeviceProperties2 fixture_properties
#include "fsr4_device_source.h"
#undef vkGetPhysicalDeviceProperties2
int main(void){
 for(scenario=0;scenario<15;scenario++){
  queries=0;g_vk.phys=(VkPhysicalDevice)1;
  g_vk.props.limits.maxComputeWorkGroupInvocations=scenario==10?255:256;g_vk.props.limits.maxComputeWorkGroupSize[0]=scenario==11?255:256;
  g_vk.props.limits.maxStorageBufferRange=scenario==12?99820991:99820992;
  VkPhysicalDeviceFeatures2 available={.features={.shaderInt16=scenario!=2,.shaderStorageImageWriteWithoutFormat=scenario!=4}};
  VkPhysicalDeviceVulkan12Features h12={.shaderFloat16=scenario!=1};
  VkPhysicalDeviceVulkan13Features h13={.shaderIntegerDotProduct=scenario!=3},enabled={0};const char *extensions[2]={0};uint32_t count=0;
  fsr4_device_prepare(&available,&h12,&h13,scenario!=8,extensions,&count,&enabled);
  bool capable=scenario==0||scenario==14;assert(g_vk.fsr4_lite==capable&&(g_vk.fsr4_reason==NULL)==capable);
  assert(count==(capable?1u:0u)&&enabled.shaderIntegerDotProduct==capable&&queries==(scenario==13?0u:1u));
  if(capable)assert(!strcmp(extensions[0],VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME));
 }
 assert(monado_fsr4_extent(107,60,320,180));assert(monado_fsr4_extent(127,60,320,180));
 assert(!monado_fsr4_extent(128,60,320,180));assert(!monado_fsr4_extent(60,128,180,320));
 assert(!monado_fsr4_extent(3840,2160,3840,2160));assert(monado_fsr4_extent(3840,2160,3841,2160));
 assert(monado_fsr4_extent(0,60,320,180));assert(monado_fsr4_extent(321,180,320,180));
 puts("PASS actual native FSR4 capability15cases, explicit configuration gate, long-edge128 and extent boundaries");return 0;
}
