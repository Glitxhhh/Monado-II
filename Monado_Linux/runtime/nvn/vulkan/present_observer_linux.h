// [XCDE] LINUX-099: authored read-only ABI for the local downstream Vulkan layer.
// Output measures accepted presentation requests below NVIDIA's implicit layer,
// not physical scanout. No generated/native classification is inferred.
#pragma once
#include <stdint.h>
#define MONADO_PRESENT_OBSERVER_ABI 1u
#define MONADO_PRESENT_OBSERVER_LAYER "VK_LAYER_MONADO_present_observer"
#define MONADO_PRESENT_OBSERVER_RESOURCE "libmonado-present-observer.so"
#define MONADO_PRESENT_OBSERVER_SYMBOL "monado_present_observer_snapshot"
enum {
    MONADO_PRESENT_OBSERVER_ATTACHED = 1u << 0, // device/swapchain attachment seen
    MONADO_PRESENT_OBSERVER_SEEN = 1u << 1,     // at least one accepted output
    MONADO_PRESENT_OBSERVER_READY = 1u << 2,    // genuine complete rolling window
    MONADO_PRESENT_OBSERVER_SATURATED = 1u << 3,// event rate exceeded bounded ring
    MONADO_PRESENT_OBSERVER_FAULT = 1u << 4    // bookkeeping allocation failed
};
typedef struct MonadoPresentObserverSnapshot {
    uint32_t abi, bytes, flags, reserved;
    uint64_t accepted, failed, last_accepted_ns, window_ns;
    double output_fps;
} MonadoPresentObserverSnapshot;
typedef int (*MonadoPresentObserverSnapshotFn)(uint64_t now_ns,
    MonadoPresentObserverSnapshot *out, uint32_t bytes);
#ifdef __cplusplus
extern "C" {
#endif
int monado_present_observer_snapshot(uint64_t now_ns,
    MonadoPresentObserverSnapshot *out, uint32_t bytes);
#ifdef __cplusplus
}
#endif
