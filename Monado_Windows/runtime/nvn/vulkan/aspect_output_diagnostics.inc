// [XCDE] LINUX-093: retain enough normal-log evidence to distinguish output
// placement from camera/composite decisions. Bounded and render-thread-only;
// ordinary 16:9 users do not take this diagnostic path.
static void aspect_output_note(const OutScale *o) {
    static unsigned notes;
    static int mode = -1;
    static uint32_t width, height;
    static double shape, sx, sy;
    if (!xcde_aspect_enabled() || notes >= 32) return;
    if (mode == g_settings.aspect && width == g_vk.sc_extent.width && height == g_vk.sc_extent.height &&
        fabs(shape - o->shape) < 0.00001 && fabs(sx - o->sx) < 0.00001 && fabs(sy - o->sy) < 0.00001) return;
    mode = g_settings.aspect; width = g_vk.sc_extent.width; height = g_vk.sc_extent.height;
    shape = o->shape; sx = o->sx; sy = o->sy;
    LOG("[aspect] mode %d drawable %ux%u factor %.6f scale %.6f %.6f viewport %.1f %.1f %.1f %.1f\n",
        mode, width, height, shape, sx, sy, o->vx, o->vy, o->vw, o->vh);
    if (++notes == 32) LOG("[aspect] further output transition diagnostics suppressed\n");
}

