// Actual production optional device-feature preparation with deterministic query mocks.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../runtime/nvn/vulkan/vk.h"
#include "../../runtime/nvn/vulkan/fsr4_policy.h"
VkG g_vk;static unsigned scenario,queries;
static void fixture_properties(VkPhysicalDevice physical,VkPhysicalDeviceProperties2 *out){assert(physical==(VkPhysicalDevice)1);queries++;
 VkPhysicalDeviceSubgroupProperties *subgroup=out->pNext;assert(subgroup->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES);subgroup->supportedStages=scenario==5?0:VK_SHADER_STAGE_COMPUTE_BIT;subgroup->supportedOperations=scenario==6?0:VK_SUBGROUP_FEATURE_QUAD_BIT;
 VkPhysicalDeviceFloatControlsProperties *controls=subgroup->pNext;assert(controls->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES);controls->shaderDenormPreserveFloat16=scenario!=7;
 VkPhysicalDevicePushDescriptorPropertiesKHR *push=controls->pNext;assert(push->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR);push->maxPushDescriptors=scenario==9?10:11;
}
#define vkGetPhysicalDeviceProperties2 fixture_properties
// GENERATED_DEVICE_PREPARATION
#undef vkGetPhysicalDeviceProperties2
int main(void){
 for(scenario=0;scenario<14;scenario++){
  g_vk.phys=(VkPhysicalDevice)1;g_vk.props.limits.maxComputeWorkGroupInvocations=scenario==10?255:256;g_vk.props.limits.maxComputeWorkGroupSize[0]=scenario==11?255:256;g_vk.props.limits.maxStorageBufferRange=scenario==12?99820991:99820992;
  VkPhysicalDeviceFeatures2 available={.features={.shaderInt16=scenario!=2,.shaderStorageImageWriteWithoutFormat=scenario!=4}};VkPhysicalDeviceVulkan12Features h12={.shaderFloat16=scenario!=1};VkPhysicalDeviceVulkan13Features h13={.shaderIntegerDotProduct=scenario!=3},enabled={0};const char *extensions[2]={0};uint32_t count=0;
  fsr4_device_prepare(&available,&h12,&h13,scenario!=8,extensions,&count,&enabled);
#ifdef XCDE_LINUX_FSR4
  bool capable=scenario==0 || scenario==13;assert(g_vk.fsr4_lite==capable && (g_vk.fsr4_reason==NULL)==capable && count==(capable?1u:0u) && enabled.shaderIntegerDotProduct==capable);if(capable)assert(!strcmp(extensions[0],VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME));
#else
  assert(!g_vk.fsr4_lite && g_vk.fsr4_reason && !count && !enabled.shaderIntegerDotProduct && !queries);
#endif
 }
 puts("PASS actual FSR4 optional device feature contract:14 cases; no unused matrices/VALVE/Int8 dependency");return 0;
}
