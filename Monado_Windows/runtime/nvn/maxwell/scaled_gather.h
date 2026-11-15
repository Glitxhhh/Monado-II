// [XCDE] LINUX-103: adapted from Monado-Reborn 3cc9eeb46dc879fe36929d13758754c429b8377b.
// Copyright (c) 2026 Peiikai and Monado contributors. MIT; see docs/UPSTREAM.md.
#pragma once
// Included as a C string in generated GLSL for scaled guest-texel gathers.
// Each component samples the centre of a guest texel, preserving the gather
// ordering (bottom-left, bottom-right, top-right, top-left) after scaling.
// Native gather scalar taps also preserve its base-level behavior when the
// sampler has minLod>0. textureLod(0) would clamp to that other mip instead.
static const char scaled_gather_glsl[] =
"vec4 native_gather(sampler2D t, vec2 uv, int component) {\n"
" if (component == 0) return textureGather(t, uv, 0);\n"
" if (component == 1) return textureGather(t, uv, 1);\n"
" if (component == 2) return textureGather(t, uv, 2);\n"
" return textureGather(t, uv, 3);\n"
"}\n"
"vec4 rgather(sampler2D t, vec2 uv, vec2 sc, int component) {\n"
" if (any(isnan(sc)) || any(isinf(sc)) || any(lessThanEqual(sc,vec2(0.0))) || sc == vec2(1.0)) return native_gather(t,uv,component);\n"
" vec2 size = vec2(textureSize(t, 0));\n"
" vec2 base = floor(uv * size / sc - 0.5);\n"
" vec2 p0 = (floor((base + vec2(0.5, 1.5)) * sc) + 0.5) / size;\n"
" vec2 p1 = (floor((base + vec2(1.5, 1.5)) * sc) + 0.5) / size;\n"
" vec2 p2 = (floor((base + vec2(1.5, 0.5)) * sc) + 0.5) / size;\n"
" vec2 p3 = (floor((base + vec2(0.5, 0.5)) * sc) + 0.5) / size;\n"
" return vec4(native_gather(t,p0,component).w, native_gather(t,p1,component).w,\n"
"             native_gather(t,p2,component).w, native_gather(t,p3,component).w);\n"
"}\n";
