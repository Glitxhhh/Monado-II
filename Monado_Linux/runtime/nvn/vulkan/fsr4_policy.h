// [XCDE] LINUX-103: requirements from the audited portable Lite SPIR-V graph.
// No VALVE mixed-dot, FP8/matrix, Int8 or compute-derivative feature is required.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct {
 bool float16,int16,integer_dot,storage_write,compute_subgroup,quad,denorm16,push_descriptor;
 uint32_t push_count,workgroup_invocations,workgroup_x;uint64_t storage_range;
} MonadoFsr4Caps;
static inline const char *monado_fsr4_missing(const MonadoFsr4Caps *c) {
 if(!c->float16)return "shaderFloat16 unavailable";
 if(!c->int16)return "shaderInt16 unavailable";
 if(!c->integer_dot)return "shaderIntegerDotProduct unavailable";
 if(!c->storage_write)return "storage image writes without format unavailable";
 if(!c->compute_subgroup || !c->quad)return "compute subgroup quad operations unavailable";
 if(!c->denorm16)return "FP16 denormal preservation unavailable";
 if(!c->push_descriptor || c->push_count<11)return "11 push descriptors unavailable";
 if(c->workgroup_invocations<256 || c->workgroup_x<256)return "256-invocation compute workgroups unavailable";
 if(c->storage_range<UINT64_C(99820992))return "FSR4 Lite scratch storage range unavailable";
 return NULL;
}
