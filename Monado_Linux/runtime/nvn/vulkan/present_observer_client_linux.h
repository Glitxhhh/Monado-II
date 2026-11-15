// [XCDE] LINUX-099: optional receipt-bound downstream counter client, MIT.
// Only the explicitly selected driver method requests it. Measurement also feeds
// local diagnostics when the optional on-screen display is disabled.
#pragma once
#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "present_observer_linux.h"
static MonadoPresentObserverSnapshotFn observer_client_snapshot;
static void *observer_client_module;
static bool observer_client_attempted, observer_client_enabled;
static bool observer_client_begin(bool requested) {
    if (!requested) return false;
    const char *controlled = getenv("MONADO_PRESENT_OBSERVER");
    if (!controlled || strcmp(controlled,"1")) return false;
    if (observer_client_attempted) return observer_client_enabled;
    observer_client_attempted = true;
    char path[4096];
    if (!rt_resource(MONADO_PRESENT_OBSERVER_RESOURCE,NULL,path,sizeof path)) {
        LOG("[fg-counter] verified observer companion unavailable; driver output unknown\n");return false;
    }
    observer_client_module=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if (!observer_client_module) {
        LOG("[fg-counter] observer companion could not load; driver output unknown\n");return false;
    }
    void *symbol=dlsym(observer_client_module,MONADO_PRESENT_OBSERVER_SYMBOL);
    if (!symbol || sizeof symbol != sizeof observer_client_snapshot) {
        LOG("[fg-counter] observer ABI unavailable; driver output unknown\n");return false;
    }
    memcpy(&observer_client_snapshot,&symbol,sizeof observer_client_snapshot);
    observer_client_enabled=true;
    LOG("[fg-counter] downstream presentation measurement requested; generated identity unavailable\n");
    return true;
}
static void observer_client_disable(void) {
    // Vulkan's loader may still own this module. Never unload its dispatch code.
    observer_client_enabled=false;
}
static bool observer_client_read(uint64_t now,MonadoPresentObserverSnapshot *out) {
    if(!out)return false;
    memset(out,0,sizeof *out);
    if(!observer_client_enabled||!observer_client_snapshot)return false;
    MonadoPresentObserverSnapshot snapshot={0};
    if(!observer_client_snapshot(now,&snapshot,sizeof snapshot)||snapshot.abi!=MONADO_PRESENT_OBSERVER_ABI||snapshot.bytes!=sizeof snapshot||snapshot.reserved)return false;
    // Preserve ABI-qualified error counters/flags for diagnostics even when a
    // rate cannot be displayed. The bool alone qualifies measurement readiness.
    *out=snapshot;
    if(!(snapshot.flags&MONADO_PRESENT_OBSERVER_ATTACHED)||(snapshot.flags&(MONADO_PRESENT_OBSERVER_FAULT|MONADO_PRESENT_OBSERVER_SATURATED)))return false;
    if(!isfinite(snapshot.output_fps)||snapshot.output_fps<0||snapshot.output_fps>1e9)return false;
    return true;
}
static bool observer_client_rates(uint64_t now,double *rate,bool *ready) {
    if(!rate||!ready)return false;
    *rate=0;*ready=false;
    MonadoPresentObserverSnapshot snapshot;
    if(!observer_client_read(now,&snapshot))return false;
    *rate=snapshot.output_fps;*ready=(snapshot.flags&MONADO_PRESENT_OBSERVER_READY)!=0;
    return true;
}
