// Authored Win32 fixture process isolation; never launches game code.
#pragma once
#include <windows.h>
#include <assert.h>
#include <wchar.h>
static void fixture_abi_child(void) {
    wchar_t exe[4096], command[4200];
    assert(GetModuleFileNameW(NULL,exe,4096));
    assert(swprintf(command,4200,L"\"%ls\" --abi-refusal",exe)>0);
    STARTUPINFOW si={.cb=sizeof si};PROCESS_INFORMATION pi={0};
    assert(CreateProcessW(exe,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi));
    DWORD wait=WaitForSingleObject(pi.hProcess,10000);
    if(wait!=WAIT_OBJECT_0){TerminateProcess(pi.hProcess,88);WaitForSingleObject(pi.hProcess,10000);}
    assert(wait==WAIT_OBJECT_0);DWORD code=0;assert(GetExitCodeProcess(pi.hProcess,&code));assert(code==86);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
}
