// [XCDE] LINUX-017: the sole shared module linked with NVIDIA's static SDK loader.
// Build with GNU C++ ABI, -fPIC, hidden visibility and a version script exporting
// only the getter. Never link the archive into the host ELF.
#include "dlss_ngx_api.h"

static const MonadoNgxApi api = {
    1,
    sizeof(MonadoNgxApi),
#define MONADO_NGX_ADDRESS(function) &::function,
    MONADO_NGX_FUNCTIONS(MONADO_NGX_ADDRESS)
#undef MONADO_NGX_ADDRESS
};

extern "C" __attribute__((visibility("default")))
const MonadoNgxApi *monado_ngx_api(void) {
    return &api;
}
