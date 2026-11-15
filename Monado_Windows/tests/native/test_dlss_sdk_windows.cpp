// Actual production DLSS + explicit local SDK, authored pixels only.
// Hidden SDL window; no game, profile, input, model download or FPS claim.
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" {
#include "settings.h"
#include "nvn/vulkan/dlss.h"
Settings g_settings;
VkG g_vk;
}
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL %s:%d: %s; DLSS: %s\n",__FILE__,__LINE__,#x,dlss_status());exit(1);} }while(0)
#define VK_OK(x) do { VkResult result=(x);if(result!=VK_SUCCESS){fprintf(stderr,"Vulkan %d %s:%d: %s\n",result,__FILE__,__LINE__,#x);exit(1);} }while(0)
static constexpr uint32_t width=320,height=192;
static std::vector<VkImageView> views;
static unsigned validation_errors;
static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT *data,void*) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT){validation_errors++;fprintf(stderr,"VALIDATION %s\n",data->pMessage);}
    return VK_FALSE;
}
// The production integration requests actual views through this renderer boundary.
// This fixture supplies real VkImageViews over its real authored images.
extern "C" VkImageView vk_image_view(TexBackend *image,VkImageViewType type,VkFormat format,
    uint32_t level,uint32_t levels,uint32_t layer,uint32_t layers,VkComponentMapping components,VkImageAspectFlags aspect) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};info.image=image->image;info.viewType=type;
    info.format=format;info.components=components;info.subresourceRange={aspect,level,levels,layer,layers};
    VkImageView view;VK_OK(vkCreateImageView(g_vk.dev,&info,nullptr,&view));views.push_back(view);return view;
}
static uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(g_vk.phys,&memory);
    for(uint32_t i=0;i<memory.memoryTypeCount;i++)if((bits&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&properties)==properties)return i;
    CHECK(false);return 0;
}
struct Image {
    TexBackend image{};VkDeviceMemory memory{};
    explicit Image(VkFormat format) {
        image.format=format;image.width=width;image.height=height;image.depth=image.layers=image.levels=image.samples=1;
        image.aspect=VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=VK_IMAGE_TYPE_2D;info.format=format;
        info.extent={width,height,1};info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VK_OK(vkCreateImage(g_vk.dev,&info,nullptr,&image.image));VkMemoryRequirements requirements;vkGetImageMemoryRequirements(g_vk.dev,image.image,&requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memory_type(requirements.memoryTypeBits,0);
        VK_OK(vkAllocateMemory(g_vk.dev,&allocation,nullptr,&memory));VK_OK(vkBindImageMemory(g_vk.dev,image.image,memory,0));
    }
    ~Image(){vkDestroyImage(g_vk.dev,image.image,nullptr);vkFreeMemory(g_vk.dev,memory,nullptr);}
};
static void barrier(VkCommandBuffer command) {
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};memory.srcStageMask=memory.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    memory.srcAccessMask=memory.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.memoryBarrierCount=1;dependency.pMemoryBarriers=&memory;vkCmdPipelineBarrier2(command,&dependency);
}
static void transition(VkCommandBuffer command,VkImage image,VkImageLayout before,VkImageLayout after) {
    VkImageMemoryBarrier2 memory{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};memory.srcStageMask=memory.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    memory.srcAccessMask=memory.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    memory.oldLayout=before;memory.newLayout=after;memory.srcQueueFamilyIndex=memory.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    memory.image=image;memory.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.imageMemoryBarrierCount=1;dependency.pImageMemoryBarriers=&memory;vkCmdPipelineBarrier2(command,&dependency);
}
static bool contains(const std::vector<const char*>&names,const char *name){for(auto n:names)if(!strcmp(n,name))return true;return false;}
static float halton(unsigned value,unsigned base){float result=0,scale=1;while(value){scale/=base;result+=scale*(value%base);value/=base;}return result;}
static bool hud_pixel(unsigned x,unsigned y){return x>=width*3/4&&y<height/4;}
static void signal(float x,float y,bool hud,bool restarted,float *rgb){
    if(hud){rgb[0]=.8f;rgb[1]=.2f;rgb[2]=.1f;return;}
    rgb[0]=.15f+.45f*x/width;rgb[1]=.15f+.45f*y/height;rgb[2]=.35f+.12f*sinf(x*.06f)*cosf(y*.07f);
    if(restarted)std::swap(rgb[0],rgb[2]);
}
static void pixels(VkCommandBuffer command,VkQueue queue,VkSwapchainKHR swapchain,const std::vector<VkImage>&swap_images,
    VkExtent2D extent,PFN_vkAcquireNextImageKHR acquire,PFN_vkQueuePresentKHR present,PFN_vkDeviceWaitIdle wait_idle,bool legacy_r8) {
    (void)extent;
    Image color(VK_FORMAT_R8G8B8A8_UNORM),motion(VK_FORMAT_R16G16_SFLOAT),depth(VK_FORMAT_R32_SFLOAT),output(VK_FORMAT_R8G8B8A8_UNORM),mask(legacy_r8?VK_FORMAT_R8_UNORM:VK_FORMAT_R16_SFLOAT);
    VkBuffer buffer;VkDeviceMemory memory;VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=width*height*4;info.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_OK(vkCreateBuffer(g_vk.dev,&info,nullptr,&buffer));VkMemoryRequirements requirements;vkGetBufferMemoryRequirements(g_vk.dev,buffer,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_OK(vkAllocateMemory(g_vk.dev,&allocation,nullptr,&memory));VK_OK(vkBindBufferMemory(g_vk.dev,buffer,memory,0));uint8_t *mapping;VK_OK(vkMapMemory(g_vk.dev,memory,0,VK_WHOLE_SIZE,0,(void**)&mapping));
    const size_t color_bytes=width*height*4,mask_stride=legacy_r8?1:2;
    VkBuffer upload;VkDeviceMemory upload_memory;info.size=color_bytes+width*height*mask_stride;info.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VK_OK(vkCreateBuffer(g_vk.dev,&info,nullptr,&upload));vkGetBufferMemoryRequirements(g_vk.dev,upload,&requirements);
    allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_OK(vkAllocateMemory(g_vk.dev,&allocation,nullptr,&upload_memory));VK_OK(vkBindBufferMemory(g_vk.dev,upload,upload_memory,0));uint8_t *uploaded;VK_OK(vkMapMemory(g_vk.dev,upload_memory,0,VK_WHOLE_SIZE,0,(void**)&uploaded));
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VkSemaphore available,ready;VK_OK(vkCreateSemaphore(g_vk.dev,&sem,nullptr,&available));VK_OK(vkCreateSemaphore(g_vk.dev,&sem,nullptr,&ready));
    TmpCamera camera{};camera.ok=true;camera.view[0]=camera.view[5]=camera.view[10]=1;
    const float near_plane=.1f,far_plane=1000.f;camera.proj[0]=1.7320508f/(float(width)/height);camera.proj[5]=1.7320508f;
    camera.proj[10]=-(far_plane+near_plane)/(far_plane-near_plane);camera.proj[11]=-2*far_plane*near_plane/(far_plane-near_plane);camera.proj[14]=-1;
    memcpy(camera.viewproj,camera.proj,sizeof camera.proj);
    const double reprojection[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const unsigned frame_count=legacy_r8?3:20;unsigned samples=0;std::vector<bool> initialized(swap_images.size());std::vector<uint8_t> previous_pixels(color_bytes);
    for(unsigned frame=0;frame<frame_count;frame++) {
        if(frame==16){VK_OK(wait_idle(g_vk.dev));dlss_reset();dlss_reset();CHECK(dlss_available());}
        const bool restarted=frame>=16;const unsigned movement=frame>=12&&frame<16?frame-11:0;
        const float jitter[2]={halton(frame+1,2)-.5f,halton(frame+1,3)-.5f};
        for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++){
            float rgb[3];bool hud=hud_pixel(x,y);signal((float)x+.5f+movement-jitter[0],(float)y+.5f-jitter[1],hud,restarted,rgb);
            size_t pixel=y*width+x;for(unsigned c=0;c<3;c++)uploaded[pixel*4+c]=(uint8_t)std::clamp((int)std::lround(rgb[c]*255.f),0,255);uploaded[pixel*4+3]=255;
            if(legacy_r8)uploaded[color_bytes+pixel]=hud?255:0;
            else{uint16_t half=hud?0x3c00:0;memcpy(uploaded+color_bytes+pixel*2,&half,2);}
        }
        uint32_t acquired;VkResult result=acquire(g_vk.dev,swapchain,UINT64_MAX,available,VK_NULL_HANDLE,&acquired);CHECK(result==VK_SUCCESS||result==VK_SUBOPTIMAL_KHR);CHECK(acquired<swap_images.size());
        VK_OK(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;VK_OK(vkBeginCommandBuffer(command,&begin));
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};Image *images[]={&color,&motion,&depth,&output,&mask};
        for(auto image:images)if(!frame)transition(command,image->image.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
        VkMemoryBarrier2 host_write{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};host_write.srcStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host_write.srcAccessMask=VK_ACCESS_2_HOST_WRITE_BIT;host_write.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;host_write.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo uploaded_data{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};uploaded_data.memoryBarrierCount=1;uploaded_data.pMemoryBarriers=&host_write;vkCmdPipelineBarrier2(command,&uploaded_data);
        VkBufferImageCopy input_copy{};input_copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};input_copy.imageExtent={width,height,1};
        vkCmdCopyBufferToImage(command,upload,color.image.image,VK_IMAGE_LAYOUT_GENERAL,1,&input_copy);input_copy.bufferOffset=color_bytes;vkCmdCopyBufferToImage(command,upload,mask.image.image,VK_IMAGE_LAYOUT_GENERAL,1,&input_copy);
        VkClearColorValue c{};c.float32[0]=movement?1.f/width:0.f;vkCmdClearColorImage(command,motion.image.image,VK_IMAGE_LAYOUT_GENERAL,&c,1,&range);
        c.float32[0]=.5f;vkCmdClearColorImage(command,depth.image.image,VK_IMAGE_LAYOUT_GENERAL,&c,1,&range);c.float32[0]=c.float32[1]=c.float32[2]=.843137f;vkCmdClearColorImage(command,output.image.image,VK_IMAGE_LAYOUT_GENERAL,&c,1,&range);barrier(command);
        CHECK(dlss_evaluate(command,&color.image,&motion.image,&depth.image,&output.image,&mask.image,jitter,frame==0||frame==16,&camera,reprojection,frame+1,frame));barrier(command);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};vkCmdCopyImageToBuffer(command,output.image.image,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&copy);
        VkMemoryBarrier2 host_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};host_read.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;host_read.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;host_read.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host_read.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo readback{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};readback.memoryBarrierCount=1;readback.pMemoryBarriers=&host_read;vkCmdPipelineBarrier2(command,&readback);
        // A real intercepted present completes Streamline's frame lifecycle;
        // the hidden surface itself is unrelated to the readback oracle.
        transition(command,swap_images[acquired],initialized[acquired]?VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);initialized[acquired]=true;
        VkClearColorValue black{};vkCmdClearColorImage(command,swap_images[acquired],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
        transition(command,swap_images[acquired],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);barrier(command);VK_OK(vkEndCommandBuffer(command));
        VkPipelineStageFlags wait_stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&available;submit.pWaitDstStageMask=&wait_stage;submit.commandBufferCount=1;submit.pCommandBuffers=&command;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&ready;
        VK_OK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));VkPresentInfoKHR presentation{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};presentation.waitSemaphoreCount=1;presentation.pWaitSemaphores=&ready;presentation.swapchainCount=1;presentation.pSwapchains=&swapchain;presentation.pImageIndices=&acquired;
        result=present(queue,&presentation);CHECK(result==VK_SUCCESS||result==VK_SUBOPTIMAL_KHR);VK_OK(wait_idle(g_vk.dev));
        unsigned frame_samples=0,max_error=0,max_static_delta=0;for(unsigned y=12;y<height-12;y++)for(unsigned x=12;x<width-12;x++){
            // Exclude only the three-pixel sharp HUD boundary; smooth scene
            // pixels and the tagged opaque HUD interior are both measured.
            if((x>=width*3/4-3&&x<=width*3/4+3)||(y>=height/4-3&&y<=height/4+3))continue;
            float rgb[3];signal((float)x+.5f+movement,(float)y+.5f,hud_pixel(x,y),restarted,rgb);
            for(unsigned channel=0;channel<3;channel++){
            size_t at=(y*width+x)*4+channel;int expected=(int)std::lround(rgb[channel]*255.f),actual=mapping[at];
            max_error=std::max(max_error,(unsigned)abs(actual-expected));
            if(frame>0&&frame<12)max_static_delta=std::max(max_static_delta,(unsigned)abs(actual-previous_pixels[at]));
            if(abs(actual-expected)>16){fprintf(stderr,"DLSS pixel mismatch frame%u (%u,%u) channel%u got%d expected%d tolerance16\n",frame,x,y,channel,actual,expected);CHECK(false);}frame_samples++;
            }
        }
        CHECK(max_static_delta<=16);memcpy(previous_pixels.data(),mapping,color_bytes);
        samples+=frame_samples;printf("DLSS frame%u reset%u mask%s motion%s jitter%.5f,%.5f accepted, submit/present/idle complete, %u RGB samples error%u staticdelta%u within16/255\n",frame,frame==0||frame==16,legacy_r8?"R8_UNORM":"R16_SFLOAT",movement?"moving":"stationary-zero",jitter[0],jitter[1],frame_samples,max_error,max_static_delta);fflush(stdout);
    }
    dlss_reset();dlss_reset();for(auto view:views)vkDestroyImageView(g_vk.dev,view,nullptr);views.clear();
    vkDestroySemaphore(g_vk.dev,available,nullptr);vkDestroySemaphore(g_vk.dev,ready,nullptr);vkUnmapMemory(g_vk.dev,memory);vkDestroyBuffer(g_vk.dev,buffer,nullptr);vkFreeMemory(g_vk.dev,memory,nullptr);
    vkUnmapMemory(g_vk.dev,upload_memory);vkDestroyBuffer(g_vk.dev,upload,nullptr);vkFreeMemory(g_vk.dev,upload_memory,nullptr);
    printf("PASS actual production DLSS SDK: %u structured authored DLAA frames, real fifth HUD mask, Halton/stationary-zero/moving input, double reset/fresh restart, %u bounded RGB samples; no game or performance claim\n",frame_count,samples);
}
int main(int argc,char **argv) {
    bool legacy_r8=argc==2&&!strcmp(argv[1],"--mask-r8");CHECK(argc==1||legacy_r8||(argc==2&&!strcmp(argv[1],"--mask-r16")));
    g_settings.upscaler=UPSCALE_DLSS;g_settings.upscale_quality=UQ_NATIVE;
    CHECK(SDL_Init(SDL_INIT_VIDEO));SDL_Window *window=SDL_CreateWindow("Monado authored hidden DLSS oracle",width,height,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);CHECK(window);
    CHECK(dlss_init_early());Uint32 count=0;const char *const *sdl=SDL_Vulkan_GetInstanceExtensions(&count);CHECK(sdl);
    std::vector<const char*> extensions(sdl,sdl+count);const char *required[128];int requirements=dlss_instance_exts(required,128);for(int i=0;i<requirements;i++)if(!contains(extensions,required[i]))extensions.push_back(required[i]);
    uint32_t nl=0;VK_OK(vkEnumerateInstanceLayerProperties(&nl,nullptr));std::vector<VkLayerProperties> layers(nl);VK_OK(vkEnumerateInstanceLayerProperties(&nl,layers.data()));bool validate=false;for(auto&layer:layers)validate|=!strcmp(layer.layerName,"VK_LAYER_KHRONOS_validation");
    const char *layer="VK_LAYER_KHRONOS_validation";if(validate&&!contains(extensions,VK_EXT_DEBUG_UTILS_EXTENSION_NAME))extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Monado authored native DLSS oracle";app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};instance.pApplicationInfo=&app;instance.enabledExtensionCount=(uint32_t)extensions.size();instance.ppEnabledExtensionNames=extensions.data();if(validate){instance.enabledLayerCount=1;instance.ppEnabledLayerNames=&layer;}VK_OK(vkCreateInstance(&instance,nullptr,&g_vk.inst));
    VkDebugUtilsMessengerEXT messenger{};if(validate){auto create=(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst,"vkCreateDebugUtilsMessengerEXT");CHECK(create);VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};m.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;m.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;m.pfnUserCallback=validation;VK_OK(create(g_vk.inst,&m,nullptr,&messenger));}
    puts(validate?"Khronos validation enabled":"Khronos validation unavailable; no validation-layer pass claimed");
    VkSurfaceKHR ordinary;CHECK(SDL_Vulkan_CreateSurface(window,g_vk.inst,nullptr,&ordinary));uint32_t devices=0;VK_OK(vkEnumeratePhysicalDevices(g_vk.inst,&devices,nullptr));std::vector<VkPhysicalDevice> physical(devices);VK_OK(vkEnumeratePhysicalDevices(g_vk.inst,&devices,physical.data()));g_vk.qfam=UINT32_MAX;
    for(auto device:physical){VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(device,&properties);if(properties.vendorID!=0x10de||properties.apiVersion<VK_API_VERSION_1_3)continue;uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(device,&families,nullptr);std::vector<VkQueueFamilyProperties> queues(families);vkGetPhysicalDeviceQueueFamilyProperties(device,&families,queues.data());for(unsigned family=0;family<families;family++){VkBool32 supported;VK_OK(vkGetPhysicalDeviceSurfaceSupportKHR(device,family,ordinary,&supported));if(supported&&(queues[family].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){g_vk.phys=device;g_vk.qfam=family;g_vk.props=properties;break;}}if(g_vk.phys)break;}
    CHECK(g_vk.phys);printf("GPU %s driver%u API%u.%u.%u family%u\n",g_vk.props.deviceName,g_vk.props.driverVersion,VK_API_VERSION_MAJOR(g_vk.props.apiVersion),VK_API_VERSION_MINOR(g_vk.props.apiVersion),VK_API_VERSION_PATCH(g_vk.props.apiVersion),g_vk.qfam);
    dlss_validate_adapter(g_vk.phys,VK_API_VERSION_1_3,g_vk.qfam);const char *device_extensions[128]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};uint32_t n_ext=1,extra_graphics=0,extra_compute=0;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};f13.synchronization2=VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};f12.pNext=&f13;dlss_device_requirements(g_vk.inst,g_vk.phys,device_extensions,&n_ext,128,&f12,&f13,&extra_graphics,&extra_compute);
    unsigned queue_count=1+extra_graphics+extra_compute;CHECK(queue_count<=16);float priority[16];std::fill(priority,priority+16,1.f);VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue.queueFamilyIndex=g_vk.qfam;queue.queueCount=queue_count;queue.pQueuePriorities=priority;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};vkGetPhysicalDeviceFeatures(g_vk.phys,&features.features);features.pNext=&f12;
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device.pNext=&features;device.queueCreateInfoCount=1;device.pQueueCreateInfos=&queue;device.enabledExtensionCount=n_ext;device.ppEnabledExtensionNames=device_extensions;VK_OK(vkCreateDevice(g_vk.phys,&device,nullptr,&g_vk.dev));vkGetDeviceQueue(g_vk.dev,g_vk.qfam,0,&g_vk.queue);
    dlss_device_created(g_vk.inst,g_vk.phys,g_vk.dev,g_vk.qfam);CHECK(dlss_create_surface(window,g_vk.inst,g_vk.phys,g_vk.qfam,&g_vk.surface));CHECK(dlss_available());vkDestroySurfaceKHR(g_vk.inst,ordinary,nullptr);
    auto create=(PFN_vkCreateSwapchainKHR)dlss_proxy(g_vk.dev,"vkCreateSwapchainKHR");auto destroy=(PFN_vkDestroySwapchainKHR)dlss_proxy(g_vk.dev,"vkDestroySwapchainKHR");auto get_images=(PFN_vkGetSwapchainImagesKHR)dlss_proxy(g_vk.dev,"vkGetSwapchainImagesKHR");auto acquire=(PFN_vkAcquireNextImageKHR)dlss_proxy(g_vk.dev,"vkAcquireNextImageKHR");auto present=(PFN_vkQueuePresentKHR)dlss_proxy(g_vk.dev,"vkQueuePresentKHR");auto wait_idle=(PFN_vkDeviceWaitIdle)dlss_proxy(g_vk.dev,"vkDeviceWaitIdle");CHECK(create&&destroy&&get_images&&acquire&&present&&wait_idle);
    VkSurfaceCapabilitiesKHR capabilities;VK_OK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys,g_vk.surface,&capabilities));CHECK(capabilities.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT);uint32_t formats=0;VK_OK(vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys,g_vk.surface,&formats,nullptr));CHECK(formats);std::vector<VkSurfaceFormatKHR> surface_formats(formats);VK_OK(vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys,g_vk.surface,&formats,surface_formats.data()));
    auto format=surface_formats[0];for(auto candidate:surface_formats)if(candidate.format==VK_FORMAT_B8G8R8A8_UNORM){format=candidate;break;}VkExtent2D extent=capabilities.currentExtent;if(extent.width==UINT32_MAX)extent={width,height};CHECK(extent.width&&extent.height);
    VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;for(unsigned i=0;i<4;i++)if(capabilities.supportedCompositeAlpha&(1u<<i)){alpha=(VkCompositeAlphaFlagBitsKHR)(1u<<i);break;}
    VkSwapchainCreateInfoKHR swap{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};swap.surface=g_vk.surface;swap.minImageCount=std::max(2u,capabilities.minImageCount);if(capabilities.maxImageCount)swap.minImageCount=std::min(swap.minImageCount,capabilities.maxImageCount);swap.imageFormat=format.format;swap.imageColorSpace=format.colorSpace;swap.imageExtent=extent;swap.imageArrayLayers=1;swap.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;swap.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;swap.preTransform=capabilities.currentTransform;swap.compositeAlpha=alpha;swap.presentMode=VK_PRESENT_MODE_FIFO_KHR;swap.clipped=VK_TRUE;VkSwapchainKHR swapchain;VK_OK(create(g_vk.dev,&swap,nullptr,&swapchain));uint32_t image_count=0;VK_OK(get_images(g_vk.dev,swapchain,&image_count,nullptr));std::vector<VkImage> images(image_count);VK_OK(get_images(g_vk.dev,swapchain,&image_count,images.data()));
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.queueFamilyIndex=g_vk.qfam;pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VK_OK(vkCreateCommandPool(g_vk.dev,&pool,nullptr,&g_vk.pool));VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};command.commandPool=g_vk.pool;command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command.commandBufferCount=1;VkCommandBuffer cb;VK_OK(vkAllocateCommandBuffers(g_vk.dev,&command,&cb));
    pixels(cb,g_vk.queue,swapchain,images,extent,acquire,present,wait_idle,legacy_r8);CHECK(!validation_errors);VK_OK(wait_idle(g_vk.dev));destroy(g_vk.dev,swapchain,nullptr);vkDestroyCommandPool(g_vk.dev,g_vk.pool,nullptr);
    auto instance_proxy=(PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(GetModuleHandleW(L"sl.interposer.dll"),"vkGetInstanceProcAddr");CHECK(instance_proxy);
    auto destroy_surface=(PFN_vkDestroySurfaceKHR)instance_proxy(g_vk.inst,"vkDestroySurfaceKHR");CHECK(destroy_surface);destroy_surface(g_vk.inst,g_vk.surface,nullptr);dlss_shutdown();vkDestroyDevice(g_vk.dev,nullptr);
    CHECK(!validation_errors);if(messenger){auto destroy_messenger=(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst,"vkDestroyDebugUtilsMessengerEXT");destroy_messenger(g_vk.inst,messenger,nullptr);}vkDestroyInstance(g_vk.inst,nullptr);SDL_DestroyWindow(window);SDL_Quit();return 0;
}
