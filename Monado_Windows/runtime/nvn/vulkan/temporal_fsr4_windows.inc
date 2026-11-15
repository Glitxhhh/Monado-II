// [XCDE] Optional native Windows Vulkan Lite provider. No ambient model ENV,
// ELF loader or automatic selection. Exact local files stay locked for the run.
#include "fsr4_api.h"
#include "fsr4_policy.h"
#include "fsr4_model_identity.h"
#include "../../platform/windows_library_identity.h"
static char fsr4_asset_dir[4096],fsr4_component_path[4096];
static char fsr4_reason[512]="FSR4 Lite unavailable: build the native component and explicitly import locally prepared models; native capture tools/models are absent";
static const MonadoFsr4Api *fsr4_api;
static int fsr4_load_state;
static void *fsr4_context;
static bool fsr4_failed;
static uint32_t fsr4_width,fsr4_height,fsr4_ultra;
static uint64_t fsr4_ring[8];static unsigned fsr4_slot;
static WsGuard fsr4_model_guards[52];static HANDLE fsr4_model_files[52];static unsigned fsr4_model_count;
static void fsr4_note(const char *reason){snprintf(fsr4_reason,sizeof fsr4_reason,"%s",reason?reason:"FSR4 Lite unavailable");}
static bool fsr4_absolute(const char *path) {
    wchar_t value[4096];if(!path||!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,value,4096))return false;
    return (wcslen(value)>=3&&value[1]==L':'&&(value[2]==L'\\'||value[2]==L'/')) || (value[0]==L'\\'&&value[1]==L'\\');
}
static void fsr4_release_models(void){
    while(fsr4_model_count){unsigned i=--fsr4_model_count;CloseHandle(fsr4_model_files[i]);ws_guard_close(&fsr4_model_guards[i]);}
    fsr4_asset_dir[0]=0;
}
bool tmp_fsr4_set_asset_dir(const char *path) {
    if(fsr4_context||fsr4_load_state){fsr4_note("FSR4 assets can only be configured before provider initialization");return false;}
    fsr4_release_models();
    if(!fsr4_absolute(path)||strlen(path)>3900){fsr4_note("FSR4 models need an explicit absolute ordinary Unicode path");return false;}
    char entry[4096],id[65];
    if(snprintf(entry,sizeof entry,"%s/models.id",path)>=(int)sizeof entry || !wl_read_hex(entry,id) || strcmp(id,MONADO_FSR4_MODEL_ID)){
        fsr4_note("FSR4 local model receipt/SDK identity absent or changed; import verified prepared Lite assets");return false;
    }
    const char *sets[]={"t1080_m0","t1080_m1","t2160_m0","t2160_m1"};
    const char *passes[]={"spd.spv","prepass.spv","pass0_post.spv","pass1.spv","pass1_post.spv","pass1_postb.spv","pass2.spv","pass2_post.spv","pass3.spv","pass3_post.spv","postpass.spv","rcas.spv","initializer.bin"};
    for(unsigned set=0;set<4;set++)for(unsigned pass=0;pass<13;pass++){
        unsigned index=fsr4_model_count;char side[4096],expected[65];
        if(snprintf(entry,sizeof entry,"%s/lite/%s/%s",path,sets[set],passes[pass])>=(int)sizeof entry ||
            snprintf(side,sizeof side,"%s.sha256",entry)>=(int)sizeof side || !wl_read_hex(side,expected) || ws_guard_open(&fsr4_model_guards[index],entry))goto invalid;
        HANDLE file=CreateFileW(fsr4_model_guards[index].path,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        LARGE_INTEGER size,zero={0};DWORD count=0;uint32_t magic=0;
        bool valid=file!=INVALID_HANDLE_VALUE&&ws_plain(file,false,true)&&wl_owned(file)&&GetFileType(file)==FILE_TYPE_DISK&&
            GetFileSizeEx(file,&size)&&(pass==12?size.QuadPart==27392:size.QuadPart>=20&&size.QuadPart<=16*1024*1024&&size.QuadPart%4==0)&&wl_hash_matches(file,expected);
        if(valid&&pass!=12)valid=SetFilePointerEx(file,zero,NULL,FILE_BEGIN)&&ReadFile(file,&magic,sizeof magic,&count,NULL)&&count==sizeof magic&&magic==0x07230203u;
        if(!valid){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);ws_guard_close(&fsr4_model_guards[index]);goto invalid;}
        fsr4_model_files[index]=file;fsr4_model_count++;
    }
    // The parent lease for each model also retains the root; obtain a normalized
    // Unicode spelling from a checked handle path without resolving a junction.
    WsGuard root;
    if(snprintf(entry,sizeof entry,"%s/monado-root-lease",path)>=(int)sizeof entry||ws_guard_open(&root,entry))goto invalid;
    wchar_t *slash=wcsrchr(root.path,L'\\');if(slash)*slash=0;
    int converted=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,root.path,-1,fsr4_asset_dir,sizeof fsr4_asset_dir,NULL,NULL);ws_guard_close(&root);
    if(!converted)goto invalid;
    fsr4_note("FSR4 Lite local models verified; native component/device checks remain");return true;
