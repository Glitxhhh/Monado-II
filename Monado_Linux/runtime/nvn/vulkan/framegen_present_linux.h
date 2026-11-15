// [XCDE] LINUX-099: one bounded presentation owner. No renderer recording,
// shared settings, command pools or UI objects are touched by this worker.
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "framegen_pacing_linux.h"

typedef struct FgPresentJob {
    VkDevice device;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkSemaphore timeline, done[2];
    uint32_t image[2];
    uint64_t sequence;
    float interval_ms;
    PFN_vkQueuePresentKHR present;
    bool shared_queue, suboptimal;
} FgPresentJob;
typedef struct FgPresentResult {
    FgPresentJob job;
    VkResult result[2];
    // Pacing uses CLOCK_MONOTONIC; meter timestamps use the caller's explicit
    // SDL clock callback. They must never be mixed across these timebases.
    uint64_t call_ns[2], meter_ns[2];
    uint64_t gpu_wait_ns, present_ns, idle_ns, rebases, late;
    bool valid;
} FgPresentResult;
typedef struct FgPresentOps {
    uint64_t (*now)(void);
    uint64_t (*meter_now)(void);
    bool (*wait_until)(uint64_t deadline);
    VkResult (*wait_ready)(const FgPresentJob *job);
    VkResult (*present)(const FgPresentJob *job, unsigned image);
} FgPresentOps;
typedef struct FgPresenter {
    pthread_mutex_t lock;
    pthread_cond_t ready, complete;
    pthread_t thread;
    bool started, stopping, busy, queued;
    FgPresentJob job;
    FgPresentResult result;
    FgPresentOps ops;
    FgPacingClock clock; // worker only; reset exclusively after idle under lock
} FgPresenter;
#define FG_PRESENTER_INITIALIZER { .lock=PTHREAD_MUTEX_INITIALIZER, .ready=PTHREAD_COND_INITIALIZER, .complete=PTHREAD_COND_INITIALIZER }

