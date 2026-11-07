// No overlay menu yet (M10 brings the ImGui menu back); the renderer calls these every frame.  [XCDE]
#include "../ui/menu.h"
bool menu_handle_event(const SDL_Event *e) { (void)e; return false; }
bool menu_is_open(void) { return false; }
void menu_apply_window_requests(void *w) { (void)w; }
void menu_render(void *cmd, uint32_t w, uint32_t h) { (void)cmd; (void)w; (void)h; }
void menu_test_open(void) {}
