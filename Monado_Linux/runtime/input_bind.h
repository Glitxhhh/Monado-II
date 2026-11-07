// Control remapping (runtime/hle/bindings.c): which controller button / key drives each Switch input. Edited from
// the in-game menu's Controls tab and the start screen (runtime/ui/controls_ui.mm), saved in larecomp.ini.
// Menus (the overlay, the start screen) never use these: they keep the fixed default layout, so no binding can lock
// the player out.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union SDL_Event SDL_Event;

// Switch inputs, in nn::hid button-bit order (runtime/hle/input.c BTN_*), then the keyboard's stick directions.
enum {
    BIND_A, BIND_B, BIND_X, BIND_Y, BIND_LSTICK, BIND_RSTICK, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS, BIND_MINUS,
    BIND_LEFT, BIND_UP, BIND_RIGHT, BIND_DOWN,
    BIND_PAD_COUNT,                                      // controller: the 16 buttons (its sticks stay the sticks)
    BIND_LS_UP = BIND_PAD_COUNT, BIND_LS_DOWN, BIND_LS_LEFT, BIND_LS_RIGHT,
    BIND_RS_UP, BIND_RS_DOWN, BIND_RS_LEFT, BIND_RS_RIGHT,
    // [XCDE] keyboard / mouse actions: the arts palette's slots (runtime/hle/party.c art macro: 1-4 the left arts, 5-8
    // the right ones, talent the middle), the party switch (the member above / below)
    BIND_ART1, BIND_ART2, BIND_ART3, BIND_ART4, BIND_ART5, BIND_ART6, BIND_ART7, BIND_ART8, BIND_TALENT,
    BIND_PARTY_UP, BIND_PARTY_DOWN,
    BIND_KEY_COUNT                                       // keyboard: buttons + both sticks + actions
};
enum { BIND_DEV_PAD, BIND_DEV_KEY, BIND_DEV_KEY2 };  // (KEY2: each keyboard input's second key)
// [XCDE] Mouse buttons and the wheel bind like keys: values from BIND_MOUSE_BASE (above every SDL_Scancode)
enum {
    BIND_MOUSE_BASE = 0x400,
    BIND_MOUSE_LEFT = BIND_MOUSE_BASE + 1, BIND_MOUSE_MIDDLE, BIND_MOUSE_RIGHT, BIND_MOUSE_X1, BIND_MOUSE_X2,  // (SDL_BUTTON_*)
    BIND_WHEEL_UP, BIND_WHEEL_DOWN,
    BIND_MOUSE_END
};
// Mouse camera: the mouse moves the camera (the right stick) with the right button held (xcde.ini mouse_look = 1) or
// always (2: the cursor captured); 0 off. mouse_sens = 1..100, mouse_invert = 0 / 1.
extern int g_mouse_look, g_mouse_sens;
extern bool g_mouse_invert;
// Controller binding values: an SDL_GamepadButton, or the analog triggers as buttons
enum { PADB_LTRIGGER = 26 /* SDL_GAMEPAD_BUTTON_COUNT */, PADB_RTRIGGER, PADB_HOST_COUNT };

extern int16_t g_bind_pad[BIND_PAD_COUNT];  // PADB value per Switch button
extern int16_t g_bind_key[BIND_KEY_COUNT];  // SDL_Scancode (or BIND_MOUSE_*) per input (0 = none)
extern int16_t g_bind_key2[BIND_KEY_COUNT]; // its second key

const char *bind_input_name(int slot);         // "A", "D-pad Up", "Left stick Up"
const char *bind_pad_label(int host, int fam); // controller button in the pad family's own words (fam: input_pad_family)
const char *bind_key_label(int scancode);
int bind_label_family(void);                   // the connected controller's family (also on the start screen)
void bind_reset(int dev);
bool bind_is_default(int dev);
bool bind_is_default_slot(int dev, int slot);

// "Click a binding, then press": begun from the UI; the next press (any thread's SDL event) is taken. Escape cancels,
// as does BIND_CAPTURE_MS without a press. On a conflict the two inputs swap.
#define BIND_CAPTURE_MS 5000
void bind_capture_begin(int dev, int slot);
void bind_capture_cancel(void);
// capturing: dev / slot (else -1), ms left
bool bind_capture_active(int *dev, int *slot, int *ms_left);
// Main thread, before anything else sees the event: true = consumed by a capture
bool bind_capture_event(const SDL_Event *e);
// Last change for the UI: 0 none, 1 set, 2 swapped (with *other), 3 cancelled, 4 timed out, 5 reset; *serial bumps
int bind_last_result(int *dev, int *slot, int *other, int *serial);

// larecomp.ini: bind_pad_<input> / bind_key_<input> (only the ones that differ from the defaults are written)
bool bind_settings_load(const char *key, const char *val);
void bind_settings_save(FILE *f);

// bind_* names (larecomp.ini): Switch input "a", "zl", "ls_up"... -> slot; controller button "south"... -> PADB value
int bind_slot_from_name(const char *name);
int bind_pad_from_name(const char *name);

// Button prompts (runtime/nvn/texpatch.c): the controller button now driving Switch input `slot` (BIND_A..)
int bind_prompt_host(int slot);

#ifdef __cplusplus
}
#endif
