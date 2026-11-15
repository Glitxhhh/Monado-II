// Authored test-only fixed guest ranges. No game process or data is accessed.
// Linux authored fixtures use mmap solely for anonymous writable ranges; this
// adapter reserves each complete Win32 allocation granule without replacement,
// then commits only the requested pages. It never substitutes production memory.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif
static void *fixture_memory_map(void *address, size_t bytes) {
    SYSTEM_INFO system; GetSystemInfo(&system);
    uintptr_t begin = (uintptr_t)address;
    uintptr_t base = begin & ~(uintptr_t)(system.dwAllocationGranularity - 1);
    if (!bytes || begin + bytes < begin) return (void *)-1;
    size_t extent = begin - base + bytes;
    void *reserved = VirtualAlloc((void *)base, extent, MEM_RESERVE, PAGE_NOACCESS);
    if (reserved != (void *)base) {
        if (reserved) VirtualFree(reserved, 0, MEM_RELEASE);
        return (void *)-1;
    }
    void *mapped = VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE);
    if (mapped != address) { VirtualFree(reserved, 0, MEM_RELEASE); return (void *)-1; }
    return mapped;
}
static int fixture_memory_unmap(void *address, size_t bytes) {
    (void)bytes; MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(address, &info, sizeof info)) return -1;
    return VirtualFree(info.AllocationBase, 0, MEM_RELEASE) ? 0 : -1;
}
// These test-only argument tokens describe the original authored mappings;
// every copied fixture requests precisely the writable anonymous mapping above.
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 1
#define MAP_SHARED 2
#define MAP_ANONYMOUS 4
#define MAP_FIXED_NOREPLACE 8
#define mmap(address, bytes, protection, flags, fd, offset) fixture_memory_map((address), (bytes))
#define munmap(address, bytes) fixture_memory_unmap((address), (bytes))
