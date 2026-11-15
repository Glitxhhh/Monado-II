#include "nvn/repeat_present_policy_windows.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 assert(monado_windows_repeat_count(false,true,false,179.88f,30.f)==1);
 assert(monado_windows_repeat_count(false,true,false,60.f,30.f)==1);
 assert(monado_windows_repeat_count(true,true,false,179.88f,30.f)==6);
 assert(monado_windows_repeat_count(true,true,false,60.f,30.f)==2);
 assert(monado_windows_repeat_count(true,true,false,120.f,30.f)==4);
 assert(monado_windows_repeat_count(true,false,false,179.88f,30.f)==1);
 assert(monado_windows_repeat_count(true,true,true,179.88f,30.f)==1);
 assert(monado_windows_repeat_count(true,true,false,0.f,30.f)==1);
 assert(monado_windows_repeat_count(true,true,false,179.88f,0.f)==1);
 assert(monado_windows_repeat_count(true,true,false,NAN,30.f)==1);
 assert(monado_windows_repeat_count(true,true,false,INFINITY,30.f)==1);
 assert(monado_windows_repeat_count(true,true,false,60.f,NAN)==1);
 assert(monado_windows_repeat_count(true,true,false,60.f,INFINITY)==1);
 assert(monado_windows_repeat_count(true,true,false,60.f,24.f)==1);
 assert(monado_windows_repeat_count(true,true,false,1000.f,30.f)==1);
 puts("PASS native Windows repeat policy15cases: no nominal-Hz extrapolation by default; explicit legacy diagnostic request bounded; FG/unusable paths single output");return 0;}
