// [XCDE] LINUX-097: independent display/native-cap contract, no game or GPU.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../../Monado_Linux/runtime/nvn/framegen_policy.h"
int main(void) {
    const int rates[] = {30,60,90,120,144,165,180,240,0};
    const float displays[] = {59.94f,60,90,120,144,165,180,240};
    unsigned cases=0;
    for (unsigned i=0;i<sizeof rates/sizeof *rates;i++) {
        for (unsigned j=0;j<sizeof displays/sizeof *displays;j++) {
            int requested=rates[i]; float hz=displays[j];
            assert(fg_policy_base_fps(requested,hz,false)==requested);
            float base=fg_policy_base_fps(requested,hz,true);
            // Two unique outputs must fit the refresh ceiling; a lower native
            // cap is respected, rather than increasing simulation/render work.
            assert(base>0 && base*2<=hz+.001f);
            if(requested>0) assert(base<=requested);
            assert(fabsf(base-(requested>0 && requested*2<hz ? requested : hz/2))<.001f);
            cases++;
        }
        const float unknown[] = {0,-1,1,30,58,NAN,INFINITY,1001};
        for(unsigned j=0;j<sizeof unknown/sizeof *unknown;j++) {
            assert(fg_policy_base_fps(rates[i],unknown[j],true)==rates[i]);cases++;
        }
    }
    assert(fg_policy_base_fps(120,180,true)==90);
    assert(fg_policy_base_fps(60,180,true)==60); // 2x cannot promise 3x.
    printf("PASS refresh policy %u cases\n",cases);
}
