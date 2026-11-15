// [XCDE] LINUX-103: offscreen execution of the production authored encoder.
// Included after our Vulkan fixture helpers; no window or game dependencies.
struct TemporalEncoder {
    VkRenderPass pass{};VkFramebuffer framebuffer{};VkDescriptorSetLayout set_layout{};
    VkPipelineLayout layout{};VkPipeline pipeline{};VkDescriptorPool descriptors{};VkDescriptorSet set{};VkSampler sampler{};
    uint32_t width,height;
    static VkShaderModule shader(const std::string&path){
        FILE*f=fopen(path.c_str(),"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long size=ftell(f);CHECK(size>20&&size%4==0);
        rewind(f);std::vector<uint32_t>words(size/4);CHECK(fread(words.data(),1,size,f)==size);CHECK(!fclose(f));
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=size;info.pCode=words.data();
        VkShaderModule m{};VK_OK(vkCreateShaderModule(device,&info,nullptr,&m));return m;
    }
    TemporalEncoder(const Image&input,const Image&output,const char*shader_root):width(output.width),height(output.height){
        VkAttachmentDescription attachment{};attachment.format=output.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_GENERAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&attachment;rp.subpassCount=1;rp.pSubpasses=&sub;
        VK_OK(vkCreateRenderPass(device,&rp,nullptr,&pass));
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=pass;fb.attachmentCount=1;fb.pAttachments=&output.view;
        fb.width=width;fb.height=height;fb.layers=1;VK_OK(vkCreateFramebuffer(device,&fb,nullptr,&framebuffer));
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=1;li.pBindings=&binding;
        VK_OK(vkCreateDescriptorSetLayout(device,&li,nullptr,&set_layout));
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,32};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount=1;pl.pSetLayouts=&set_layout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;VK_OK(vkCreatePipelineLayout(device,&pl,nullptr,&layout));
        auto vs=shader(std::string(shader_root)+"/post.vert.spv"),fs=shader(std::string(shader_root)+"/tmp_srgb.frag.spv");
        VkPipelineShaderStageCreateInfo stages[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vs,"main",nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,fs,"main",nullptr}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0,0,(float)width,(float)height,0,1};VkRect2D scissor{{0,0},{width,height}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=1;vp.pViewports=&viewport;vp.scissorCount=1;vp.pScissors=&scissor;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&blend;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;
        pi.pInputAssemblyState=&ia;pi.pViewportState=&vp;pi.pRasterizationState=&rs;pi.pMultisampleState=&ms;pi.pColorBlendState=&cb;pi.layout=layout;pi.renderPass=pass;
        VK_OK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline));vkDestroyShaderModule(device,vs,nullptr);vkDestroyShaderModule(device,fs,nullptr);
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets=1;pool_info.poolSizeCount=1;pool_info.pPoolSizes=&pool_size;VK_OK(vkCreateDescriptorPool(device,&pool_info,nullptr,&descriptors));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=descriptors;ai.descriptorSetCount=1;ai.pSetLayouts=&set_layout;
        VK_OK(vkAllocateDescriptorSets(device,&ai,&set));VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_LINEAR;
        si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;si.maxLod=0;VK_OK(vkCreateSampler(device,&si,nullptr,&sampler));
        VkDescriptorImageInfo image{sampler,input.view,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=set;write.dstBinding=0;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.descriptorCount=1;write.pImageInfo=&image;
        vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    }
    void record(){
        VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};memory.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;memory.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&memory,0,nullptr,0,nullptr);
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass;begin.framebuffer=framebuffer;begin.renderArea={{0,0},{width,height}};
        vkCmdBeginRenderPass(command,&begin,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);
        float push[8]={1.f/width,1.f/height};vkCmdPushConstants(command,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,push);vkCmdDraw(command,3,1,0,0);
        vkCmdEndRenderPass(command);memory.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;memory.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&memory,0,nullptr,0,nullptr);
    }
    ~TemporalEncoder(){vkDestroySampler(device,sampler,nullptr);vkDestroyDescriptorPool(device,descriptors,nullptr);vkDestroyPipeline(device,pipeline,nullptr);
        vkDestroyPipelineLayout(device,layout,nullptr);vkDestroyDescriptorSetLayout(device,set_layout,nullptr);vkDestroyFramebuffer(device,framebuffer,nullptr);vkDestroyRenderPass(device,pass,nullptr);}
};
