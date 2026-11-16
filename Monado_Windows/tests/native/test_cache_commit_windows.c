// Actual Win32 transaction boundary, authored Vulkan query; no GPU/game/session control.
#include <windows.h>
#include <vulkan/vulkan.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <io.h>
static int fail_write,fail_flush,fail_replace,query_mode,queries;
static BOOL WINAPI fixture_write(HANDLE h,LPCVOID b,DWORD n,LPDWORD got,LPOVERLAPPED ov){
    if(fail_write){SetLastError(ERROR_DISK_FULL);return FALSE;}return WriteFile(h,b,n,got,ov);}
static BOOL WINAPI fixture_flush(HANDLE h){if(fail_flush){SetLastError(ERROR_DISK_FULL);return FALSE;}return FlushFileBuffers(h);}
static BOOL WINAPI fixture_move(LPCWSTR a,LPCWSTR b,DWORD flags){if(fail_replace){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}return MoveFileExW(a,b,flags);}
#define WriteFile fixture_write
#define FlushFileBuffers fixture_flush
#define MoveFileExW fixture_move
#include "../../runtime/nvn/vulkan/pipeline_cache_io_windows.h"
#include "../../runtime/nvn/vulkan/variant_cache_windows.h"
#include "../../runtime/nvn/vulkan/precompile_status.h"
#include "../../runtime/nvn/vulkan/present_policy_windows.h"
#undef WriteFile
#undef FlushFileBuffers
#undef MoveFileExW
VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice d,VkPipelineCache c,size_t *n,void *p){
    (void)d;(void)c;++queries;
    if(query_mode==2)return VK_ERROR_OUT_OF_HOST_MEMORY;
    if(!p){*n=query_mode==3?1024ull*1024*1024+1:40;return VK_SUCCESS;}
    if(query_mode==1&&queries==2)return VK_INCOMPLETE;
    uint32_t header[8]={32,1,0x10de,0x1234,0,0,0,0};memcpy(p,header,32);memcpy((char*)p+32,"authored",8);*n=40;return VK_SUCCESS;
}
static bool save(void){return pipeline_cache_save_native((VkDevice)(uintptr_t)1,(VkPipelineCache)(uintptr_t)2);}
static void old_preserved(void){FILE *f=wct_read("cache/vk_pipelines.bin",64);assert(f);char b[41]={0};assert(fread(b,1,40,f)==40&&!memcmp(b+32,"original",8));fclose(f);}
#include "cache_fixture_path_windows.h"
int wmain(int argc,wchar_t **argv){
    assert(argc==3&&cache_fixture_cwd(argv[2]));char mode[128];assert(WideCharToMultiByte(CP_UTF8,0,argv[1],-1,mode,sizeof mode,NULL,NULL));
    if(!strcmp(mode,"reparse")){assert(!wct_directory("cache"));assert(!wct_read("cache/sentinel",64));return 0;}
    assert(wct_directory("cache"));
    if(!strcmp(mode,"present-policy")){
        int caps[]={0,30,60,90,120,240};for(unsigned i=0;i<6;i++){
            assert(windows_present_fifo(false,true,caps[i],true,false,false,false,false));
            assert(!windows_present_fifo(false,false,caps[i],true,false,false,false,false));
            assert(!windows_present_fifo(true,true,caps[i],true,false,false,false,false));
            assert(!windows_present_fifo(false,true,caps[i],true,false,false,false,true));
            assert(!windows_present_fifo(false,true,caps[i],true,true,false,false,false));
            assert(!windows_present_fifo(false,true,caps[i],true,false,true,false,false));
            assert(windows_present_fifo(false,true,caps[i],true,false,true,true,false));
            assert(windows_present_fifo(false,true,caps[i],false,false,false,false,false)==(caps[i]>0&&caps[i]<=60));
        }return 0;
    }
    if(!strcmp(mode,"pipeline-unknown")){
        assert(wct_write("cache/vk_pipelines.bin","private unknown",15,NULL,64));assert(!save());FILE *f=wct_read("cache/vk_pipelines.bin",64);assert(f);char b[16]={0};assert(fread(b,1,15,f)==15&&!strcmp(b,"private unknown"));fclose(f);return 0;
    }
    if(!strcmp(mode,"variants")||!strcmp(mode,"variant-malformed")){
        if(!strcmp(mode,"variant-malformed")){assert(wct_write("cache/variants.bin","private malformed",17,NULL,64));WsGuard g;
            assert(!variant_cache_open_native("cache/variants.bin",64,&g));FILE *f=wct_read("cache/variants.bin",64);assert(f);char b[18]={0};assert(fread(b,1,17,f)==17&&!strcmp(b,"private malformed"));fclose(f);
        }else{
            WsGuard g;FILE *f=variant_cache_open_native("cache/variants.bin",64,&g);assert(f);char b[64]={0};assert(fwrite(b,1,64,f)==64&&!fflush(f)&&!_commit(_fileno(f)));assert(!fclose(f));ws_guard_close(&g);
            f=variant_cache_open_native("cache/variants.bin",64,&g);assert(f&&fread(b,1,64,f)==64);fclose(f);ws_guard_close(&g);
            f=variant_cache_open_native("cache/variants.bin",32,&g);assert(!f);
        }return 0;
    }
    if(!strcmp(mode,"status-parser")){
        const char s[]=PRECOMPILE_WORK_MAGIC "\nsession bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\nidentity aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\nphase done\nstate complete\ngpu_cache_uuid 00000000000000000000000000000000\nfiles_total 1\nfiles_done 1\nshaders 2\nprograms 1\nprograms_done 1\nseed_total 0\nseed_done 0\nvariants_total 0\nvariants_done 0\nattempted 1\nbuilt 1\nfailed 0\nskipped 0\nunsupported 0\nerrors 0\ngpu_vendor 0\ngpu_device 0\ngpu_driver 0\n";
        assert(precompile_work_valid(s,sizeof s-1));assert(!precompile_work_valid(s,sizeof s-2));
        char bad[4096];snprintf(bad,sizeof bad,"%ssession bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n",s);assert(!precompile_work_valid(bad,strlen(bad)));return 0;
    }
    uint8_t initial[40];uint32_t header[8]={32,1,0x10de,0x1234,0,0,0,0};memcpy(initial,header,32);memcpy(initial+32,"original",8);
    assert(wct_write("cache/vk_pipelines.bin",initial,sizeof initial,NULL,64));
    if(!strcmp(mode,"hardlink")){
        wchar_t from[WS_PATH],to[WS_PATH];assert(wct_absolute("cache/vk_pipelines.bin",from)&&wct_absolute("hardlink-copy",to));assert(CreateHardLinkW(to,from,NULL));
        assert(!save());HANDLE h=CreateFileW(from,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);assert(h!=INVALID_HANDLE_VALUE);char b[40];DWORD got;assert(ReadFile(h,b,40,&got,NULL)&&got==40&&!memcmp(b,initial,40));CloseHandle(h);return 0;
    }
    if(!strcmp(mode,"locked")){FILE *held=wct_read("cache/vk_pipelines.bin",64);assert(held);assert(!save());fclose(held);old_preserved();return 0;}
    if(!strcmp(mode,"incomplete"))query_mode=1;
    else if(!strcmp(mode,"get-failure"))query_mode=2;
    else if(!strcmp(mode,"get-bound"))query_mode=3;
    else if(!strcmp(mode,"flush-failure"))fail_flush=1;
    else if(!strcmp(mode,"write-failure"))fail_write=1;
    else if(!strcmp(mode,"replace-failure"))fail_replace=1;
    else assert(!strcmp(mode,"normal"));
    bool ok=save();bool expected=!query_mode||query_mode==1;expected=expected&&!fail_flush&&!fail_write&&!fail_replace;
    assert(ok==expected);fail_write=fail_flush=fail_replace=0;
    if(!ok)old_preserved();else{FILE *f=wct_read("cache/vk_pipelines.bin",64);assert(f);char b[40];assert(fread(b,1,40,f)==40&&!memcmp(b+32,"authored",8));fclose(f);}
    puts("native cache transaction PASS");return 0;
}
