// [XCDE] LINUX-103: execute the actual patched SDK allocator on authored heaps.
#define vkGetPhysicalDeviceMemoryProperties monado_mock_memory_properties
#include <vulkan/vulkan.h>
static VkPhysicalDeviceMemoryProperties test_memory;
VKAPI_ATTR void VKAPI_CALL monado_mock_memory_properties(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties*out){*out=test_memory;}
#include MONADO_SDK_VK_SOURCE
#undef vkGetPhysicalDeviceMemoryProperties
#include <cassert>
#include <cstdio>
int main(){
    VkMemoryRequirements r{};r.memoryTypeBits=7;VkMemoryPropertyFlags p=999;
    auto device=reinterpret_cast<VkPhysicalDevice>(uintptr_t(1));
    const auto local=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,visible=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        coherent=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,cached=VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    test_memory.memoryTypeCount=1;test_memory.memoryTypes[0].propertyFlags=local|visible|coherent|cached;
    assert(findMemoryTypeIndex(device,r,local,p)==0 && (p&local));
    assert(findMemoryTypeIndex(device,r,visible|coherent,p)==0);
    test_memory.memoryTypeCount=3;test_memory.memoryTypes[1].propertyFlags=local;
    test_memory.memoryTypes[2].propertyFlags=visible|coherent;
    assert(findMemoryTypeIndex(device,r,local,p)==1 && p==local); // prefer invisible VRAM
    assert(findMemoryTypeIndex(device,r,visible|coherent,p)==0);
    r.memoryTypeBits=2;assert(findMemoryTypeIndex(device,r,visible|coherent,p)==UINT32_MAX&&p==0);
    r.memoryTypeBits=4;assert(findMemoryTypeIndex(device,r,local,p)==UINT32_MAX);
    r.memoryTypeBits=1;test_memory.memoryTypes[0].propertyFlags=visible;
    assert(findMemoryTypeIndex(device,r,visible|coherent,p)==UINT32_MAX); // all bits required
    assert(findMemoryTypeIndex(device,r,visible,p)==0&&p==visible);
    r.memoryTypeBits=0;assert(findMemoryTypeIndex(device,r,0,p)==UINT32_MAX);
    printf("native SDK memory selector: 9 UMA/discrete/property/mask cases passed\n");
}
