"""Actual production loader with an authored SDK DLL, Vulkan/SDL boundary mocks.

No NVIDIA code, GPU, game, human profile or visible window is used. Production
initialization/adapter/surface/cleanup bodies are compiled unchanged; only SDK
DLL and Vulkan/SDL external boundary behavior is synthetic.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import unittest
import uuid

PORT=Path(__file__).resolve().parents[2]
SDK=r'''
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <sl.h>
#include <sl_dlss.h>
#include <sl_helpers_vk.h>
#include <cstring>
static char scenario[80];static unsigned stats[4];
static bool is(const char *name){GetEnvironmentVariableA("MONADO_AUTHORED_DLSS",scenario,sizeof scenario);return !strcmp(scenario,name);}
static sl::Result bad(){return (sl::Result)1;}
#define API extern "C"
API unsigned authored_stats(unsigned i){return stats[i];}
API sl::Result slInit(const sl::Preferences &p,uint64_t){
 if(p.numPathsToPlugins!=1||!p.pathsToPlugins||!p.pathsToPlugins[0]||!wcsncmp(p.pathsToPlugins[0],L"\\\\?\\",4))return bad();
 return is("init-fail")?bad():sl::Result::eOk;
}
API sl::Result slShutdown(){stats[0]++;return sl::Result::eOk;}
API sl::Result slFreeResources(sl::Feature,const sl::ViewportHandle&){return sl::Result::eOk;}
API sl::Result slGetFeatureRequirements(sl::Feature,sl::FeatureRequirements &r){
 if(is("requirements-fail"))return bad();static const char *extensions[]={"VK_AUTHORED_required"};static const char *features[]={"shaderInt8"};
 static const char *instance[]={"VK_AUTHORED_instance"};r.vkNumInstanceExtensions=1;r.vkInstanceExtensions=instance;
 r.vkNumDeviceExtensions=1;r.vkDeviceExtensions=extensions;r.vkNumFeatures12=1;r.vkFeatures12=features;
 r.vkNumGraphicsQueuesRequired=is("zero-queues")?0:1;r.vkNumComputeQueuesRequired=is("zero-queues")?0:1;return sl::Result::eOk;
}
API sl::Result slSetVulkanInfo(const sl::VulkanInfo &i){stats[1]=i.graphicsQueueIndex;stats[2]=i.computeQueueIndex;return is("device-fail")?bad():sl::Result::eOk;}
#ifndef OMIT_SUPPORT
API sl::Result slIsFeatureSupported(sl::Feature,const sl::AdapterInfo&){return is("unsupported")?bad():sl::Result::eOk;}
#endif
static sl::Result options(const sl::ViewportHandle&,const sl::DLSSOptions&){return sl::Result::eOk;}
#ifndef OMIT_FUNCTION
API sl::Result slGetFeatureFunction(sl::Feature,const char*,void *&f){f=is("options-missing")?nullptr:(void*)&options;return sl::Result::eOk;}
#endif
API sl::Result slGetNewFrameToken(sl::FrameToken*&,const uint32_t*){return bad();}
API sl::Result slSetConstants(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&){return sl::Result::eOk;}
API sl::Result slSetTagForFrame(const sl::FrameToken&,const sl::ViewportHandle&,const sl::ResourceTag*,uint32_t,sl::CommandBuffer*){return sl::Result::eOk;}
API sl::Result slEvaluateFeature(sl::Feature,const sl::FrameToken&,const sl::BaseStructure**,uint32_t,sl::CommandBuffer*){return sl::Result::eOk;}
static VKAPI_ATTR VkResult VKAPI_CALL create(VkInstance,const VkWin32SurfaceCreateInfoKHR*,const VkAllocationCallbacks*,VkSurfaceKHR *out){if(is("surface-fail"))return VK_ERROR_INITIALIZATION_FAILED;*out=(VkSurfaceKHR)1234;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL destroy(VkInstance,VkSurfaceKHR,const VkAllocationCallbacks*){stats[3]++;}
API VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance,const char *name){if(is("surface-hook-missing"))return nullptr;return !strcmp(name,"vkCreateWin32SurfaceKHR")?(PFN_vkVoidFunction)&create:!strcmp(name,"vkDestroySurfaceKHR")?(PFN_vkVoidFunction)&destroy:nullptr;}
API VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice,const char *name){return is("device-proxy-missing")&&!strcmp(name,"vkAcquireNextImageKHR")?nullptr:(PFN_vkVoidFunction)&destroy;}
'''

PRELUDE=r'''
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <cstring>
#include <cassert>
#include <string>
static const char *authored_case;static HWND authored_window;static unsigned authored_loads;
static bool is(const char *name){return !strcmp(authored_case,name);}
static DWORD authored_module_path(HMODULE module,wchar_t *output,DWORD capacity){
 if(is("long-module-path")||is("long-extended-module-path")){
  std::wstring path=L"C:\\";while(path.size()<300)path+=L"authored folder\\";path+=L"xcde.exe";
  if(is("long-extended-module-path"))path=L"\\\\?\\"+path;
  if(path.size()>=capacity)return capacity;memcpy(output,path.c_str(),(path.size()+1)*sizeof(wchar_t));return (DWORD)path.size();
 }
 wchar_t path[4096];DWORD n=GetModuleFileNameW(module,path,4096);if(!n||n>=4096)return 0;
 if(is("extended-module-path")){if(n+4>=capacity)return capacity;memcpy(output,L"\\\\?\\",4*sizeof(wchar_t));memcpy(output+4,path,(n+1)*sizeof(wchar_t));return n+4;}
 if(n>=capacity)return capacity;memcpy(output,path,(n+1)*sizeof(wchar_t));return n;
}
static VkResult authored_instance_extensions(const char*,uint32_t *n,VkExtensionProperties *p){if(p){memset(p,0,sizeof *p);strcpy(p->extensionName,is("instance-extension-missing")?"VK_OTHER":"VK_AUTHORED_instance");}*n=1;return VK_SUCCESS;}
static void authored_properties(VkPhysicalDevice,VkPhysicalDeviceProperties *p){memset(p,0,sizeof *p);p->vendorID=is("non-nvidia")?0x1002:0x10de;}
static VkResult authored_extensions(VkPhysicalDevice,const char*,uint32_t *n,VkExtensionProperties *p){if(p){memset(p,0,sizeof *p);strcpy(p->extensionName,is("extension-missing")?"VK_OTHER":"VK_AUTHORED_required");}*n=1;return VK_SUCCESS;}
static void authored_features(VkPhysicalDevice,VkPhysicalDeviceFeatures2 *p){auto *f=(VkPhysicalDeviceVulkan12Features*)p->pNext;f->shaderInt8=is("feature-missing")?VK_FALSE:VK_TRUE;}
static void authored_queues(VkPhysicalDevice,uint32_t *n,VkQueueFamilyProperties *p){if(p){memset(p,0,sizeof *p);p->queueCount=is("queue-missing")?1:4;p->queueFlags=VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT;}*n=1;}
static VkResult authored_present(VkPhysicalDevice,uint32_t,VkSurfaceKHR,VkBool32 *p){*p=is("present-fail")?VK_FALSE:VK_TRUE;return VK_SUCCESS;}
static SDL_PropertiesID authored_window_properties(SDL_Window*){return 1;}
static void *authored_pointer(SDL_PropertiesID,const char*,void*){return authored_window;}
#define vkEnumerateInstanceExtensionProperties authored_instance_extensions
#define vkGetPhysicalDeviceProperties authored_properties
#define vkEnumerateDeviceExtensionProperties authored_extensions
#define vkGetPhysicalDeviceFeatures2 authored_features
#define vkGetPhysicalDeviceQueueFamilyProperties authored_queues
#define vkGetPhysicalDeviceSurfaceSupportKHR authored_present
#define SDL_GetWindowProperties authored_window_properties
#define SDL_GetPointerProperty authored_pointer
#define GetModuleFileNameW authored_module_path
'''

MAIN=r'''
Settings g_settings;
int main(int argc,char **argv){assert(argc==2);authored_case=argv[1];SetEnvironmentVariableA("MONADO_AUTHORED_DLSS",authored_case);g_settings.upscaler=UPSCALE_DLSS;
 bool early=dlss_init_early();
 if(is("long-module-path")||is("long-extended-module-path")){assert(!early&&!sl_initialized&&!dlss_available()&&authored_loads==0&&!GetModuleHandleW(L"sl.interposer.dll"));assert(strstr(dlss_status(),"shorter installation folder"));return 0;}
 if(is("init-fail")||is("requirements-fail")||is("missing-support")||is("missing-function")){assert(!early&&!dlss_available()&&!GetModuleHandleW(L"sl.interposer.dll"));assert(strstr(dlss_status(),is("init-fail")?"initialization failed":is("requirements-fail")?"plugin did not load":"required entry points"));return 0;}
 assert(early&&!dlss_available());const char *instance_extensions[2];int instance_count=dlss_instance_exts(instance_extensions,is("instance-capacity")?0:2);
 if(is("instance-capacity")||is("instance-extension-missing")){assert(instance_count==0&&!sl_ok&&!dlss_available()&&strstr(dlss_status(),"instance extensions"));dlss_shutdown();return 0;}
 assert(instance_count==1);dlss_validate_adapter((VkPhysicalDevice)1,VK_API_VERSION_1_3,0);
 VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
 const char *extensions[2]={"VK_BASE"};uint32_t count=1,gfx=99,compute=99;dlss_device_requirements((VkInstance)1,(VkPhysicalDevice)1,extensions,&count,is("extension-capacity")?1:2,&f12,&f13,&gfx,&compute);
 if(is("non-nvidia")||is("extension-missing")||is("feature-missing")||is("queue-missing")||is("extension-capacity")){assert(!sl_ok&&!dlss_available()&&count==1&&gfx==0&&compute==0&&!f12.shaderInt8);dlss_shutdown();return 0;}
 assert(count==2&&f12.shaderInt8);dlss_device_created((VkInstance)1,(VkPhysicalDevice)1,(VkDevice)1,0);
 if(is("device-fail")||is("unsupported")||is("options-missing")||is("device-proxy-missing")){assert(!dlss_available()&&!strstr(dlss_status(),"ready on"));dlss_shutdown();return 0;}
 auto stats=(unsigned(*)(unsigned))GetProcAddress(sl_mod,"authored_stats");assert(stats&&stats(1)==(is("zero-queues")?0:1)&&stats(2)==(is("zero-queues")?0:2));
 authored_window=CreateWindowExW(0,L"STATIC",L"authored hidden window",WS_POPUP,0,0,8,8,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);assert(authored_window);
 VkSurfaceKHR surface=(VkSurfaceKHR)44;bool accepted=dlss_create_surface((void*)1,(VkInstance)1,(VkPhysicalDevice)1,0,&surface);
 if(is("surface-fail")||is("surface-hook-missing")||is("present-fail")){assert(!accepted&&!dlss_available()&&surface==(VkSurfaceKHR)44&&!dlss_proxy((VkDevice)1,"vkQueuePresentKHR"));if(is("present-fail"))assert(stats(3)==1);}
 else assert(accepted&&surface==(VkSurfaceKHR)1234&&dlss_available()&&dlss_proxy((VkDevice)1,"vkQueuePresentKHR")&&strstr(dlss_status(),"ready on the selected GPU"));
 DestroyWindow(authored_window);dlss_shutdown();assert(!dlss_available()&&!GetModuleHandleW(L"sl.interposer.dll"));return 0;
}
'''

@unittest.skipUnless(os.name=='nt','native Windows authored SDK loader fixture')
class Native(unittest.TestCase):
    def test_production_loader_exports_adapter_queues_surface_and_cleanup(self):
        out=PORT/'runs'/('dlss-loader-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8]);out.mkdir()
        prefix=Path(json.loads((PORT/'.deps/windows-deps.json').read_text())['prefix'])
        env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(PORT/'.deps/zig-global-cache'))
        base=[sys.executable,'-B','-m','ziglang','c++','-std=c++17','-O1','-UNDEBUG','-march=x86_64_v2','-ffp-contract=off','-fno-fast-math','-I'+str(prefix/'include'),'-I'+str(PORT/'third_party/streamline/include'),'-I'+str(PORT/'runtime')]
        sdk=out/'authored-sdk.cpp';sdk.write_text(SDK,encoding='utf-8')
        production=PORT/'runtime/nvn/vulkan/dlss.cpp';source=production.read_text(encoding='utf-8')
        begin=source[:source.index('static sl::Resource tex(')];tail=source[source.index('// Call only once the device is idle'):]
        load_boundary='\nstatic HMODULE authored_optional_load(const wchar_t *path,char *reason,size_t capacity){authored_loads++;return monado_optional_library_load(path,reason,capacity);}\n#define monado_optional_library_load authored_optional_load\n'
        begin=begin.replace('#include "../../platform/windows_optional_library.h"','#include "'+(PORT/'runtime/platform/windows_optional_library.h').as_posix()+'"'+load_boundary).replace('#include "temporal.h"','#include "nvn/vulkan/temporal.h"').replace('#include "dlss.h"','#include "nvn/vulkan/dlss.h"').replace('#include "temporal_camera.h"','#include "nvn/vulkan/temporal_camera.h"').replace('#include "../../settings.h"','#include "settings.h"')
        harness=out/'production-loader.cpp';harness.write_text(PRELUDE+begin+tail+MAIN,encoding='utf-8')
        def run(label,argv):
            result=subprocess.run(argv,env=env,capture_output=True,text=True,timeout=180);(out/(label+'.log')).write_text(result.stdout+result.stderr,encoding='utf-8');self.assertEqual(result.returncode,0,label+': '+result.stderr[-3000:]);return result
        executable=out/'authored-loader.exe';run('compile-production',[*base,str(harness),'-o',str(executable),'-ladvapi32','-luser32'])
        variants={}
        for name,flags in [('normal',[]),('missing-support',['-DOMIT_SUPPORT']),('missing-function',['-DOMIT_FUNCTION'])]:
            exports=['authored_stats','slInit','slShutdown','slFreeResources','slGetFeatureRequirements','slSetVulkanInfo','slGetNewFrameToken','slSetConstants','slSetTagForFrame','slEvaluateFeature','vkGetInstanceProcAddr','vkGetDeviceProcAddr']
            if name!='missing-support':exports.append('slIsFeatureSupported')
            if name!='missing-function':exports.append('slGetFeatureFunction')
            definition=out/(name+'.def');definition.write_text('EXPORTS\n'+'\n'.join(exports)+'\n')
            dll=out/(name+'.dll');run('compile-'+name,[*base,*flags,'-shared',str(sdk),str(definition),'-o',str(dll)]);variants[name]=dll
        cases=['success','extended-module-path','long-module-path','long-extended-module-path','zero-queues','init-fail','requirements-fail','missing-support','missing-function','instance-capacity','instance-extension-missing','non-nvidia','extension-missing','feature-missing','queue-missing','extension-capacity','device-fail','unsupported','options-missing','device-proxy-missing','surface-hook-missing','surface-fail','present-fail']
        for name in cases:
            with self.subTest(case=name):
                directory=out/name;directory.mkdir();shutil.copyfile(executable,directory/'authored-loader.exe');shutil.copyfile(variants.get(name,variants['normal']),directory/'sl.interposer.dll');run(name,[str(directory/'authored-loader.exe'),name])
        (out/'receipt.json').write_text(json.dumps({'scope':'production loader bodies, authored SDK and Vulkan/SDL boundary mocks; no NVIDIA/GPU/game','cases':cases,'production_sha256':hashlib.sha256(production.read_bytes()).hexdigest()},indent=2)+'\n');print('Native DLSS loader evidence: '+str(out),flush=True)

if __name__=='__main__':unittest.main()
