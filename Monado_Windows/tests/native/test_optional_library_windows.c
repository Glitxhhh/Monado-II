// Authored ordinary DLL mapping/failure boundary only, no SDK/game/GPU execution.
#include <assert.h>
#include <windows.h>
#include <stdio.h>
#include "platform/windows_optional_library.h"
int wmain(int argc,wchar_t **argv){
 assert(argc==3);HANDLE lock=INVALID_HANDLE_VALUE;
 if(!wcscmp(argv[2],L"locked")){
  lock=CreateFileW(argv[1],GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
  assert(lock!=INVALID_HANDLE_VALUE);
 }
 char reason[512]={0};HMODULE module=monado_optional_library_load(argv[1],reason,sizeof reason);
 if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);
 if(!wcscmp(argv[2],L"good")){
  if(!module)fprintf(stderr,"Authored optional DLL load failed: %s\n",reason);
  assert(module);int(*value)(void)=(void*)GetProcAddress(module,"authored_provider_value");assert(value&&value()==20);FreeLibrary(module);
  puts("PASS authored optional native DLL Unicode/spaces/literal paths, held-file/ancestor mapping and local sibling dependency");
 }else{
  assert(!module&&*reason);printf("PASS rejected optional DLL: %s\n",reason);
 }
 return 0;
}
