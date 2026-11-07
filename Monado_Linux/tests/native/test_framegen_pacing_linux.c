// [XCDE] LINUX-020: actual monotonic pacing helper, deterministic syscall faults and real interruptible waits.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#ifndef FRAMEGEN_PACING_REAL_CLOCK
static unsigned clocks, waits, interrupts;
static int clock_error, wait_error;
static struct timespec clock_value;
static uint64_t expected_deadline;
static int mock_clock(clockid_t clock, struct timespec *out) {
    assert(clock == CLOCK_MONOTONIC); clocks++;
    if (clock_error) { errno=clock_error; return -1; }
    *out=clock_value; return 0;
}
static int mock_wait(clockid_t clock, int flags, const struct timespec *deadline, struct timespec *remaining) {
    assert(clock == CLOCK_MONOTONIC && flags == TIMER_ABSTIME && !remaining);
    assert(deadline->tv_sec == (time_t)(expected_deadline/1000000000ull));
    assert(deadline->tv_nsec == (long)(expected_deadline%1000000000ull));
    waits++;
    if (interrupts) { interrupts--; return EINTR; }
    return wait_error;
}
#define clock_gettime mock_clock
#define clock_nanosleep mock_wait
#endif
#include "../../runtime/nvn/vulkan/framegen_pacing_linux.h"
#ifndef FRAMEGEN_PACING_REAL_CLOCK
#undef clock_gettime
#undef clock_nanosleep
#endif

#ifdef FRAMEGEN_PACING_REAL_CLOCK
static volatile sig_atomic_t caught;
static void alarm_handler(int signum) { if(signum==SIGALRM)caught++; }
#endif
int main(void) {
    unsigned cases=0;uint64_t deadline=77;
#ifndef FRAMEGEN_PACING_REAL_CLOCK
    clock_value=(struct timespec){2,345678901};assert(fg_pacing_now()==2345678901ull && clocks==1);cases++;
    clock_error=EIO;assert(fg_pacing_now()==0 && clocks==2);clock_error=0;cases++;
    const float invalid[]={NAN,INFINITY,-INFINITY,-1,0,.999f,100.001f};
    for(unsigned i=0;i<sizeof invalid/sizeof*invalid;i++) { deadline=77;assert(!fg_pacing_deadline(1000,invalid[i],&deadline)&&deadline==77);cases++; }
    assert(!fg_pacing_deadline(0,16,&deadline));cases++;
    assert(!fg_pacing_deadline(1000,16,NULL));cases++;
    assert(fg_pacing_deadline(1000,1,&deadline)&&deadline==501000);cases++;
    assert(fg_pacing_deadline(1000,100,&deadline)&&deadline==50001000);cases++;
    assert(fg_pacing_deadline(UINT64_MAX-500000,1,&deadline)&&deadline==UINT64_MAX);cases++;
    deadline=77;assert(!fg_pacing_deadline(UINT64_MAX-499999,1,&deadline)&&deadline==77);cases++;
    assert(!fg_pacing_wait(0)&&waits==0);cases++;
    expected_deadline=UINT64_C(1234567890123456789);interrupts=3;
    assert(fg_pacing_wait(expected_deadline)&&waits==4);cases++;
    wait_error=EINVAL;assert(!fg_pacing_wait(expected_deadline)&&waits==5);cases++;
    wait_error=EIO;errno=EINTR;assert(!fg_pacing_wait(expected_deadline)&&waits==6);cases++;
    printf("actual pacing helper: %u deterministic cases passed\n",cases);
#else
    uint64_t begin=fg_pacing_now();assert(begin>0);cases++;
    assert(fg_pacing_deadline(begin,4,&deadline));assert(fg_pacing_wait(deadline));
    uint64_t after=fg_pacing_now();assert(after>=deadline);cases++;
    // An expired absolute deadline must not become a long relative sleep.
    assert(fg_pacing_wait(after-1));assert(fg_pacing_now()>=after);cases++;
    struct sigaction action={0},previous;action.sa_handler=alarm_handler;sigemptyset(&action.sa_mask);
    assert(sigaction(SIGALRM,&action,&previous)==0);
    struct itimerval timer={.it_interval={0,1000},.it_value={0,1000}},off={0};
    assert(setitimer(ITIMER_REAL,&timer,NULL)==0);
    begin=fg_pacing_now();assert(fg_pacing_deadline(begin,20,&deadline));assert(fg_pacing_wait(deadline));after=fg_pacing_now();
    assert(setitimer(ITIMER_REAL,&off,NULL)==0);assert(sigaction(SIGALRM,&previous,NULL)==0);
    assert(caught>0 && after>=deadline);cases++;
    printf("actual pacing helper: %u real-clock cases passed (%d delivered signals)\n",cases,(int)caught);
#endif
    return 0;
}
