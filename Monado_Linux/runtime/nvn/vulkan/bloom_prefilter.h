// Adapted from Monado-Reborn 3cc9eeb46dc879fe36929d13758754c429b8377b.
// Copyright (c) 2026 Peiikai and Monado contributors. MIT; see docs/UPSTREAM.md.
// [XCDE] T-BLOOM-PREFILTER: the bloom chain's first step (full-resolution HDR scene and glow buffer ->
// quarter-resolution bloom source). The game takes four bilinear taps at +-1 authored source texel, which
// tiles the 4x4 texel footprint exactly at native 720p. With render scaling the UV offsets stay the same
// while the source has more texels, so at 2x the taps read only 16 of the 64 texels under the footprint
// and sub-pixel highlights blink in and out of the bloom from frame to frame. This replacement keeps the
// authored footprint, threshold, glow-buffer max, luminance compression and tint, but integrates the whole
// footprint at any render scale (identical taps at native scale). The optional Stable mode weights each tap by
// 1/(1+luma) (Karis average), so one isolated glint cannot dominate its bloom texel ("fireflies") while
// broad highlights, whose taps share similar weights, keep their bloom.
// Launcher/F1 Bloom stability: Original=0, FullFootprint=1, StableKaris=2. Original is the default.
#pragma once
#define MONADO_BLOOM_PREFILTER_BODY \
    "layout(location=0) in vec4 v0;\n" \
    "layout(location=0) out vec4 c0;\n" \
    "layout(set=1,binding=1,std140) uniform CB1 { uvec4 cb1[16]; };\n" \
    "layout(set=1,binding=3,std140) uniform CB3 { uvec4 cb3[16]; };\n" \
    "layout(set=1,binding=19,std140) uniform Tint { vec4 ltint[2]; };\n" \
    "layout(set=1,binding=32) uniform sampler2D t0;\n" \
    "layout(set=1,binding=33) uniform sampler2D t1;\n" \
    "#define K(i,j) uintBitsToFloat(cb3[i][j])\n" \
    "void main() {\n" \
    " vec2 uv=v0.xy;\n" \
    " vec4 ox=vec4(K(3,0),K(3,2),K(4,0),K(4,2)), oy=vec4(K(3,1),K(3,3),K(4,1),K(4,3));\n" \
    " vec2 ctr=uv+vec2(dot(ox,vec4(0.25)),dot(oy,vec4(0.25)));\n" \
    " vec2 d=0.5*vec2(max(max(ox.x,ox.y),max(ox.z,ox.w))-min(min(ox.x,ox.y),min(ox.z,ox.w)),\n" \
    "                 max(max(oy.x,oy.y),max(oy.z,oy.w))-min(min(oy.x,oy.y),min(oy.z,oy.w)));\n" \
    " vec2 fp=4.0*d*vec2(textureSize(t0,0));\n" \
    " ivec2 n=clamp(ivec2(ceil(fp*0.5-0.01)),ivec2(1),ivec2(8));\n" \
    " vec2 st=4.0*d/vec2(n), o=ctr-2.0*d+0.5*st;\n" \
    " vec4 s0=vec4(0.0), s1=vec4(0.0);\n" \
    " float w0=0.0;\n" \
    " for(int y=0;y<n.y;y++) for(int x=0;x<n.x;x++) {\n" \
    "  vec2 p=o+vec2(x,y)*st;\n" \
    "  vec4 a=texture(t0,p);\n" \
    "#ifdef KARIS\n" \
    "  float w=1.0/(1.0+max(0.0,dot(a.rgb*K(1,3),vec3(0.299,0.587,0.114))));\n" \
    "#else\n" \
    "  float w=1.0;\n" \
    "#endif\n" \
    "  s0+=a*w; w0+=w;\n" \
    "  s1+=texture(t1,p);\n" \
    " }\n" \
    " vec4 a=s0*(K(1,3)/w0);\n" \
    " vec4 g=s1*(1.0/float(n.x*n.y));\n" \
    " vec3 b=max(a.rgb-K(1,0),0.0);\n" \
    " float f=(b.r+b.g+b.b)*K(1,1);\n" \
    " vec3 c=min(a.rgb,a.rgb*f)*K(2,3);\n" \
    " float al=min(a.a,a.a*f)*K(2,3);\n" \
    " vec3 m=max(g.rgb*(4.0*K(1,2)),c);\n" \
    " float l=fma(m.b,uintBitsToFloat(cb1[0].y),fma(m.g,uintBitsToFloat(cb1[0].x),m.r*0.29899999));\n" \
    " c0=vec4(m*(1.0/fma(l,K(1,3),1.0)),max(al,g.a));\n" \
    " if(ltint[0].w>0.0) { float q=dot(c0.rgb,vec3(0.299,0.587,0.114));\n" \
    "  c0.rgb=mix(c0.rgb,ltint[0].rgb*pow(q,ltint[1].w)+ltint[1].rgb*q,ltint[0].w); }\n" \
    "}\n"

static const char monado_bloom_prefilter_karis_glsl[] = "#version 460\n#define KARIS 1\n" MONADO_BLOOM_PREFILTER_BODY;
static const char monado_bloom_prefilter_box_glsl[] = "#version 460\n" MONADO_BLOOM_PREFILTER_BODY;
