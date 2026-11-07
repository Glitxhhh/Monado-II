// NVN entry points: every NVN function is looked up by name and bound to a host stub.
#include "../diag/diag.h"
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

static uint64_t nvn_proc(const char *name) {
    if (!name || !*name) return 0;
    HostFn impl = hle_lookup_impl(name);
    if (getenv("XCDE_TRACE_NVN")) LOG("[nvn] resolve %s -> %s\n", name, impl ? "host" : "UNIMPLEMENTED");
    return hle_register(name, impl);
}

HLE_FUNC("nvnBootstrapLoader", nvn_bootstrap_loader) {
    const char *name = (const char *)(uintptr_t)c->x[0];
    c->x[0] = nvn_proc(name);
}

// void* nvnDeviceGetProcAddress(const NVNdevice*, const char*)
HLE_FUNC("nvnDeviceGetProcAddress", nvn_device_get_proc_address) {
    const char *name = (const char *)(uintptr_t)c->x[1];
    diag_use("nvn", "%s", name);  // [XCDE] T034
    c->x[0] = nvn_proc(name);
}
