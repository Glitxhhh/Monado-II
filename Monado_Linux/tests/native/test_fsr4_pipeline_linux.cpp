// [XCDE] LINUX-103: production FSR4 C glue -> actual encoder -> native FG.
#define MONADO_FSR4_FIXTURE_HELPERS
#include "test_fsr4_linux.cpp"
#undef MONADO_FSR4_FIXTURE_HELPERS
#include "temporal_gpu_encode.h"
#include <cfloat>
extern "C" {
int fsr4_bridge_init(VkPhysicalDevice,VkDevice,const char*,const char*);
int fsr4_bridge_record(const MonadoFsr4Frame*,uint64_t,uint64_t);
const char *fsr4_bridge_status(void);
void fsr4_bridge_reset(void);
}
static void pipeline(unsigned rw,unsigned rh,unsigned ow,unsigned oh,const char*shader_root,const char*fg_module) {
    Image color(VK_FORMAT_R16G16B16A16_SFLOAT,rw,rh),motion(VK_FORMAT_R16G16_SFLOAT,rw,rh),depth(VK_FORMAT_R32_SFLOAT,rw,rh),
        mask(VK_FORMAT_R8_UNORM,rw,rh),output(VK_FORMAT_R16G16B16A16_SFLOAT,ow,oh),
        encoded(VK_FORMAT_R8G8B8A8_UNORM,ow,oh,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),generated(VK_FORMAT_R8G8B8A8_UNORM,ow,oh);
    TemporalEncoder encoder(output,encoded,shader_root);
    FgLinux *fg=fg_module?open_provider(fg_module):nullptr;
    unsigned generated_count=0;
    size_t pixels=size_t(rw)*rh,out_pixels=size_t(ow)*oh;
    size_t mv_off=pixels*8,depth_off=mv_off+pixels*4,mask_off=depth_off+pixels*4,read_off=(mask_off+pixels+255)&~size_t(255);
    Buffer buffer(read_off+out_pixels*16);
    double first=0,last=0;
    for(unsigned f=0;f<12;f++) {
        float jitter=(f&1)?.25f:-.25f;
        auto*c=(uint16_t*)buffer.host;auto*m=(uint16_t*)(buffer.host+mv_off);auto*z=(float*)(buffer.host+depth_off);auto*k=buffer.host+mask_off;
        for(unsigned y=0;y<rh;y++)for(unsigned x=0;x<rw;x++) {
            size_t i=size_t(y)*rw+x;bool object=x>rw/4+f&&x<rw/2+f&&y>rh/4&&y<rh*3/4;
            float stripe=(float)(0.15+0.55*(0.5+0.5*cos((x+jitter)*0.23)));
            c[i*4]=half((f>=8?.4f:.1f)+(object?.2f:0));c[i*4+1]=half(stripe);c[i*4+2]=half(.1f+.4f*y/rh);c[i*4+3]=half(1);
            m[i*2]=half(object&&f?-1.0f/rw:0);m[i*2+1]=0;z[i]=object?.3f:.8f;k[i]=object?128:0;
        }
        buffer.sync(true);begin();color.dependency(VK_ACCESS_TRANSFER_WRITE_BIT);motion.dependency(VK_ACCESS_TRANSFER_WRITE_BIT);
        depth.dependency(VK_ACCESS_TRANSFER_WRITE_BIT);mask.dependency(VK_ACCESS_TRANSFER_WRITE_BIT);output.dependency(VK_ACCESS_TRANSFER_WRITE_BIT);
        const Image*inputs[]={&color,&motion,&depth,&mask};const size_t offsets[]={0,mv_off,depth_off,mask_off};
        for(unsigned i=0;i<4;i++) {
            VkBufferImageCopy cp{};cp.bufferOffset=offsets[i];cp.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};cp.imageExtent={rw,rh,1};
            vkCmdCopyBufferToImage(command,buffer.buffer,inputs[i]->image,VK_IMAGE_LAYOUT_GENERAL,1,&cp);
        }
        VkClearColorValue sentinel{{.95f,.01f,.95f,.5f}};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(command,output.image,VK_IMAGE_LAYOUT_GENERAL,&sentinel,1,&range);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        MonadoFsr4Frame d{};d.cmd=command;d.color=fsr_image(color);d.depth=fsr_image(depth);d.motion=fsr_image(motion);d.output=fsr_image(output);d.jitter[0]=d.jitter[1]=jitter;d.reset=f==0||f==8;d.sharpness=f&1?.3f:0;
        if(!fsr4_bridge_record(&d,recording_seq,completed_seq)){fprintf(stderr,"FSR4 glue failed: %s\n",fsr4_bridge_status());exit(1);}
        encoded.dependency(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);generated.dependency(VK_ACCESS_TRANSFER_WRITE_BIT);
        encoder.record();
        FgLinuxResult fg_result=FG_LINUX_DISABLED;
        if(fg){
            FgLinuxFrame g{};g.commands=command;g.scene=fg_image(encoded);g.depth=fg_image(depth);g.motion=fg_image(motion);g.output=fg_image(generated);
            g.jitter[0]=g.jitter[1]=jitter;g.motion_scale[0]=(float)rw;g.motion_scale[1]=(float)rh;
            g.dt_ms=1000.f/60.f;g.camera_near=.1f;g.camera_far=FLT_MAX;g.camera_fov=.785f;g.meters=1;
            g.real_frame_id=f+1;g.recording_seq=recording_seq;g.completed_seq=completed_seq;g.reset=d.reset;g.depth_infinite=true;
            fg_result=::dispatch(fg,g);CHECK(fg_result==(d.reset?FG_LINUX_WARMUP:FG_LINUX_GENERATED));
            generated.dependency(VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy gc{};gc.bufferOffset=read_off+out_pixels*8;gc.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};gc.imageExtent={ow,oh,1};
            vkCmdCopyImageToBuffer(command,generated.image,VK_IMAGE_LAYOUT_GENERAL,buffer.buffer,1,&gc);
        }
        encoded.dependency(VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy ec{};ec.bufferOffset=read_off+out_pixels*12;ec.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};ec.imageExtent={ow,oh,1};
        vkCmdCopyImageToBuffer(command,encoded.image,VK_IMAGE_LAYOUT_GENERAL,buffer.buffer,1,&ec);
        output.dependency(VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy cp{};cp.bufferOffset=read_off;cp.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};cp.imageExtent={ow,oh,1};
        vkCmdCopyImageToBuffer(command,output.image,VK_IMAGE_LAYOUT_GENERAL,buffer.buffer,1,&cp);finish();buffer.sync(false);
        if(fg){
            fg_linux_collect(fg,completed_seq);size_t distinct=0;
            const auto *g=buffer.host+read_off+out_pixels*8,*e=buffer.host+read_off+out_pixels*12;
            for(size_t i=0;i<out_pixels;i++)distinct+=memcmp(g+i*4,e+i*4,3)!=0;
            if(fg_result==FG_LINUX_GENERATED){CHECK(distinct>10);generated_count++;}
            else CHECK(!distinct);
            printf("FSR4_GLUE_TO_FG frame=%u result=%d distinctRGB=%zu\n",f,fg_result,distinct);
        }
        const uint16_t*data=(const uint16_t*)(buffer.host+read_off);double red=0;float green_min=1,green_max=0;size_t changed=0;
        for(size_t i=0;i<out_pixels;i++) {
            float r=unhalf4(data[i*4]),g=unhalf4(data[i*4+1]),b=unhalf4(data[i*4+2]);
            CHECK(std::isfinite(r)&&std::isfinite(g)&&std::isfinite(b));CHECK(r>=-.02f&&r<=1.02f&&g>=-.02f&&g<=1.02f&&b>=-.02f&&b<=1.02f);
            red+=r;green_min=std::min(green_min,g);green_max=std::max(green_max,g);changed+=fabs(r-.95f)>.01f||fabs(g-.01f)>.01f;
        }
        CHECK(changed>out_pixels*99/100);CHECK(green_max-green_min>.2f);red/=out_pixels;if(f==0)first=red;if(f==11)last=red;
        printf("FSR4_GLUE frame=%u %ux%u->%ux%u reset=%d meanR=%.5f contrastG=%.5f changed=%zu finite=1\n",f,rw,rh,ow,oh,d.reset,red,green_max-green_min,changed);
    }
    CHECK(last>first+.15);VK_OK(vkDeviceWaitIdle(device));
    if(fg){CHECK(generated_count==10);CHECK(fg_linux_close(fg,completed_seq));}
    fsr4_bridge_reset();
}
int main(int argc,char **argv) {
 CHECK(argc==6);fsr4_state(argv[1]);create_device(false);CHECK(fsr4_bridge_init(physical,device,argv[2],argv[3]));
 pipeline(320,180,320,180,argv[4],argv[5]);pipeline(160,90,320,180,argv[4],argv[5]);
 CHECK(!validation_errors);shutdown();puts("PASS production FSR4 glue -> linear encoder -> nativeFG,24realdispatches,20generatedRGB-distinct,reset/sharpness; no game/WSI/performance qualification");
}
