// Execute actual production camera selection and jitter application with
// authored matrices/resources. A separate Vulkan fixture verifies pixel motion.
#include "nvn/vulkan/temporal.h"
#include "nvn/vulkan/temporal_scene_policy.h"
#include "settings.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {uint64_t addr,size;} Range;
typedef struct {struct {bool depth_minus_one;} vkey;} Prog;
static float camera_storage[4][44];
#undef HEAP_BASE
#define HEAP_BASE ((uintptr_t)camera_storage)
static struct {struct {NvnTexture *depth;}rt;Range ubuf[2][14];uint8_t ds[4];}st;
static TmpCamera tmp_cam;
static uint64_t tmp_frame=~0ull,tmp_scene_frame=~0ull,tmp_cam_frame=~0ull,present_frame;
static NvnTexture *tmp_depth_tex;
static float tmp_jit[2];static bool tmp_dm1=true,hud,temporal_on=true;
static TmpJitterGrid tmp_jitter_grid;
static TmpSceneEvidence tmp_scene_evidence;
static uint32_t jit_index,jit_phases=8;
static int env_int(const char *name){const char *value=getenv(name);return value?atoi(value):0;}
bool tmp_enabled(void){return temporal_on;}
static uint64_t renderer_present_count(void){return present_frame;}
bool tmp_hud_started(void){return hud;}
static void output_scale_base(uint32_t *w,uint32_t *h){*w=1280;*h=720;}
static VkImageView snapshot;static TexBackend fallback_image;static unsigned fallback_calls;
Settings g_settings;static VkImageView fg_scene;static unsigned evaluated,suspended,resumed,diagnosed,spatial_marked;
VkImageView tmp_hud_scene_view(uint32_t w,uint32_t h){assert(w==2560&&h==1440);return snapshot;}
static TexBackend *post_image(int slot,uint32_t w,uint32_t h){assert(slot==7&&w==2560&&h==1440);fallback_image.width=w;fallback_image.height=h;return &fallback_image;}
static VkImageView post_view(TexBackend *image){assert(image==&fallback_image);return (VkImageView)(uintptr_t)99;}
static void post_pass(VkCommandBuffer cb,const char *shader,const VkImageView *views,int count,TexBackend *out,const float constants[8],bool clear){
    (void)cb;assert(!strcmp(shader,"tmp_hud.frag")&&count==3&&out==&fallback_image&&!clear);
    assert(views[0]==(snapshot?snapshot:views[1])&&views[2]==views[0]);
    assert(constants[0]==1.f/2560&&constants[1]==1.f/1440&&constants[2]==tmp_jitter_grid.uv[0]&&constants[3]==tmp_jitter_grid.uv[1]);fallback_calls++;
}
void vk_full_barrier(VkCommandBuffer cb){(void)cb;}
void vk_frame_suspend(void){suspended++;}void vk_frame_resume(void){resumed++;}
static VkImageView raw_view(TexBackend *image){assert(image->width==2560&&image->height==1440);return (VkImageView)(uintptr_t)88;}
bool tmp_linear_output(void){return false;}
VkImageView tmp_upscale(VkCommandBuffer cb,TexBackend *color,VkImageView view,TexBackend *depth,const TmpCamera *cam,const float jitter[2],bool dm1,uint32_t ow,uint32_t oh){
    (void)cb;(void)view;(void)dm1;assert(color->width==2560&&ow==2560&&oh==1440&&!cam&&!depth);
    assert(jitter[0]==tmp_jitter_grid.uv[0]*2560&&jitter[1]==tmp_jitter_grid.uv[1]*1440);evaluated++;return VK_NULL_HANDLE;
}
void tmp_diagnostics_frame(const TmpSceneEvidence *e,uint32_t w,uint32_t h,uint32_t ow,uint32_t oh,const float jitter[2]){
    assert(e==&tmp_scene_evidence&&w==ow&&h==oh&&jitter[0]==tmp_jitter_grid.uv[0]*w);diagnosed++;
}
void tmp_note_spatial_frame(void){spatial_marked++;}
static VkImageView smaa_lut(int slot){(void)slot;assert(!"unexpected SMAA");return VK_NULL_HANDLE;}
static VkImageView fsr1_sharpen(VkCommandBuffer cb,VkImageView view,uint32_t w,uint32_t h){(void)cb;(void)view;(void)w;(void)h;assert(!"unexpected FSR1");return VK_NULL_HANDLE;}
#include "temporal_scene_production.h"
static void camera(float *m,float x){
    memset(m,0,44*sizeof *m);m[0]=m[5]=m[10]=1;m[3]=-x;
    m[12]=1.0f;m[17]=1.7777778f;m[22]=-1;m[23]=-.2f;m[26]=-1;
    for(unsigned row=0;row<4;row++)for(unsigned col=0;col<4;col++){
        const float *p=m+12+row*4;
        m[28+row*4+col]=p[0]*m[col]+p[1]*m[4+col]+p[2]*m[8+col]+(col==3?p[3]:0);
    }
}
static void near(float actual,float expected){assert(fabsf(actual-expected)<1e-6f);}
int main(int argc,char **argv){
    if(argc==2)assert(!setenv("XCDE_TMP_NOJITTER",argv[1],1));
    camera(camera_storage[0],0);camera(camera_storage[1],100);camera(camera_storage[2],0);
    TexBackend scene={.image=(VkImage)(uintptr_t)1,.width=2560,.height=1440,.aspect=VK_IMAGE_ASPECT_DEPTH_BIT};
    TexBackend shadow={.image=(VkImage)(uintptr_t)2,.width=1024,.height=1024,.aspect=VK_IMAGE_ASPECT_DEPTH_BIT};
    NvnTexture target={.width=1280,.height=720,.backend=&scene};
    NvnTexture shadow_target={.width=1024,.height=1024,.backend=&shadow};
    Prog program={.vkey={true}};st.ubuf[0][0]=(Range){(uintptr_t)camera_storage[0],176};st.ds[0]=1;
    float x=0,y=0;st.rt.depth=&shadow_target;tmp_draw(&program,&x,&y);
    assert(!tmp_cam.ok&&!x&&!y&&tmp_scene_evidence.rejected_cameras==1);
    st.rt.depth=&target;tmp_draw(&program,&x,&y);assert(tmp_cam.ok&&tmp_depth_tex==&target&&!x&&!y);
    float declared[2];tmp_jitter_grid_offset(&tmp_jitter_grid,2560,1440,declared);assert(!declared[0]&&!declared[1]);
    tmp_jitter_grid_verify(&tmp_jitter_grid,2560,1440);
    present_frame++;x=y=0;tmp_draw(&program,&x,&y);
    if(env_int("XCDE_TMP_NOJITTER")){assert(!x&&!y&&!tmp_jit[0]&&!tmp_jit[1]);}
    else {near(x,tmp_jit[0]);near(y,tmp_jit[1]);}
    tmp_jitter_grid_offset(&tmp_jitter_grid,2560,1440,declared);near(x,declared[0]);near(y,declared[1]);
    // Guest DRS: scene is70% of the actual final provider-input grid.
    target.width=896;target.height=504;scene.width=1792;scene.height=1008;
    present_frame++;x=y=0;tmp_draw(&program,&x,&y);
    tmp_jitter_grid_offset(&tmp_jitter_grid,2560,1440,declared);
    near(x*(2560.f/1792.f),declared[0]);near(y*(1440.f/1008.f),declared[1]);
    st.ds[0]=0;x=y=0;tmp_draw(&program,&x,&y);assert(tmp_scene_evidence.nodepth_draws==1);
    near(x*(2560.f/1792.f),declared[0]);near(y*(1440.f/1008.f),declared[1]);
    // A different or subsequently changed camera UBO must not be mistaken for sky/world.
    st.ubuf[0][0].addr=(uintptr_t)camera_storage[1];x=y=0;tmp_draw(&program,&x,&y);assert(!x&&!y&&tmp_scene_evidence.nodepth_draws==1);
    st.ubuf[0][0].addr=(uintptr_t)camera_storage[2];x=y=0;tmp_draw(&program,&x,&y);assert(tmp_scene_evidence.nodepth_draws==2);
    camera_storage[2][3]=7;x=y=0;tmp_draw(&program,&x,&y);assert(!x&&!y&&tmp_scene_evidence.nodepth_draws==2);
    st.ubuf[0][0].addr=(uintptr_t)camera_storage[0];hud=true;x=y=0;tmp_draw(&program,&x,&y);assert(!x&&!y&&tmp_scene_evidence.hud_draws==1);hud=false;
    // A changed actual final extent remains consistent with the same normalized shift.
    tmp_jitter_grid_offset(&tmp_jitter_grid,1920,1080,declared);
    near(tmp_jitter_grid.uv[0]*1920,declared[0]);near(tmp_jitter_grid.uv[1]*1080,declared[1]);
    // Actual production fallback is independent of a successful SDK/camera:
    // it uses the exact recorded shift, with the untouched HUD as separate input.
    TexBackend final_color={.width=2560,.height=1440};VkImageView final=(VkImageView)(uintptr_t)88;
    tmp_cam.ok=false;unsigned jittered=tmp_scene_evidence.jittered_draws;
    tmp_scene_evidence.jittered_draws=0;assert(tmp_spatial_fallback(NULL,&final_color,final)==final&&!fallback_calls);
    tmp_scene_evidence.jittered_draws=jittered;
    VkImageView expected=env_int("XCDE_TMP_NOJITTER")?final:(VkImageView)(uintptr_t)99;
    assert(tmp_spatial_fallback(NULL,&final_color,final)==expected);
    snapshot=(VkImageView)(uintptr_t)77;assert(tmp_spatial_fallback(NULL,&final_color,final)==expected);
    assert(fallback_calls==(env_int("XCDE_TMP_NOJITTER")?0:2));
    assert(tmp_scene_evidence.fallback_unjittered==!env_int("XCDE_TMP_NOJITTER"));
    // Execute the actual presentation entry as well: rejection must reach the
    // correction before resuming the window pass, with current real dimensions.
    g_settings.upscaler=UPSCALE_DLSS;assert(post_process(NULL,&final_color,2560,1440)==expected);
    assert(evaluated==1&&suspended==1&&resumed==1&&diagnosed==1);
    vk_exec_forget_texture(&target);assert(!tmp_depth_tex&&!tmp_cam.ok&&tmp_cam_frame==~0ull);
    tmp_exec_reset();assert(!tmp_jitter_grid.verified_w&&!tmp_jitter_grid.verified_h);
    assert(!tmp_scene_depth_eligible(640,360,1280,720));assert(!tmp_scene_depth_eligible(192,544,1280,720));
    assert(!tmp_scene_depth_eligible(1024,1024,1280,720));assert(tmp_scene_depth_eligible(896,504,1280,720));
    temporal_on=false;float sample[2]={1,1};tmp_jitter(sample);assert(!sample[0]&&!sample[1]);
    present_frame++;g_settings.upscaler=UPSCALE_BILINEAR;g_settings.fps=120;
    assert(post_process(NULL,&final_color,2560,1440)==final&&spatial_marked==1);
    assert(evaluated==1&&suspended==1&&resumed==1&&diagnosed==2&&!tmp_scene_evidence.jittered_draws);
    puts("PASS production camera/scene/shadow/nodepth/HUD/DRS-grid/no-jitter/lifetime contracts");return 0;
}
