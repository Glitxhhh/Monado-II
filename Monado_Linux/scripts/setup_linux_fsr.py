#!/usr/bin/env python3
"""[XCDE] LINUX-020: pinned native offscreen FidelityFX FG; never runs vendor/game code.

Only audited source/GLSL/license files are downloaded with explicit --fetch.
Builds use the existing project Clang/shaderc/SPIRV-Reflect, without CMake,
Windows executables, SDK media, driver updates or an external provider updater.
The SDK's Windows swapchain and upscaler are deliberately not registered.
"""
import argparse
import concurrent.futures
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'fsr-linux.lock.json'
NAME = 'libmonado-fidelityfx.so'

COMPAT = r'''// [XCDE] LINUX-020: bounded CRT compatibility, no platform-thread emulation.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cerrno>
#include <cstdint>
#include <locale>
#include <cstddef>
#define _countof(a) (sizeof(a)/sizeof((a)[0]))
#define __declspec(x) __attribute__((visibility("default")))
#define __debugbreak() __builtin_trap()
#define FFX_UNUSED(x) ((void)(x))
#define sprintf_s snprintf
inline int wcscpy_s(wchar_t *d, size_t n, const wchar_t *s) {
 if(!d || !s || !n) return EINVAL; size_t l=wcslen(s);
 if(l>=n){d[0]=0;return ERANGE;} wmemcpy(d,s,l+1);return 0;
}
template<size_t N> inline int wcscpy_s(wchar_t(&d)[N],const wchar_t*s){return wcscpy_s(d,N,s);}
inline int strcpy_s(char*d,size_t n,const char*s){if(!d||!s||!n)return EINVAL;size_t l=strlen(s);if(l>=n){d[0]=0;return ERANGE;}memcpy(d,s,l+1);return 0;}
template<size_t N> inline int strcpy_s(char(&d)[N],const char*s){return strcpy_s(d,N,s);}
'''

REGISTRY = r'''// [XCDE] LINUX-020: explicit offscreen-only provider registry.
// Uses the MIT SDK provider unchanged; unsupported providers return NO_PROVIDER.
#include "ffx_provider.h"
#include "ffx_provider_framegeneration.h"
#include <FidelityFX/host/backends/vk/ffx_vk.h>
const ffxProvider* GetffxProvider(ffxStructType_t type,uint64_t id,void*) {
 auto*p=&ffxProvider_FrameGeneration::Instance;
 return p->CanProvide(type)&&(!id||p->GetId()==id)?p:nullptr;
}
const ffxProvider* GetAssociatedProvider(ffxContext*c){return ((const InternalContextHeader*)*c)->provider;}
uint64_t GetProviderVersions(ffxStructType_t type,void*,uint64_t capacity,uint64_t*ids,const char**names){
 auto*p=GetffxProvider(type,0,nullptr);if(!p||!capacity)return 0;
 if(ids)ids[0]=p->GetId();if(names)names[0]=p->GetVersionName();return 1;
}
uint64_t GetProviderCount(ffxStructType_t type,void*d){return GetProviderVersions(type,d,UINT64_MAX,nullptr,nullptr);}
// This library exposes offscreen FG only; its Win32 swapchain is never registered.
// An accidental caller that omits NO_SWAPCHAIN_CONTEXT_NOTIFY fails explicitly.
FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const*){return FFX_ERROR_INVALID_ARGUMENT;}
extern "C" __attribute__((visibility("default"))) uint32_t monado_ffx_abi_version(){return 1;}
extern "C" __attribute__((visibility("default"))) const char* monado_ffx_sdk_commit(){return "@COMMIT@";}
'''

