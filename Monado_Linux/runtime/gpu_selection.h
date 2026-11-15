// [XCDE] LINUX-103: stable, vendor-neutral GPU preference; no device or file IO.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static inline bool monado_gpu_uuid_normalize(const char *source,char destination[33]) {
    if(!source || !destination)return false;
    if(!*source || !strcmp(source,"auto")){destination[0]=0;return true;}
    if(strlen(source)!=32){destination[0]=0;return false;}
    for(unsigned i=0;i<32;i++) {
        char c=source[i];if(c>='A' && c<='F')c=(char)(c-'A'+'a');
        if(!((c>='0' && c<='9') || (c>='a' && c<='f'))){destination[0]=0;return false;}
        destination[i]=c;
    }
    destination[32]=0;return true;
}
static inline void monado_gpu_uuid_format(const uint8_t source[16],char destination[33]) {
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<16;i++){destination[2*i]=hex[source[i]>>4];destination[2*i+1]=hex[source[i]&15];}
    destination[32]=0;
}
typedef struct { char uuid[33]; bool eligible; unsigned rank; } MonadoGpuChoice;
static inline int monado_gpu_select(const MonadoGpuChoice *choices,size_t count,const char *uuid,int legacy_index) {
    if(!choices)return -1;
    if(legacy_index>=0 && (size_t)legacy_index<count && choices[legacy_index].eligible)return legacy_index;
    if(uuid && *uuid)for(size_t i=0;i<count;i++)if(choices[i].eligible && !strcmp(uuid,choices[i].uuid))return (int)i;
    int best=-1;unsigned score=0;
    for(size_t i=0;i<count;i++)if(choices[i].eligible && (best<0 || choices[i].rank>score)){best=(int)i;score=choices[i].rank;}
    return best;
}
