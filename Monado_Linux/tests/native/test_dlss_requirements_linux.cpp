// [XCDE] LINUX-017: actual Linux renderer shim requirements; mocked queries, no Vulkan instance/device or NGX load.
#include <cassert>
#include <cstdio>
#include <cstring>
#ifndef DLSS_SHIM_SOURCE
#define DLSS_SHIM_SOURCE "../../runtime/nvn/vulkan/dlss.cpp"
#endif
#include DLSS_SHIM_SOURCE

Settings g_settings{};
static bool query_ok = true, require_bda = true, support_bda = true, prepared = true, ready;
static unsigned query_calls, feature_calls, shutdown_calls, device_calls, initialized_calls;
static const VkInstance instance = reinterpret_cast<VkInstance>(uintptr_t(0x1234));
static const VkPhysicalDevice physical = reinterpret_cast<VkPhysicalDevice>(uintptr_t(0x2345));
extern "C" bool ngx_linux_device_exts(VkInstance i, VkPhysicalDevice p, const char **out, uint32_t *n, uint32_t max) {
    assert(i == instance && p == physical); query_calls++;
    if (!query_ok) { prepared = false; return false; }
    const char *wanted[] = {VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    for (int j = require_bda ? 0 : 1; j < 2; j++) {
        bool duplicate = false;
        for (uint32_t k = 0; k < *n; k++) duplicate |= !strcmp(out[k], wanted[j]);
        if (!duplicate) { assert(*n < max); out[(*n)++] = wanted[j]; }
    }
    return true;
}
extern "C" void ngx_linux_shutdown(void) { shutdown_calls++; prepared = ready = false; }
extern "C" void ngx_linux_device_created(VkInstance i, VkPhysicalDevice p, VkDevice, uint32_t) {
    assert(i == instance && p == physical); device_calls++;
    if (prepared) { initialized_calls++; ready = true; }
}
extern "C" bool ngx_linux_available(void) { return ready; }
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *features) {
    assert(p == physical && features && features->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    auto *f12 = static_cast<VkPhysicalDeviceVulkan12Features *>(features->pNext);
    assert(f12 && f12->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES && !f12->pNext);
    feature_calls++; f12->bufferDeviceAddress = support_bda ? VK_TRUE : VK_FALSE;
}
int main(int argc, char **argv) {
    assert(argc == 2); const char *test = argv[1];
    if (!strcmp(test,"unsupported")) support_bda = false;
    if (!strcmp(test,"query-fail") || !strcmp(test,"query-fail-existing")) query_ok = false;
    if (!strcmp(test,"no-bda") || !strcmp(test,"no-bda-null")) require_bda = false;
    const bool existing = !strcmp(test,"existing") || !strcmp(test,"query-fail-existing");
    const bool missing = !strcmp(test,"missing-f12") || !strcmp(test,"no-bda-null");
    const char *ext[8] = {"renderer-baseline"}; uint32_t n = 1;
    if (existing) ext[n++] = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
    const uint32_t initial_n = n;
    VkPhysicalDeviceVulkan13Features f13{}; f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{}; f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = &f13; f12.timelineSemaphore = f12.shaderInt8 = f12.shaderFloat16 = VK_TRUE;
    if (!strcmp(test,"already-enabled")) f12.bufferDeviceAddress = VK_TRUE;
    auto expected_f12 = f12; auto expected_f13 = f13;
    uint32_t graphics = 99, compute = 99;
    g_settings.upscaler = UPSCALE_DLSS;
    dlss_device_requirements(instance, physical, ext, &n, 8, missing ? nullptr : &f12, &f13, &graphics, &compute);
    assert(query_calls == 1 && graphics == 0 && compute == 0 && !strcmp(ext[0],"renderer-baseline"));
    const bool enable = query_ok && require_bda && support_bda && !missing;
    const bool reject = query_ok && require_bda && (!support_bda || missing);
    if (enable) expected_f12.bufferDeviceAddress = VK_TRUE;
    assert(!memcmp(&f12,&expected_f12,sizeof f12) && !memcmp(&f13,&expected_f13,sizeof f13));
    assert(feature_calls == (query_ok && require_bda && !missing ? 1u : 0u));
    assert(shutdown_calls == (reject ? 1u : 0u));
    if (reject || !query_ok) assert(n == initial_n);
    else assert(n == (require_bda ? 3u : 2u));
    dlss_device_created(instance,physical,reinterpret_cast<VkDevice>(uintptr_t(0x3456)),0);
    assert(device_calls == 1 && initialized_calls == (query_ok && !reject ? 1u : 0u));
    assert(g_settings.upscaler == (query_ok && !reject ? UPSCALE_DLSS : UPSCALE_FSR1));
    printf("requirements %s passed: query=%u physical=%u BDA=%u shutdown=%u initialized=%u extensions=%u\n",
        test,query_calls,feature_calls,f12.bufferDeviceAddress,shutdown_calls,initialized_calls,n);
    return 0;
}
