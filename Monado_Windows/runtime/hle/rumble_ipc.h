// [XCDE] LINUX-103: bounded Horizon vibration decoding, independently testable.
// Protocol reference: switchbrew/libnx nx/source/services/hid.c (201 and206).
// Reborn single-handle reports informed envelope tests; no service code copied.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "../ipc.h"
typedef void (*RtRumbleOffer)(uint32_t handle, const float value[4], void *user);
static inline bool rt_rumble_ipc(IpcReq *req, RtRumbleOffer offer, void *user) {
    if (req->cmd == 201) {
        if (!req->data || req->data_size < 20) return false;
        uint32_t handle; float value[4];
        memcpy(&handle,req->data,4); memcpy(value,req->data+4,16);
        offer(handle,value,user); return true;
    }
    if (req->cmd != 206) return false;
    IpcBuf *handles=in_buf(req,0), *values=in_buf(req,1);
    if (!handles || !values || handles->size%4 || values->size%16 ||
        handles->size/4 != values->size/16 || handles->size/4>16) return false;
    if (!handles->size) return true;
    if (!handles->addr || !values->addr || handles->addr>UINT64_MAX-handles->size ||
        values->addr>UINT64_MAX-values->size) return false;
    for (unsigned i=0;i<handles->size/4;i++) {
        uint32_t handle;float value[4];
        memcpy(&handle,(const void *)(uintptr_t)(handles->addr+4*i),4);
        memcpy(value,(const void *)(uintptr_t)(values->addr+16*i),16);
        offer(handle,value,user);
    }
    return true;
}
