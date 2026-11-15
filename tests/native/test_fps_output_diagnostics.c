// [XCDE] LINUX-101: authored telemetry values/clocks, no SDL/Vulkan/game.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../../Monado_Linux/runtime/ui/fps_output_diagnostics_linux.h"
static unsigned clock_reads,snapshot_reads;
static bool read_available=true,clock_available=true;
static int authored_clock(clockid_t clock,struct timespec *out) {
    assert(clock==CLOCK_MONOTONIC);++clock_reads;
    out->tv_sec=12345;out->tv_nsec=6789;return clock_available?0:-1;
}
static bool observer_client_read(uint64_t now,MonadoPresentObserverSnapshot *out) {
    assert(now==UINT64_C(12345000006789));++snapshot_reads;
    out->accepted=721;out->output_fps=227.4;return read_available;
}
#define clock_gettime authored_clock
#include "SNAPSHOT_SOURCE"
#undef clock_gettime
static void clocks(void) {
    FpsOutputLogClock clock={0};
    assert(!fps_output_log_due(&clock,1,false)&&!clock.started);
    assert(fps_output_log_due(&clock,1,true));
    for(uint64_t t=1;t<UINT64_C(2000000001);t+=10000000)assert(!fps_output_log_due(&clock,t,true));
    assert(fps_output_log_due(&clock,UINT64_C(2000000001),true));
    assert(!fps_output_log_due(&clock,UINT64_C(4000000001),false));
    assert(fps_output_log_due(&clock,UINT64_C(4000000001),true));
    assert(!fps_output_log_due(&clock,100,true)); // backward clock anchors another full interval
    assert(!fps_output_log_due(&clock,UINT64_C(2000000099),true));
    assert(fps_output_log_due(&clock,UINT64_C(2000000100),true));
    clock.last_ns=UINT64_MAX-1;assert(!fps_output_log_due(&clock,UINT64_MAX,true));
    MonadoPresentObserverSnapshot snapshot;uint64_t now=42;
    assert(vk_external_fps_snapshot(&snapshot,&now));
    assert(now==UINT64_C(12345000006789)&&snapshot.accepted==721&&snapshot.output_fps==227.4);
    assert(clock_reads==1&&snapshot_reads==1);
    clock_available=false;memset(&snapshot,255,sizeof snapshot);now=42;
    assert(!vk_external_fps_snapshot(&snapshot,&now)&&now==0&&snapshot.accepted==0);
    assert(clock_reads==2&&snapshot_reads==1);
    assert(!vk_external_fps_snapshot(NULL,&now));assert(!vk_external_fps_snapshot(&snapshot,NULL));
}
static void formatting(void) {
    char line[1152];FpsOutputLogSample sample={.method=2,.sdl_ns=57,.meter_epoch=3,
        .game_ready=true,.game_fps=83.3,.output_available=true,.output_ready=true,.output_fps=227.4,
        .driver={.accepted=721,.failed=3,.window_ns=500000000,.flags=7,.last_accepted_ns=12345},.driver_now_ns=12346};
    const char *id="0123456789abcdef0123456789abcdef";
    assert(fps_output_log_format(line,sizeof line,id,&sample)>0);
    assert(strstr(line,"game_fps=83.300")&&strstr(line,"output_fps=227.400"));
    assert(strstr(line,"driver_accepted=721 driver_failed=3 driver_window_ns=500000000 driver_flags=7"));
    assert(strstr(line,"sample_sdl_ns=57")&&strstr(line,"driver_sample_monotonic_ns=12346"));
    assert(strstr(line,"generated_identity=unavailable")&&strstr(line,"generated_fps=unavailable"));
    assert(!strstr(line,"166.600")); // no Game*2 or Output-Game inference
    sample.output_ready=false;sample.game_ready=false;
    assert(fps_output_log_format(line,sizeof line,"unsafe\nlog\rkey",&sample)>0);
    assert(strstr(line,"id=direct")&&strstr(line,"output_status=warming output_fps=unavailable"));
    sample.output_available=false;assert(fps_output_log_format(line,sizeof line,id,&sample)>0);
    assert(strstr(line,"output_status=unavailable"));
    sample.output_available=sample.output_ready=true;sample.output_fps=NAN;sample.game_ready=true;sample.game_fps=INFINITY;
    assert(fps_output_log_format(line,sizeof line,id,&sample)>0);
    assert(strstr(line,"output_status=unavailable output_fps=unavailable")&&strstr(line,"game_ready=0 game_fps=unavailable"));
    assert(!strstr(line,"nan")&&!strstr(line,"inf"));
    sample.method=1;sample.game_fps=89.94;sample.output_fps=179.88;sample.fsr_inputs_ready=true;
    sample.application=(FpsPresentRates){89.94,89.94,0,179.88};
    sample.application_accepted[0]=900;sample.application_accepted[1]=900;sample.application_accepted[2]=3;
    memset(&sample.driver,0,sizeof sample.driver);sample.driver_now_ns=0;
    assert(fps_output_log_format(line,sizeof line,id,&sample)>0);
    assert(strstr(line,"generated_identity=application-classified")&&strstr(line,"fsr_inputs_ready=1"));
    assert(strstr(line,"native_fps=89.940 generated_fps=89.940 repeated_fps=0.000"));
    assert(strstr(line,"app_accepted_native=900 app_accepted_generated=900 app_accepted_repeated=3"));
    sample.output_fps=-1;assert(fps_output_log_format(line,sizeof line,id,&sample)>0);
    assert(strstr(line,"output_status=unavailable"));
    sample.output_fps=1e10;assert(fps_output_log_format(line,sizeof line,id,&sample)>0);
    assert(strstr(line,"output_fps=unavailable"));
    assert(!fps_output_log_format(line,3,id,&sample)&&line[0]==0);
    assert(!fps_output_log_format(NULL,100,id,&sample));assert(!fps_output_log_format(line,sizeof line,id,NULL));
    sample.method=0;assert(!fps_output_log_format(line,sizeof line,id,&sample));
}
int main(void) {clocks();formatting();puts("native FPS output diagnostics: PASS");return 0;}