invalid:
    fsr4_release_models();fsr4_note("FSR4 model file/hash/size/owner/reparse validation failed; existing fallback retained");return false;
}
bool tmp_fsr4_configure(const char *assets,const char *component) {
    if(fsr4_context||fsr4_load_state)return false;
    fsr4_component_path[0]=0;
    if(!fsr4_absolute(component)||strlen(component)>=sizeof fsr4_component_path){fsr4_note("FSR4 requires an explicit native component path");return false;}
    if(!tmp_fsr4_set_asset_dir(assets))return false;
    WsGuard guard;char side[4096],expected[65];
    if(snprintf(side,sizeof side,"%s.sha256",component)>=(int)sizeof side||!wl_read_hex(side,expected)||ws_guard_open(&guard,component)){
        fsr4_release_models();fsr4_note("FSR4 component receipt/path missing or invalid");return false;
    }
    HANDLE file=CreateFileW(guard.path,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    bool valid=file!=INVALID_HANDLE_VALUE&&ws_plain(file,false,true)&&wl_owned(file)&&wl_hash_matches(file,expected);
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);ws_guard_close(&guard);
    if(!valid){fsr4_release_models();fsr4_note("FSR4 component owner/hash/type invalid");return false;}
    memcpy(fsr4_component_path,component,strlen(component)+1);return true;
}
bool tmp_fsr4_configured(void){return fsr4_asset_dir[0]&&fsr4_component_path[0];}
static bool fsr4_load(void) {
    if(fsr4_load_state)return fsr4_load_state>0;
    if(!tmp_fsr4_configured()){fsr4_note("FSR4 local component/models are not configured; native capture/translation preparation remains unavailable");return false;}
    fsr4_load_state=-1;WsGuard guard;char side[4096],expected[65];
    if(snprintf(side,sizeof side,"%s.sha256",fsr4_component_path)>=(int)sizeof side || !wl_read_hex(side,expected) || ws_guard_open(&guard,fsr4_component_path)){
        fsr4_note("FSR4 native component SHA256 receipt/path invalid");return false;
    }
    HANDLE file=CreateFileW(guard.path,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    bool valid=file!=INVALID_HANDLE_VALUE&&ws_plain(file,false,true)&&wl_owned(file)&&wl_hash_matches(file,expected);
    HMODULE module=valid?LoadLibraryExW(guard.path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32):NULL;
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);ws_guard_close(&guard);
    if(!module){fsr4_note("FSR4 native component absent, changed, unsafe or could not load");return false;}
    MonadoFsr4GetApi get=(MonadoFsr4GetApi)(void*)GetProcAddress(module,"monado_fsr4_get_api");
    const MonadoFsr4Api *api=get?get(MONADO_FSR4_ABI):NULL;
    if(!api||api->abi!=MONADO_FSR4_ABI||!api->create||!api->destroy||!api->build||!api->record||!api->error){
        FreeLibrary(module);fsr4_note("FSR4 native component ABI mismatch");return false;
    }
    // Retain the actual module for the process lifetime; GPU-idle reset owns
    // context destruction. No provider pointer may outlive its loaded module.
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
    const char *reason=monado_fsr4_extent(rw,rh,ow,oh);if(reason)fsr4_note(reason);return !reason;
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
