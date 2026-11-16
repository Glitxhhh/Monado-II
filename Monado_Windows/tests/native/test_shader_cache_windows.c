// Native shaderc/cache fixture. Only authored GLSL and disposable state; no Vulkan device or game data.
#include <windows.h>
#include <winioctl.h>
#include <io.h>
#include <errno.h>
static int fail_lock,fail_sync,fail_write;
static BOOL WINAPI fixture_lock(HANDLE h,DWORD flags,DWORD a,DWORD b,DWORD c,LPOVERLAPPED ov){
    if(fail_lock){SetLastError(ERROR_LOCK_VIOLATION);return FALSE;}return LockFileEx(h,flags,a,b,c,ov);
}
static BOOL WINAPI fixture_write(HANDLE h,LPCVOID p,DWORD n,LPDWORD got,LPOVERLAPPED ov){
    if(fail_write){SetLastError(ERROR_DISK_FULL);return FALSE;}return WriteFile(h,p,n,got,ov);
}
static int fixture_commit(int fd){if(fail_sync){errno=EIO;return -1;}return _commit(fd);}
#define LockFileEx fixture_lock
#define WriteFile fixture_write
#define _commit fixture_commit
#include "../../runtime/nvn/vulkan/shaders.c"
#undef LockFileEx
#undef WriteFile
#undef _commit
#include <assert.h>
VkG g_vk;
const char smaa_hlsl_src[]="";
void rt_fatal_trace(void){abort();}
void vk_device_fault_report(void){abort();}
static const char fragment[]="#version 450\nlayout(location=0) out vec4 c;void main(){c=vec4(.2,.4,.7,1.);}\n";
static uint32_t *compile(size_t *n){uint32_t *p=vk_glsl_to_spirv(fragment,STAGE_FRAGMENT,"authored.frag",n);assert(p&&*n>5);return p;}
#include "cache_fixture_path_windows.h"
int wmain(int argc,wchar_t **argv){
    assert(argc==3&&cache_fixture_cwd(argv[2]));char mode[128];assert(WideCharToMultiByte(CP_UTF8,0,argv[1],-1,mode,sizeof mode,NULL,NULL));
    assert(wct_directory("cache")&&wct_directory("cache/spirv"));size_t n=0;uint32_t *a,*b;
    if(!strcmp(mode,"loose-locked")||!strcmp(mode,"loose-repair"))assert(!_putenv_s("XCDE_SPIRV_LOOSE","1"));
    if(!strcmp(mode,"huge-record")){PackEntry e={1,16,UINT32_MAX,0};assert(!pack_read(e,1,&n));return 0;}
    if(!strcmp(mode,"sparse64")){
        assert(pack_open());HANDLE file=(HANDLE)_get_osfhandle(pack_fd);DWORD bytes;
        assert(DeviceIoControl(file,FSCTL_SET_SPARSE,NULL,0,NULL,0,&bytes,NULL));assert(!_chsize_s(pack_fd,0x100001000ull));
        uint32_t data[]={0x07230203,0x00010600,0,1,0};assert(pack_put(0xabcdef,data,5));
        const PackEntry *e=pack_find(0xabcdef);assert(e&&e->off>UINT32_MAX);b=pack_get(0xabcdef,&n);assert(b&&n==5);free(b);
    }else if(!strcmp(mode,"lock-failure")){
        fail_lock=1;uint32_t data[]={0x07230203,0x00010600,0,1,0};assert(!pack_put(0xabcdef,data,5));
        assert(fd_size(pack_fd)==0&&fd_size(idx_fd)==0&&vk_spirv_cache_errors());fail_lock=0;
    }else if(!strcmp(mode,"write-failure-render")){
        fail_write=1;a=compile(&n);assert(a&&vk_spirv_cache_errors());free(a);fail_write=0;
    }else{
        a=compile(&n);uint64_t hash=src_hash(fragment,STAGE_FRAGMENT);
        if(!strcmp(mode,"corrupt-repair")||!strcmp(mode,"structure-repair")){
            const PackEntry *e=pack_find(hash);assert(e);uint32_t bad=0;
            assert(pwrite64_(pack_fd,&bad,4,e->off+(!strcmp(mode,"structure-repair")?20:0))==4);
            assert(!vk_spirv_cached(fragment,STAGE_FRAGMENT));b=compile(&n);free(b);b=compile(&n);free(b);
            assert(vk_spirv_misses==2&&vk_spirv_hits==1);
        }else if(!strcmp(mode,"loose-locked")||!strcmp(mode,"loose-repair")){
            char path[80];snprintf(path,sizeof path,"cache/spirv/%016llx.spv",(unsigned long long)hash);
            if(!strcmp(mode,"loose-locked")){
                FILE *held=wct_read(path,16u*1024*1024);assert(held);cache_store(hash,a,n);assert(vk_spirv_cache_errors());fclose(held);
                b=loose_load(hash,&n);assert(b&&!memcmp(a,b,n*4));free(b);
            }else{
                uint32_t bad[5]={0};assert(wct_write(path,bad,sizeof bad,NULL,16u*1024*1024));
                assert(!vk_spirv_cached(fragment,STAGE_FRAGMENT));b=compile(&n);assert(b&&vk_spirv_misses==2);free(b);
            }
        }else if(!strcmp(mode,"flush-failure")){
            assert(vk_spirv_cache_flush());fail_sync=1;assert(!vk_spirv_cache_flush()&&vk_spirv_cache_errors());fail_sync=0;
        }else{
            assert(!strcmp(mode,"cold-warm"));size_t count;b=compile(&count);assert(count==n&&!memcmp(a,b,n*4));free(b);
            assert(vk_spirv_hits==1&&vk_spirv_misses==1);
        }
        free(a);
    }
    assert(vk_spirv_cache_flush());puts("native shader cache PASS");return 0;
}
