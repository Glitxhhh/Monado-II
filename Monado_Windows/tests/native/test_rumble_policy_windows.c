// Accepted authored portable protocol fixture against Windows production headers.
// Authored protocol/envelope regressions; never opens a physical controller.
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../../runtime/hle/rumble_policy.h"
int main(void) {
    RtRumble r = {0}; uint16_t out[2];
    float low[4] = {.5f,160,0,320}, high[4] = {0,160,.75f,320};
    assert(rt_rumble_offer(&r,0x03000000,low,1000000000ull));
    assert(rt_rumble_offer(&r,0x03000001,high,1000000000ull));
    assert(rt_rumble_poll(&r,1000000000ull,true,out));
    assert(out[0]==32768 && out[1]==49151);
    assert(!rt_rumble_poll(&r,1001000000ull,true,out));
    assert(rt_rumble_poll(&r,1075000000ull,true,out));
    // Expiry must stop both motors even when no new vibration packet arrives.
    assert(rt_rumble_poll(&r,1100000000ull,true,out));assert(!out[0]&&!out[1]);
    assert(!rt_rumble_offer(&r,0x03000100,low,1200000000ull));
    assert(!rt_rumble_offer(&r,0x03000002,low,1200000000ull));
    assert(rt_rumble_offer(&r,0x03000001,low,1200000000ull)); // single-handle low band
    assert(rt_rumble_poll(&r,1200000000ull,true,out));assert(out[0]&&!out[1]);
    assert(rt_rumble_poll(&r,1201000000ull,false,out));assert(!out[0]&&!out[1]);
    assert(!rt_rumble_poll(&r,1202000000ull,true,out)); // reenable cannot revive stale effect
    low[0]=NAN;low[2]=INFINITY;assert(rt_rumble_offer(&r,0x03000000,low,1300000000ull));
    assert(!rt_rumble_poll(&r,1300000000ull,true,out));
    low[0]=2;low[2]=-1;assert(rt_rumble_offer(&r,0x03000000,low,1400000000ull));
    assert(rt_rumble_poll(&r,1400000000ull,true,out));assert(out[0]==65535&&!out[1]);
    assert(rt_rumble_poll(&r,1,true,out));assert(!out[0]&&!out[1]); // clock rollback
    assert(rt_rumble_amplitude(-1)==0 && rt_rumble_amplitude(INFINITY)==0);
    puts("rumble policy: PASS");
}
