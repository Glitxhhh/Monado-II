// [XCDE] LINUX-046: recorded NVN bindings select stages independently.
#pragma once
#include "nvn.h"

// NVN's runtime stage indices, also used unchanged by uniform/texture bindings:
// vertex0, fragment1, geometry2, tess-control3, tess-evaluation4, compute5.
// These are not VkShaderStageFlagBits (whose ordering differs).
static inline void nvn_program_bind_stages(NvnProgram *bound, const NvnProgram *recorded,
                                           uint32_t stages) {
    stages &= 0x3fu;
    for (unsigned i = 0; i < 6; ++i) {
        uint32_t bit = 1u << i;
        if (!(stages & bit)) continue;
        if (recorded && (recorded->stage_mask & bit)) {
            bound->stages[i] = recorded->stages[i];
            bound->stages[i].backend = NULL;
            bound->stage_mask |= bit;
        } else {
            memset(&bound->stages[i], 0, sizeof bound->stages[i]);
            bound->stage_mask &= ~bit;
        }
    }
    bound->backend = NULL; bound->frozen = NULL;
}
