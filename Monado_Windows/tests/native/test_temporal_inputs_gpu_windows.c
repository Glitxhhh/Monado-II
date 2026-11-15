// Actual production tmp_prep.frag + negative-height set_viewport over authored
// perspective geometry. CPU oracles intersect/project world points independently.
// No game, SDK, window, profile or cache input is consumed.
#include "temporal_input_gpu_helpers.h"
static VkPipelineLayout scene_layout,input_layout;
static VkPipeline scene_pipeline,input_pipeline;
static VkDescriptorSetLayout input_set_layout;
static VkDescriptorSet input_set;
static struct {VkViewport vp;} ss;
enum {SS_VP=1};
static int g_debug_frame;
static bool ss_need(unsigned bit,bool same){(void)bit;(void)same;return true;}
static VkCommandBuffer vk_cmd(void){return command;}
#define LOG(...) fprintf(stderr,__VA_ARGS__)
#include "temporal_input_production.h"

typedef struct Camera {double x,y,z,yaw;} Camera;
typedef struct Scenario {
    const char *name;uint32_t width,height,scene_w,scene_h,verified_w,verified_h;
    float sample[2];bool history;unsigned negative;
} Scenario;
static const double near_z=.1,far_z=100.,fov_y=1.0471975511965976;
static const double plane_x=.1,plane_y=-.05,plane_z=4.;
static void multiply(const double a[16],const double b[16],double result[16]){
    double out[16]={0};for(unsigned r=0;r<4;r++)for(unsigned c=0;c<4;c++)for(unsigned k=0;k<4;k++)out[r*4+c]+=a[r*4+k]*b[k*4+c];memcpy(result,out,sizeof out);
}
static void matrices(Camera camera,double view[16],double inverse[16]){
    const double c=cos(camera.yaw),s=sin(camera.yaw);
    const double v[16]={c,0,-s,-c*camera.x+s*camera.z,0,1,0,-camera.y,s,0,c,-s*camera.x-c*camera.z,0,0,0,1};
    const double i[16]={c,0,s,camera.x,0,1,0,camera.y,-s,0,c,camera.z,0,0,0,1};memcpy(view,v,sizeof v);memcpy(inverse,i,sizeof i);
}
static void projection(double matrix[16],double inverse[16]){
    double b=1./tan(fov_y*.5),a=b/(16./9.),c=-(far_z+near_z)/(far_z-near_z),d=-2.*far_z*near_z/(far_z-near_z);
    const double p[16]={a,0,0,0,0,b,0,0,0,0,c,d,0,0,-1,0};
    const double ip[16]={1/a,0,0,0,0,1/b,0,0,0,0,0,-1,0,0,1/d,c/d};memcpy(matrix,p,sizeof p);memcpy(inverse,ip,sizeof ip);
}
static void column_floats(const double source[16],float out[16]){for(unsigned r=0;r<4;r++)for(unsigned c=0;c<4;c++)out[c*4+r]=(float)source[r*4+c];}
static void ray_plane(Camera camera,double u,double v,double point[3]){
    // Independent geometric oracle: ray in camera coordinates, rotate it into
    // world space, then intersect z + .1*x - .05*y + 4 == 0.
    double b=1./tan(fov_y*.5),a=b/(16./9.);
    double cx=(2*u-1)/a,cy=(1-2*v)/b,c=cos(camera.yaw),s=sin(camera.yaw);
    double direction[3]={c*cx-s,cy,-s*cx-c};
    double distance=-(camera.z+plane_x*camera.x+plane_y*camera.y+plane_z)/
        (direction[2]+plane_x*direction[0]+plane_y*direction[1]);
    point[0]=camera.x+distance*direction[0];point[1]=camera.y+distance*direction[1];point[2]=camera.z+distance*direction[2];
}
static void project(Camera camera,const double point[3],double out[3]){
    double x=point[0]-camera.x,y=point[1]-camera.y,z=point[2]-camera.z,c=cos(camera.yaw),s=sin(camera.yaw);
    double cx=c*x-s*z,cz=s*x+c*z,b=1./tan(fov_y*.5),a=b/(16./9.);
    out[0]=.5+.5*a*cx/-cz;out[1]=.5-.5*b*y/-cz;
    out[2]=.5+.5*((-(far_z+near_z)/(far_z-near_z))*cz-2*far_z*near_z/(far_z-near_z))/-cz;
}
static Image rectangle(uint32_t width,uint32_t height,VkFormat format,VkImageUsageFlags usage,VkImageAspectFlags aspect){
    Image result={.width=width,.height=height};VkImageCreateInfo info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=format,
        .extent={width,height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage};
    OK(vkCreateImage(device,&info,NULL,&result.image));VkMemoryRequirements need;vkGetImageMemoryRequirements(device,result.image,&need);
    VkMemoryAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=need.size,.memoryTypeIndex=memory_type(need.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    OK(vkAllocateMemory(device,&allocate,NULL,&result.memory));OK(vkBindImageMemory(device,result.image,result.memory,0));
    VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=result.image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=format,.subresourceRange={aspect,0,1,0,1}};
    OK(vkCreateImageView(device,&view,NULL,&result.view));return result;
}
static VkPipeline input_graphics(VkShaderModule vertex,VkShaderModule fragment){
    VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.lineWidth=1,.cullMode=VK_CULL_MODE_NONE};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState attachments[4]={{.colorWriteMask=15},{.colorWriteMask=3},{.colorWriteMask=1},{.colorWriteMask=1}};
    VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=4,.pAttachments=attachments};
    VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=dynamic_states};
    VkFormat formats[]={VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16_SFLOAT};
    VkPipelineRenderingCreateInfo rendering={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,.colorAttachmentCount=4,.pColorAttachmentFormats=formats};
    VkGraphicsPipelineCreateInfo info={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&rendering,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&rs,.pMultisampleState=&ms,
        .pDepthStencilState=&ds,.pColorBlendState=&blend,.pDynamicState=&dynamic,.layout=input_layout};
    VkPipeline result;OK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,NULL,&result));return result;
}
static void run_scenario(const Scenario *test,Camera prior,Camera current){
    Image scene=rectangle(test->scene_w,test->scene_h,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
    Image depth=rectangle(test->scene_w,test->scene_h,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    Image final=rectangle(test->width,test->height,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
    VkFormat formats[]={VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16_SFLOAT};Image outputs[4];
    for(unsigned i=0;i<4;i++)outputs[i]=rectangle(test->width,test->height,formats[i],VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
    size_t count=(size_t)test->width*test->height,offsets[]={0,count*16,count*24},bytes=count*28;VkBuffer buffer;VkDeviceMemory memory;void *mapped;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};OK(vkCreateBuffer(device,&bi,NULL,&buffer));
    VkMemoryRequirements need;vkGetBufferMemoryRequirements(device,buffer,&need);VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=need.size,.memoryTypeIndex=memory_type(need.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    OK(vkAllocateMemory(device,&ma,NULL,&memory));OK(vkBindBufferMemory(device,buffer,memory,0));OK(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,&mapped));
    VkDescriptorImageInfo images[4]={{sampler,final.view,VK_IMAGE_LAYOUT_GENERAL},{sampler,depth.view,VK_IMAGE_LAYOUT_GENERAL},{sampler,final.view,VK_IMAGE_LAYOUT_GENERAL},{sampler,final.view,VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[4];for(unsigned i=0;i<4;i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=input_set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[i]};vkUpdateDescriptorSets(device,4,writes,0,NULL);
    TmpJitterGrid grid={0};tmp_jitter_grid_verify(&grid,test->verified_w,test->verified_h);tmp_jitter_grid_begin(&grid,test->sample);float draw_jitter[2],declared[2];
    tmp_jitter_grid_offset(&grid,test->scene_w,test->scene_h,draw_jitter);tmp_jitter_grid_offset(&grid,test->width,test->height,declared);
    if(test->negative==1){draw_jitter[0]=test->sample[0];draw_jitter[1]=test->sample[1];}
    if(test->negative==2)draw_jitter[1]=-draw_jitter[1];
    double p[16],ip[16],cv[16],ci[16],pv[16],pi[16],clip[16],reprojection[16],temporary[16];projection(p,ip);matrices(current,cv,ci);matrices(prior,pv,pi);multiply(p,cv,clip);
    if(test->negative==3){multiply(p,cv,reprojection);multiply(reprojection,pi,temporary);}else{multiply(p,pv,reprojection);multiply(reprojection,ci,temporary);}multiply(temporary,ip,reprojection);
    float scene_constants[16];column_floats(clip,scene_constants);float constants[28]={0};column_floats(reprojection,constants);
    constants[16]=declared[0];constants[17]=declared[1];constants[18]=(float)test->width;constants[19]=(float)test->height;constants[20]=1;constants[21]=test->history?1:0;
    OK(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};OK(vkBeginCommandBuffer(command,&begin));
    barrier(&scene,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);barrier(&depth,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=scene.view,.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingAttachmentInfo z={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=depth.view,.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.clearValue.depthStencil={1,0}};
    VkRenderingInfo rendering={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{test->scene_w,test->scene_h}},.layerCount=1,.colorAttachmentCount=1,.pColorAttachments=&color,.pDepthAttachment=&z};
    vkCmdBeginRendering(command,&rendering);set_viewport(draw_jitter[0],draw_jitter[1],(float)test->scene_w,(float)test->scene_h,0,1);
    VkRect2D scissor={{0,0},{test->scene_w,test->scene_h}};vkCmdSetScissor(command,0,1,&scissor);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,scene_pipeline);
    vkCmdPushConstants(command,scene_layout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof scene_constants,scene_constants);vkCmdDraw(command,6,1,0,0);vkCmdEndRendering(command);
    barrier(&scene,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);barrier(&final,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffsets={{0,0,0},{(int32_t)test->scene_w,(int32_t)test->scene_h,1}},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{0,0,0},{(int32_t)test->width,(int32_t)test->height,1}}};
    vkCmdBlitImage(command,scene.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,final.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
    barrier(&final,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);barrier(&depth,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    VkRenderingAttachmentInfo attachments[4];for(unsigned i=0;i<4;i++){barrier(&outputs[i],VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
        attachments[i]=(VkRenderingAttachmentInfo){.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=outputs[i].view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL,.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};}
    rendering.renderArea.extent=(VkExtent2D){test->width,test->height};rendering.colorAttachmentCount=4;rendering.pColorAttachments=attachments;rendering.pDepthAttachment=NULL;
    vkCmdBeginRendering(command,&rendering);VkViewport viewport={0,0,(float)test->width,(float)test->height,0,1};vkCmdSetViewport(command,0,1,&viewport);
    scissor.extent=(VkExtent2D){test->width,test->height};vkCmdSetScissor(command,0,1,&scissor);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,input_pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,input_layout,0,1,&input_set,0,NULL);vkCmdPushConstants(command,input_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof constants,constants);
    vkCmdDraw(command,3,1,0,0);vkCmdEndRendering(command);
    for(unsigned i=0;i<3;i++){barrier(&outputs[i],VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy={.bufferOffset=offsets[i],.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={test->width,test->height,1}};vkCmdCopyImageToBuffer(command,outputs[i].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);}
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
    OK(vkEndCommandBuffer(command));OK(vkResetFences(device,1,&fence));VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};OK(vkQueueSubmit(queue,1,&submit,fence));OK(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull));
    const float *colors=mapped,*motion=(const float*)((const unsigned char*)mapped+offsets[1]),*depths=(const float*)((const unsigned char*)mapped+offsets[2]);
    double max_color=0,max_depth=0,max_motion=0;unsigned samples=0,color_bad=0,motion_bad=0;
    for(unsigned y=16;y+16<test->height;y+=15)for(unsigned x=16;x+16<test->width;x+=19){
        size_t at=(size_t)y*test->width+x;double decoded[3]={(colors[at*4]-.5)/.03,(colors[at*4+1]-.5)/.03,0};decoded[2]=-plane_z-plane_x*decoded[0]-plane_y*decoded[1];double screen[3];project(current,decoded,screen);
        double color_error=fmax(fabs((screen[0]+grid.uv[0])*test->width-(x+.5)),fabs((screen[1]+grid.uv[1])*test->height-(y+.5)));
        if(color_error>max_color)max_color=color_error;if(color_error>.025)color_bad++;
        unsigned dx=(unsigned)((uint64_t)x*test->scene_w/test->width),dy=(unsigned)((uint64_t)y*test->scene_h/test->height);double point[3],sample[3];
        ray_plane(current,(dx+.5-draw_jitter[0])/test->scene_w,(dy+.5-draw_jitter[1])/test->scene_h,point);project(current,point,sample);
        double depth_error=fabs(depths[at]-sample[2]);if(depth_error>max_depth)max_depth=depth_error;CHECK(isfinite(depths[at])&&depth_error<2e-6);
        double u=(x+.5-declared[0])/test->width,v=(y+.5-declared[1])/test->height,previous_screen[3];ray_plane(current,u,v,point);project(prior,point,previous_screen);
        double expected_x=test->history?previous_screen[0]-u:0,expected_y=test->history?previous_screen[1]-v:0;
        double motion_error=fmax(fabs(motion[at*2]-expected_x)*test->width,fabs(motion[at*2+1]-expected_y)*test->height);
        CHECK(isfinite(motion[at*2])&&isfinite(motion[at*2+1]));if(motion_error>max_motion)max_motion=motion_error;if(motion_error>.04)motion_bad++;samples++;
    }
    printf("%s: %ux%u -> %ux%u negative%u samples%u max color-coordinate %.7fpx depth %.9g motion %.7fpx\n",test->name,test->scene_w,test->scene_h,test->width,test->height,test->negative,samples,max_color,max_depth,max_motion);fflush(stdout);
    CHECK(samples>=20);
    if(test->negative==1||test->negative==2)CHECK(color_bad>=samples/2);else CHECK(!color_bad);
    if(test->negative==3)CHECK(motion_bad>=samples/2);else if(!test->negative)CHECK(!motion_bad);
    vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,NULL);vkFreeMemory(device,memory,NULL);
    for(unsigned i=0;i<4;i++)destroy_image(&outputs[i]);destroy_image(&scene);destroy_image(&depth);destroy_image(&final);
}
int main(void){
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Authored production temporal input geometry oracle",.apiVersion=VK_API_VERSION_1_3};VkInstanceCreateInfo instance_info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VkInstance instance;OK(vkCreateInstance(&instance_info,NULL,&instance));
    uint32_t count=0;OK(vkEnumeratePhysicalDevices(instance,&count,NULL));CHECK(count&&count<=32);VkPhysicalDevice devices[32];OK(vkEnumeratePhysicalDevices(instance,&count,devices));physical=devices[0];
    for(unsigned i=0;i<count;i++){VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(devices[i],&properties);if(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)physical=devices[i];}
    VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(physical,&properties);printf("GPU=%s driver=%u; actual production prep shader, authored pixels only; validation not enabled/no validation pass\n",properties.deviceName,properties.driverVersion);
    VkFormatProperties support;vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R32G32B32A32_SFLOAT,&support);VkFormatFeatureFlags required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;CHECK((support.optimalTilingFeatures&required)==required);
    uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,NULL);CHECK(families&&families<=64);VkQueueFamilyProperties queues[64];vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,queues);uint32_t family=UINT32_MAX;for(unsigned i=0;i<families;i++)if(queues[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=i;break;}CHECK(family!=UINT32_MAX);
    float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};VkPhysicalDeviceVulkan13Features features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.dynamicRendering=true};VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&features,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};OK(vkCreateDevice(physical,&di,NULL,&device));vkGetDeviceQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};VkCommandPool pool;OK(vkCreateCommandPool(device,&pi,NULL,&pool));VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};OK(vkAllocateCommandBuffers(device,&ca,&command));VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};OK(vkCreateFence(device,&fi,NULL,&fence));
    VkPushConstantRange range={VK_SHADER_STAGE_VERTEX_BIT,0,64};VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.pushConstantRangeCount=1,.pPushConstantRanges=&range};OK(vkCreatePipelineLayout(device,&li,NULL,&scene_layout));
    VkDescriptorSetLayoutBinding bindings[4];for(unsigned i=0;i<4;i++)bindings[i]=(VkDescriptorSetLayoutBinding){i,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};VkDescriptorSetLayoutCreateInfo si={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=4,.pBindings=bindings};OK(vkCreateDescriptorSetLayout(device,&si,NULL,&input_set_layout));
    range=(VkPushConstantRange){VK_SHADER_STAGE_FRAGMENT_BIT,0,112};li.setLayoutCount=1;li.pSetLayouts=&input_set_layout;OK(vkCreatePipelineLayout(device,&li,NULL,&input_layout));
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4};VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};VkDescriptorPool descriptor_pool;OK(vkCreateDescriptorPool(device,&dpi,NULL,&descriptor_pool));VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&input_set_layout};OK(vkAllocateDescriptorSets(device,&da,&input_set));
    VkSamplerCreateInfo sampler_info={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};OK(vkCreateSampler(device,&sampler_info,NULL,&sampler));
    VkShaderModule scene_vertex=shader("#version 450\nlayout(push_constant)uniform C{mat4 clip;}pc;layout(location=0)out vec3 world;void main(){vec2 p[6]=vec2[](vec2(-10,-10),vec2(10,-10),vec2(-10,10),vec2(-10,10),vec2(10,-10),vec2(10,10));vec2 xy=p[gl_VertexIndex];world=vec3(xy,-4.-.1*xy.x+.05*xy.y);vec4 q=pc.clip*vec4(world,1);q.z=(q.z+q.w)*.5;gl_Position=q;}",shaderc_vertex_shader);
    VkShaderModule scene_fragment=shader("#version 450\nlayout(location=0)in vec3 world;layout(location=0)out vec4 o;void main(){o=vec4(.5+.03*world.xy,.2,1.);}",shaderc_fragment_shader);
    VkShaderModule post_vertex=shader(production_post_glsl,shaderc_vertex_shader),prep_fragment=shader(production_prep_glsl,shaderc_fragment_shader);
    scene_pipeline=pipeline(scene_vertex,scene_fragment,true,true,scene_layout);input_pipeline=input_graphics(post_vertex,prep_fragment);
    const Scenario scenarios[]={
        {"native-zero-jitter",160,90,160,90,160,90,{0,0},true,0},
        {"native-Halton",160,90,160,90,160,90,{.25f,-.16666667f},true,0},
        {"DRS70-Halton",160,90,112,63,160,90,{.25f,-.16666667f},true,0},
        {"changed-input-Halton",192,108,134,76,160,90,{-.375f,.16666667f},true,0},
        {"reset-without-history",160,90,112,63,160,90,{.125f,-.3888889f},false,0},
        {"negative-old-grid",160,90,112,63,160,90,{.25f,-.16666667f},true,1},
        {"negative-jitter-Y",160,90,112,63,160,90,{.25f,-.16666667f},true,2},
        {"negative-reprojection",160,90,112,63,160,90,{.25f,-.16666667f},true,3}};
    Camera previous={-.12,.1,0,.01},current={.18,-.08,0,-.015};for(unsigned i=0;i<sizeof scenarios/sizeof *scenarios;i++)run_scenario(&scenarios[i],previous,current);
    OK(vkDeviceWaitIdle(device));vkDestroyPipeline(device,scene_pipeline,NULL);vkDestroyPipeline(device,input_pipeline,NULL);vkDestroyShaderModule(device,scene_vertex,NULL);vkDestroyShaderModule(device,scene_fragment,NULL);vkDestroyShaderModule(device,post_vertex,NULL);vkDestroyShaderModule(device,prep_fragment,NULL);
    vkDestroySampler(device,sampler,NULL);vkDestroyDescriptorPool(device,descriptor_pool,NULL);vkDestroyDescriptorSetLayout(device,input_set_layout,NULL);vkDestroyPipelineLayout(device,scene_layout,NULL);vkDestroyPipelineLayout(device,input_layout,NULL);vkDestroyFence(device,fence,NULL);vkDestroyCommandPool(device,pool,NULL);vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
    puts("PASS actual production temporal inputs: perspective camera-motion/depth/jitter Y/DRS/changed-input/reset; old-grid/wrong-Y/reprojection controls detected");return 0;
}
