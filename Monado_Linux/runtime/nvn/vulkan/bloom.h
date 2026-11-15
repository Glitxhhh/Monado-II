// Adapted from Monado-Reborn 3cc9eeb46dc879fe36929d13758754c429b8377b.
// Copyright (c) 2026 Peiikai and Monado contributors. MIT; see docs/UPSTREAM.md.
// [XCDE] T-WATER-BLOOM: dense, energy-normalized reconstruction for XCDE's four-corner bloom filter (guest
// fragment program 0887965a4681061d, exec.c bloom_tent_src). Keeps its authored UV radius and tint interface.
#pragma once
static const char monado_bloom_tent_glsl[] =
    "#version 460\n"
    "layout(location=0) in vec4 v0;\n"
    "layout(location=0) out vec4 c0;\n"
    "layout(set=1,binding=3,std140) uniform CB3 { uvec4 cb3[16]; };\n"
    "layout(set=1,binding=19,std140) uniform Tint { vec4 ltint[2]; };\n"
    "layout(set=1,binding=32) uniform sampler2D t0;\n"
    "void main() {\n"
    " vec2 uv=v0.xy, d=abs(uintBitsToFloat(cb3[1].zw));\n"
    " vec3 c=4.0*texture(t0,uv).rgb;\n"
    " c+=2.0*(texture(t0,uv+vec2(d.x,0)).rgb+texture(t0,uv-vec2(d.x,0)).rgb\n"
    "        +texture(t0,uv+vec2(0,d.y)).rgb+texture(t0,uv-vec2(0,d.y)).rgb);\n"
    " c+=texture(t0,uv+d).rgb+texture(t0,uv-d).rgb\n"
    "   +texture(t0,uv+vec2(d.x,-d.y)).rgb+texture(t0,uv+vec2(-d.x,d.y)).rgb;\n"
    " c0=vec4(c*(1.0/16.0),1.0);\n"
    " if(ltint[0].w>0.0) { float l=dot(c0.rgb,vec3(0.299,0.587,0.114));\n"
    "  c0.rgb=mix(c0.rgb,ltint[0].rgb*pow(l,ltint[1].w)+ltint[1].rgb*l,ltint[0].w); }\n"
    "}\n";
