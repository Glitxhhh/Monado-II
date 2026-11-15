// [XCDE] LINUX-103: optional native Lite provider glue. Included after Img.
// Model authentication is the Python launch-plan boundary. C requires the same
// explicit canonical ordinary layout; no environment or ambient search path.
#if defined(__linux__) && defined(XCDE_LINUX_FSR4)
#include "fsr4_api.h"
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>
static char fsr4_asset_dir[4096],fsr4_reason[512]="FSR4 Lite models not configured";
static const MonadoFsr4Api *fsr4_api;
static int fsr4_load_state;
static void *fsr4_context;
static bool fsr4_failed;
static uint32_t fsr4_width,fsr4_height,fsr4_ultra;
static uint64_t fsr4_ring[8];static unsigned fsr4_slot;
static void fsr4_note(const char *reason) {snprintf(fsr4_reason,sizeof fsr4_reason,"%s",reason?reason:"FSR4 Lite unavailable");}
static bool fsr4_ordinary(const char *path,bool directory,off_t required) {
    struct stat status;
    return !lstat(path,&status) && (directory?S_ISDIR(status.st_mode):S_ISREG(status.st_mode)) &&
        (directory || (required?status.st_size==required:status.st_size>=20 && status.st_size%4==0));
}
bool tmp_fsr4_set_asset_dir(const char *path) {
    if(fsr4_context || fsr4_load_state) {fsr4_note("FSR4 assets can only be configured before provider initialization");return false;}
    fsr4_asset_dir[0]=0;
    char canonical[PATH_MAX],entry[PATH_MAX];
    if(!path || path[0]!='/' || !realpath(path,canonical) || strcmp(path,canonical) || !fsr4_ordinary(path,true,0)) {
        fsr4_note("FSR4 models require a canonical ordinary directory");return false;
    }
    if(strlen(path)>=sizeof fsr4_asset_dir){fsr4_note("FSR4 model path too long");return false;}
    const char *sets[]={"t1080_m0","t1080_m1","t2160_m0","t2160_m1"};
    const char *passes[]={"spd.spv","prepass.spv","pass0_post.spv","pass1.spv","pass1_post.spv","pass1_postb.spv","pass2.spv","pass2_post.spv","pass3.spv","pass3_post.spv","postpass.spv","rcas.spv","initializer.bin"};
    if(snprintf(entry,sizeof entry,"%s/lite",path)>=(int)sizeof entry || !fsr4_ordinary(entry,true,0)) {
        fsr4_note("FSR4 Lite model directory absent or symlinked");return false;
    }
    for(unsigned set=0;set<4;set++) {
        if(snprintf(entry,sizeof entry,"%s/lite/%s",path,sets[set])>=(int)sizeof entry || !fsr4_ordinary(entry,true,0)) {
            fsr4_note("FSR4 Lite model set absent or symlinked");return false;
        }
        for(unsigned pass=0;pass<13;pass++) {
            if(snprintf(entry,sizeof entry,"%s/lite/%s/%s",path,sets[set],passes[pass])>=(int)sizeof entry || !fsr4_ordinary(entry,false,pass==12?27392:0)) {
                fsr4_note("FSR4 Lite model file absent, symlinked or malformed");return false;
            }
            if(pass!=12) {FILE *file=fopen(entry,"rb");uint32_t magic=0;bool valid=file && fread(&magic,sizeof magic,1,file)==1 && magic==0x07230203u;if(file)fclose(file);if(!valid){fsr4_note("FSR4 model SPIR-V header invalid");return false;}}
        }
    }
    memcpy(fsr4_asset_dir,path,strlen(path)+1);fsr4_note("FSR4 Lite models configured");return true;
}
static bool fsr4_load(void) {
    if(fsr4_load_state)return fsr4_load_state>0;
    char path[4096];
    if(!rt_resource("libmonado-fsr4.so",NULL,path,sizeof path)){fsr4_load_state=-1;fsr4_note("native FSR4 component absent");return false;}
    void *module=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if(!module){fsr4_load_state=-1;fsr4_note("native FSR4 component could not load");return false;}
    MonadoFsr4GetApi get=(MonadoFsr4GetApi)dlsym(module,"monado_fsr4_get_api");
    const MonadoFsr4Api *api=get?get(MONADO_FSR4_ABI):NULL;
    if(!api || api->abi!=MONADO_FSR4_ABI || !api->create || !api->destroy || !api->build || !api->record || !api->error) {
        dlclose(module);fsr4_load_state=-1;fsr4_note("native FSR4 component ABI mismatch");return false;
    }
    fsr4_api=api;fsr4_load_state=1;return true;
}
const char *tmp_fsr4_status(void){return fsr4_reason;}
bool tmp_fsr4_supported(void) {
    if(!fsr4_asset_dir[0])return false;
    if(!g_vk.fsr4_lite){fsr4_note(g_vk.fsr4_reason?g_vk.fsr4_reason:"Vulkan FSR4 capabilities not initialized");return false;}
    if(fsr4_failed)return false;
    return fsr4_load();
}
static bool fsr4_frame_available(void) {return tmp_fsr4_supported();}
static bool fsr4_size_supported(uint32_t rw,uint32_t rh,uint32_t ow,uint32_t oh) {
    if(!rw || !rh || !ow || !oh || rw>ow || rh>oh || ow>3840 || oh>2160) {
        fsr4_note("FSR4 Lite output exceeds its supported extent; temporal/spatial fallback selected");return false;
    }
    // The audited captured auto-exposure graph produces black for long edges
    // below128. Reject before recording; fallback can recover next frame as DRS
    // or quality changes, without disabling the provider for the whole window.
    if((rw>rh?rw:rh)<128){fsr4_note("FSR4 Lite input long edge below128; temporal/spatial fallback selected");return false;}
    return true;
}
static void fsr4_reset(void) {
    // Caller contract: GPU idle, all submitted/unsubmitted renderer work retired.
    if(fsr4_context)fsr4_api->destroy(fsr4_context);
    fsr4_context=NULL;fsr4_failed=false;fsr4_width=fsr4_height=fsr4_ultra=0;
    memset(fsr4_ring,0,sizeof fsr4_ring);fsr4_slot=0;
}
static bool fsr4_collect(bool rebuild) {
    uint64_t pending=vk_pending_seq(),completed=vk_completed_seq();
    uint64_t required=0;
    for(unsigned slot=0;slot<8;slot++)if((rebuild || slot==fsr4_slot) && fsr4_ring[slot]>required)required=fsr4_ring[slot];
    if(required>completed) {
        if(required>=pending){fsr4_note("FSR4 waiting for an earlier recording in the current submission");return false;}
        vk_wait_seq(required);
    }
    return true;
}
static bool fsr4_evaluate(VkCommandBuffer cb,const Img *color,const Img *motion,const Img *depth,const Img *output,const float jitter[2],bool reset) {
    if(!tmp_fsr4_supported() || !cb || !color || !motion || !depth || !output || !jitter ||
        !isfinite(jitter[0]) || !isfinite(jitter[1]) || color->b.format!=F_LINEAR || output->b.format!=F_LINEAR ||
        motion->b.format!=F_MV || depth->b.format!=F_DEPTH || !color->v || !depth->v || !motion->v || !output->v ||
        !color->b.image || !depth->b.image || !motion->b.image || !output->b.image ||
        !color->b.width || !color->b.height || color->b.width>output->b.width || color->b.height>output->b.height ||
        motion->b.width!=color->b.width || motion->b.height!=color->b.height || depth->b.width!=color->b.width || depth->b.height!=color->b.height)return false;
    if(!fsr4_size_supported(color->b.width,color->b.height,output->b.width,output->b.height))return false;
    unsigned ultra=g_settings.upscale_quality==UQ_ULTRA;
    bool rebuild=!fsr4_context || fsr4_width!=output->b.width || fsr4_height!=output->b.height || fsr4_ultra!=ultra;
    if(!fsr4_collect(rebuild))return false;
    if(!fsr4_context) {
        fsr4_context=fsr4_api->create(g_vk.phys,g_vk.dev,fsr4_asset_dir);
        if(!fsr4_context){fsr4_failed=true;fsr4_note("native FSR4 Lite context creation failed; fallback selected");return false;}
    }
    if(rebuild) {
        if(!fsr4_api->build(fsr4_context,output->b.width,output->b.height,ultra)) {
            fsr4_failed=true;snprintf(fsr4_reason,sizeof fsr4_reason,"FSR4 build failed: %.400s",fsr4_api->error(fsr4_context));return false;
        }
        fsr4_width=output->b.width;fsr4_height=output->b.height;fsr4_ultra=ultra;
        memset(fsr4_ring,0,sizeof fsr4_ring);fsr4_slot=0;reset=true;
    }
    MonadoFsr4Frame frame={.cmd=cb,.jitter={jitter[0],jitter[1]},.reset=reset,.ultra=ultra,.sharpness=xcde_sharpness(&g_settings)};
    const Img *images[]={color,depth,motion,output};MonadoFsr4Image *bindings[]={&frame.color,&frame.depth,&frame.motion,&frame.output};
    for(unsigned i=0;i<4;i++)*bindings[i]=(MonadoFsr4Image){images[i]->b.image,images[i]->v,images[i]->b.width,images[i]->b.height};
    // A failed record may have emitted partial commands. Keep the context alive
    // and remember the submission until reset/retirement; never free it eagerly.
    fsr4_ring[fsr4_slot]=vk_pending_seq();fsr4_slot=(fsr4_slot+1)%8;
    bool success=fsr4_api->record(fsr4_context,&frame)!=0;
    if(!success){fsr4_failed=true;snprintf(fsr4_reason,sizeof fsr4_reason,"FSR4 dispatch failed: %.400s",fsr4_api->error(fsr4_context));}
    else fsr4_note("native FSR4 Lite active (linear HDR, portable mixed dot)");
    return success;
}
#else
bool tmp_fsr4_set_asset_dir(const char *path){(void)path;return false;}
bool tmp_fsr4_supported(void){return false;}
const char *tmp_fsr4_status(void){return "native FSR4 component not compiled";}
static bool fsr4_frame_available(void){return false;}
static bool fsr4_size_supported(uint32_t rw,uint32_t rh,uint32_t ow,uint32_t oh){(void)rw;(void)rh;(void)ow;(void)oh;return false;}
static void fsr4_reset(void){}
static bool fsr4_evaluate(VkCommandBuffer cb,const Img *color,const Img *motion,const Img *depth,const Img *output,const float jitter[2],bool reset) {
    (void)cb;(void)color;(void)motion;(void)depth;(void)output;(void)jitter;(void)reset;return false;
}
#endif
