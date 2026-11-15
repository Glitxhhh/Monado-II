// [XCDE] LINUX-103: Monado-Reborn MIT synthetic fixture adapted, commit3cc9eeb4.
// T-PROP-LOD: synthetic material groups, no game data required.
#include "../../Monado_Linux/runtime/prop_lod.h"
#include "../../Monado_Linux/runtime/graphics_options.h"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
static void u32(uint8_t *p,uint32_t v){memcpy(p,&v,4);}
static void f32(uint8_t *p,float v){memcpy(p,&v,4);}
static float getf(uint8_t *p){float v;memcpy(&v,p,4);return v;}
static void fixture(uint8_t *b) {
    memset(b,0,0x8000);u32(b,0x50000);u32(b+12,0x100);u32(b+16,0x1000);u32(b+24,0x3000);
    u32(b+0x100+28,0x80);u32(b+0x100+32,6);
    u32(b+0x1000,0x40);u32(b+0x1004,6);
    const char *name[]={"tree_trunk_high","tree_trunk_low","bridge_wall_high","bridge_wall_low","mystery_high","mystery_low"};
    for(unsigned i=0;i<6;i++) {
        u32(b+0x180+i*68,0x600+i*48);u32(b+0x180+i*68+4,1);
        uint16_t mat=i;memcpy(b+0x700+i*48+14,&mat,2);
        u32(b+0x1040+i*116,0x1000+i*64);strcpy((char *)b+0x2000+i*64,name[i]);
    }
    u32(b+0x3004,3);u32(b+0x3008,0x80);u32(b+0x300c,6);u32(b+0x3010,0x100);
    for(unsigned i=0;i<3;i++) {
        u32(b+0x3080+i*8,i*2);u32(b+0x3084+i*8,2);
        for(unsigned j=0;j<2;j++) {unsigned at=0x3100+(i*2+j)*12;f32(b+at+4,j?1000:100);u32(b+at+8,i*2+j);}
    }
}
int main(int argc,char **argv) {
    uint8_t *b=malloc(0x8000),*original=malloc(0x8000);assert(b&&original);fixture(original);
    for(int i=0;i<3;i++) {
        float scale=(float[]){.5f,1.5f,2}[i];memcpy(b,original,0x8000);
        assert(prop_lod_adjust(b,0x8000,scale)==1);assert(getf(b+0x3104)==100*scale);
        for(unsigned j=0;j<0x8000;j++)if(j<0x3104 || j>=0x3108)assert(b[j]==original[j]);
    }
    memcpy(b,original,0x8000);u32(b+24,0xfffffff0);assert(!prop_lod_adjust(b,0x8000,.5f));
    memcpy(b,original,0x8000);u32(b+0x3084,0xffffffff);assert(!prop_lod_adjust(b,0x8000,.5f));
    memcpy(b,original,0x8000);u32(b+0x3004,4);u32(b+0x3098,0);u32(b+0x309c,2);assert(!prop_lod_adjust(b,0x8000,.5f)); // shared LODs untouched
    memcpy(b,original,0x8000);f32(b+0x3104,NAN);assert(!prop_lod_adjust(b,0x8000,.5f));
    for(size_t size=0;size<0x3120;size+=17) {memcpy(b,original,0x8000);prop_lod_adjust(b,size,.5f);}
    free(b);free(original);
    puts("Selective LOD bounds/final-distance preservation/malformed resources PASS");
}