static void fg_presenter_wait(FgPresenter *p, uint64_t deadline, FgPresentResult *r) {
    uint64_t before=p->ops.now();
    if (!deadline || !before) return;
    if (before<deadline) p->ops.wait_until(deadline);
    uint64_t after=p->ops.now();
    if (after>=before) r->idle_ns+=after-before;
    // Oversleep is diagnostic lateness, not a count of dropped/displayed frames.
    r->late+=after>deadline;
}
static void *fg_presenter_main(void *arg) {
    FgPresenter *p=arg;
    pthread_mutex_lock(&p->lock);
    for (;;) {
        while (!p->queued && !p->stopping) pthread_cond_wait(&p->ready,&p->lock);
        if (p->stopping) break;
        FgPresentJob job=p->job; p->queued=false;
        pthread_mutex_unlock(&p->lock);
        FgPresentResult r={.job=job,.valid=true};
        uint64_t before=p->ops.now();
        VkResult ready=p->ops.wait_ready(&job);
        uint64_t now=p->ops.now();
        if (now>=before) r.gpu_wait_ns=now-before;
        FgPacingPlan plan={0};
        bool timed=ready==VK_SUCCESS && fg_pacing_schedule(&p->clock,now,job.interval_ms,&plan);
        r.rebases=timed&&plan.rebased;
        r.late=timed&&plan.missed;
        if (!timed) fg_pacing_clock_reset(&p->clock);
        for (unsigned i=0;i<2;i++) {
            // Even an OUT_OF_DATE first presentation must resolve the second
            // owned image before the caller is allowed to destroy this chain.
            if (ready==VK_SUCCESS && (i==0 || r.result[0]==VK_SUCCESS || r.result[0]==VK_SUBOPTIMAL_KHR)) {
                uint64_t deadline=i ? fg_pacing_output_deadline(&p->clock,plan.real_ns) : plan.generated_ns;
                if (timed) fg_presenter_wait(p,deadline,&r);
            }
            r.call_ns[i]=p->ops.now();
            r.result[i]=ready==VK_SUCCESS ? p->ops.present(&job,i) : ready;
            uint64_t end=p->ops.now();
            if(end>=r.call_ns[i])r.present_ns+=end-r.call_ns[i];
            if(r.result[i]==VK_SUCCESS||r.result[i]==VK_SUBOPTIMAL_KHR){
                r.meter_ns[i]=p->ops.meter_now();
                fg_pacing_observe(&p->clock,r.call_ns[i]);
            }else fg_pacing_clock_reset(&p->clock);
        }
        pthread_mutex_lock(&p->lock);
        p->result=r; p->busy=false;
        pthread_cond_broadcast(&p->complete);
    }
    pthread_mutex_unlock(&p->lock);return NULL;
}
static bool fg_presenter_start(FgPresenter *p,const FgPresentOps *ops) {
    pthread_mutex_lock(&p->lock);
    if(p->started){pthread_mutex_unlock(&p->lock);return true;}
    if(!ops||!ops->now||!ops->meter_now||!ops->wait_until||!ops->wait_ready||!ops->present){pthread_mutex_unlock(&p->lock);return false;}
    p->ops=*ops; p->stopping=false;
    if(pthread_create(&p->thread,NULL,fg_presenter_main,p)){pthread_mutex_unlock(&p->lock);return false;}
    p->started=true;pthread_mutex_unlock(&p->lock);return true;
}
// One active pair, no unbounded queue. The producer records/submits the next
// frame before this publication barrier, overlapping its work with old output.
static bool fg_presenter_enqueue(FgPresenter *p,const FgPresentJob *job,FgPresentResult *previous) {
    pthread_mutex_lock(&p->lock);
    if(!p->started||p->stopping){pthread_mutex_unlock(&p->lock);return false;}
    while(p->busy)pthread_cond_wait(&p->complete,&p->lock);
    if(previous)*previous=p->result;
    p->result.valid=false;p->job=*job;p->busy=p->queued=true;
    pthread_cond_signal(&p->ready);pthread_mutex_unlock(&p->lock);return true;
}
// The render thread consumes successful events and performs all FPS/UI calls.
// Other threads may wait with out==NULL, but never consume renderer events.
static bool fg_presenter_collect(FgPresenter *p,bool wait,FgPresentResult *out) {
    pthread_mutex_lock(&p->lock);
    while(wait&&p->busy)pthread_cond_wait(&p->complete,&p->lock);
    bool have=!p->busy&&p->result.valid;
    if(have&&out){*out=p->result;p->result.valid=false;}
    pthread_mutex_unlock(&p->lock);return have;
}
// A non-rendering drain must validate failures without consuming renderer-owned
// FPS events. Inspect the immutable completed result under the same lock.
static bool fg_presenter_inspect(FgPresenter *p,bool wait,FgPresentResult *out) {
    pthread_mutex_lock(&p->lock);
    while(wait&&p->busy)pthread_cond_wait(&p->complete,&p->lock);
    bool have=!p->busy&&p->result.valid;
    if(have&&out)*out=p->result;
    pthread_mutex_unlock(&p->lock);return have;
}
static void fg_presenter_reset(FgPresenter *p) {
    pthread_mutex_lock(&p->lock);
    while(p->busy)pthread_cond_wait(&p->complete,&p->lock);
    p->result.valid=false;fg_pacing_clock_reset(&p->clock);
    pthread_mutex_unlock(&p->lock);
}
// Test/process teardown only; ordinary reset keeps the idle worker available.
static void fg_presenter_stop(FgPresenter *p) {
    pthread_mutex_lock(&p->lock);
    if(!p->started){pthread_mutex_unlock(&p->lock);return;}
    while(p->busy)pthread_cond_wait(&p->complete,&p->lock);
    p->stopping=true;pthread_cond_signal(&p->ready);pthread_mutex_unlock(&p->lock);
    pthread_join(p->thread,NULL);
    pthread_mutex_lock(&p->lock);p->started=p->stopping=false;p->result.valid=false;fg_pacing_clock_reset(&p->clock);pthread_mutex_unlock(&p->lock);
}
