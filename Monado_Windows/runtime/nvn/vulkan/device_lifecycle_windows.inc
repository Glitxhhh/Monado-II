// Guest producers are stopped before reboot. Queued renderer work still owns
// borrowed guest resources until the executor has drained and the GPU is idle.
void backend_reset(void) {
    if (!g_vk.dev) {
        output_scale_reset_dynamic();
        fps_overlay_reset();
        fps_game_output = fps_repeated_output = false;
        atomic_store_explicit(&g_framegen_cap_fps, 0.f, memory_order_release);
        return;
    }
    extern void backend_drain(void);
    backend_drain();
    tmp_fg_drain();
    sc_wait_idle(g_vk.dev);
    output_scale_reset_dynamic();
    fps_overlay_reset();
    fps_game_output = fps_repeated_output = false;
    atomic_store_explicit(&g_framegen_cap_fps, 0.f, memory_order_release);
    extern void vk_exec_reset(void);
    extern void vk_textures_reset(void);
    // The executor drops temporal/provider and borrowed texture references.
    // Only then may the native texture allocations/views be destroyed.
    vk_exec_reset();
    vk_textures_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {} // platform owns the native reservation
void backend_clear_window(void) {
    // Non-game clear/exit frames must not carry valid generation or FPS history.
    fps_overlay_reset();
    fps_game_output = fps_repeated_output = false;
    atomic_store_explicit(&g_framegen_cap_fps, 0.f, memory_order_release);
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) {
        tmp_fg_idle(vk_cmd());
        vk_frame_end(NULL);
    }
}
