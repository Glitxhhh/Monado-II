// [XCDE] LINUX-099: actual pthread output worker, authored queue/GPU gates only.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#ifndef FRAMEGEN_ASYNC_HEADER
#define FRAMEGEN_ASYNC_HEADER "../../runtime/nvn/vulkan/framegen_present_linux.h"
#endif
#include FRAMEGEN_ASYNC_HEADER
static FgPresenter output=FG_PRESENTER_INITIALIZER;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static uint64_t allowed,waiting,epoch;
static unsigned presents;static uint64_t sequences[256];static unsigned image_order[256];
static VkResult first_error=VK_SUCCESS,wait_error=VK_SUCCESS;
static atomic_bool enqueuing,enqueued,resetting,reset_done;
static FgPresentResult previous;
static uint64_t meter_now(void){return fg_pacing_now()-epoch+1;}
static VkResult ready(const FgPresentJob *job){
 pthread_mutex_lock(&gate);waiting=job->sequence;pthread_cond_broadcast(&changed);
 while(allowed<job->sequence)pthread_cond_wait(&changed,&gate);
 VkResult result=wait_error;pthread_mutex_unlock(&gate);return result;
}
static VkResult present(const FgPresentJob *job,unsigned i){
 assert(i<2&&job->device==(VkDevice)(uintptr_t)0x11&&job->queue==(VkQueue)(uintptr_t)0x22);
 assert(job->swapchain==(VkSwapchainKHR)(uintptr_t)0x33&&job->image[i]==2+i);
 assert(job->done[i]==(VkSemaphore)(uintptr_t)(0x44+i));
 pthread_mutex_lock(&gate);assert(presents<256);sequences[presents]=job->sequence;image_order[presents++]=i;
 VkResult result=i==0?first_error:VK_SUCCESS;pthread_mutex_unlock(&gate);return result;
}
static void await_worker(uint64_t seq){pthread_mutex_lock(&gate);while(waiting<seq)pthread_cond_wait(&changed,&gate);pthread_mutex_unlock(&gate);}
static void release_gpu(uint64_t seq){pthread_mutex_lock(&gate);allowed=seq;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);}
static FgPresentJob job(uint64_t seq){return(FgPresentJob){.device=(VkDevice)(uintptr_t)0x11,.queue=(VkQueue)(uintptr_t)0x22,.swapchain=(VkSwapchainKHR)(uintptr_t)0x33,.done={(VkSemaphore)(uintptr_t)0x44,(VkSemaphore)(uintptr_t)0x45},.image={2,3},.sequence=seq,.interval_ms=20};}
static void *enqueue_next(void *arg){(void)arg;FgPresentJob j=job(43);atomic_store(&enqueuing,true);assert(fg_presenter_enqueue(&output,&j,&previous));atomic_store(&enqueued,true);return NULL;}
static void *reset_output(void *arg){(void)arg;atomic_store(&resetting,true);fg_presenter_reset(&output);atomic_store(&reset_done,true);return NULL;}
static void yield_until(atomic_bool *flag){while(!atomic_load(flag)){struct timespec t={0,100000};nanosleep(&t,NULL);}}
int main(void){
 epoch=fg_pacing_now();FgPresentResult r={0};FgPresentJob a=job(42);
 assert(!fg_presenter_enqueue(&output,&a,&r));assert(!fg_presenter_start(&output,NULL));
 FgPresentOps ops={fg_pacing_now,meter_now,fg_pacing_wait,ready,present};
 assert(fg_presenter_start(&output,&ops));assert(fg_presenter_start(&output,&ops));
 assert(fg_presenter_enqueue(&output,&a,&r)&&!r.valid);
 await_worker(42);assert(!fg_presenter_collect(&output,false,&r));
 // The renderer advances independently while the output worker is held on GPU.
 unsigned producer_progress=1;producer_progress++;assert(producer_progress==2&&!presents);
 a.sequence=999;a.image[0]=99; // worker must own an immutable copied snapshot
 pthread_t next;assert(!pthread_create(&next,NULL,enqueue_next,NULL));yield_until(&enqueuing);
 struct timespec brief={0,1000000};nanosleep(&brief,NULL);assert(!atomic_load(&enqueued));
 release_gpu(42);assert(!pthread_join(next,NULL));assert(atomic_load(&enqueued));
 assert(previous.valid&&previous.job.sequence==42&&previous.result[0]==VK_SUCCESS&&previous.result[1]==VK_SUCCESS);
 assert(previous.call_ns[1]-previous.call_ns[0]>=10000000);
 assert(previous.meter_ns[0]<previous.call_ns[0]&&previous.meter_ns[1]<previous.call_ns[1]);
 assert(previous.meter_ns[1]>=previous.meter_ns[0]);assert(presents==2&&sequences[0]==42&&sequences[1]==42&&image_order[0]==0&&image_order[1]==1);
 await_worker(43);assert(!fg_presenter_collect(&output,false,&r));
 pthread_t reset;assert(!pthread_create(&reset,NULL,reset_output,NULL));yield_until(&resetting);nanosleep(&brief,NULL);assert(!atomic_load(&reset_done));
 release_gpu(43);assert(!pthread_join(reset,NULL)&&atomic_load(&reset_done));assert(!fg_presenter_collect(&output,false,&r));
 pthread_mutex_lock(&output.lock);assert(!output.busy&&!output.clock.active);pthread_mutex_unlock(&output.lock);
 first_error=VK_ERROR_OUT_OF_DATE_KHR;release_gpu(44);a=job(44);assert(fg_presenter_enqueue(&output,&a,&r));assert(fg_presenter_collect(&output,true,&r));
 assert(r.valid&&r.result[0]==VK_ERROR_OUT_OF_DATE_KHR&&r.result[1]==VK_SUCCESS&&r.meter_ns[0]==0&&r.meter_ns[1]);
 assert(presents==6&&sequences[4]==44&&sequences[5]==44); // both old-SC images resolved
 first_error=VK_SUBOPTIMAL_KHR;release_gpu(45);a=job(45);assert(fg_presenter_enqueue(&output,&a,&r));assert(fg_presenter_collect(&output,true,&r));assert(r.result[0]==VK_SUBOPTIMAL_KHR&&r.result[1]==VK_SUCCESS&&r.meter_ns[0]);
 wait_error=VK_ERROR_DEVICE_LOST;release_gpu(46);a=job(46);unsigned old=presents;assert(fg_presenter_enqueue(&output,&a,&r));assert(fg_presenter_collect(&output,true,&r));assert(r.result[0]==VK_ERROR_DEVICE_LOST&&r.result[1]==VK_ERROR_DEVICE_LOST&&!r.meter_ns[0]&&!r.meter_ns[1]&&presents==old);
 wait_error=first_error=VK_SUCCESS;
 for(uint64_t i=47;i<67;i++){release_gpu(i);a=job(i);a.interval_ms=2;assert(fg_presenter_enqueue(&output,&a,&r));assert(fg_presenter_collect(&output,true,&r));assert(r.job.sequence==i&&r.result[0]==VK_SUCCESS&&r.result[1]==VK_SUCCESS&&r.call_ns[1]-r.call_ns[0]>=1000000);if(i%3==0)fg_presenter_reset(&output);}
 fg_presenter_stop(&output);assert(!output.started);assert(fg_presenter_start(&output,&ops));fg_presenter_stop(&output);
 puts("actual bounded pthread presenter: producer overlap, backpressure, immutable jobs, phase, SDL-clock separation, error resolution, reset and restart passed");return 0;
}
