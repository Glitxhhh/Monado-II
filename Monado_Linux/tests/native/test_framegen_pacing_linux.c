// [XCDE] LINUX-020: actual monotonic pacing helper, deterministic syscall faults and real interruptible waits.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#ifndef FRAMEGEN_PACING_REAL_CLOCK
static unsigned clocks, waits, interrupts;
static int clock_error, wait_error;
static unsigned clock_fail_on;
static struct timespec clock_value;
static uint64_t expected_deadline;
static uint64_t interrupt_advance, oversleep, error_advance;
static bool backwards;
static uint64_t mock_now(void) { return (uint64_t)clock_value.tv_sec*1000000000ull+(uint64_t)clock_value.tv_nsec; }
static void mock_set(uint64_t value) { clock_value=(struct timespec){(time_t)(value/1000000000ull),(long)(value%1000000000ull)}; }
static int mock_clock(clockid_t clock, struct timespec *out) {
    assert(clock == CLOCK_MONOTONIC); clocks++;
    if (clock_error || clocks==clock_fail_on) { errno=clock_error?clock_error:EIO; return -1; }
    *out=clock_value; return 0;
}
static int mock_wait(clockid_t clock, int flags, const struct timespec *deadline, struct timespec *remaining) {
    assert(clock == CLOCK_MONOTONIC && flags == TIMER_ABSTIME && !remaining);
    assert(deadline->tv_sec == (time_t)(expected_deadline/1000000000ull));
    assert(deadline->tv_nsec == (long)(expected_deadline%1000000000ull));
    waits++;
    if (interrupts) { interrupts--; mock_set(mock_now()+interrupt_advance); return EINTR; }
    if (backwards) mock_set(1);
    else if (wait_error) mock_set(mock_now()+error_advance);
    else mock_set((mock_now()>expected_deadline?mock_now():expected_deadline)+oversleep);
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
    uint64_t elapsed=99;
    assert(!fg_pacing_wait_elapsed(0,&elapsed)&&elapsed==0);cases++;
    wait_error=0;mock_set(5000000000ull);expected_deadline=5008000000ull;
    assert(fg_pacing_wait_elapsed(expected_deadline,&elapsed)&&elapsed==8000000ull);cases++;
    // Six milliseconds of preparation consume six milliseconds of the same
    // deadline, leaving two milliseconds to account as deliberate idle.
    mock_set(5006000000ull);
    assert(fg_pacing_wait_elapsed(expected_deadline,&elapsed)&&elapsed==2000000ull);cases++;
    mock_set(5012000000ull);
    assert(fg_pacing_wait_elapsed(expected_deadline,&elapsed)&&elapsed==0);cases++;
    mock_set(5000000000ull);interrupts=2;interrupt_advance=1000000;oversleep=2000000;
    assert(fg_pacing_wait_elapsed(expected_deadline,&elapsed)&&elapsed==10000000ull);cases++;
    oversleep=interrupt_advance=0;wait_error=EIO;error_advance=1000000;mock_set(5000000000ull);
    assert(!fg_pacing_wait_elapsed(expected_deadline,&elapsed)&&elapsed==1000000ull);cases++;
    wait_error=0;error_advance=0;mock_set(5000000000ull);clock_fail_on=clocks+2;
    assert(fg_pacing_wait_elapsed(expected_deadline,&elapsed)&&elapsed==0);clock_fail_on=0;cases++;
    mock_set(5000000000ull);clock_error=EIO;
    assert(fg_pacing_wait_elapsed(expected_deadline,&elapsed)&&elapsed==0);clock_error=0;cases++;
    mock_set(5000000000ull);backwards=true;
    assert(fg_pacing_wait_elapsed(expected_deadline,&elapsed)&&elapsed==0);backwards=false;cases++;
    mock_set(5000000000ull);unsigned old_clocks=clocks;
    assert(fg_pacing_wait_elapsed(expected_deadline,NULL)&&clocks==old_clocks);cases++;
    const int targets[]={30,60,120,165};
    const uint64_t halves[]={16666666ull,8333333ull,4166666ull,3030303ull};
    for(unsigned i=0;i<sizeof targets/sizeof*targets;i++) {
        float measured=40.f,interval=fg_pacing_interval_ms(measured,targets[i]);
        assert(measured==40.f&&fg_pacing_deadline(1000,interval,&deadline)&&deadline==1000+halves[i]);cases++;
        assert(fg_pacing_interval_ms(4.f,targets[i])==4.f);cases++;
    }
    float world=fg_pacing_interval_ms(40.f,165),cutscene=fg_pacing_interval_ms(40.f,30);
    assert(cutscene>world&&fg_pacing_interval_ms(40.f,165)==world);cases++;
    assert(fg_pacing_interval_ms(40.f,0)==40.f&&fg_pacing_interval_ms(40.f,-60)==40.f);cases++;
    for(unsigned i=0;i<sizeof invalid/sizeof*invalid;i++) {
        float interval=fg_pacing_interval_ms(invalid[i],60);
        assert(isnan(invalid[i])?isnan(interval):interval==invalid[i]);
        deadline=77;assert(!fg_pacing_deadline(1000,interval,&deadline)&&deadline==77);cases++;
    }
    float interval=fg_pacing_interval_ms(40.f,60);
    assert(fg_pacing_deadline(UINT64_MAX-8333333ull,interval,&deadline)&&deadline==UINT64_MAX);cases++;
    deadline=77;assert(!fg_pacing_deadline(UINT64_MAX-8333332ull,interval,&deadline)&&deadline==77);cases++;
    assert(fg_pacing_interval_ms(40.f,INT_MIN)==40.f);cases++;
    interval=fg_pacing_interval_ms(40.f,INT_MAX);assert(isfinite(interval)&&interval>0&&interval<1.f);cases++;
    double old_period=16.,bounded_period=16.;
    for(unsigned i=0;i<32;i++) {
        float paced=fg_pacing_interval_ms((float)bounded_period,60);
        assert(paced<=1000.f/60.f&&paced<=bounded_period);
        old_period=12.+old_period*.5;bounded_period=12.+paced*.5;
    }
    assert(old_period>23.99&&bounded_period<=20.334&&bounded_period>20.3);cases++;
    printf("MODEL synchronous_feedback_ms=%.6f bounded_pacing_ms=%.6f; scheduling arithmetic only, no game-performance claim\n",old_period,bounded_period);
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
    uint64_t elapsed;
    begin=fg_pacing_now();assert(fg_pacing_deadline(begin,4,&deadline));
    assert(fg_pacing_wait_elapsed(deadline,&elapsed));after=fg_pacing_now();
    assert(after>=deadline&&elapsed>0&&elapsed<=after-begin);cases++;
    printf("MEASURE observed_wait_ns=%llu outer_ns=%llu\n",(unsigned long long)elapsed,(unsigned long long)(after-begin));
    begin=fg_pacing_now();assert(fg_pacing_wait_elapsed(begin-1,&elapsed));after=fg_pacing_now();
    assert(elapsed<=after-begin);cases++;
    printf("MEASURE expired_wait_ns=%llu outer_ns=%llu\n",(unsigned long long)elapsed,(unsigned long long)(after-begin));
    printf("actual pacing helper: %u real-clock cases passed (%d delivered signals)\n",cases,(int)caught);
#endif
    return 0;
}
