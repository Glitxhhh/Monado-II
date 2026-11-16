// Authored fault injection only; included before the real production TU.
#pragma once
#include <windows.h>
HANDLE WINAPI authored_CreateJobObjectW(LPSECURITY_ATTRIBUTES,LPCWSTR);
BOOL WINAPI authored_SetInformationJobObject(HANDLE,JOBOBJECTINFOCLASS,LPVOID,DWORD);
BOOL WINAPI authored_AssignProcessToJobObject(HANDLE,HANDLE);
DWORD WINAPI authored_ResumeThread(HANDLE);
#define CreateJobObjectW authored_CreateJobObjectW
#define SetInformationJobObject authored_SetInformationJobObject
#define AssignProcessToJobObject authored_AssignProcessToJobObject
#define ResumeThread authored_ResumeThread
