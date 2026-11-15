// Actual native model/DLL guard/configuration, authored bytes only; no GPU calls.
#include <assert.h>
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include "nvn/vulkan/temporal.h"
#include "settings.h"
typedef struct {TexBackend b;VkImageView v;} Img;
enum {F_LINEAR=VK_FORMAT_R16G16B16A16_SFLOAT,F_MV=VK_FORMAT_R16G16_SFLOAT,F_DEPTH=VK_FORMAT_R32_SFLOAT};
VkG g_vk;Settings g_settings;
#include "../../runtime/nvn/vulkan/temporal_fsr4_windows.inc"
int wmain(int argc,wchar_t **argv){
 assert(argc==4);char assets[4096],component[4096];
 assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[1],-1,assets,sizeof assets,NULL,NULL));
 assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[2],-1,component,sizeof component,NULL,NULL));
 assert(!tmp_fsr4_configured()&&!tmp_fsr4_supported());
 if(!wcscmp(argv[3],L"bad-abi")){
  assert(tmp_fsr4_configure(assets,component));g_vk.fsr4_lite=true;assert(!tmp_fsr4_supported());
  assert(strstr(tmp_fsr4_status(),"ABI mismatch"));fsr4_release_models();puts("PASS actual native FSR4 wrong DLL ABI rejection");return 0;
 }
 bool good=!wcscmp(argv[3],L"good");
 if(!good){assert(!tmp_fsr4_configure(assets,component));assert(!tmp_fsr4_configured()&&fsr4_model_count==0);puts("PASS actual native FSR4 corrupt/missing/hash/hardlink rejection");return 0;}
 bool configured=tmp_fsr4_configure(assets,component);
 if(!configured)fprintf(stderr,"FSR4 authored configuration failed: %s (Win32=%lu, errno=%d)\n",tmp_fsr4_status(),GetLastError(),errno);
 assert(configured&&tmp_fsr4_configured()&&fsr4_model_count==52);
 assert(!tmp_fsr4_supported());g_vk.fsr4_lite=true;assert(tmp_fsr4_supported());
 assert(fsr4_api&&fsr4_api->abi==2&&!fsr4_context);
 assert(!fsr4_size_supported(107,60,320,180)&&fsr4_size_supported(128,60,320,180));
 assert(!tmp_fsr4_set_asset_dir(assets)); // Startup-only ownership survives provider load.
 wchar_t path[4096];swprintf(path,4096,L"%ls/lite/t1080_m0/prepass.spv",argv[1]);
 HANDLE write=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
 assert(write==INVALID_HANDLE_VALUE);assert(!DeleteFileW(path));
 fsr4_reset();fsr4_release_models();assert(!fsr4_model_count);
 puts("PASS actual native FSR4 owned52-file model/DLL hash leases, ABI load, capability fallback, immutable files and startup-only lifetime; no GPU dispatch");return 0;
}