COMPILER = r'''// [XCDE] LINUX-020: shaderc + SDK SPIRV-Reflect, authored SDK GLSL only.
#include <shaderc/shaderc.h>
#include "spirv_reflect.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
namespace fs=std::filesystem;
static std::string read(const fs::path&p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("include/file unavailable: "+p.string());return std::string(std::istreambuf_iterator<char>(f),{});}
struct Included{shaderc_include_result result{};std::string name,data;};
static shaderc_include_result* include(void*ctx,const char*request,int,const char*requesting,size_t){
 auto*x=new Included;try{fs::path root=*(fs::path*)ctx;bool found=false;
 for(auto candidate:{fs::path(requesting).parent_path()/request,root/request}){auto p=fs::weakly_canonical(candidate);auto rel=p.lexically_relative(root);if(rel.empty()||*rel.begin()==".."||!fs::is_regular_file(p))continue;x->name=p.string();x->data=read(p);found=true;break;}
 if(!found)throw std::runtime_error("SDK include unavailable: "+std::string(request));}catch(const std::exception&e){x->data=e.what();}
 x->result.source_name=x->name.c_str();x->result.source_name_length=x->name.size();x->result.content=x->data.c_str();x->result.content_length=x->data.size();x->result.user_data=x;return &x->result;
}
static void release(void*,shaderc_include_result*r){delete (Included*)r->user_data;}
static void quoted(FILE*f,const char*s){fputc('"',f);for(;*s;s++){if(*s=='"'||*s=='\\')fputc('\\',f);fputc(*s,f);}fputc('"',f);}
int main(int argc,char**argv){try{
 if(argc<5)return 2;fs::path root=fs::canonical(argv[1]);auto src=read(argv[2]);
 auto c=shaderc_compiler_initialize();auto o=shaderc_compile_options_initialize();
 shaderc_compile_options_set_source_language(o,shaderc_source_language_glsl);
 shaderc_compile_options_set_target_env(o,shaderc_target_env_vulkan,shaderc_env_version_vulkan_1_2);
 shaderc_compile_options_set_optimization_level(o,shaderc_optimization_level_size);
 shaderc_compile_options_set_generate_debug_info(o);
 shaderc_compile_options_set_include_callbacks(o,include,release,&root);
 for(int i=5;i<argc;i++){char*sep=strchr(argv[i],'=');if(!sep)return 3;shaderc_compile_options_add_macro_definition(o,argv[i],sep-argv[i],sep+1,strlen(sep+1));}
 auto r=shaderc_compile_into_spv(c,src.data(),src.size(),shaderc_compute_shader,argv[2],"main",o);
 if(shaderc_result_get_compilation_status(r)!=shaderc_compilation_status_success){fputs(shaderc_result_get_error_message(r),stderr);return 4;}
 size_t n=shaderc_result_get_length(r);const char*data=shaderc_result_get_bytes(r);FILE*f=fopen(argv[3],"wb");if(!f||fwrite(data,1,n,f)!=n||fclose(f))return 5;
 SpvReflectShaderModule m{};if(spvReflectCreateShaderModule(n,data,&m)!=SPV_REFLECT_RESULT_SUCCESS)return 6;
 uint32_t count=0;spvReflectEnumerateDescriptorBindings(&m,&count,nullptr);std::vector<SpvReflectDescriptorBinding*>b(count);spvReflectEnumerateDescriptorBindings(&m,&count,b.data());
 f=fopen(argv[4],"w");if(!f)return 7;fputs("[",f);bool comma=false;
 for(auto*x:b){const char*type=nullptr;switch(x->descriptor_type){
 case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:type="constantBuffer";break;
 case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:type="srvTexture";break;
 case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:type="uavTexture";break;
 case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:type="sampler";break;
 case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:type=x->resource_type==SPV_REFLECT_RESOURCE_FLAG_SRV?"srvBuffer":"uavBuffer";break;
 default:return 8;}
 if(!x->name||!*x->name)return 9;if(comma)fputs(",",f);comma=true;
 fputs("{\"kind\":",f);quoted(f,type);fputs(",\"name\":",f);quoted(f,x->name);fprintf(f,",\"binding\":%u,\"count\":%u,\"set\":%u}",x->binding,x->count,x->set);
 }fputs("]\n",f);if(fclose(f))return 10;spvReflectDestroyShaderModule(&m);shaderc_result_release(r);shaderc_compile_options_release(o);shaderc_compiler_release(c);return 0;
 }catch(const std::exception&e){fprintf(stderr,"%s\n",e.what());return 11;}}
'''

def digest(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def source_root(lock):
    return ROOT / '.deps/fsr/source' / lock['commit']

def verify_source(lock):
    root=source_root(lock)
    if root.absolute()!=root.resolve():raise ValueError('FSR source root must have ordinary non-symlink parents')
    for name,item in lock['files'].items():
        p=root/name
        if p.is_symlink() or not p.is_file() or p.stat().st_size!=item['bytes'] or digest(p)!=item['sha256']:
            raise ValueError('missing or changed pinned FSR source: '+name)
    return root

def fetch(lock):
    root=source_root(lock)
    if root.absolute()!=root.resolve():raise ValueError('FSR source root must have ordinary non-symlink parents')
    root.mkdir(parents=True,exist_ok=True)
    def one(entry):
        name,item=entry;p=root/name
        if p.exists():
            if p.is_symlink() or not p.is_file() or digest(p)!=item['sha256']:raise ValueError('refuse existing changed source: '+name)
            return
        url='https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/'+lock['commit']+'/'+name
        with urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'Monado-Linux-FSR-source'}),timeout=60) as stream:data=stream.read(item['bytes']+1)
        if len(data)!=item['bytes'] or hashlib.sha256(data).hexdigest()!=item['sha256']:raise ValueError('pinned source mismatch: '+name)
        p.parent.mkdir(parents=True,exist_ok=True)
        with p.open('xb') as stream:stream.write(data)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:list(pool.map(one,lock['files'].items()))
    return verify_source(lock)

