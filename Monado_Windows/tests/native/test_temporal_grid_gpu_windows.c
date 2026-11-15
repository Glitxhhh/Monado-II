// Authored offscreen Vulkan oracle: depth-tested world and depth-disabled sky
// have the same normalized displacement; HUD stays fixed after the scene blit.
// No vendor SDK, game shader, game input, window or cache is used.
#include "../../runtime/nvn/vulkan/temporal_scene_policy.h"
#include <vulkan/vulkan.h>
#include <shaderc/shaderc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "temporal_hud_production.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define OK(x) do{VkResult r_=(x);if(r_!=VK_SUCCESS){fprintf(stderr,"FAIL %d: %s VkResult %d\n",__LINE__,#x,r_);exit(1);}}while(0)
static VkPhysicalDevice physical;static VkDevice device;static VkQueue queue;
static VkCommandBuffer command;static VkFence fence;
typedef struct Image{VkImage image;VkDeviceMemory memory;VkImageView view;uint32_t width,height;}Image;
static VkBuffer readback;static VkDeviceMemory readback_memory;static float *pixels;
static VkPipelineLayout layout;static VkPipeline world_pipeline,sky_pipeline,hud_pipeline;
static VkPipelineLayout fallback_layout;static VkDescriptorSetLayout fallback_set_layout;
static VkPipeline fallback_pipeline;static VkSampler sampler;static VkDescriptorSet fallback_set;
static const uint32_t color_size=100;
static uint32_t memory_type(uint32_t allowed,VkMemoryPropertyFlags flags){
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    for(uint32_t i=0;i<memory.memoryTypeCount;i++)if((allowed&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&flags)==flags)return i;
    CHECK(!"required native GPU memory type missing");return 0;
}
static Image image(uint32_t size,VkFormat format,VkImageUsageFlags usage,VkImageAspectFlags aspect){
    Image result={.width=size,.height=size};
    VkImageCreateInfo create={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=format,
        .extent={size,size,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage};
    OK(vkCreateImage(device,&create,NULL,&result.image));VkMemoryRequirements requirements;vkGetImageMemoryRequirements(device,result.image,&requirements);
    VkMemoryAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    OK(vkAllocateMemory(device,&allocate,NULL,&result.memory));OK(vkBindImageMemory(device,result.image,result.memory,0));
    VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=result.image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=format,.subresourceRange={aspect,0,1,0,1}};
    OK(vkCreateImageView(device,&view,NULL,&result.view));return result;
}
static void destroy_image(Image *image){vkDestroyImageView(device,image->view,NULL);vkDestroyImage(device,image->image,NULL);vkFreeMemory(device,image->memory,NULL);}
static void barrier(Image *image,VkImageAspectFlags aspect,VkImageLayout from,VkImageLayout to){
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,.oldLayout=from,.newLayout=to,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image->image,.subresourceRange={aspect,0,1,0,1}};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&b);
}
static VkShaderModule shader(const char *source,shaderc_shader_kind kind){
    shaderc_compiler_t compiler=shaderc_compiler_initialize();CHECK(compiler);
    shaderc_compile_options_t options=shaderc_compile_options_initialize();CHECK(options);
    shaderc_compile_options_set_target_env(options,shaderc_target_env_vulkan,shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t result=shaderc_compile_into_spv(compiler,source,strlen(source),kind,"authored-jitter-grid","main",options);
    if(shaderc_result_get_compilation_status(result)!=shaderc_compilation_status_success)fprintf(stderr,"shader: %s\n",shaderc_result_get_error_message(result));
    CHECK(shaderc_result_get_compilation_status(result)==shaderc_compilation_status_success);
    VkShaderModuleCreateInfo create={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=shaderc_result_get_length(result),.pCode=(const uint32_t*)shaderc_result_get_bytes(result)};
    VkShaderModule module;OK(vkCreateShaderModule(device,&create,NULL,&module));shaderc_result_release(result);shaderc_compile_options_release(options);shaderc_compiler_release(compiler);return module;
}
static VkPipeline pipeline(VkShaderModule vertex,VkShaderModule fragment,bool depth,bool depth_attachment,VkPipelineLayout selected_layout){
    VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo input={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.lineWidth=1,.cullMode=VK_CULL_MODE_NONE};
    VkPipelineMultisampleStateCreateInfo samples={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthTestEnable=depth,.depthWriteEnable=depth,.depthCompareOp=VK_COMPARE_OP_LESS};
    VkPipelineColorBlendAttachmentState attachment={.colorWriteMask=15};VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment};
    VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=states};
    VkFormat format=VK_FORMAT_R32G32B32A32_SFLOAT;VkPipelineRenderingCreateInfo rendering={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,.colorAttachmentCount=1,.pColorAttachmentFormats=&format,.depthAttachmentFormat=depth_attachment?VK_FORMAT_D32_SFLOAT:VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo create={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&rendering,.stageCount=2,.pStages=stages,
        .pVertexInputState=&input,.pInputAssemblyState=&assembly,.pViewportState=&viewport,.pRasterizationState=&raster,
        .pMultisampleState=&samples,.pDepthStencilState=&ds,.pColorBlendState=&blend,.pDynamicState=&dynamic,.layout=selected_layout};
    VkPipeline result;OK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&create,NULL,&result));return result;
}
static void render(Image *scene,Image *depth,Image *output,Image *corrected,float x,float y,const float correction[2],float *result){
    if(correction){
        VkDescriptorImageInfo inputs[3]={{sampler,scene->view,VK_IMAGE_LAYOUT_GENERAL},{sampler,output->view,VK_IMAGE_LAYOUT_GENERAL},{sampler,scene->view,VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[3];for(unsigned i=0;i<3;i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=fallback_set,
            .dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&inputs[i]};
        vkUpdateDescriptorSets(device,3,writes,0,NULL);
    }
    OK(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo start={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};OK(vkBeginCommandBuffer(command,&start));
    barrier(scene,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(depth,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=scene->view,.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingAttachmentInfo z={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=depth->view,.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.clearValue.depthStencil={1,0}};
    VkRenderingInfo draw={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{scene->width,scene->height}},.layerCount=1,.colorAttachmentCount=1,.pColorAttachments=&color,.pDepthAttachment=&z};
    vkCmdBeginRendering(command,&draw);VkViewport viewport={x,y,(float)scene->width,(float)scene->height,0,1};vkCmdSetViewport(command,0,1,&viewport);
    int hud=0;vkCmdPushConstants(command,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof hud,&hud);
    for(unsigned side=0;side<2;side++){
        VkRect2D scissor={{side?(int32_t)(scene->width/2):0,0},{scene->width/2,scene->height}};
        vkCmdSetScissor(command,0,1,&scissor);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,side?sky_pipeline:world_pipeline);vkCmdDraw(command,3,1,0,0);
    }
    vkCmdEndRendering(command);barrier(scene,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(output,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffsets={{0,0,0},{(int32_t)scene->width,(int32_t)scene->height,1}},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{0,0,0},{(int32_t)color_size,(int32_t)color_size,1}}};
    vkCmdBlitImage(command,scene->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
    barrier(output,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    // Unshifted HUD rendered after reconstruction; its color must stay identical.
    color.imageView=output->view;color.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;draw.renderArea.extent=(VkExtent2D){color_size,color_size};draw.pDepthAttachment=NULL;
    vkCmdBeginRendering(command,&draw);viewport=(VkViewport){0,0,(float)color_size,(float)color_size,0,1};vkCmdSetViewport(command,0,1,&viewport);
    VkRect2D hud_rect={{0,0},{color_size,10}};vkCmdSetScissor(command,0,1,&hud_rect);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,hud_pipeline);
    hud=1;vkCmdPushConstants(command,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof hud,&hud);vkCmdDraw(command,3,1,0,0);vkCmdEndRendering(command);
    Image *read=output;
    if(correction){
        barrier(scene,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
        barrier(output,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
        barrier(corrected,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        color.imageView=corrected->view;color.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        vkCmdBeginRendering(command,&draw);vkCmdSetViewport(command,0,1,&viewport);
        VkRect2D all={{0,0},{color_size,color_size}};vkCmdSetScissor(command,0,1,&all);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,fallback_pipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,fallback_layout,0,1,&fallback_set,0,NULL);
        float constants[8]={1.f/color_size,1.f/color_size,correction[0],correction[1]};
        vkCmdPushConstants(command,fallback_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof constants,constants);
        vkCmdDraw(command,3,1,0,0);vkCmdEndRendering(command);read=corrected;
    }
    barrier(read,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={color_size,color_size,1}};
    vkCmdCopyImageToBuffer(command,read->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
    OK(vkEndCommandBuffer(command));OK(vkResetFences(device,1,&fence));VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    OK(vkQueueSubmit(queue,1,&submit,fence));OK(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull));memcpy(result,pixels,color_size*color_size*16);
}
int main(void){
    VkApplicationInfo application={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Authored temporal jitter grid oracle",.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo create={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&application};VkInstance instance;OK(vkCreateInstance(&create,NULL,&instance));
    uint32_t count=0;OK(vkEnumeratePhysicalDevices(instance,&count,NULL));CHECK(count&&count<32);VkPhysicalDevice devices[32];OK(vkEnumeratePhysicalDevices(instance,&count,devices));physical=devices[0];
    for(uint32_t i=0;i<count;i++){VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(devices[i],&properties);if(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)physical=devices[i];}
    VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(physical,&properties);printf("GPU=%s driver=%u; synthetic pixels only, validation disabled\n",properties.deviceName,properties.driverVersion);
    CHECK(properties.limits.subTexelPrecisionBits>=8&&properties.limits.subTexelPrecisionBits<=30);
    VkFormatProperties format;vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R32G32B32A32_SFLOAT,&format);
    VkFormatFeatureFlags required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;
    CHECK((format.optimalTilingFeatures&required)==required);
    uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,NULL);CHECK(families<64);VkQueueFamilyProperties queues[64];vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,queues);uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<families;i++)if(queues[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=i;break;}CHECK(family!=UINT32_MAX);
    float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceVulkan13Features features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.dynamicRendering=true};VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&features,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    OK(vkCreateDevice(physical,&di,NULL,&device));vkGetDeviceQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};VkCommandPool pool;OK(vkCreateCommandPool(device,&cp,NULL,&pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};OK(vkAllocateCommandBuffers(device,&ca,&command));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};OK(vkCreateFence(device,&fi,NULL,&fence));
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=color_size*color_size*16,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};OK(vkCreateBuffer(device,&bc,NULL,&readback));VkMemoryRequirements requirements;vkGetBufferMemoryRequirements(device,readback,&requirements);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    OK(vkAllocateMemory(device,&ma,NULL,&readback_memory));OK(vkBindBufferMemory(device,readback,readback_memory,0));OK(vkMapMemory(device,readback_memory,0,VK_WHOLE_SIZE,0,(void**)&pixels));
    VkPushConstantRange range={VK_SHADER_STAGE_FRAGMENT_BIT,0,4};VkPipelineLayoutCreateInfo lc={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.pushConstantRangeCount=1,.pPushConstantRanges=&range};OK(vkCreatePipelineLayout(device,&lc,NULL,&layout));
    VkDescriptorSetLayoutBinding bindings[3];for(unsigned i=0;i<3;i++)bindings[i]=(VkDescriptorSetLayoutBinding){i,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo sl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=3,.pBindings=bindings};OK(vkCreateDescriptorSetLayout(device,&sl,NULL,&fallback_set_layout));
    range.size=32;lc.setLayoutCount=1;lc.pSetLayouts=&fallback_set_layout;OK(vkCreatePipelineLayout(device,&lc,NULL,&fallback_layout));
    VkDescriptorPoolSize pool_size={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3};VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&pool_size};
    VkDescriptorPool descriptor_pool;OK(vkCreateDescriptorPool(device,&dp,NULL,&descriptor_pool));VkDescriptorSetAllocateInfo ds={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&fallback_set_layout};OK(vkAllocateDescriptorSets(device,&ds,&fallback_set));
    VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_LINEAR,.minFilter=VK_FILTER_LINEAR,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=0};OK(vkCreateSampler(device,&si,NULL,&sampler));
    VkShaderModule vertex=shader("#version 450\nlayout(location=0)out vec2 uv;void main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);uv=p;gl_Position=vec4(p*2.-1.,.5,1.);}",shaderc_vertex_shader);
    VkShaderModule fragment=shader("#version 450\nlayout(location=0)in vec2 uv;layout(location=0)out vec4 c;layout(push_constant)uniform P{int hud;}p;void main(){c=p.hud!=0?vec4(.75,.25,0,1):vec4(uv,1,1);}",shaderc_fragment_shader);
    world_pipeline=pipeline(vertex,fragment,true,true,layout);sky_pipeline=pipeline(vertex,fragment,false,true,layout);hud_pipeline=pipeline(vertex,fragment,false,false,layout);
    VkShaderModule fallback=shader(production_hud_glsl,shaderc_fragment_shader);fallback_pipeline=pipeline(vertex,fallback,false,false,fallback_layout);
    Image output=image(color_size,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
    Image corrected=image(color_size,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
    float *baseline=malloc(color_size*color_size*16),*shifted=malloc(color_size*color_size*16);CHECK(baseline&&shifted);
    unsigned regressions=0;
    for(unsigned mode=0;mode<3;mode++){
        uint32_t size=mode?70:100;Image scene=image(size,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
        Image depth=image(size,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
        TmpJitterGrid grid={0};tmp_jitter_grid_verify(&grid,mode==2?128:100,mode==2?128:100);const float sample[2]={.25f,-.16666667f};tmp_jitter_grid_begin(&grid,sample);
        float actual[2],declared[2];tmp_jitter_grid_offset(&grid,size,size,actual);tmp_jitter_grid_offset(&grid,color_size,color_size,declared);
        render(&scene,&depth,&output,&corrected,0,0,NULL,baseline);render(&scene,&depth,&output,&corrected,actual[0],actual[1],NULL,shifted);
        for(unsigned side=0;side<2;side++){
            unsigned at=(50*color_size+(side?75:25))*4;
            CHECK(fabsf((baseline[at]-shifted[at])*color_size-declared[0])<.002f);
            CHECK(fabsf((baseline[at+1]-shifted[at+1])*color_size-declared[1])<.002f);
        }
        unsigned hud=5*color_size*4+50*4;CHECK(shifted[hud]==.75f&&shifted[hud+1]==.25f&&baseline[hud]==shifted[hud]);
        if(mode==1){render(&scene,&depth,&output,&corrected,sample[0],sample[1],NULL,shifted);unsigned at=(50*color_size+25)*4;
            CHECK(fabsf((baseline[at]-shifted[at])*color_size-declared[0])>.05f);regressions++;}
        // Actual runtime fallback/HUD shader, no SDK evaluation or current camera
        // is needed to remove the exact normalized shift already drawn.
        render(&scene,&depth,&output,&corrected,actual[0],actual[1],grid.uv,shifted);
        // Two linear filters may each quantize a source fraction by one device
        // subtexel. Express that documented hardware bound in final-color pixels.
        float tolerance=fmaxf(.002f,2.f*color_size/size/(1u<<properties.limits.subTexelPrecisionBits));
        for(unsigned side=0;side<2;side++){unsigned at=(50*color_size+(side?75:25))*4;
            printf("fallback grid %u side%u residual %.7f %.7f pixels; sampler bound %.7f\n",size,side,(baseline[at]-shifted[at])*color_size,(baseline[at+1]-shifted[at+1])*color_size,tolerance);
            CHECK(fabsf(baseline[at]-shifted[at])*color_size<tolerance&&fabsf(baseline[at+1]-shifted[at+1])*color_size<tolerance);}
        CHECK(shifted[hud]==.75f&&shifted[hud+1]==.25f);
        printf("grid %u -> %u drawn %.7f %.7f declared %.7f %.7f: world/sky displacement + fixed HUD PASS\n",size,color_size,actual[0],actual[1],declared[0],declared[1]);destroy_image(&scene);destroy_image(&depth);
    }
    CHECK(regressions==1);OK(vkDeviceWaitIdle(device));free(baseline);free(shifted);destroy_image(&output);destroy_image(&corrected);vkDestroyPipeline(device,world_pipeline,NULL);vkDestroyPipeline(device,sky_pipeline,NULL);vkDestroyPipeline(device,hud_pipeline,NULL);
    vkDestroyPipeline(device,fallback_pipeline,NULL);vkDestroyShaderModule(device,fallback,NULL);vkDestroySampler(device,sampler,NULL);vkDestroyDescriptorPool(device,descriptor_pool,NULL);
    vkDestroyPipelineLayout(device,fallback_layout,NULL);vkDestroyDescriptorSetLayout(device,fallback_set_layout,NULL);
    vkDestroyShaderModule(device,vertex,NULL);vkDestroyShaderModule(device,fragment,NULL);vkDestroyPipelineLayout(device,layout,NULL);vkUnmapMemory(device,readback_memory);vkDestroyBuffer(device,readback,NULL);vkFreeMemory(device,readback_memory,NULL);
    vkDestroyFence(device,fence,NULL);vkDestroyCommandPool(device,pool,NULL);vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
    puts("PASS actual GPU native/70%-DRS/changed-input grid oracle; old unscaled jitter counterexample detected");return 0;
}
