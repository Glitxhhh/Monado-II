// Authored Windows SDK output counter: successful provider presentations, never scanout.
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct FpsProviderCounter {
    uint64_t anchor_ns, last_ns, anchor_count, last_count;
    double rate;
    bool started, ready;
} FpsProviderCounter;
static inline void fps_provider_reset(FpsProviderCounter *m) {*m=(FpsProviderCounter){0};}
static inline bool fps_provider_sample(FpsProviderCounter *m,uint64_t now,uint64_t count,bool available) {
    if(!available) {fps_provider_reset(m);return false;}
    if(!m->started || now<m->last_ns || now-m->last_ns>UINT64_C(2000000000) || count<m->last_count) {
        fps_provider_reset(m);m->started=true;m->anchor_ns=m->last_ns=now;m->anchor_count=m->last_count=count;return false;
    }
    m->last_ns=now;m->last_count=count;
    uint64_t elapsed=now-m->anchor_ns;
    if(elapsed>=UINT64_C(500000000)) {
        uint64_t delta=count-m->anchor_count;
        if(delta>UINT64_C(1000000)) {fps_provider_reset(m);return false;}
        m->rate=(double)delta*1e9/(double)elapsed;m->ready=true;m->anchor_ns=now;m->anchor_count=count;
    }
    return m->ready;
}
