#include <assert.h>
#include <stdio.h>
#include "../../Monado_Linux/runtime/hle/rumble_ipc.h"
static int calls;static uint32_t last;static float low,high;
static void offer(uint32_t h,const float v[4],void *user) {
    assert(user==(void *)123);calls++;last=h;low=v[0];high=v[2];
}
int main(void) {
    uint8_t data[32]={0};uint32_t h=0x03000001;float v[4]={.2f,160,.7f,320};
    memcpy(data,&h,4);memcpy(data+4,v,16);
    IpcReq r={.cmd=201,.data=data,.data_size=32};
    assert(rt_rumble_ipc(&r,offer,(void *)123));assert(calls==1&&last==h&&low==.2f&&high==.7f);
    r.data_size=19;assert(!rt_rumble_ipc(&r,offer,(void *)123));assert(calls==1);
    uint32_t hh[2]={0x03000000,0x03000001};float vv[2][4]={{.3f,160,0,320},{0,160,.4f,320}};
    r=(IpcReq){.cmd=206,.nx=2,.x={{(uintptr_t)hh,sizeof hh},{(uintptr_t)vv,sizeof vv}}};
    assert(rt_rumble_ipc(&r,offer,(void *)123));assert(calls==3&&last==hh[1]&&low==0&&high==.4f);
    r.x[1].size=16;assert(!rt_rumble_ipc(&r,offer,(void *)123));assert(calls==3);
    r.x[1].size=32;r.x[0].size=7;assert(!rt_rumble_ipc(&r,offer,(void *)123));
    r.x[0].size=8;r.x[0].addr=0;assert(!rt_rumble_ipc(&r,offer,(void *)123));
    r.x[0].addr=UINT64_MAX;assert(!rt_rumble_ipc(&r,offer,(void *)123));
    r.x[0].addr=(uintptr_t)hh;r.x[0].size=68;r.x[1].size=272;
    assert(!rt_rumble_ipc(&r,offer,(void *)123));
    r=(IpcReq){.cmd=206,.na=2,.a={{(uintptr_t)hh,sizeof hh},{(uintptr_t)vv,sizeof vv}}};
    assert(rt_rumble_ipc(&r,offer,(void *)123));assert(calls==5);
    r.cmd=202;assert(!rt_rumble_ipc(&r,offer,(void *)123));
    puts("rumble IPC: PASS");
}
