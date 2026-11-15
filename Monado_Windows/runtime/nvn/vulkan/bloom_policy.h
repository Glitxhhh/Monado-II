// [XCDE] LINUX-103: exact recognized bloom passes only; source extent checked.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "bloom.h"
#include "bloom_prefilter.h"
static inline unsigned bloom_kind_from_hash(uint64_t hash) {
    return hash==UINT64_C(0x2aa23b65117ab1a5)?1:hash==UINT64_C(0x0887965a4681061d)?2:0;
}
static inline unsigned bloom_kind(const uint8_t *code,size_t size) {
    if(!code || size<0x230)return 0;
    uint64_t hash=UINT64_C(1469598103934665603);
    for(size_t i=0x30;i<0x230;i++){hash^=code[i];hash*=UINT64_C(0x100000001b3);}
    return bloom_kind_from_hash(hash);
}
static inline unsigned bloom_mode(unsigned kind,int mode) {return kind && mode>=1 && mode<=2?(unsigned)mode:0;}
static inline uint64_t bloom_cache_identity(uint64_t original,unsigned mode) {
    return mode>=1 && mode<=2?(original^mode)*UINT64_C(0x100000001b3):original;
}
static inline const char *bloom_source(unsigned kind,unsigned mode,const char *original) {
    if(!bloom_mode(kind,(int)mode))return original;
    if(kind==2)return monado_bloom_tent_glsl;
    return mode==2?monado_bloom_prefilter_karis_glsl:monado_bloom_prefilter_box_glsl;
}
