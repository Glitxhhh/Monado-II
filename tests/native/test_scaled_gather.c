// Windows parity: authored complete Maxwell instruction translation, no game/GPU.
#include "nvn/maxwell/shader.h"
#include "nvn/maxwell/scaled_gather.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define EXIT_W UINT64_C(0xe30000000007000f)
#define NOP_W UINT64_C(0x50b0000000070f00)
static uint64_t tld4s(int comp) {
    return UINT64_C(0xdf00000000000000) | (UINT64_C(3)<<36) | (UINT64_C(4)<<28) |
           (UINT64_C(9)<<20) | (UINT64_C(7)<<16) | (UINT64_C(8)<<8) | ((uint64_t)comp<<52);
}
static uint64_t tld4(int comp,int offmode,int dc) {
    return UINT64_C(0xc838000000000000) | ((uint64_t)comp<<56) | ((uint64_t)offmode<<54) |
           ((uint64_t)dc<<50) | (UINT64_C(3)<<36) | (UINT64_C(15)<<31) |
           (UINT64_C(1)<<29) | (UINT64_C(9)<<20) | (UINT64_C(7)<<16) | (UINT64_C(8)<<8);
}
static ShaderInfo *translate(uint64_t instruction,int stage,unsigned integer) {
    unsigned char blob[0x200]={0};
    uint64_t code[4]={0,instruction,EXIT_W,NOP_W};
    memcpy(blob+(stage==STAGE_COMPUTE?0:0x80),code,sizeof code);
    ShaderKey key={0};key.glsl=1;key.tex_uint=integer;
    return maxwell_translate(blob,sizeof blob,stage,&key);
}
int main(void) {
    for(int component=0;component<4;component++) {
        ShaderInfo *s=translate(tld4s(component),STAGE_FRAGMENT,0);
        assert(s && s->msl && s->uses_rscale && strstr(s->msl,"rgather(t0,"));
        assert(strstr(s->msl,"rscale[1].xy") && s->ntex==1);maxwell_free(s);
        s=translate(tld4(component,0,0),STAGE_FRAGMENT,0);
        assert(s && s->msl && s->uses_rscale && strstr(s->msl,"rgather(t0,"));maxwell_free(s);
    }
    for(int mode=1;mode<=2;mode++) {
        ShaderInfo *s=translate(tld4(0,mode,0),STAGE_FRAGMENT,0);
        assert(s && s->msl && !strstr(s->msl,"rgather("));maxwell_free(s);
    }
    ShaderInfo *s=translate(tld4(0,0,1),STAGE_FRAGMENT,0);
    assert(s && s->msl && !strstr(s->msl,"rgather("));maxwell_free(s);
    int stages[]={STAGE_VERTEX,STAGE_COMPUTE};
    for(unsigned i=0;i<sizeof stages/sizeof *stages;i++) {
        s=translate(tld4s(0),stages[i],0);
        assert(s && s->msl && !strstr(s->msl,"rgather("));maxwell_free(s);
    }
    s=translate((tld4s(0) & ~(UINT64_C(8191)<<36)) | (UINT64_C(8)<<36),STAGE_FRAGMENT,1);
    assert(s && s->msl && s->tex[0].is_uint && !strstr(s->msl,"rgather("));maxwell_free(s);
    // Source-level guard preserves the native gather base-level/minLod contract.
    // This fixture does not establish GLSL compilation, pixels or physical GPU behavior.
    assert(strstr(scaled_gather_glsl,"sc == vec2(1.0)") && !strstr(scaled_gather_glsl,"textureLod"));
    assert(strstr(scaled_gather_glsl,"native_gather(t,p0,component).w") &&
           strstr(scaled_gather_glsl,"native_gather(t,p3,component).w"));
    puts("authored native scaled gather translator eligibility/component contracts: PASS");
    return 0;
}
