// [XCDE] WINPARITY-105: authored native Vulkan feedback/copy pixel oracle.
// No game code, shaders, windows, saves, vendor SDKs or performance claims.
#include <vulkan/vulkan.h>
#include <shaderc/shaderc.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "nvn/vulkan/feedback_device.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
#define VK_OK(x) do { VkResult status=(x); if(status!=VK_SUCCESS){fprintf(stderr,"Vulkan %d at %s:%d\n",status,__FILE__,__LINE__);abort();} } while(0)
static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static VkCommandPool pool;
static VkCommandBuffer command;
static uint32_t family;
static bool feedback_enabled;
static unsigned validation_errors;
static VkDebugUtilsMessengerEXT messenger;
static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT *data,void*) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT){validation_errors++;fprintf(stderr,"VALIDATION %s\n",data->pMessage);}
    return VK_FALSE;
}
static uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties properties;vkGetPhysicalDeviceMemoryProperties(physical,&properties);
    for(uint32_t i=0;i<properties.memoryTypeCount;i++)if((bits&(1u<<i))&&(properties.memoryTypes[i].propertyFlags&required)==required)return i;
    CHECK(false);return 0;
}
static void begin() {
    VK_OK(vkResetCommandPool(device,pool,0));
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};info.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_OK(vkBeginCommandBuffer(command,&info));
}
static void finish() {
    VK_OK(vkEndCommandBuffer(command));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
    VK_OK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));VK_OK(vkQueueWaitIdle(queue));
}
static void barrier() {
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};memory.srcStageMask=memory.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    memory.srcAccessMask=memory.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.memoryBarrierCount=1;dependency.pMemoryBarriers=&memory;
    vkCmdPipelineBarrier2(command,&dependency);
}
struct Buffer {
    VkBuffer buffer{};VkDeviceMemory memory{};uint8_t *host{};
    Buffer() {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=512;info.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VK_OK(vkCreateBuffer(device,&info,nullptr,&buffer));VkMemoryRequirements requirements;vkGetBufferMemoryRequirements(device,buffer,&requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;
        allocation.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_OK(vkAllocateMemory(device,&allocation,nullptr,&memory));VK_OK(vkBindBufferMemory(device,buffer,memory,0));
        void *mapping;VK_OK(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,&mapping));host=(uint8_t*)mapping;
    }
    ~Buffer(){vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,memory,nullptr);}
};
struct Image {
    VkImage image{};VkDeviceMemory memory{};VkImageView view{};
    Image(bool native_feedback=false) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=VK_IMAGE_TYPE_2D;info.format=VK_FORMAT_R8G8B8A8_UNORM;
        info.extent={8,8,1};info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if(native_feedback)info.usage|=VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
        VK_OK(vkCreateImage(device,&info,nullptr,&image));VkMemoryRequirements requirements;vkGetImageMemoryRequirements(device,image,&requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;
        allocation.memoryTypeIndex=memory_type(requirements.memoryTypeBits,0);VK_OK(vkAllocateMemory(device,&allocation,nullptr,&memory));VK_OK(vkBindImageMemory(device,image,memory,0));
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=info.format;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VK_OK(vkCreateImageView(device,&v,nullptr,&view));begin();transition(VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);finish();
    }
    void transition(VkImageLayout old_layout,VkImageLayout next_layout) const {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};b.srcStageMask=b.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask=b.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;b.oldLayout=old_layout;b.newLayout=next_layout;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};d.imageMemoryBarrierCount=1;d.pImageMemoryBarriers=&b;vkCmdPipelineBarrier2(command,&d);
    }
    ~Image(){vkDestroyImageView(device,view,nullptr);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,memory,nullptr);}
};
static VkShaderModule shader(const char *source,shaderc_shader_kind kind) {
    shaderc_compiler_t compiler=shaderc_compiler_initialize();CHECK(compiler);
    shaderc_compile_options_t options=shaderc_compile_options_initialize();CHECK(options);
    shaderc_compile_options_set_target_env(options,shaderc_target_env_vulkan,shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t result=shaderc_compile_into_spv(compiler,source,strlen(source),kind,"authored-feedback","main",options);CHECK(result);
    if(shaderc_result_get_compilation_status(result)!=shaderc_compilation_status_success)fprintf(stderr,"%s",shaderc_result_get_error_message(result));
    CHECK(shaderc_result_get_compilation_status(result)==shaderc_compilation_status_success);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=shaderc_result_get_length(result);info.pCode=(const uint32_t*)shaderc_result_get_bytes(result);
    VkShaderModule module;VK_OK(vkCreateShaderModule(device,&info,nullptr,&module));shaderc_result_release(result);shaderc_compile_options_release(options);shaderc_compiler_release(compiler);return module;
}
struct Draw {
    VkDescriptorSetLayout descriptors{};VkPipelineLayout layout{};VkPipeline pipeline{};VkSampler sampler{};VkDescriptorPool descriptor_pool{};VkDescriptorSet set{};
    bool native;
    Draw(bool use_native,const Image &sample):native(use_native) {
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo d{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};d.bindingCount=1;d.pBindings=&binding;VK_OK(vkCreateDescriptorSetLayout(device,&d,nullptr,&descriptors));
        VkPipelineLayoutCreateInfo l{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};l.setLayoutCount=1;l.pSetLayouts=&descriptors;VK_OK(vkCreatePipelineLayout(device,&l,nullptr,&layout));
        VkShaderModule vertex=shader("#version 450\nvec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));void main(){gl_Position=vec4(p[gl_VertexIndex],0,1);}",shaderc_vertex_shader);
        VkShaderModule fragment=shader("#version 450\nlayout(set=0,binding=0)uniform sampler2D source;layout(location=1)out vec4 color;void main(){color=texelFetch(source,ivec2(gl_FragCoord.xy),0);}",shaderc_fragment_shader);
        VkPipelineShaderStageCreateInfo stages[2]{};for(unsigned i=0;i<2;i++){stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=i?fragment:vertex;stages[i].pName="main";}
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0,0,8,8,0,1};VkRect2D scissor{{0,0},{8,8}};VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState masks[2]{};masks[1].colorWriteMask=15;VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=2;blend.pAttachments=masks;
        VkFormat formats[2]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM};VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=2;rendering.pColorAttachmentFormats=formats;
        VkGraphicsPipelineCreateInfo p{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};p.pNext=&rendering;p.stageCount=2;p.pStages=stages;p.pVertexInputState=&vi;p.pInputAssemblyState=&ia;p.pViewportState=&vp;p.pRasterizationState=&rs;p.pMultisampleState=&ms;p.pColorBlendState=&blend;p.layout=layout;
        if(native)p.flags=VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;VK_OK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&p,nullptr,&pipeline));
        vkDestroyShaderModule(device,vertex,nullptr);vkDestroyShaderModule(device,fragment,nullptr);
        VkSamplerCreateInfo sm{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sm.magFilter=sm.minFilter=VK_FILTER_NEAREST;sm.addressModeU=sm.addressModeV=sm.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_OK(vkCreateSampler(device,&sm,nullptr,&sampler));VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=1;dp.poolSizeCount=1;dp.pPoolSizes=&size;VK_OK(vkCreateDescriptorPool(device,&dp,nullptr,&descriptor_pool));
        VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};a.descriptorPool=descriptor_pool;a.descriptorSetCount=1;a.pSetLayouts=&descriptors;VK_OK(vkAllocateDescriptorSets(device,&a,&set));
        VkDescriptorImageInfo sampled{sampler,sample.view,native?VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT:VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&sampled;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    }
    void record(const Image &input,const Image &output,bool load_clear) {
        VkImageLayout feedback_layout=native?VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT:VK_IMAGE_LAYOUT_GENERAL;
        if(native)input.transition(VK_IMAGE_LAYOUT_GENERAL,feedback_layout);
        VkRenderingAttachmentInfo attachments[2]{};for(unsigned i=0;i<2;i++){attachments[i].sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;attachments[i].imageView=i?output.view:input.view;attachments[i].imageLayout=i?VK_IMAGE_LAYOUT_GENERAL:feedback_layout;attachments[i].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachments[i].storeOp=VK_ATTACHMENT_STORE_OP_STORE;}
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};info.renderArea={{0,0},{8,8}};info.layerCount=1;info.colorAttachmentCount=2;info.pColorAttachments=attachments;
        if(native&&load_clear){
            attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachments[0].clearValue.color.float32[0]=.2f;attachments[0].clearValue.color.float32[1]=.4f;attachments[0].clearValue.color.float32[2]=.6f;attachments[0].clearValue.color.float32[3]=1;
            vkCmdBeginRendering(command,&info);vkCmdEndRendering(command);
            // Production ends load clears before enabling reads in the next pass.
            input.transition(feedback_layout,VK_IMAGE_LAYOUT_GENERAL);input.transition(VK_IMAGE_LAYOUT_GENERAL,feedback_layout);attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
        }
        vkCmdBeginRendering(command,&info);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);vkCmdDraw(command,3,1,0,0);vkCmdEndRendering(command);
        if(native)input.transition(feedback_layout,VK_IMAGE_LAYOUT_GENERAL);
    }
    ~Draw(){vkDestroyDescriptorPool(device,descriptor_pool,nullptr);vkDestroySampler(device,sampler,nullptr);vkDestroyPipeline(device,pipeline,nullptr);vkDestroyPipelineLayout(device,layout,nullptr);vkDestroyDescriptorSetLayout(device,descriptors,nullptr);}
};
static void oracle() {
    Image input(feedback_enabled),snapshot,output;Buffer staging;
    Draw copied(false,snapshot);Draw *native=feedback_enabled?new Draw(true,input):nullptr;
    unsigned cases=0;
    for(unsigned frame=0;frame<32;frame++)for(unsigned mode=0;mode<(native?2u:1u);mode++) {
        bool clear=(frame%3)==0;uint8_t expected[256];for(unsigned i=0;i<256;i++)expected[i]=(uint8_t)(frame*17+i*13);
        memcpy(staging.host,expected,256);memset(staging.host+256,0,256);begin();barrier();
        VkBufferImageCopy upload{};upload.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};upload.imageExtent={8,8,1};
        vkCmdCopyBufferToImage(command,staging.buffer,input.image,VK_IMAGE_LAYOUT_GENERAL,1,&upload);barrier();
        if(clear)for(unsigned i=0;i<64;i++){expected[i*4]=51;expected[i*4+1]=102;expected[i*4+2]=153;expected[i*4+3]=255;}
        if(!mode) {
            if(clear){VkClearColorValue color{};color.float32[0]=.2f;color.float32[1]=.4f;color.float32[2]=.6f;color.float32[3]=1;VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(command,input.image,VK_IMAGE_LAYOUT_GENERAL,&color,1,&range);barrier();}
            VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.extent={8,8,1};vkCmdCopyImage(command,input.image,VK_IMAGE_LAYOUT_GENERAL,snapshot.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);barrier();copied.record(input,output,false);
        } else native->record(input,output,clear);
        barrier();VkBufferImageCopy readback{};readback.bufferOffset=256;readback.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};readback.imageExtent={8,8,1};vkCmdCopyImageToBuffer(command,output.image,VK_IMAGE_LAYOUT_GENERAL,staging.buffer,1,&readback);barrier();finish();
        for(unsigned i=0;i<256;i++)if(staging.host[256+i]!=expected[i]){fprintf(stderr,"pixel mismatch frame%u native%u byte%u got%u expected%u\n",frame,mode,i,staging.host[256+i],expected[i]);CHECK(false);}cases++;
    }
    delete native;printf("PASS authored Vulkan feedback: %u exact pixel readbacks, changing inputs, snapshot/native equivalence, clear-before-sample\n",cases);
}
static bool has_extension(const std::vector<VkExtensionProperties>&list,const char *name){for(auto&e:list)if(!strcmp(e.extensionName,name))return true;return false;}
int main() {
    uint32_t nl=0;VK_OK(vkEnumerateInstanceLayerProperties(&nl,nullptr));std::vector<VkLayerProperties> layers(nl);VK_OK(vkEnumerateInstanceLayerProperties(&nl,layers.data()));bool validate=false;for(auto&l:layers)validate|=!strcmp(l.layerName,"VK_LAYER_KHRONOS_validation");
    const char *layer="VK_LAYER_KHRONOS_validation",*extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Monado authored feedback oracle";app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;if(validate){ci.enabledLayerCount=ci.enabledExtensionCount=1;ci.ppEnabledLayerNames=&layer;ci.ppEnabledExtensionNames=&extension;}
    VK_OK(vkCreateInstance(&ci,nullptr,&instance));
    if(validate){auto create=(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT");CHECK(create);VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};m.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;m.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;m.pfnUserCallback=validation;VK_OK(create(instance,&m,nullptr,&messenger));}
    puts(validate?"Khronos validation enabled":"Khronos validation unavailable; no validation-layer pass claimed");
    uint32_t n=0;VK_OK(vkEnumeratePhysicalDevices(instance,&n,nullptr));CHECK(n);std::vector<VkPhysicalDevice> devices(n);VK_OK(vkEnumeratePhysicalDevices(instance,&n,devices.data()));
    for(auto d:devices){VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(d,&p);if(p.apiVersion<VK_API_VERSION_1_3)continue;if(!physical||p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)physical=d;if(p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)break;}CHECK(physical);
    VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(physical,&p);printf("GPU %s driver%u API%u.%u.%u\n",p.deviceName,p.driverVersion,VK_API_VERSION_MAJOR(p.apiVersion),VK_API_VERSION_MINOR(p.apiVersion),VK_API_VERSION_PATCH(p.apiVersion));
    uint32_t nq=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&nq,nullptr);std::vector<VkQueueFamilyProperties> queues(nq);vkGetPhysicalDeviceQueueFamilyProperties(physical,&nq,queues.data());family=UINT32_MAX;for(unsigned i=0;i<nq;i++)if(queues[i].queueCount&&(queues[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){family=i;break;}CHECK(family!=UINT32_MAX);
    uint32_t ne=0;VK_OK(vkEnumerateDeviceExtensionProperties(physical,nullptr,&ne,nullptr));std::vector<VkExtensionProperties> extensions(ne);VK_OK(vkEnumerateDeviceExtensionProperties(physical,nullptr,&ne,extensions.data()));
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT feedback;feedback_enabled=monado_feedback_device_features(physical,has_extension(extensions,VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME),&feedback);
    VkPhysicalDeviceVulkan13Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};features.dynamicRendering=features.synchronization2=VK_TRUE;if(feedback_enabled)features.pNext=&feedback;
    float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkPhysicalDeviceFeatures available;vkGetPhysicalDeviceFeatures(physical,&available);CHECK(available.independentBlend);
    VkPhysicalDeviceFeatures core{};core.independentBlend=VK_TRUE;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.pNext=&features;di.pEnabledFeatures=&core;di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;const char *feedback_extension=VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME;if(feedback_enabled){di.enabledExtensionCount=1;di.ppEnabledExtensionNames=&feedback_extension;}
    VK_OK(vkCreateDevice(physical,&di,nullptr,&device));vkGetDeviceQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=family;pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VK_OK(vkCreateCommandPool(device,&pi,nullptr,&pool));VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};a.commandPool=pool;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;VK_OK(vkAllocateCommandBuffers(device,&a,&command));
    puts(feedback_enabled?"Production capability query enabled native feedback":"SKIP native feedback unavailable; snapshot GPU oracle remains required");
    oracle();CHECK(!validation_errors);VK_OK(vkDeviceWaitIdle(device));vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);
    if(messenger){auto destroy=(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT");destroy(instance,messenger,nullptr);}vkDestroyInstance(instance,nullptr);return 0;
}
