// SPDX-License-Identifier: MIT
// Native FSR4 on authored images; no game, SDL, window or human profile.
#include <vulkan/vulkan.h>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
static VkResult fsr4_fixture_device(VkPhysicalDevice physical,const VkDeviceCreateInfo*ci,const VkAllocationCallbacks*a,VkDevice*out){
    VkPhysicalDeviceVulkan13Features q13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features q12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};q12.pNext=&q13;
    VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};q.pNext=&q12;vkGetPhysicalDeviceFeatures2(physical,&q);
    VkPhysicalDeviceFloatControlsProperties fc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};sg.pNext=&fc;
    VkPhysicalDevicePushDescriptorPropertiesKHR pd{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};pd.pNext=&sg;
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};props.pNext=&pd;vkGetPhysicalDeviceProperties2(physical,&props);
    if(!q12.shaderFloat16||!q13.shaderIntegerDotProduct||!q.features.shaderInt16||!q.features.shaderStorageImageWriteWithoutFormat||
       !fc.shaderDenormPreserveFloat16||!(sg.supportedStages&VK_SHADER_STAGE_COMPUTE_BIT)||!(sg.supportedOperations&VK_SUBGROUP_FEATURE_QUAD_BIT)||
       pd.maxPushDescriptors<11||props.properties.limits.maxComputeWorkGroupInvocations<256||props.properties.limits.maxComputeWorkGroupSize[0]<256){
        std::fprintf(stderr,"SKIP FSR4 required native capability unavailable on %s: fp16=%u intdot=%u int16=%u denorm16=%u quad=%u push=%u\n",
          props.properties.deviceName,q12.shaderFloat16,q13.shaderIntegerDotProduct,q.features.shaderInt16,fc.shaderDenormPreserveFloat16,sg.supportedOperations,pd.maxPushDescriptors);std::exit(77);
    }
    uint32_t n=0;vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,nullptr);std::vector<VkExtensionProperties>ext(n);vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,ext.data());
    bool push=false;for(auto&e:ext)push|=!strcmp(e.extensionName,VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);if(!push)std::exit(77);
    VkDeviceCreateInfo copy=*ci;std::vector<const char*>names;
    for(uint32_t i=0;i<ci->enabledExtensionCount;i++)names.push_back(ci->ppEnabledExtensionNames[i]);
    names.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);copy.enabledExtensionCount=(uint32_t)names.size();copy.ppEnabledExtensionNames=names.data();
    for(auto*p=(VkBaseOutStructure*)copy.pNext;p;p=p->pNext)if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
        ((VkPhysicalDeviceVulkan13Features*)p)->shaderIntegerDotProduct=VK_TRUE;
    return vkCreateDevice(physical,&copy,a,out);
}
#define vkCreateDevice fsr4_fixture_device
#define main original_framegen_fixture_main
#include "test_framegen_linux.cpp"
#undef main
#undef vkCreateDevice
#include "../../runtime/nvn/vulkan/fsr4_api.h"
#include <dlfcn.h>
#include <fstream>

