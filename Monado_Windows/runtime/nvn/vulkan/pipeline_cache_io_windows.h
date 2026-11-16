// [XCDE] Checked native pipeline-cache snapshot. Joined workers / caller mutex own Vulkan synchronization.
#pragma once
#include "../../platform/windows_cache_transaction.h"
static bool pipeline_cache_known_header(const void *data,size_t size) {
    if(size<32)return false;uint32_t header[2];memcpy(header,data,sizeof header);
    return header[0]==32&&header[1]==VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
}
static bool pipeline_cache_save_native(VkDevice device,VkPipelineCache cache) {
    if(!device||!cache||!wct_directory("cache"))return false;
    FILE *old=wct_read("cache/vk_pipelines.bin",1024ull*1024*1024);
    if(old){uint8_t header[32];size_t count=fread(header,1,sizeof header,old);
        bool known=!ferror(old)&&pipeline_cache_known_header(header,count);if(fclose(old))known=false;
        if(!known)return false; // Unrecognized human/foreign bytes remain untouched.
    }else if(errno!=ENOENT)return false;
    // VkPipelineCache may grow between size and data calls. Never publish an incomplete snapshot.
    for(unsigned attempt=0;attempt<4;attempt++){
        size_t size=0;
        if(vkGetPipelineCacheData(device,cache,&size,NULL)!=VK_SUCCESS||!size||size>1024ull*1024*1024)return false;
        void *data=malloc(size);if(!data)return false;
        size_t actual=size;VkResult result=vkGetPipelineCacheData(device,cache,&actual,data);
        if(result==VK_INCOMPLETE){free(data);continue;}
        bool ok=result==VK_SUCCESS&&actual<=size&&pipeline_cache_known_header(data,actual)&&wct_write("cache/vk_pipelines.bin",data,actual,NULL,1024ull*1024*1024);
        free(data);return ok;
    }
    return false;
}
