// [XCDE] LINUX-099: synthetic counter ABI/error fixture, no Vulkan or hardware.
#include "../../Monado_Linux/runtime/nvn/vulkan/present_observer_linux.h"
#include <math.h>
#include <string.h>
#ifndef MISSING_SYMBOL
static unsigned mode;
void authored_configure(unsigned value){mode=value;}
int monado_present_observer_snapshot(uint64_t now,MonadoPresentObserverSnapshot *out,uint32_t bytes){
 if(!out||bytes!=sizeof *out)return 0;memset(out,0,sizeof *out);out->abi=MONADO_PRESENT_OBSERVER_ABI;out->bytes=sizeof *out;out->flags=MONADO_PRESENT_OBSERVER_ATTACHED|MONADO_PRESENT_OBSERVER_SEEN|MONADO_PRESENT_OBSERVER_READY;out->output_fps=227.4;
 out->accepted=701;out->failed=4;out->window_ns=500000000;out->last_accepted_ns=now?now-1:0;
 switch(mode){case 1:return 0;case 2:out->abi++;break;case 3:out->bytes--;break;case 4:out->reserved=1;break;case 5:out->flags=0;break;case 6:out->flags|=MONADO_PRESENT_OBSERVER_FAULT;break;case 7:out->flags|=MONADO_PRESENT_OBSERVER_SATURATED;break;case 8:out->output_fps=NAN;break;case 9:out->output_fps=-1;break;case 10:out->output_fps=1e10;break;case 11:out->flags=MONADO_PRESENT_OBSERVER_ATTACHED;out->output_fps=0;break;}
 return 1;
}
#else
int authored_fixture_no_counter(void){return 1;}
#endif
