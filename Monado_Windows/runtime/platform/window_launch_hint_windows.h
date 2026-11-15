// Per-launch monitor identity; no persisted SDL IDs or ambient environment setting.
#pragma once
#include <stdbool.h>
#include <string.h>
extern char g_window_launch_monitor[32];
static inline bool window_launch_monitor_valid(const char *name) {
    const char prefix[]="\\\\.\\DISPLAY";
    if(!name||strncmp(name,prefix,sizeof prefix-1))return false;
    size_t n=strlen(name),start=sizeof prefix-1;
    if(n<=start||n>=32)return false;
    for(size_t i=start;i<n;i++)if(name[i]<'0'||name[i]>'9')return false;
    return true;
}
