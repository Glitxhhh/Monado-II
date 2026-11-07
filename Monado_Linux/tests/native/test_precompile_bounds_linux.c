// [XCDE] LINUX-018: exact attribute-name offset overflow reproducer; authored metadata only.
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#ifndef PRECOMPILE_SOURCE
#define PRECOMPILE_SOURCE "../../runtime/nvn/vulkan/precompile.c"
#endif
#include PRECOMPILE_SOURCE
static void word(uint8_t *d,size_t off,uint32_t value) { memcpy(d+off,&value,4); }
int main(void) {
    uint8_t *d=calloc(1,0x200); assert(d);
    word(d,8,0x30);word(d,12,1);word(d,24,0x40);memcpy(d+0x40,"SLCT",4);
    word(d,0x50,1);word(d,0x54,0x30);word(d,0x70,0x40);word(d,0x74,0x100);
    size_t p=0x80;word(d,p+24,0x58);word(d,p+28,1);word(d,p+80,1);word(d,p+84,0x70);
    word(d,p+0x70,UINT32_MAX);word(d,p+0x74,0);
    size_t controls[4];uint32_t masks[4];
    spch_inputs(d,0x200,0,controls,masks,4);
    free(d);puts("attribute UINT32_MAX offset rejected without out-of-bounds access");return 0;
}
