// Native guest reservations stay owned by platform/windows.c. Commit only the
// checked import prefix; importing reserved, uncommitted pages is not valid.
#pragma once
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static bool windows_host_import_range(void *base, size_t bytes, bool committed) {
    uintptr_t at = (uintptr_t)base;
    if (!base || !bytes || bytes > UINTPTR_MAX - at) return false;
    uintptr_t end = at + bytes;
    void *allocation = NULL;
    while (at < end) {
        MEMORY_BASIC_INFORMATION region;
        if (VirtualQuery((void *)at, &region, sizeof region) != sizeof region ||
            !region.AllocationBase || !region.RegionSize ||
            (region.State != MEM_COMMIT && region.State != MEM_RESERVE)) return false;
        if (!allocation) allocation = region.AllocationBase;
        if (region.AllocationBase != allocation ||
            (region.State == MEM_COMMIT &&
             (region.Type != MEM_PRIVATE || region.Protect != PAGE_READWRITE)) ||
            (committed && region.State != MEM_COMMIT)) return false;
        uintptr_t first = (uintptr_t)region.BaseAddress;
        if (region.RegionSize > UINTPTR_MAX - first || first + region.RegionSize <= at) return false;
        at = first + region.RegionSize;
    }
    return true;
}

static bool windows_host_import_commit(void *base, size_t bytes, char *error, size_t capacity) {
    SYSTEM_INFO info; GetSystemInfo(&info);
    if (!info.dwPageSize || ((uintptr_t)base % info.dwPageSize) ||
        bytes % info.dwPageSize || !windows_host_import_range(base, bytes, false)) {
        snprintf(error, capacity, "guest import extent is not one private writable reservation");
        return false;
    }
    // MEM_COMMIT leaves already committed bytes unchanged. Never release,
    // replace or unprotect the reservation to make an import succeed.
    if (VirtualAlloc(base, bytes, MEM_COMMIT, PAGE_READWRITE) != base ||
        !windows_host_import_range(base, bytes, true)) {
        snprintf(error, capacity, "guest import commit failed (Win32 %lu)", GetLastError());
        return false;
    }
    return true;
}
