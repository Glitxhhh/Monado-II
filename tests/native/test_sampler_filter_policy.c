#include <assert.h>
#include <stdio.h>
#include "../../Monado_Linux/runtime/nvn/vulkan/sampler_filter_policy.h"
int main(void) {
    for(unsigned choice=0;choice<7;choice++)
      for(unsigned mf=0;mf<7;mf++)for(unsigned mag=0;mag<2;mag++)
       for(unsigned cmp=0;cmp<2;cmp++)for(unsigned red=0;red<3;red++)
        for(unsigned mask=0;mask<8;mask++)for(unsigned levels=1;levels<3;levels++) {
          float a=rt_sampler_anisotropy(choice,mf,mag,cmp,red,mask&1,mask&2,mask&4,levels,16);
          bool eligible=choice>=1&&choice<=4&&(mf==3||mf==5)&&mag==1&&!cmp&&!red&&(mask&1)&&!(mask&6)&&levels==2;
          assert(a==(eligible?(float)(1u<<choice):0.f));
        }
    assert(rt_sampler_anisotropy(4,5,1,0,0,true,false,false,5,8)==8);
    assert(rt_sampler_anisotropy(4,5,1,0,0,true,false,false,5,NAN)==0);
    assert(rt_sampler_anisotropy(4,5,1,0,0,true,false,false,5,1)==0);
    puts("sampler policy: PASS");
}
