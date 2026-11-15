#include "ui/fps_provider_counter.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void){FpsProviderCounter m={0};
 assert(!fps_provider_sample(&m,0,100,true));assert(!m.ready);
 assert(fps_provider_sample(&m,500000000,160,true)&&fabs(m.rate-120)<.001);
 assert(fps_provider_sample(&m,1000000000,160,true)&&m.rate==0);
 assert(!fps_provider_sample(&m,1100000000,159,true)&&!m.ready);
 assert(!fps_provider_sample(&m,1000000000,160,true)&&!m.ready);
 assert(!fps_provider_sample(&m,4000000000,170,true)&&!m.ready);
 assert(!fps_provider_sample(&m,4500000000,1000171,true)&&!m.ready);
 assert(!fps_provider_sample(&m,5000000000,1000172,false)&&!m.started);
 puts("PASS provider accepted-total: exact deltas, stalled output, counter/clock/epoch reset, unavailable/absurd count rejection; no generated-rate inference");return 0;}
