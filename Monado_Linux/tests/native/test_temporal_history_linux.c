// [XCDE] LINUX-017: execute actual temporal history/math/jitter policy without a GPU or game data.
#include <assert.h>
#include <stdio.h>
#include "../../runtime/nvn/vulkan/temporal.c"

Settings g_settings;
static bool provider_ready = true, deterministic;
static unsigned resets;
static uint64_t present_frame;
bool dlss_available(void) { return provider_ready; }
void dlss_reset(void) { resets++; }
bool diag_deterministic(void) { return deterministic; }
uint64_t renderer_present_count(void) { return present_frame; }

static TmpCamera camera(void) {
    TmpCamera c = {.ok = true};
    c.view[0] = c.view[5] = c.view[10] = 1;
    c.proj[0] = c.proj[5] = c.proj[10] = c.proj[15] = 1;
    memcpy(c.viewproj, c.proj, sizeof c.proj);
    return c;
}
static void begin(void) {
    tmp_reset();
    g_settings.upscaler = UPSCALE_DLSS;
    g_settings.upscale_quality = UQ_QUALITY;
    provider_ready = true; deterministic = false;
    unsetenv("XCDE_TMP_OFF"); unsetenv("XCDE_TMP_DETERMINISTIC");
}
static void seed(const TmpCamera *c, uint64_t frame) { history_commit(c, 1280, 720, 1920, 1080, 2, frame, true); }
static bool prepare(const TmpCamera *c, uint64_t frame, bool *have, double m[16]) {
    const float j[2] = {0, 0};
    return history_prepare(c, j, 1280, 720, 1920, 1080, 2, frame, m, have);
}
static void zero_matrix(const double m[16]) { for (int i = 0; i < 16; i++) assert(m[i] == 0); }
int main(void) {
    unsigned cases = 0;
    begin();
    TmpCamera c = camera();
    bool have; double m[16];
    assert(prepare(&c, 0, &have, m) && !have); zero_matrix(m); cases++;
    seed(&c, 10);
    assert(prepare(&c, 11, &have, m) && have);
    for (int i = 0; i < 16; i++) assert(fabs(m[i] - (i % 5 == 0)) < 1e-12);
    cases++;
    TmpCamera moved = c; moved.view[3] = moved.viewproj[3] = .25f;
    assert(prepare(&moved, 11, &have, m) && have && fabs(m[3] + .25) < 1e-12); cases++;
    assert(prepare(&c, 12, &have, m) && !have); zero_matrix(m); cases++; // skipped evaluation
    assert(prepare(&c, 10, &have, m) && !have); cases++; // duplicate frame
    const float j[2] = {0, 0};
    assert(history_prepare(&c,j,960,540,1920,1080,2,11,m,&have) && !have); cases++;
    assert(history_prepare(&c,j,1280,720,2560,1440,2,11,m,&have) && !have); cases++;
    assert(history_prepare(&c,j,1280,720,1920,1080,1,11,m,&have) && !have); cases++;
    g_settings.upscale_quality = UQ_BALANCED;
    assert(prepare(&c, 11, &have, m) && !have); cases++;
    g_settings.upscale_quality = UQ_QUALITY; g_settings.upscaler = UPSCALE_FSR3;
    assert(prepare(&c, 11, &have, m) && !have); cases++;
    g_settings.upscaler = UPSCALE_DLSS;
    moved.view[3] = 2;
    assert(prepare(&moved, 11, &have, m) && !have); zero_matrix(m); cases++;
    history_commit(NULL, 0,0,0,0,0,0,false);
    assert(!previous.ok && prepare(&c, 11, &have, m) && !have); cases++;
    assert(!prepare(NULL, 11, &have, m) && !have); cases++;
    TmpCamera bad = c; bad.ok = false;
    assert(!prepare(&bad, 11, &have, m)); cases++;
    bad = c; bad.view[3] = NAN;
    assert(!prepare(&bad, 11, &have, m)); cases++;
    bad = c; bad.proj[0] = INFINITY;
    assert(!prepare(&bad, 11, &have, m)); cases++;
    bad = c; bad.viewproj[3] = NAN;
    assert(!prepare(&bad, 11, &have, m)); cases++;
    bad = c; memset(bad.viewproj, 0, sizeof bad.viewproj);
    assert(!prepare(&bad, 11, &have, m)); cases++;
    const float bad_j[2] = {NAN, 0};
    assert(!history_prepare(&c,bad_j,1280,720,1920,1080,2,11,m,&have)); cases++;
    assert(!history_prepare(&c,j,0,720,1920,1080,2,11,m,&have)); cases++;
    assert(!history_prepare(&c,j,1280,720,16385,1080,2,11,m,&have)); cases++;
    assert(!history_prepare(&c,j,1922,720,1920,1080,2,11,m,&have)); cases++;
    begin();
    assert(tmp_enabled()); seed(&c, 10);
    provider_ready = false; assert(!tmp_enabled() && !previous.ok); cases++;
    provider_ready = true; assert(tmp_enabled()); cases++;
    deterministic = true; assert(!tmp_enabled()); cases++;
    setenv("XCDE_TMP_DETERMINISTIC", "1", 1); assert(tmp_enabled()); cases++;
    setenv("XCDE_TMP_OFF", "1", 1); assert(!tmp_enabled()); cases++;
    begin();
    float first[2], cur[2]; tmp_jitter(first);
    assert(fabsf(first[0]) < 1e-7 && fabsf(first[1] + 1.0f / 6) < 1e-6); cases++;
    for (int i = 0; i < 7; i++) tmp_jitter(cur);
    tmp_jitter(cur); assert(cur[0] == first[0] && cur[1] == first[1]); cases++;
    seed(&c, 10); game_vel = (TexBackend *)(uintptr_t)0x1234; game_vel_frame = snap_frame = present_frame = 10;
    assert(tmp_hud_started());
    unsigned before = resets; tmp_reset(); tmp_jitter(cur);
    assert(resets == before + 1 && !previous.ok && !game_vel && game_vel_frame == ~0ull && !tmp_hud_started());
    assert(cur[0] == first[0] && cur[1] == first[1]); cases++;
    assert(!tmp_fg_active() && !tmp_frame_gen_wanted() && !tmp_fg_hudless_target());
    assert(!tmp_fg_install(NULL,NULL,NULL,NULL,NULL,NULL,NULL)); cases++;
    double singular[16] = {0}, inv[16]; assert(!mat_inv(singular,inv)); singular[0] = NAN; assert(!mat_inv(singular,inv)); cases++;
    TexBackend velocity = {0}, other = {0};
    tmp_set_velocity(&velocity); tmp_forget_texture(&other); assert(game_vel == &velocity); cases++;
    tmp_forget_texture(&velocity); assert(!game_vel && game_vel_frame == ~0ull); cases++;
    printf("temporal history/math/jitter: %u cases passed; actual source, no GPU/game\n", cases);
    return 0;
}
