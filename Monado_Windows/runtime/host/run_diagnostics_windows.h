// [XCDE] Native bounded run diagnostics, adapted from accepted LINUX-099.
// Passive only: no timing/settings mutation, private paths or binding values.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool run_diag_begin(bool precompile);
void run_diag_end(void);
const char *run_diag_id(void);
void run_diag_requested_file(const char *path, bool ignored);
void run_diag_settings(const char *event);
void run_diag_renderer(const char *event,const char *gpu,const char *mode,unsigned w,unsigned h,
                       float display_hz,float target,float cap,bool ready);
void run_diag_pacing(float target,bool cutscene_30);
void run_diag_limiter(int interval,float base,float applied);
// Render-thread measurements. Windows SDK exposes accepted Output totals,
// without native/generated identity or physical scanout information.
void run_diag_sample(uint64_t now_ns,bool selected,bool game_ready,double game,
                     bool output_available,bool output_ready,double output,uint64_t accepted,bool inputs_ready);
#ifdef __cplusplus
}
#endif
