// [XCDE] LINUX-101: bounded-rate local FPS samples, MIT. Authored data only.
// SDL timestamps schedule logging; observer timestamps remain CLOCK_MONOTONIC.
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fps_meter.h"
#include "../nvn/vulkan/present_observer_linux.h"

typedef struct FpsOutputLogClock {
    uint64_t last_ns;
    bool started;
} FpsOutputLogClock;

// A swapchain/meter reset does not reset this clock. A backward SDL clock starts
// another full interval rather than flooding logs or subtracting unsigned time.
static inline bool fps_output_log_due(FpsOutputLogClock *clock,uint64_t now_ns,bool enabled) {
    if(!enabled)return false;
    if(!clock->started) {clock->started=true;clock->last_ns=now_ns;return true;}
    if(now_ns<clock->last_ns) {clock->last_ns=now_ns;return false;}
    if(now_ns-clock->last_ns<UINT64_C(2000000000))return false;
    clock->last_ns=now_ns;return true;
}

typedef struct FpsOutputLogSample {
    uint64_t sdl_ns,meter_epoch;
    unsigned method; // 1: selected FSR, 2: selected NVIDIA driver method
    bool game_ready,output_available,output_ready,fsr_inputs_ready;
    double game_fps,output_fps;
    FpsPresentRates application;
    uint64_t application_accepted[3];
    MonadoPresentObserverSnapshot driver;
    uint64_t driver_now_ns;
} FpsOutputLogSample;

static inline bool fps_output_log_valid_rate(bool ready,double rate) {
    return ready&&isfinite(rate)&&rate>=0&&rate<=1e9;
}
static inline const char *fps_output_log_id(const char *id) {
    if(!id||strlen(id)!=32)return "direct";
    for(unsigned i=0;i<32;i++)if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')))return "direct";
    return id;
}
static inline void fps_output_log_number(char out[32],bool ready,double number) {
    if(fps_output_log_valid_rate(ready,number))snprintf(out,32,"%.3f",number);
    else snprintf(out,32,"unavailable");
}

// No generated/native identity is inferred for the driver. Its total is the
// observed downstream rate, independent of the game's rate and application SC.
static inline size_t fps_output_log_format(char *out,size_t bytes,const char *run_id,
                                           const FpsOutputLogSample *sample) {
    if(!out||!bytes||!sample||(sample->method!=1&&sample->method!=2))return 0;
    char game[32],output[32],native[32],generated[32],repeated[32];
    bool output_ready=sample->output_available&&fps_output_log_valid_rate(sample->output_ready,sample->output_fps);
    fps_output_log_number(game,sample->game_ready,sample->game_fps);
    fps_output_log_number(output,output_ready,sample->output_fps);
    bool app_ready=sample->method==1&&output_ready;
    fps_output_log_number(native,app_ready,sample->application.native);
    fps_output_log_number(generated,app_ready,sample->application.generated);
    fps_output_log_number(repeated,app_ready,sample->application.repeated);
    const char *status=!sample->output_available||(sample->output_ready&&!output_ready)?"unavailable":output_ready?"ready":"warming";
    int count=snprintf(out,bytes,
        "[fps-output] id=%s method=%s sample_sdl_ns=%llu meter_epoch=%llu game_ready=%d game_fps=%s output_status=%s output_fps=%s output_kind=%s fsr_inputs_ready=%d generated_identity=%s native_fps=%s generated_fps=%s repeated_fps=%s app_accepted_native=%llu app_accepted_generated=%llu app_accepted_repeated=%llu driver_accepted=%llu driver_failed=%llu driver_window_ns=%llu driver_flags=%u driver_sample_monotonic_ns=%llu driver_last_accepted_monotonic_ns=%llu\n",
        fps_output_log_id(run_id),sample->method==2?"nvidia-smooth-motion-requested":"fsr-requested",
        (unsigned long long)sample->sdl_ns,(unsigned long long)sample->meter_epoch,
        fps_output_log_valid_rate(sample->game_ready,sample->game_fps),game,status,output,
        sample->method==2?"driver-accepted-presentations-not-scanout":"application-accepted-presentations-not-scanout",
        sample->method==1&&sample->fsr_inputs_ready,sample->method==2?"unavailable":"application-classified",
        native,generated,repeated,
        (unsigned long long)sample->application_accepted[0],(unsigned long long)sample->application_accepted[1],
        (unsigned long long)sample->application_accepted[2],(unsigned long long)sample->driver.accepted,
        (unsigned long long)sample->driver.failed,(unsigned long long)sample->driver.window_ns,sample->driver.flags,
        (unsigned long long)sample->driver_now_ns,(unsigned long long)sample->driver.last_accepted_ns);
    if(count<0||(size_t)count>=bytes) {out[0]=0;return 0;}
    return (size_t)count;
}
