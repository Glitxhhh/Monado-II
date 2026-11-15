// Real VirtualAlloc reservations plus the production executor lifecycle.
// Vulkan responses are controlled here; the real GPU probe is separate.
#include "../../runtime/nvn/vulkan/host_import.h"
#include "../../runtime/nvn/vulkan/host_import_memory_windows.h"
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); abort(); } } while (0)
static unsigned char *heap;
#define HEAP_BASE ((uintptr_t)heap)
#define HEAP_MAX (1024ull*1024)
static uint64_t heap_size;
static uint64_t kernel_heap_size(void) { return heap_size; }
static struct { VkPhysicalDevice phys; VkDevice dev; VkPhysicalDeviceProperties props; bool host_import; } g_vk;
static VkHostImport gmem;
static int recorded, submitted, flushes, destroys, imports, fail_import;
static jmp_buf fatal_jump;
static char last_error[512];
#define FATAL(...) do { snprintf(last_error,sizeof last_error,__VA_ARGS__); longjmp(fatal_jump,1); } while (0)
#define LOG(...) fprintf(stderr,__VA_ARGS__)
static void renderer_flush(void) { flushes++; recorded=0; submitted=0; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physical,VkPhysicalDeviceProperties2 *properties) {
    (void)physical;
    ((VkPhysicalDeviceExternalMemoryHostPropertiesEXT*)properties->pNext)->minImportedHostPointerAlignment=65536;
}
VkResult vk_host_import_buffer(VkPhysicalDevice physical,VkDevice device,void *host,
        VkDeviceSize accessible,VkDeviceSize bytes,VkBufferUsageFlags usage,VkHostImport *out,char *error,size_t capacity) {
    (void)physical;(void)device;(void)usage;
    CHECK(!out->buffer&&!out->memory&&host==heap&&accessible>=bytes);
    CHECK(windows_host_import_range(host,(size_t)accessible,true));
    if(fail_import) { snprintf(error,capacity,"authored bind failure");return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    imports++;*out=(VkHostImport){(VkBuffer)(uintptr_t)imports,(VkDeviceMemory)(uintptr_t)imports,bytes,accessible,1};return VK_SUCCESS;
}
void vk_host_import_destroy(VkDevice device,VkHostImport *out) {
    (void)device;
    if(out->buffer) { CHECK(!recorded&&!submitted);destroys++; }
    memset(out,0,sizeof *out);
}
#include "../../runtime/nvn/vulkan/host_import_windows.inc"
static int failure(void (*action)(void),const char *message) {
    if(!setjmp(fatal_jump)) { action();return 0; }
    return strstr(last_error,message)!=NULL;
}
static void info_action(void) { (void)gmem_info(); }
static void import_action(void) { gmem_import(65536); }
int main(void) {
    heap=VirtualAlloc(NULL,HEAP_MAX,MEM_RESERVE,PAGE_READWRITE);CHECK(heap);
    CHECK(VirtualAlloc(heap,65536,MEM_COMMIT,PAGE_READWRITE)==heap);
    heap[0]=91;heap[65535]=37;
    heap_size=65535;g_vk.host_import=true;g_vk.props.limits.maxStorageBufferRange=HEAP_MAX;
    VkDescriptorBufferInfo first=gmem_info();CHECK(first.range==65536&&imports==1&&heap[0]==91&&heap[65535]==37);
    CHECK(failure(import_action,"overlap"));
    recorded=submitted=1;heap_size=131073;
    CHECK(failure(info_action,"grew during"));CHECK(!destroys&&gmem.size==65536&&recorded&&submitted);
    gmem_prepare_command();CHECK(flushes==1&&destroys==1&&imports==2&&gmem.size==196608);
    CHECK(heap[0]==91&&heap[65535]==37&&heap[196607]==0);
    gmem_prepare_command();CHECK(flushes==1&&imports==2);
    gmem_reset();CHECK(destroys==2&&!gmem.buffer&&!gmem.memory&&!gmem_tried);
    CHECK(VirtualFree(heap,HEAP_MAX,MEM_DECOMMIT));heap_size=65536;
    first=gmem_info();CHECK(first.range==65536&&heap[0]==0&&imports==3);
    gmem_reset();fail_import=1;CHECK(failure(info_action,"authored bind failure"));CHECK(!gmem.buffer&&!gmem.memory);
    fail_import=0;g_vk.host_import=false;CHECK(failure(info_action,"cannot return dummy data"));
    g_vk.host_import=true;heap_size=0;CHECK(failure(info_action,"invalid guest heap"));
    heap_size=HEAP_MAX+1;CHECK(failure(info_action,"invalid guest heap"));
    heap_size=131072;g_vk.props.limits.maxStorageBufferRange=65536;CHECK(failure(info_action,"descriptor range"));
    char error[256];DWORD prior;
    CHECK(VirtualProtect(heap,65536,PAGE_READONLY,&prior));
    CHECK(!windows_host_import_commit(heap,131072,error,sizeof error));
    MEMORY_BASIC_INFORMATION region;CHECK(VirtualQuery(heap,&region,sizeof region)==sizeof region&&region.Protect==PAGE_READONLY);
    CHECK(VirtualProtect(heap,65536,PAGE_READWRITE,&prior));
    CHECK(!windows_host_import_commit(heap+1,65536,error,sizeof error));
    CHECK(!windows_host_import_commit(heap,HEAP_MAX+65536,error,sizeof error));
    CHECK(!windows_host_import_commit((void*)(UINTPTR_MAX-4095),8192,error,sizeof error));
    CHECK(VirtualFree(heap,0,MEM_RELEASE));
    CHECK(!windows_host_import_commit(heap,65536,error,sizeof error));
    puts("native guest import lifecycle/commit/failure checks PASS");return 0;
}
