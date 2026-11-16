// [XCDE] LINUX-018: checked precompile child/supervisor wire protocol. Values are single ASCII tokens.
#pragma once
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#define PRECOMPILE_WORK_MAGIC "monado_precompile_work 1"
#define PRECOMPILE_CANCEL_MAGIC "monado_precompile_cancel 1"
#define PRECOMPILE_WORK_PATH "cache/precompile_work.txt"
#define PRECOMPILE_CANCEL_PREFIX "cache/precompile_cancel-"
#define PRECOMPILE_ID_ENV "XCDE_PRECOMPILE_ID"
#define PRECOMPILE_SESSION_ENV "XCDE_PRECOMPILE_SESSION"
// Work fields: session, identity, phase, state; decimal files_total/files_done/shaders/programs/programs_done,
// seed_total/seed_done/variants_total/variants_done/attempted/built/failed/skipped/unsupported/errors,
// gpu_vendor/gpu_device/gpu_driver; gpu_cache_uuid is32hex. Unknown future fields may be ignored.
// phase: scan|build|seed|pipelines|done. state: running|complete|partial|cancelled|failed.
// Cancel file: first PRECOMPILE_CANCEL_MAGIC, then "session <current32hex>". Stale sessions never cancel work.
static inline int precompile_hex(const char *s, size_t n) {
    if (!s || strlen(s) != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return 0;
    return 1;
}
// Bounded strict known fields; future token fields are ignored. No duplicate or partial proof is accepted.
static inline int precompile_work_valid(const char *bytes,size_t size) {
    static const char *names[]={"session","identity","phase","state","gpu_cache_uuid",
        "files_total","files_done","shaders","programs","programs_done","seed_total","seed_done",
        "variants_total","variants_done","attempted","built","failed","skipped","unsupported","errors",
        "gpu_vendor","gpu_device","gpu_driver"};
    size_t prefix=sizeof(PRECOMPILE_WORK_MAGIC "\n")-1;
    if(!bytes||size>4096||size<=prefix||memcmp(bytes,PRECOMPILE_WORK_MAGIC "\n",prefix)||bytes[size-1]!='\n')return 0;
    uint32_t seen=0;unsigned rows=0;
    for(size_t offset=prefix;offset<size;){
        if(++rows>64)return 0;size_t end=offset;while(end<size&&bytes[end]!='\n')end++;
        if(end==size||end-offset>127)return 0;char line[128];memcpy(line,bytes+offset,end-offset);line[end-offset]=0;
        char *value=strchr(line,' ');if(!value||value==line||!value[1])return 0;*value++=0;
        for(const char *p=line;*p;p++)if(!((*p>='a'&&*p<='z')||*p=='_'))return 0;
        for(const char *p=value;*p;p++)if(*p<=32||*p>=127)return 0;
        for(unsigned i=0;i<sizeof names/sizeof names[0];i++)if(!strcmp(line,names[i])){
            if(seen&(1u<<i))return 0;seen|=1u<<i;
            if(i==0||i==1||i==4){if(!precompile_hex(value,i==1?64:32))return 0;}
            else if(i==2){if(strcmp(value,"scan")&&strcmp(value,"build")&&strcmp(value,"seed")&&strcmp(value,"pipelines")&&strcmp(value,"done"))return 0;}
            else if(i==3){if(strcmp(value,"running")&&strcmp(value,"complete")&&strcmp(value,"partial")&&strcmp(value,"cancelled")&&strcmp(value,"failed"))return 0;}
            else {uint64_t n=0;for(const char *p=value;*p;p++){if(*p<'0'||*p>'9'||n>(UINT64_MAX-(unsigned)(*p-'0'))/10)return 0;n=n*10+(unsigned)(*p-'0');}}
        }
        offset=end+1;
    }
    return seen==((1u<<23)-1);
}
