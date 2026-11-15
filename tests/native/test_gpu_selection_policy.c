// [XCDE] LINUX-103: stable identity, reordered/missing/ineligible GPUs and Auto.
#include <assert.h>
#include <stdio.h>
#include "../../Monado_Linux/runtime/gpu_selection.h"
int main(void){
 char uuid[33];assert(monado_gpu_uuid_normalize("auto",uuid)&&!*uuid);assert(monado_gpu_uuid_normalize("",uuid)&&!*uuid);
 assert(monado_gpu_uuid_normalize("ABCDEF0123456789ABCDEF0123456789",uuid));assert(!strcmp(uuid,"abcdef0123456789abcdef0123456789"));
 const char *bad[]={"f","abcdef0123456789abcdef012345678","abcdef0123456789abcdef01234567890","abcdef0123456789abcdef0123456789z","../device","auto ;comment"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){assert(!monado_gpu_uuid_normalize(bad[i],uuid));assert(!*uuid);}
 uint8_t bytes[16];for(unsigned i=0;i<16;i++)bytes[i]=(uint8_t)(i*17);monado_gpu_uuid_format(bytes,uuid);assert(!strcmp(uuid,"00112233445566778899aabbccddeeff"));
 MonadoGpuChoice devices[3]={ {.uuid="11111111111111111111111111111111",.eligible=true,.rank=2}, {.uuid="22222222222222222222222222222222",.eligible=true,.rank=3}, {.uuid="33333333333333333333333333333333",.eligible=false,.rank=9} };
 assert(monado_gpu_select(devices,3,"",-1)==1);assert(monado_gpu_select(devices,3,devices[0].uuid,-1)==0);
 assert(monado_gpu_select(devices,3,devices[2].uuid,-1)==1);assert(monado_gpu_select(devices,3,"ffffffffffffffffffffffffffffffff",-1)==1);
 assert(monado_gpu_select(devices,3,devices[1].uuid,0)==0);assert(monado_gpu_select(devices,3,devices[0].uuid,2)==0);
 MonadoGpuChoice swapped[3]={devices[1],devices[0],devices[2]};assert(monado_gpu_select(swapped,3,devices[0].uuid,-1)==1);
 devices[0].eligible=devices[1].eligible=false;assert(monado_gpu_select(devices,3,"",-1)==-1);assert(monado_gpu_select(NULL,0,"",-1)==-1);
 puts("GPU stable UUID/Auto/reorder/missing/ineligible/legacy override selection PASS");
}
