// [XCDE] Native local variant stream. Unknown/truncated files are preserved, never silently truncated.
#pragma once
#include "../../platform/windows_cache_transaction.h"
static bool variant_cache_dimensions_native(uint32_t colors,uint32_t samples,uint8_t topology,
    uint32_t streams,uint32_t attributes,const uint32_t bindings[32]) {
    if(colors>8||!samples||samples>64||(samples&(samples-1))||topology>2||(streams&~0x1ffffu))return false;
    for(unsigned i=0;i<32;i++)if((attributes&(1u<<i))&&bindings[i]>=17)return false;
    return true;
}
static FILE *variant_cache_open_native(const char *path,size_t record_size,WsGuard *guard) {
    if(!record_size||record_size>1024*1024||wct_guard_open(guard,path))return NULL;
    HANDLE h=wct_owned_file(guard->path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,OPEN_ALWAYS);
    LARGE_INTEGER extent;uint32_t header[2]={0x56435843u,(uint32_t)record_size};DWORD count;
    if(h==INVALID_HANDLE_VALUE)goto fail;
    if(!GetFileSizeEx(h,&extent)||extent.QuadPart<0||extent.QuadPart>256ll*1024*1024)goto fail;
    if(!extent.QuadPart){
        if(!WriteFile(h,header,sizeof header,&count,NULL)||count!=sizeof header||!FlushFileBuffers(h))goto fail;
    } else {
        uint32_t existing[2];
        if(extent.QuadPart<8||(extent.QuadPart-8)%record_size||!ReadFile(h,existing,sizeof existing,&count,NULL)||
            count!=sizeof existing||memcmp(header,existing,sizeof header))goto fail;
    }
    {int fd=_open_osfhandle((intptr_t)h,O_RDWR|O_BINARY);if(fd<0)goto fail;
        FILE *f=_fdopen(fd,"r+b");if(!f){_close(fd);ws_guard_close(guard);return NULL;}
        if(_fseeki64(f,8,SEEK_SET)){fclose(f);ws_guard_close(guard);return NULL;}return f;}
fail:
    if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);ws_guard_close(guard);return NULL;
}
