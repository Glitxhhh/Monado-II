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
// Accepted tiny-window boundary: the render LONG edge must reach128. A
// 128x60 input is valid; the retained107x60 Ultra reproducer must fall back.
static inline const char *monado_fsr4_extent(uint32_t rw,uint32_t rh,uint32_t ow,uint32_t oh) {
 if(!rw||!rh||!ow||!oh||rw>ow||rh>oh||ow>3840||oh>2160)return "FSR4 Lite extent unsupported; existing fallback selected";
 if((rw>rh?rw:rh)<128)return "FSR4 Lite input long edge below128; existing fallback selected";
 return NULL;
}