static void fsr4_state(const char*state){
    char canonical[PATH_MAX],error[256];CHECK(realpath(state,canonical)&&!strcmp(state,canonical));CHECK(chdir(canonical)==0);
    const char*names[]={"XDG_CACHE_HOME","XDG_CONFIG_HOME","XDG_DATA_HOME","MESA_SHADER_CACHE_DIR","__GL_SHADER_DISK_CACHE_PATH","TMPDIR"};
    for(unsigned i=0;i<std::size(names);i++){std::string p=std::string(canonical)+"/private-"+std::to_string(i);CHECK(!mkdir(p.c_str(),0700)||errno==EEXIST);CHECK(!setenv(names[i],p.c_str(),1));}
    if(!rt_linux_state_guard(canonical,error,sizeof(error))){fprintf(stderr,"FSR4 state guard: %s\n",error);exit(1);}printf("FSR4 private state %s; HOME unchanged; no game/window\n",canonical);
}
static float unhalf4(uint16_t h){unsigned s=h>>15,e=(h>>10)&31,m=h&1023;float f=e==0?std::ldexp((float)m,-24):e==31?INFINITY:std::ldexp(1.f+m/1024.f,(int)e-15);return s?-f:f;}
static MonadoFsr4Image fsr_image(const Image&i){return{i.image,i.view,i.width,i.height};}
static void exercise(const MonadoFsr4Api*api,const char*assets,unsigned rw,unsigned rh,unsigned ow,unsigned oh,bool ultra,unsigned frames){
    void*context=api->create(physical,device,assets);CHECK(context);
    if(!api->build(context,ow,oh,ultra)){fprintf(stderr,"FSR4 build error: %s\n",api->error(context));exit(1);}
    CHECK(!api->build(context,0,oh,ultra));CHECK(!api->record(context,nullptr));
    Image color(VK_FORMAT_R16G16B16A16_SFLOAT,rw,rh),depth(VK_FORMAT_R32_SFLOAT,rw,rh),motion(VK_FORMAT_R16G16_SFLOAT,rw,rh),output(VK_FORMAT_R16G16B16A16_SFLOAT,ow,oh);
    const size_t pixels=size_t(rw)*rh,out_pixels=size_t(ow)*oh,motion_offset=pixels*8,depth_offset=motion_offset+pixels*4,read_offset=(depth_offset+pixels*4+255)&~size_t(255);
    Buffer buffer(read_offset+out_pixels*8);double first=0,last=0;
    for(unsigned frame=0;frame<frames;frame++){
        float jitter=(frame&1)?.25f:-.25f;auto*c=(uint16_t*)buffer.host,*m=(uint16_t*)(buffer.host+motion_offset);auto*z=(float*)(buffer.host+depth_offset);
        for(unsigned y=0;y<rh;y++)for(unsigned x=0;x<rw;x++){
            size_t i=size_t(y)*rw+x;bool object=x>rw/4+frame&&x<rw/2+frame&&y>rh/4&&y<rh*3/4;
            c[i*4]=half((frame>=8?.4f:.1f)+(object?.2f:0));c[i*4+1]=half(.15f+.55f*(.5f+.5f*cos((x+jitter)*.23f)));c[i*4+2]=half(.1f+.4f*y/rh);c[i*4+3]=half(1);
            m[i*2]=half(object&&frame?-1.f/rw:0);m[i*2+1]=0;z[i]=object?.3f:.8f;
        }
        buffer.sync(true);begin();Image*inputs[]={&color,&motion,&depth};const size_t offsets[]={0,motion_offset,depth_offset};
        for(unsigned i=0;i<3;i++){inputs[i]->dependency(VK_ACCESS_TRANSFER_WRITE_BIT);VkBufferImageCopy copy{};copy.bufferOffset=offsets[i];copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={rw,rh,1};vkCmdCopyBufferToImage(command,buffer.buffer,inputs[i]->image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);}
        output.dependency(VK_ACCESS_TRANSFER_WRITE_BIT);VkClearColorValue sentinel{{.95f,.01f,.95f,.5f}};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(command,output.image,VK_IMAGE_LAYOUT_GENERAL,&sentinel,1,&range);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        MonadoFsr4Frame input{};input.cmd=command;input.color=fsr_image(color);input.depth=fsr_image(depth);input.motion=fsr_image(motion);input.output=fsr_image(output);input.jitter[0]=input.jitter[1]=jitter;
        input.reset=frame==0||frame==8;input.ultra=ultra;input.sharpness=frame&1?.3f:0;
        if(!api->record(context,&input)){fprintf(stderr,"FSR4 record error: %s\n",api->error(context));exit(1);}
        output.dependency(VK_ACCESS_TRANSFER_READ_BIT);VkBufferImageCopy copy{};copy.bufferOffset=read_offset;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={ow,oh,1};vkCmdCopyImageToBuffer(command,output.image,VK_IMAGE_LAYOUT_GENERAL,buffer.buffer,1,&copy);finish();buffer.sync(false);
        auto*data=(uint16_t*)(buffer.host+read_offset);size_t changed=0;double red=0;float low=10,high=-10;
        for(size_t i=0;i<out_pixels;i++){float r=unhalf4(data[i*4]),g=unhalf4(data[i*4+1]),b=unhalf4(data[i*4+2]);CHECK(std::isfinite(r)&&std::isfinite(g)&&std::isfinite(b));CHECK(r>=-.1f&&r<=4&&g>=-.1f&&g<=4&&b>=-.1f&&b<=4);red+=r;low=std::min(low,g);high=std::max(high,g);changed+=fabs(r-.95f)>.01||fabs(g-.01f)>.01;}
        red/=out_pixels;fprintf(stderr,"FSR4 diagnostic dims %ux%u->%ux%u ultra=%u frame=%u red=%.6f lowG=%.6f highG=%.6f changed=%zu\n",rw,rh,ow,oh,ultra,frame,red,low,high,changed);CHECK(changed>out_pixels*99/100);CHECK(high-low>.15f);if(frame==0)first=red;last=red;
        printf("FSR4 native frame=%u %ux%u->%ux%u ultra=%u reset=%u sharp=%.1f meanR=%.5f contrastG=%.5f changed=%zu finite=1\n",frame,rw,rh,ow,oh,ultra,input.reset,input.sharpness,red,high-low,changed);
    }
    if(frames>8)CHECK(last>first+.15);VK_OK(vkDeviceWaitIdle(device));api->destroy(context);
}
#ifndef MONADO_FSR4_FIXTURE_HELPERS
int main(int argc,char**argv){
    CHECK(argc==4);fsr4_state(argv[1]);create_device(false);
    void*module=dlopen(argv[2],RTLD_NOW|RTLD_LOCAL);if(!module)fprintf(stderr,"%s\n",dlerror());CHECK(module);
    auto get=(MonadoFsr4GetApi)dlsym(module,"monado_fsr4_get_api");CHECK(get&&!get(999));auto*api=get(MONADO_FSR4_ABI);CHECK(api&&api->abi==2);
    CHECK(!api->create(VK_NULL_HANDLE,device,argv[3]));CHECK(!api->create(physical,device,"missing-assets"));CHECK(!api->build(nullptr,320,180,0));CHECK(!api->record(nullptr,nullptr));
    if(const char*value=getenv("FSR4_FIXTURE_SINGLE_CASE")){unsigned rw,rh,ow,oh;CHECK(sscanf(value,"%u,%u,%u,%u",&rw,&rh,&ow,&oh)==4);exercise(api,argv[3],rw,rh,ow,oh,true,3);dlclose(module);shutdown();puts("PASS native FSR4 single size case");return 0;}
    if(!getenv("FSR4_FIXTURE_ULTRA_ONLY")){exercise(api,argv[3],320,180,320,180,false,12);exercise(api,argv[3],160,90,320,180,false,12);}
    exercise(api,argv[3],640,360,1920,1080,true,3);exercise(api,argv[3],1286,720,1928,1080,false,3);exercise(api,argv[3],643,360,1928,1080,true,3);
    CHECK(validation_errors==0);dlclose(module);shutdown();printf("PASS native FSR4 Lite all4modelsets,%urealframes,NativeAA/scaled,sharpness/reset/readback; no game\n",getenv("FSR4_FIXTURE_ULTRA_ONLY")?9:33);
}
#endif
