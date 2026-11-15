// [XCDE] LINUX-103: actual replacement GLSL and translator instruction tests, authored inputs.
#define main legacy_texture_main
#include "test_translate_tex.c"
#undef main
#include <assert.h>
#include <math.h>
#include <shaderc/shaderc.h>
#include "../../runtime/nvn/vulkan/bloom_policy.h"
#include "../../runtime/nvn/maxwell/scaled_gather.h"
static shaderc_compiler_t compiler;
static void compile(const char *name,const char *source,int vertex,const char *output){
 shaderc_compile_options_t options=shaderc_compile_options_initialize();assert(options);shaderc_compile_options_set_target_env(options,shaderc_target_env_vulkan,shaderc_env_version_vulkan_1_3);
 shaderc_compile_options_set_optimization_level(options,shaderc_optimization_level_performance);
 shaderc_compilation_result_t result=shaderc_compile_into_spv(compiler,source,strlen(source),vertex?shaderc_vertex_shader:shaderc_fragment_shader,name,"main",options);assert(result);
 if(shaderc_result_get_compilation_status(result)!=shaderc_compilation_status_success){fprintf(stderr,"%s: %s\n",name,shaderc_result_get_error_message(result));abort();}
 if(output){char path[4096];assert(snprintf(path,sizeof path,"%s/%s.spv",output,name)<(int)sizeof path);FILE*f=fopen(path,"wb");assert(f);size_t n=shaderc_result_get_length(result);assert(fwrite(shaderc_result_get_bytes(result),1,n,f)==n);assert(!fclose(f));}
 shaderc_result_release(result);shaderc_compile_options_release(options);
}
static ShaderInfo *variant(uint64_t instruction,int stage,uint32_t integer) {
 unsigned char blob[0x200]={0};uint64_t code[4]={0,instruction,EXIT_W,NOP_W};memcpy(blob+(stage==STAGE_COMPUTE?0:0x80),code,sizeof code);
 ShaderKey key={0};key.glsl=1;key.tex_uint=integer;return maxwell_translate(blob,sizeof blob,stage,&key);
}
int main(int argc,char **argv){
 assert(argc==2);assert(legacy_texture_main()==0);compiler=shaderc_compiler_initialize();assert(compiler);
 // Matching is bounded to immutable owned shader bytes, with exact positive hash IDs.
 unsigned char unknown[0x230]={0};assert(!bloom_kind(NULL,0));assert(!bloom_kind((void*)1,0x22f));assert(!bloom_kind(unknown,sizeof unknown));
 assert(bloom_kind_from_hash(UINT64_C(0x2aa23b65117ab1a5))==1 && bloom_kind_from_hash(UINT64_C(0x0887965a4681061d))==2);assert(!bloom_kind_from_hash(UINT64_C(0x0887965a4681061c)));
 uint64_t identities[3];for(unsigned i=0;i<3;i++)identities[i]=bloom_cache_identity(UINT64_C(0xcbf29ce484222325),i);
 assert(identities[0]==UINT64_C(0xcbf29ce484222325) && identities[0]!=identities[1] && identities[1]!=identities[2]);for(unsigned i=0;i<1000;i++)assert(bloom_cache_identity(UINT64_C(0xcbf29ce484222325),i%3)==identities[i%3]);
 const char *original="unchanged";for(int mode=-1;mode<=3;mode++){assert(bloom_source(0,mode,original)==original);if(mode<1||mode>2)assert(bloom_source(1,mode,original)==original);}
 assert(bloom_source(1,1,original)==monado_bloom_prefilter_box_glsl);assert(bloom_source(1,2,original)==monado_bloom_prefilter_karis_glsl);assert(bloom_source(2,1,original)==monado_bloom_tent_glsl);
 compile("bloom_box.frag",monado_bloom_prefilter_box_glsl,0,argv[1]);compile("bloom_karis.frag",monado_bloom_prefilter_karis_glsl,0,argv[1]);compile("bloom_tent.frag",monado_bloom_tent_glsl,0,argv[1]);
 const char *vertex="#version 460\nlayout(location=0)out vec4 v0;void main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);v0=vec4(p,0,1);gl_Position=vec4(p*2.-1.,0,1);}";
 compile("renderer.vert",vertex,1,argv[1]);
 compile("feedback.frag","#version 460\nlayout(set=1,binding=32)uniform sampler2D t0;layout(location=0)in vec4 v0;layout(location=1)out vec4 c1;void main(){c1=texture(t0,v0.xy);}",0,argv[1]);
 char gathered[12000];snprintf(gathered,sizeof gathered,"#version 460\nlayout(set=1,binding=32)uniform sampler2D t0;layout(set=1,binding=3,std140)uniform Params{vec4 scale;};layout(location=0)in vec4 v0;layout(location=0)out vec4 c0;\n%s\nvoid main(){c0=rgather(t0,v0.xy,scale.xy,0);}",scaled_gather_glsl);compile("gather.frag",gathered,0,argv[1]);
 // Complete instruction translations take the new path only for the precise simple form.
 for(int comp=0;comp<4;comp++){
  ShaderInfo*s=translate(tld4s(0,8,9,4,3,comp,0,0,0,0));assert(s&&s->msl&&s->uses_rscale&&strstr(s->msl,"rgather(t0,"));compile("translated_gather.frag",s->msl,0,NULL);maxwell_free(s);
  s=translate(tld4(0,8,9,3,comp,0,0,1,15));assert(s&&s->msl&&s->uses_rscale&&strstr(s->msl,"rgather(t0,"));compile("translated_tld4.frag",s->msl,0,NULL);maxwell_free(s);
 }
 for(int mode=1;mode<=2;mode++){ShaderInfo*s=translate(tld4(0,8,9,3,0,mode,0,1,15));assert(s&&s->msl&&!strstr(s->msl,"rgather("));maxwell_free(s);}
 ShaderInfo*s=translate(tld4(0,8,9,3,0,0,1,1,15));assert(s&&s->msl&&!strstr(s->msl,"rgather("));maxwell_free(s);
 for(int stage=STAGE_VERTEX;stage<=STAGE_COMPUTE;stage++) {
  if(stage!=STAGE_VERTEX && stage!=STAGE_COMPUTE)continue;
  s=variant(tld4s(0,8,9,4,8,0,0,0,0,0),stage,0);assert(s&&s->msl&&!strstr(s->msl,"rgather("));maxwell_free(s);
 }
 s=variant(tld4s(0,8,9,4,8,0,0,0,0,0),STAGE_FRAGMENT,1);assert(s&&s->msl&&s->tex[0].is_uint&&!strstr(s->msl,"rgather("));compile("integer_gather.frag",s->msl,0,NULL);maxwell_free(s);
 shaderc_compiler_release(compiler);puts("PASS actual bloom GLSL compile/exact selection/mode identities and precise TLD4/TLD4S instruction variants");return 0;
}