def strip_source_debug(path):
    # Keep OpName/OpMemberName for the SDK reflection contract, and every
    # executable instruction/capability. Source text and line tables can dwarf
    # the actual kernel by 100x; glslang's ordinary SDK -Os path omits them too.
    data=path.read_bytes()
    if len(data)%4 or len(data)<20:raise ValueError('malformed compiled SPIR-V')
    words=struct.unpack('<'+'I'*(len(data)//4),data)
    if words[0]!=0x07230203:raise ValueError('compiled shader is not SPIR-V')
    keep=list(words[:5]);i=5
    while i<len(words):
        count=words[i]>>16;opcode=words[i]&65535
        if not count or i+count>len(words):raise ValueError('invalid SPIR-V instruction extent')
        if opcode not in (2,3,4,7,8,317,330):keep.extend(words[i:i+count])
        i+=count
    path.write_bytes(struct.pack('<'+'I'*len(keep),*keep))

def emit_header(destination,name,options,rows):
    kinds=('constantBuffer','srvTexture','uavTexture','srvBuffer','uavBuffer','sampler','rtAccelerationStructure')
    counts=('numConstantBuffers','numSRVTextures','numUAVTextures','numSRVBuffers','numUAVBuffers','numSamplers','numRTAccelerationStructures')
    lines=['// Generated from pinned, MIT-authored AMD GLSL, not game-derived.','#pragma once','#include <stdint.h>']
    lines+=['typedef union '+name+'_PermutationKey { struct {']
    lines+=['uint32_t '+o+' : 1;' for o in options];lines+=['}; uint32_t index; } '+name+'_PermutationKey;']
    lines+=['typedef struct '+name+'_PermutationInfo { uint32_t blobSize; const unsigned char* blobData;']
    for k,c in zip(kinds,counts):lines+=['uint32_t '+c+'; const char** '+k+'Names; const uint32_t* '+k+'Bindings; const uint32_t* '+k+'Counts; const uint32_t* '+k+'Spaces;']
    lines+=['} '+name+'_PermutationInfo;']
    values=[];unique={};indices=[]
    for spv,bindings in rows:
        data=spv.read_bytes();identity=(data,json.dumps(bindings,sort_keys=True))
        if identity in unique:indices.append(unique[identity]);continue
        i=len(values);unique[identity]=i;indices.append(i)
        prefix='g_'+name+'_'+str(i);lines+=['alignas(4) static const unsigned char '+prefix+'_data[] = {'+','.join(map(str,data))+'};']
        value=[str(len(data)),prefix+'_data']
        for kind in kinds:
            group=[b for b in bindings if b['kind']==kind]
            if not group:value+=['0','0','0','0','0'];continue
            base=prefix+'_'+kind
            lines+=['static const char* '+base+'_names[] = {'+','.join(json.dumps(b['name']) for b in group)+'};']
            for field in ('binding','count','set'):lines+=['static const uint32_t '+base+'_'+field+'[] = {'+','.join(str(b[field]) for b in group)+'};']
            value += [str(len(group)),base+'_names',base+'_binding',base+'_count',base+'_set']
        values+=['{'+','.join(value)+'}']
    lines+=['static const uint32_t g_'+name+'_IndirectionTable[] = {'+','.join(map(str,indices))+'};']
    lines+=['static const '+name+'_PermutationInfo g_'+name+'_PermutationInfo[] = {'+','.join(values)+'};']
    destination.write_text('\n'.join(lines)+'\n')

ARENA_HELPERS = r'''// [XCDE] LINUX-020: typed scratch arenas need absolute alignment.
// AMD's EffectContext is alignas(32); malloc and 4-byte-rounded preceding
// array sizes do not guarantee that alignment. Keep the over-aligned type.
static uint8_t* monadoAlignScratchPointerVK(uint8_t*p,size_t a) {
    return reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(p)+a-1)&~uintptr_t(a-1));
}
static size_t monadoScratchSizeVK(size_t contexts,uint32_t extensions) {
    if(!contexts || contexts>UINT32_MAX)return 0;
    size_t total=sizeof(BackendContext_VK);
    auto append=[&](size_t count,size_t width,size_t alignment)->bool {
        if(!width || count>SIZE_MAX/width)return false;
        const size_t bytes=count*width,padding=alignment-1;
        // Existing SDK map lengths are uint32_t: reject, never wrap them.
        if(bytes>UINT32_MAX || total>SIZE_MAX-padding || total+padding>SIZE_MAX-bytes)return false;
        total+=padding+bytes;return true;
    };
    const size_t viewsPerContext=size_t(FFX_MAX_QUEUED_FRAMES)*FFX_MAX_RESOURCE_COUNT*2;
    if(contexts>(SIZE_MAX-FFX_MAX_BINDLESS_DESCRIPTOR_COUNT)/viewsPerContext)return 0;
    if(!append(contexts, size_t(FFX_MAX_GPU_JOBS)*sizeof(FfxGpuJobDescription),alignof(FfxGpuJobDescription)) ||
       !append(contexts*viewsPerContext+FFX_MAX_BINDLESS_DESCRIPTOR_COUNT,sizeof(BackendContext_VK::VkResourceView),alignof(BackendContext_VK::VkResourceView)) ||
       !append(contexts,FFX_CONSTANT_BUFFER_RING_BUFFER_SIZE,alignof(uint8_t)) ||
       !append(contexts,size_t(FFX_MAX_PASS_COUNT)*sizeof(BackendContext_VK::PipelineLayout),alignof(BackendContext_VK::PipelineLayout)) ||
       !append(contexts,size_t(FFX_MAX_RESOURCE_COUNT)*sizeof(BackendContext_VK::Resource),alignof(BackendContext_VK::Resource)) ||
       !append(contexts,sizeof(BackendContext_VK::EffectContext),alignof(BackendContext_VK::EffectContext)) ||
       !append(extensions,sizeof(VkExtensionProperties),alignof(VkExtensionProperties)))return 0;
    return total;
}
FFX_API size_t ffxGetScratchMemorySizeVK(VkPhysicalDevice physicalDevice, size_t maxContexts) {
    uint32_t extensions=0;
    if(physicalDevice && vkEnumerateDeviceExtensionProperties(physicalDevice,nullptr,&extensions,nullptr)!=VK_SUCCESS)return 0;
    return monadoScratchSizeVK(maxContexts,extensions);
}

'''

def patch_vulkan_arena(data):
    text=data.decode();a=text.index('FFX_API size_t ffxGetScratchMemorySizeVK(');b=text.index('// Create a FfxDevice from a VkDevice',a)
    text=text[:a]+ARENA_HELPERS+text[b:]
    old='''    FFX_RETURN_ON_ERROR(
        scratchBufferSize >= ffxGetScratchMemorySizeVK(((VkDeviceContext*)device)->vkPhysicalDevice, maxContexts),
        FFX_ERROR_INSUFFICIENT_MEMORY);'''
    new='''    FFX_RETURN_ON_ERROR(device, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(reinterpret_cast<uintptr_t>(scratchBuffer)%alignof(BackendContext_VK)==0, FFX_ERROR_INVALID_ARGUMENT);
    const size_t requiredScratch=ffxGetScratchMemorySizeVK(((VkDeviceContext*)device)->vkPhysicalDevice,maxContexts);
    FFX_RETURN_ON_ERROR(requiredScratch && scratchBufferSize>=requiredScratch, FFX_ERROR_INSUFFICIENT_MEMORY);'''
    if text.count(old)!=1:raise ValueError('SDK arena size precondition drifted')
    text=text.replace(old,new)
    for field,typ in [('pGpuJobs','FfxGpuJobDescription'),('pResourceViews','BackendContext_VK::VkResourceView'),('pStagingRingBuffer','uint8_t'),('pPipelineLayouts','BackendContext_VK::PipelineLayout'),('pResources','BackendContext_VK::Resource'),('pEffectContexts','BackendContext_VK::EffectContext'),('extensionProperties','VkExtensionProperties')]:
        pattern=r'(?m)^(        )(backendContext->'+field+r'\s*=.*pMem.*;)$'
        text,count=re.subn(pattern,lambda m:m[1]+'pMem=monadoAlignScratchPointerVK(pMem,alignof('+typ+'));\n'+m[0],text)
        if count!=1:raise ValueError('SDK arena mapping drifted: '+field)
    after='backendContext->extensionProperties = (VkExtensionProperties*)pMem;'
    guard='''
        const uintptr_t scratchStart=reinterpret_cast<uintptr_t>(backendInterface->scratchBuffer);
        const uintptr_t arenaEnd=reinterpret_cast<uintptr_t>(pMem);
        FFX_RETURN_ON_ERROR(arenaEnd>=scratchStart && arenaEnd-scratchStart<=backendInterface->scratchBufferSize &&
            size_t(backendContext->numDeviceExtensions)*sizeof(VkExtensionProperties)<=backendInterface->scratchBufferSize-(arenaEnd-scratchStart), FFX_ERROR_INSUFFICIENT_MEMORY);
'''
    if text.count(after)!=1:raise ValueError('SDK arena end mapping drifted')
    return text.replace(after,after+guard).encode()

LAYOUT_TEST = r'''// [XCDE] LINUX-020: actual native SDK arena types/helpers, no GPU calls.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "@VULKAN_SOURCE@"
int main(){
 assert(alignof(BackendContext_VK)<=alignof(max_align_t));
 assert(alignof(BackendContext_VK::EffectContext)==32);
 assert(!monadoScratchSizeVK(0,0));assert(!monadoScratchSizeVK(SIZE_MAX,UINT32_MAX));assert(!monadoScratchSizeVK(UINT32_MAX,UINT32_MAX));
 unsigned checked=0;
 for(size_t contexts:{size_t(1),size_t(2),size_t(4),size_t(8)})for(uint32_t ext:{0u,17u,1024u}){
  size_t required=monadoScratchSizeVK(contexts,ext);assert(required);
  auto*allocation=(uint8_t*)malloc(required+64);assert(allocation);
  for(size_t offset=0;offset<64;offset++){
   uint8_t*base=allocation+offset;if(uintptr_t(base)%alignof(BackendContext_VK))continue;
   uint8_t*p=base+sizeof(BackendContext_VK);
   auto take=[&](size_t count,size_t width,size_t alignment)->void*{
    p=monadoAlignScratchPointerVK(p,alignment);assert(uintptr_t(p)%alignment==0);
    void*result=p;p+=count*width;assert(size_t(p-base)<=required);return result;
   };
   take(contexts*FFX_MAX_GPU_JOBS,sizeof(FfxGpuJobDescription),alignof(FfxGpuJobDescription));
   take(contexts*FFX_MAX_QUEUED_FRAMES*FFX_MAX_RESOURCE_COUNT*2+FFX_MAX_BINDLESS_DESCRIPTOR_COUNT,sizeof(BackendContext_VK::VkResourceView),alignof(BackendContext_VK::VkResourceView));
   take(contexts*FFX_CONSTANT_BUFFER_RING_BUFFER_SIZE,1,1);
   take(contexts*FFX_MAX_PASS_COUNT,sizeof(BackendContext_VK::PipelineLayout),alignof(BackendContext_VK::PipelineLayout));
   take(contexts*FFX_MAX_RESOURCE_COUNT,sizeof(BackendContext_VK::Resource),alignof(BackendContext_VK::Resource));
   auto*effects=(BackendContext_VK::EffectContext*)take(contexts,sizeof(BackendContext_VK::EffectContext),alignof(BackendContext_VK::EffectContext));
   for(size_t i=0;i<contexts;i++){
    effects[i].bindlessTextureSrvHeapStart=0;effects[i].bindlessTextureSrvHeapSize=0;effects[i].bindlessBufferSrvHeapSize=0;
    effects[i].bindlessTextureUavHeapStart=0;effects[i].bindlessTextureUavHeapSize=0;effects[i].bindlessBufferUavHeapSize=0;
   }
   take(ext,sizeof(VkExtensionProperties),alignof(VkExtensionProperties));checked++;
  }free(allocation);
 }printf("typed scratch arena: %u base/context/extension layouts; overflow guards and aligned field stores pass\n",checked);
}
'''

def build(lock,output,jobs):
    src=verify_source(lock);out=output.absolute()
    if out!=out.resolve() or out.exists() or not out.is_relative_to(ROOT/'.deps/fsr/build'):raise ValueError('fresh ordinary output beneath .deps/fsr/build required')
    out.mkdir(parents=True);(out/'obj').mkdir();(out/'logs').mkdir();(out/'shaders').mkdir();(out/'generated').mkdir()
    sysroot=Path((ROOT/'.deps/graphics/active-sysroot.txt').read_text().strip());lib=sysroot/'usr/lib/x86_64-linux-gnu';cc=ROOT/'.deps/bin/clang-cc';cxx=ROOT/'.deps/bin/clang-cxx'
    env={'PATH':'/usr/bin:/bin','LANG':'C.UTF-8','LC_ALL':'C.UTF-8','LD_LIBRARY_PATH':str(lib),'TMPDIR':str(out)}
    receipt={'schema':1,'status':'running','scope':lock['scope'],'sdk_commit':lock['commit'],'lock_sha256':digest(LOCK),'setup_sha256':digest(__file__),'output':str(out),'source':str(src),'commands':[],'shader_count':0,'dependencies':{'sysroot':str(sysroot),'sysroot_marker_sha256':digest(sysroot/'.monado-deps.json'),'cc_sha256':digest(cc),'cxx_sha256':digest(cxx),'shaderc_sha256':digest(lib/'libshaderc.so')}}
    receipt_lock=threading.RLock()
    def save():
        with receipt_lock:(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    def run(name,argv):
        t=time.monotonic();r=subprocess.run(list(map(str,argv)),cwd=ROOT,env=env,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=180)
        (out/'logs'/(name+'.stdout.log')).write_bytes(r.stdout);(out/'logs'/(name+'.stderr.log')).write_bytes(r.stderr)
        with receipt_lock:
            receipt['commands'].append({'name':name,'argv':list(map(str,argv)),'cwd':str(ROOT),'exit':r.returncode,'seconds':time.monotonic()-t});save()
        if r.returncode:raise RuntimeError(name+' failed: '+r.stderr.decode(errors='replace')[:1600])
    try:
        save();(out/'compat.h').write_text(COMPAT);(out/'registry.cpp').write_text(REGISTRY.replace('@COMMIT@',lock['commit']));(out/'compiler.cpp').write_text(COMPILER)
        # The original pinned source is immutable. Record narrow Linux portability
        # patches in this build's working copy, including case-sensitive includes.
        patched=out/'source';receipt['source_patches']={}
        for name,item in lock['files'].items():
            original=src/name;data=original.read_bytes();changed=data
            if original.suffix in ('.h','.hpp','.cpp','.c'):
                changed=changed.replace(b'FidelityFx/',b'FidelityFX/')
                changed=changed.replace(b'#define FFX_API_ENTRY __declspec(dllexport)',b'#define FFX_API_ENTRY __attribute__((visibility("default")))')
                # Linux wchar_t is 32-bit: private resource/pipeline name arrays
                # exceed AMD's Windows-sized opaque contexts. Grow the internal
                # FI/OF storage; retain the SDK's sizeof static assertions.
                if name.endswith(('ffx_frameinterpolation.h','ffx_opticalflow.h')):
                    changed=changed.replace(b'_CONTEXT_SIZE (FFX_SDK_DEFAULT_CONTEXT_SIZE)',b'_CONTEXT_SIZE (2 * FFX_SDK_DEFAULT_CONTEXT_SIZE)')
                if name=='sdk/src/backends/vk/ffx_vk.cpp':changed=patch_vulkan_arena(changed)
                if name=='ffx-api/src/backends.cpp':
                    old=b'void* scratchBuffer = alloc.alloc(scratchBufferSize);'
                    new=b'if (!scratchBufferSize) return FFX_API_RETURN_ERROR_MEMORY;\n            void* scratchBuffer = alloc.alloc(scratchBufferSize);\n            if (!scratchBuffer) return FFX_API_RETURN_ERROR_MEMORY;'
                    changed=changed.replace(old,new)
            target=patched/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(changed)
            if changed!=data:receipt['source_patches'][name]={'before_sha256':item['sha256'],'after_sha256':digest(target)}
        src=patched;save()
        reflect=src/'sdk/tools/ffx_shader_compiler/libs/SPIRV-Reflect';inc=src/'sdk/include';api=src/'ffx-api/include';shared=src/'sdk/src/backends/shared'
        flags=['-std=c++17','-O1','-UNDEBUG','-fPIC','-fno-strict-aliasing','-ffp-contract=off','-ffunction-sections','-fdata-sections','-fvisibility=hidden','-pthread','-fms-extensions','-DFFX_BACKEND_VK=1','-DFFX_FI=1','-DFFX_OF=1','-DFFX_GCC=1','-include',out/'compat.h','-I'+str(inc),'-I'+str(api),'-I'+str(shared),'-I'+str(src/'sdk/src/shared'),'-I'+str(src/'sdk/src/components'),'-I'+str(src/'ffx-api/src'),'-I'+str(sysroot/'usr/include'),'-I'+str(out/'generated')]
        run('reflect',[cc,'-O1','-fPIC','-I'+str(reflect),'-c',reflect/'spirv_reflect.c','-o',out/'obj/reflect.o'])
        run('shader-compiler',[cxx,'-std=c++17','-O1','-I'+str(sysroot/'usr/include'),'-I'+str(reflect),out/'compiler.cpp',out/'obj/reflect.o',lib/'libshaderc.so','-Wl,-rpath,'+str(lib),'-o',out/'shader-compiler'])
        specs=[]
        for effect,options in [('frameinterpolation',('LOW_RES_MOTION_VECTORS','JITTER_MOTION_VECTORS','INVERTED_DEPTH')),('opticalflow',('HDR_COLOR_INPUT',))]:
            for shader in sorted((src/'sdk/src/backends/vk/shaders'/effect).glob('*.glsl')):
                for half,suffix in ((0,''),(0,'_wave64'),(1,'_16bit'),(1,'_wave64_16bit')):
                    name=shader.stem+suffix;rows=[];keys=['FFX_'+effect.upper()+'_OPTION_'+o for o in options]
                    for i in range(1<<len(keys)):
                        spv=out/'shaders'/(name+'_'+str(i)+'.spv');meta=spv.with_suffix('.json')
                        definitions=['FFX_GPU=1','FFX_GLSL=1','FFX_HALF='+str(half)]+[k+'='+str((i>>j)&1) for j,k in enumerate(keys)]
                        if effect=='frameinterpolation':definitions+=['FFX_FRAMEINTERPOLATION_INPAINTING_PYRAMID_USE_LANCZOS_TYPE=2']
                        specs.append((name,i,keys,spv,meta,shader,definitions))
        # shaderc compiler objects are per-process; source compilation is isolated and bounded.
        def shader_job(s):
            name,i,keys,spv,meta,shader,definitions=s
            run('shader-'+name+'-'+str(i),[out/'shader-compiler',inc/'FidelityFX/gpu',shader,spv,meta,*definitions]);strip_source_debug(spv);return s
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:list(pool.map(shader_job,specs))
        groups={}
        for name,i,keys,spv,meta,*_ in specs:groups.setdefault(name,(keys,[]))[1].append((spv,json.loads(meta.read_text())))
        for name,(keys,rows) in groups.items():emit_header(out/'generated'/(name+'_permutations.h'),name,keys,rows)
        receipt['shader_count']=len(specs);save()
        sources=[out/'registry.cpp',src/'ffx-api/src/ffx_api.cpp',src/'ffx-api/src/backends.cpp',src/'ffx-api/src/ffx_provider_framegeneration.cpp',src/'ffx-api/src/validation.cpp',src/'sdk/src/shared/ffx_assert.cpp',src/'sdk/src/shared/ffx_message.cpp',src/'sdk/src/shared/ffx_breadcrumbs_list.cpp',src/'sdk/src/shared/ffx_object_management.cpp',src/'sdk/src/components/frameinterpolation/ffx_frameinterpolation.cpp',src/'sdk/src/components/opticalflow/ffx_opticalflow.cpp',shared/'ffx_shader_blobs.cpp',shared/'blob_accessors/ffx_frameinterpolation_shaderblobs.cpp',shared/'blob_accessors/ffx_opticalflow_shaderblobs.cpp',src/'sdk/src/backends/vk/ffx_vk.cpp']
        objects=[]
        for i,source in enumerate(sources):
            obj=out/'obj'/('sdk-'+str(i)+'.o');run('compile-'+str(i),[cxx,*flags,'-c',source,'-o',obj]);objects.append(obj)
        (out/'layout-test.cpp').write_text(LAYOUT_TEST.replace('@VULKAN_SOURCE@',str(src/'sdk/src/backends/vk/ffx_vk.cpp')))
        run('compile-layout-test',[cxx,*flags,out/'layout-test.cpp',lib/'libvulkan.so','-Wl,--gc-sections','-Wl,-rpath,'+str(lib),'-o',out/'layout-test'])
        run('scratch-layout-test',[out/'layout-test'])
        (out/'exports.map').write_text('{ global: ffxCreateContext; ffxDestroyContext; ffxConfigure; ffxQuery; ffxDispatch; monado_ffx_abi_version; monado_ffx_sdk_commit; local: *; };\n')
        run('link',[cxx,'-shared','-pthread',*objects,'-Wl,--gc-sections','-Wl,-z,defs','-Wl,--version-script,'+str(out/'exports.map'),lib/'libvulkan.so','-o',out/NAME])
        run('exports',['nm','-D','--defined-only',out/NAME]);run('dynamic',['readelf','-d',out/NAME])
        exports=(out/'logs/exports.stdout.log').read_text().splitlines()
        symbols={line.split()[-1] for line in exports if line.split()}
        expected={'ffxCreateContext','ffxDestroyContext','ffxConfigure','ffxQuery','ffxDispatch','monado_ffx_abi_version','monado_ffx_sdk_commit'}
        if symbols!=expected:raise ValueError('unexpected FidelityFX bridge exports: '+str(symbols))
        if receipt['setup_sha256']!=digest(__file__) or receipt['lock_sha256']!=digest(LOCK):raise ValueError('FSR setup recipe/lock changed during build')
        verify_source(lock)
        shutil.copyfile(src/'LICENSE.txt',out/'AMD-FidelityFX-LICENSE.txt');receipt.update(status='passed',library={'path':str(out/NAME),'bytes':(out/NAME).stat().st_size,'sha256':digest(out/NAME)},license_sha256=digest(out/'AMD-FidelityFX-LICENSE.txt'))
        receipt['shader_outputs']={str(p.relative_to(out)):digest(p) for p in sorted((out/'shaders').iterdir())}
        active=ROOT/'.deps/fsr/active-build.txt'
        if active.is_symlink():raise ValueError('active FSR build marker cannot be a symlink')
        temporary=active.with_name('.active-build.'+str(os.getpid()));temporary.write_text(str(out)+'\n');os.replace(temporary,active)
    except Exception as e:receipt.update(status='failed',error=str(e));raise
    finally:save()
    return receipt

def verify_existing():
    """Verify an already built dependency; never fetch, compile, dlopen or run it.

    Public build-recipe contract: paths below are strings, hashes are SHA256.
    Consumers must keep the library/license adjacent and receipt-bound.
    """
    lock=json.loads(LOCK.read_text());src=verify_source(lock);active=ROOT/'.deps/fsr/active-build.txt'
    if active.is_symlink() or not active.is_file():raise ValueError('native FidelityFX dependency is not built; run setup_linux_fsr.py --build --output <fresh .deps/fsr/build/path>')
    out=Path(active.read_text().strip())
    if not out.is_absolute() or out!=out.resolve() or not out.is_relative_to(ROOT/'.deps/fsr/build'):raise ValueError('invalid FSR active build marker')
    rp=out/'receipt.json'
    if rp.is_symlink() or not rp.is_file():raise ValueError('missing ordinary FSR receipt')
    receipt=json.loads(rp.read_text())
    if receipt.get('status')!='passed' or receipt.get('sdk_commit')!=lock['commit'] or receipt.get('lock_sha256')!=digest(LOCK) or receipt.get('setup_sha256')!=digest(__file__):raise ValueError('native FSR receipt does not match the pinned SDK and setup recipe')
    library=out/NAME;lic=out/'AMD-FidelityFX-LICENSE.txt'
    if library.is_symlink() or not library.is_file() or library.stat().st_size!=receipt['library']['bytes'] or digest(library)!=receipt['library']['sha256']:raise ValueError('native FSR library identity changed')
    if lic.is_symlink() or not lic.is_file() or digest(lic)!=receipt['license_sha256']:raise ValueError('native FSR MIT license identity changed')
    sysroot=Path(receipt['dependencies']['sysroot'])
    if digest(sysroot/'.monado-deps.json')!=receipt['dependencies']['sysroot_marker_sha256']:raise ValueError('native FSR graphics dependency changed')
    return {'library_path':str(library),'header_dir':str(out/'source/ffx-api/include'),'receipt_path':str(rp),'source_root':str(src),'source_commit':lock['commit'],'lock_sha256':digest(LOCK),'library_sha256':receipt['library']['sha256'],'license_path':str(lic),'license_sha256':receipt['license_sha256'],'abi_version':1,'sdk_version':'1.1.4'}

def ensure_sdk():
    return verify_existing()

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--fetch',action='store_true');ap.add_argument('--build',action='store_true');ap.add_argument('--output',type=Path);ap.add_argument('--jobs',type=int,choices=range(1,9),default=4);args=ap.parse_args();lock=json.loads(LOCK.read_text())
    try:
        root=fetch(lock) if args.fetch else verify_source(lock)
        if args.build:
            if not args.output:raise ValueError('--build requires a fresh --output under .deps/fsr/build')
            r=build(lock,args.output,args.jobs);print('library:',r['library']['path']);print('receipt:',Path(r['output'])/'receipt.json')
        else:
            print('verified source:',root)
            if (ROOT/'.deps/fsr/active-build.txt').exists():print(json.dumps(verify_existing(),indent=2))
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as e:ap.exit(1,str(e)+'\n')
if __name__=='__main__':main()
