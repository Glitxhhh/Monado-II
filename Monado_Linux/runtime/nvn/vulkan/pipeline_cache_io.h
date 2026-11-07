// [XCDE] LINUX-018: checked, atomic Linux pipeline cache replacement.
// Caller owns the cache and serializes file writes. A failed snapshot or write
// leaves the prior file intact until rename; directory-sync failure is reported
// even though the complete replacement may already be installed.
#pragma once
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool vk_pipeline_cache_write_file(VkDevice dev, VkPipelineCache cache, const char *path) {
    if (!dev || !cache || !path) return false;
    void *data = NULL;
    size_t size = 0;
    bool snapshot = false;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        if (vkGetPipelineCacheData(dev, cache, &size, NULL) != VK_SUCCESS || !size) break;
        free(data);
        data = malloc(size);
        if (!data) break;
        size_t capacity = size;
        VkResult result = vkGetPipelineCacheData(dev, cache, &size, data);
        if (result == VK_SUCCESS && size && size <= capacity) { snapshot = true; break; }
        if (result != VK_INCOMPLETE) break;
    }
    if (!snapshot) { free(data); return false; }
    char temporary[4096], parent[4096];
    int n = snprintf(temporary, sizeof temporary, "%s.XXXXXX", path);
    if (n < 0 || n >= (int)sizeof temporary || strlen(path) >= sizeof parent) { free(data); return false; }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (slash) { if (slash == parent) slash[1] = 0; else *slash = 0; }
    else strcpy(parent, ".");
    int fd = mkstemp(temporary);
    bool ok = fd >= 0;
    if (ok && fcntl(fd, F_SETFD, FD_CLOEXEC)) ok = false;
    size_t done = 0;
    while (ok && done < size) {
        ssize_t count = write(fd, (char *)data + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        done += (size_t)count;
    }
    free(data);
    if (ok && fsync(fd)) ok = false;
    if (fd >= 0 && close(fd)) ok = false;
    if (ok && rename(temporary, path)) ok = false;
    if (!ok) { if (fd >= 0) unlink(temporary); return false; }
    fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    ok = fsync(fd) == 0;
    if (close(fd)) ok = false;
    return ok;
}
