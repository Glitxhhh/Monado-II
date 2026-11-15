// In-game overlay menu (Dear ImGui). Open with F1 / Esc, or Minus+Plus (Back+Start) on a controller.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef union SDL_Event SDL_Event;

// Main (UI) thread: returns true if the event was consumed by the menu (the game must not see it).
bool menu_handle_event(const SDL_Event *e);
bool menu_is_open(void);
// [XCDE] LINUX-097: all calls belong to the render thread. Real game frames
// feed frame once. The presenter reports only successfully accepted game output
// images, excluding clear/startup UI; repeats are never labeled generated.
// generation means the selected FG mode, including warmup/fallback frames.
// External driver generation is unobservable here and is labeled explicitly.
void fps_overlay_frame(uint64_t now_ns);
// LINUX-101: render-thread diagnostic tick in the SDL nanosecond clock.
// Method: 0 ordinary, 1 integrated FSR selected, 2 NVIDIA driver selected.
// Measurement continues while the on-screen display is hidden.
void fps_overlay_diagnostics(uint64_t now_ns, unsigned method, bool fsr_inputs_ready);
void fps_overlay_generation(bool active, bool external);
void fps_overlay_present(uint64_t now_ns, bool generated, bool repeated);
void fps_overlay_reset(void);
bool fps_overlay_visible(void);
void fps_overlay_render(void *cmd, uint32_t target_w, uint32_t target_h);
// Main thread: apply window requests (fullscreen) made from the menu.
void menu_apply_window_requests(void *sdl_window);

#if defined(__APPLE__) && defined(__OBJC__)
// Render thread: draw the overlay into the drawable after the game image (no-op while closed).
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target);
#elif !defined(__APPLE__)
// Render thread, inside the window image's rendering (Vulkan command buffer cmd).
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h);
#endif

#ifdef __cplusplus
}
#endif
