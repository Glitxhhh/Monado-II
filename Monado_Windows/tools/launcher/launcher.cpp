// Monado launcher: edits xcde.ini (graphics, scaling, controls, audio, language) and starts the game.
// Standalone (SDL3 + Dear ImGui with the SDL renderer); the game reads the same file when its window opens
// (runtime/host/settings_ini.c). Build: tools/launcher/build.sh. Run: "Play Monado.bat" at the repository root.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <sys/stat.h>
#include <algorithm>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#ifdef _WIN32
#include <windows.h>
#endif

// ------------------------------------------------------------------ settings (same keys as settings_ini.c)
// the Switch inputs, then the keyboard / mouse actions (runtime/input_bind.h BIND_ART1..: the arts palette, the party)
enum { NACT = 35, NPAD = 16, NSTICK = 24 };
static const char *const k_ini[NACT] = {"a", "b", "x", "y", "lstick", "rstick", "l", "r", "zl", "zr", "plus", "minus",
                                        "left", "up", "right", "down", "ls_up", "ls_down", "ls_left", "ls_right",
                                        "rs_up", "rs_down", "rs_left", "rs_right",
                                        "art1", "art2", "art3", "art4", "art5", "art6", "art7", "art8", "talent",
                                        "party_up", "party_down"};
static const char *const k_names[NACT] = {"A (acción, usar arte, aceptar)", "B (saltar, atrás)", "X (menú principal)", "Y (mapa)",
                                          "Pulsar stick izq.", "Pulsar stick der.", "L (objetivo anterior)", "R (objetivo siguiente)",
                                          "ZL (cámara)", "ZR (comandos de combate)", "+ (menú del sistema)", "- (Minus)",
                                          "Cruceta izquierda (arte)", "Cruceta arriba", "Cruceta derecha (arte)", "Cruceta abajo",
                                          "Avanzar", "Retroceder", "Izquierda", "Derecha",
                                          "Cámara arriba", "Cámara abajo", "Cámara izquierda", "Cámara derecha",
                                          "Arte 1 (izquierda)", "Arte 2", "Arte 3", "Arte 4 (junto al centro)",
                                          "Arte 5 (junto al centro)", "Arte 6", "Arte 7", "Arte 8 (derecha)",
                                          "Arte de talento (centro)", "Personaje de arriba", "Personaje de abajo"};
// mouse buttons / wheel bind like keys (runtime/input_bind.h BIND_MOUSE_*; their xcde.ini names)
enum { MOUSE_BASE = 0x400, MOUSE_END = MOUSE_BASE + 8, M_LEFT = MOUSE_BASE + 1, M_MIDDLE, M_RIGHT, M_X1, M_X2, M_WUP, M_WDOWN };
static const char *const k_mouse_ini[8] = {"", "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2", "Wheel Up", "Wheel Down"};
static const char *const k_mouse_ui[8] = {"", "Ratón izq.", "Ratón central", "Ratón der.", "Ratón lateral 1", "Ratón lateral 2",
                                          "Rueda arriba", "Rueda abajo"};
// controller buttons by position (runtime/hle/bindings.c k_pad_ini; 26/27 = the analog triggers)
enum { PADB_LT = 26, PADB_RT, PADB_COUNT };
static const char *const k_pad_ini[PADB_COUNT] = {
    "south", "east", "west", "north", "back", "guide", "start", "lstick", "rstick", "lshoulder", "rshoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "misc1", "rpaddle1", "lpaddle1", "rpaddle2", "lpaddle2",
    "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6", "ltrigger", "rtrigger"};
static const char *const k_pad_label[PADB_COUNT] = {
    "Abajo (Xbox A / PS Cruz / Switch B)", "Derecha (Xbox B / PS Círculo / Switch A)", "Izquierda (Xbox X / PS Cuadrado / Switch Y)",
    "Arriba (Xbox Y / PS Triángulo / Switch X)", "Back / View / Share", "Guide / Home", "Start / Menu / Options",
    "Pulsar stick izq.", "Pulsar stick der.", "LB / L1 / L", "RB / R1 / R", "Cruceta arriba", "Cruceta abajo",
    "Cruceta izquierda", "Cruceta derecha", "Misc 1", "Paleta der. 1", "Paleta izq. 1", "Paleta der. 2", "Paleta izq. 2",
    "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "LT / L2 / ZL", "RT / R2 / ZR"};
static const int k_def_pad[NPAD] = {1, 0, 3, 2, 7, 8, 9, 10, PADB_LT, PADB_RT, 6, 4, 13, 11, 14, 12};
// (runtime/hle/bindings.c DEF_KEY / DEF_KEY2: keyboard + mouse as an MMO)
static const int k_def_key[NACT] = {
    SDL_SCANCODE_F, SDL_SCANCODE_SPACE, SDL_SCANCODE_I, SDL_SCANCODE_M, SDL_SCANCODE_LSHIFT, M_MIDDLE,
    SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_J,
    SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN,
    SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_5,
    SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6,
    SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7,
    SDL_SCANCODE_8, SDL_SCANCODE_T, SDL_SCANCODE_Z, SDL_SCANCODE_X};
static const int k_def_key2[NACT] = {
    SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE, 0, 0, 0, SDL_SCANCODE_KP_0, 0, SDL_SCANCODE_TAB, 0, 0, 0, 0,
    0, SDL_SCANCODE_R, 0, SDL_SCANCODE_G};
static const char *T(const char *es);  // (launcher language, below)
static const char *key_ini_name(int k) { return k > MOUSE_BASE && k < MOUSE_END ? k_mouse_ini[k - MOUSE_BASE] : k ? SDL_GetScancodeName((SDL_Scancode)k) : "none"; }
static const char *key_ui_name(int k) { return k > MOUSE_BASE && k < MOUSE_END ? T(k_mouse_ui[k - MOUSE_BASE]) : k ? SDL_GetScancodeName((SDL_Scancode)k) : T("(ninguna)"); }
static int key_from_ini(const char *v) {
    for (int i = 1; i < 8; i++) if (!strcmp(v, k_mouse_ini[i])) return MOUSE_BASE + i;
    return strcmp(v, "none") ? SDL_GetScancodeFromName(v) : SDL_SCANCODE_UNKNOWN;
}

struct Cfg {
    bool fullscreen = false;
    int win = 0;            // index into k_wins
    int res = 0;            // index into k_res
    int upscaler = 1;       // 0 bilinear, 1 FSR 1, 2 FSR 3.1, 3 DLSS
    int uquality = 1;       // FSR 3 / DLSS: 0 native AA, 1 quality, 2 balanced, 3 performance, 4 ultra performance
    bool frame_gen = false;
    bool smaa = false;
    bool dof = true;        // the game's depth of field (T183)
    bool game_blur = true;  // the game's own AA / temporal AA / motion blur (lib_nx.ini)
    bool vsync = true;      // FIFO presentation at 30 / 60 fps
    int cutscenes = 0;      // T224: 0 at the FPS setting, 1 at 30 (xcde.ini cutscenes = same | 30)
    bool cut_fg = false;    // T224: cutscenes at 30 + frame generation, gameplay without it (xcde.ini cutscenes = 30fg)
    bool party_switch = false;  // party mod: switch the controlled character (F2 / L2 + L1 / R1), runtime/hle/party.c
    bool show_fps = false;
    int fps = 0;  // index into k_fps_ini
    int volume = 100;
    bool movie_audio = true;
    int lang = 0;           // index into k_langs
    int pad[NPAD];
    int key[NACT], key2[NACT];  // SDL_Scancode or mouse (MOUSE_BASE + ...)
    int mouse_look = 1;         // the mouse moves the camera: 0 no, 1 with the right button held, 2 always
    int mouse_sens = 50;
    int stick_deadzone = 0;     // % of the stick's travel read as centred (0: raw)
    bool mouse_invert = false;
    int ui_lang = 1;            // the launcher's own language: 0 Spanish, 1 English (xcde.ini launcher_lang)
    int icons = 0;              // button prompts: 0 auto (the connected controller, else the keyboard), 1 Switch, 2 Xbox, 3 PlayStation, 4 keyboard
};
// [XCDE] button prompt packs: romfs mods the player installs (GameBanana, CompC: "Xbox control layout" 694794,
// "PS5 DualSense control layout" 541180 - CC BY-NC-ND, so not shipped) in these folders; the launcher turns the one
// for the chosen / connected controller on (xcde.ini mod_<folder>) and keeps them out of the Mods tab.
// (keyboard: drawn by the game from the key bindings, runtime/services/kbicons.c - xcde.ini mod_icons_keyboard)
static const char *const k_icon_ini[] = {"auto", "switch", "xbox", "playstation", "keyboard"};
static const char *const k_icon_mod[] = {"icons_xbox", "icons_playstation"};
static bool is_icon_mod(const std::string &n) { return n == k_icon_mod[0] || n == k_icon_mod[1] || n == "icons_keyboard"; }
// 2 Xbox, 3 PlayStation, 4 keyboard (auto without a controller), else 1 (Switch)
static int icons_effective(int sel) {
    if (sel) return sel;
    int n = 0, t = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) t = SDL_GetGamepadTypeForID(ids[0]);
    SDL_free(ids);
    if (n <= 0) return 4;
    return t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE ? 2 :
           t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5 ? 3 : 1;
}
static const char *const k_ups_ini[] = {"bilinear", "fsr", "fsr3", "dlss"};
static const char *const k_uq_ini[] = {"native", "quality", "balanced", "performance", "ultra"};
static const char *const k_wins[] = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"};
static const char *const k_res_ini[] = {"match", "1", "2", "3", "75%", "50%", "dynamic"};
static const char *const k_res_ui[] = {"Igual que la ventana (recomendado)", "1x - 720p (como Switch)", "2x - 1440p",
                                       "3x - 2160p (4K)", "75% de la ventana", "50% de la ventana", "Dinámica (según rendimiento)"};
static const char *const k_fps_ini[] = {"30", "60", "120", "144", "165", "unlimited"};
static const char *const k_lang_ini[] = {"auto", "en-US", "es", "fr", "de", "it", "ja"};
static const char *const k_lang_ui[] = {"Inglés (por defecto)", "Inglés", "Español", "Francés", "Alemán", "Italiano", "Japonés"};

static Cfg g;

// [XCDE] launcher language (xcde.ini launcher_lang = es | en): the Spanish text is the key, T() gives the English one.
static const char *const k_en[][2] = {
    {"A (acción, usar arte, aceptar)", "A (action, use art, confirm)"}, {"B (saltar, atrás)", "B (jump, back)"},
    {"X (menú principal)", "X (main menu)"}, {"Y (mapa)", "Y (map)"}, {"Pulsar stick izq.", "Left stick press"},
    {"Pulsar stick der.", "Right stick press"}, {"L (objetivo anterior)", "L (previous target)"},
    {"R (objetivo siguiente)", "R (next target)"}, {"ZL (cámara)", "ZL (camera)"}, {"ZR (comandos de combate)", "ZR (battle commands)"},
    {"+ (menú del sistema)", "+ (system menu)"}, {"Cruceta izquierda (arte)", "D-pad left (art)"}, {"Cruceta arriba", "D-pad up"},
    {"Cruceta derecha (arte)", "D-pad right (art)"}, {"Cruceta abajo", "D-pad down"}, {"Avanzar", "Forward"},
    {"Retroceder", "Back"}, {"Izquierda", "Left"}, {"Derecha", "Right"}, {"Cámara arriba", "Camera up"},
    {"Cámara abajo", "Camera down"}, {"Cámara izquierda", "Camera left"}, {"Cámara derecha", "Camera right"},
    {"Arte 1 (izquierda)", "Art 1 (left)"}, {"Arte 2", "Art 2"}, {"Arte 3", "Art 3"}, {"Arte 4 (junto al centro)", "Art 4 (next to centre)"},
    {"Arte 5 (junto al centro)", "Art 5 (next to centre)"}, {"Arte 6", "Art 6"}, {"Arte 7", "Art 7"}, {"Arte 8 (derecha)", "Art 8 (right)"},
    {"Arte de talento (centro)", "Talent art (centre)"}, {"Personaje de arriba", "Character above"}, {"Personaje de abajo", "Character below"},
    {"Ratón izq.", "Mouse left"}, {"Ratón central", "Mouse middle"}, {"Ratón der.", "Mouse right"}, {"Ratón lateral 1", "Mouse side 1"},
    {"Ratón lateral 2", "Mouse side 2"}, {"Rueda arriba", "Wheel up"}, {"Rueda abajo", "Wheel down"}, {"(ninguna)", "(none)"},
    {"Abajo (Xbox A / PS Cruz / Switch B)", "Bottom (Xbox A / PS Cross / Switch B)"},
    {"Derecha (Xbox B / PS Círculo / Switch A)", "Right (Xbox B / PS Circle / Switch A)"},
    {"Izquierda (Xbox X / PS Cuadrado / Switch Y)", "Left (Xbox X / PS Square / Switch Y)"},
    {"Arriba (Xbox Y / PS Triángulo / Switch X)", "Top (Xbox Y / PS Triangle / Switch X)"},
    {"Cruceta izquierda", "D-pad left"}, {"Cruceta derecha", "D-pad right"}, {"Paleta der. 1", "Right paddle 1"},
    {"Paleta izq. 1", "Left paddle 1"}, {"Paleta der. 2", "Right paddle 2"}, {"Paleta izq. 2", "Left paddle 2"},
    {"Igual que la ventana (recomendado)", "Match the window (recommended)"}, {"1x - 720p (como Switch)", "1x - 720p (like Switch)"},
    {"75% de la ventana", "75% of the window"}, {"50% de la ventana", "50% of the window"},
    {"Dinámica (según rendimiento)", "Dynamic (by performance)"},
    {"Inglés (por defecto)", "English (default)"}, {"Inglés", "English"}, {"Español", "Spanish"}, {"Francés", "French"},
    {"Alemán", "German"}, {"Italiano", "Italian"}, {"Japonés", "Japanese"},
    {"Pantalla", "Display"}, {"Pantalla completa", "Fullscreen"}, {"Tamaño de la ventana", "Window size"},
    {"Resolución y escalado", "Resolution and scaling"}, {"Resolución interna", "Internal resolution"},
    {"El juego original renderiza a 720p. Más resolución = imagen más nítida y más carga para la GPU.",
     "The original game renders at 720p. More resolution = a sharper image and more GPU load."},
    {"Bilineal (suave)", "Bilinear (soft)"}, {"AMD FSR 1.0 (nítido)", "AMD FSR 1.0 (sharp)"},
    {"AMD FSR 3.1 (temporal, cualquier GPU)", "AMD FSR 3.1 (temporal, any GPU)"},
    {"NVIDIA DLSS (temporal, GPU RTX)", "NVIDIA DLSS (temporal, RTX GPU)"}, {"Escalado / antialiasing", "Upscaling / anti-aliasing"},
    {"FSR 1.0: escalado espacial, solo cuando la resolución interna es menor que la ventana.\n"
     "FSR 3.1 y DLSS: reconstrucción temporal - renderizan a menos resolución (según la calidad)\n"
     "y reconstruyen la imagen de la ventana con varios frames: más FPS y bordes sin dientes.\n"
     "Sustituyen al antialiasing temporal del juego. DLSS necesita una GPU NVIDIA RTX; si no\n"
     "está disponible se usa FSR 3.1.",
     "FSR 1.0: spatial upscaling, only when the internal resolution is below the window.\n"
     "FSR 3.1 and DLSS: temporal reconstruction - they render at a lower resolution (by quality)\n"
     "and rebuild the window's image from several frames: more FPS and smooth edges.\n"
     "They replace the game's temporal anti-aliasing. DLSS needs an NVIDIA RTX GPU; without\n"
     "one, FSR 3.1 is used."},
    {"Nativo (solo antialiasing: DLAA / FSR Native AA)", "Native (anti-aliasing only: DLAA / FSR Native AA)"},
    {"Calidad (67 %)", "Quality (67 %)"}, {"Equilibrado (59 %)", "Balanced (59 %)"}, {"Rendimiento (50 %)", "Performance (50 %)"},
    {"Ultra rendimiento (33 %)", "Ultra performance (33 %)"}, {"Calidad del escalado", "Upscaling quality"},
    {"Con 'Resolución interna' en 'Igual que la ventana': la resolución a la que se renderiza\n"
     "antes de reconstruir (porcentaje del lado de la ventana).",
     "With 'Internal resolution' on 'Match the window': the resolution rendered\n"
     "before reconstruction (percentage of the window's side)."},
    {"Generación de frames (FSR 3)", "Frame generation (FSR 3)"},
    {"Inserta un frame interpolado entre cada dos del juego en todo el juego: el doble de imágenes\n"
     "por segundo (p. ej. FPS 30 -> 60 en pantalla). La de AMD: funciona con FSR 3.1 y con DLSS.\n"
     "Añade algo de latencia. Se aplica al iniciar el juego.",
     "Inserts an interpolated frame between every two of the game's, everywhere: twice the images\n"
     "per second (e.g. 30 FPS -> 60 on screen). AMD's: works with FSR 3.1 and with DLSS.\n"
     "Adds some latency. Applied when the game starts."},
    {"Generación de frames solo en cinemáticas", "Frame generation in cutscenes only"},
    {"El juego va a tus FPS sin generación de frames; en las cinemáticas (donde el juego anima a\n"
     "los personajes a 30) baja a 30 parejos y la generación las lleva a 60 en pantalla.\n"
     "Pensado para jugar a 60 en un monitor de 60 Hz. Se aplica al iniciar el juego.",
     "The game runs at your FPS without frame generation; in cutscenes (where the game animates\n"
     "characters at 30) it drops to an even 30 and generation brings them to 60 on screen.\n"
     "Meant for playing at 60 on a 60 Hz monitor. Applied when the game starts."},
    {"Se aplica cuando la resolución interna es menor que la ventana.", "Applies when the internal resolution is below the window."},
    {"Calidad", "Quality"}, {"Antialiasing SMAA", "SMAA anti-aliasing"},
    {"Profundidad de campo (desenfoque del juego)", "Depth of field (the game's blur)"},
    {"El desenfoque de lo cercano y lo lejano que el juego aplica también en Switch.\n"
     "Desactivado: toda la escena nítida (y algo menos de trabajo para la GPU).",
     "The near / far blur the game also applies on Switch.\n"
     "Off: the whole scene sharp (and a bit less GPU work)."},
    {"Antialiasing y desenfoque de movimiento del juego", "The game's anti-aliasing and motion blur"},
    {"El antialiasing temporal (TMAA), el desenfoque de movimiento y el del pelo que el juego\n"
     "aplica para los 720p de la Switch: a 1080p o más suavizan toda la imagen.\n"
     "Desactivado: imagen nítida (combínalo con SMAA para los bordes).",
     "The temporal anti-aliasing (TMAA), motion blur and hair blur the game applies for the\n"
     "Switch's 720p: at 1080p or more they soften the whole image.\n"
     "Off: a sharp image (combine it with SMAA for the edges)."},
    {"Mostrar FPS en la barra de título", "Show FPS in the title bar"}, {"Velocidad", "Speed"},
    {"30 (como en Switch)", "30 (like Switch)"}, {"Sin límite", "Unlimited"},
    {"Más de 30: la velocidad del mundo, el reloj y las escenas no cambian (probado a 60 y 120);\n"
     "la cámara queda un poco más lejos. El V-Sync forzado en el panel de NVIDIA / AMD o el límite\n"
     "de RivaTuner también limitan los FPS: déjalos en 'configuración de la aplicación' / 0.",
     "Above 30: world speed, the clock and scenes don't change (tested at 60 and 120);\n"
     "the camera sits a bit farther. V-Sync forced in the NVIDIA / AMD panel or a RivaTuner\n"
     "limit also cap the FPS: leave them on 'application setting' / 0."},
    {"V-Sync (con 30 / 60 FPS)", "V-Sync (at 30 / 60 FPS)"},
    {"Cada imagen se muestra exactamente una vez, sincronizada con la pantalla: movimiento\n"
     "fluido (sin imágenes repetidas o saltadas). Con más de 60 FPS o sin límite no se aplica.",
     "Each image is shown exactly once, in sync with the display: smooth motion\n"
     "(no repeated or skipped images). Not applied above 60 FPS or unlimited."},
    {"Como el resto del juego", "Like the rest of the game"}, {"30 estables (como en Switch)", "Steady 30 (like Switch)"},
    {"Cinemáticas", "Cutscenes"},
    {"En las cinemáticas el juego anima a los personajes a 30 aunque juegues a más FPS: con la\n"
     "cámara a 60 encima se notan saltos (p. ej. el pelo de Dickson).\n"
     "30 estables: las cinemáticas van a 30 parejos, cámara y personajes juntos (con V-Sync en un\n"
     "monitor de 60 Hz cada imagen dura exactamente dos refrescos).\n"
     "Con 'Generación de frames solo en cinemáticas' también van a 30, con generación encima.",
     "In cutscenes the game animates characters at 30 even at higher FPS: with the camera\n"
     "at 60 on top, jumps show (e.g. Dickson's hair).\n"
     "Steady 30: cutscenes run at an even 30, camera and characters together (with V-Sync on a\n"
     "60 Hz monitor each image lasts exactly two refreshes).\n"
     "With 'Frame generation in cutscenes only' they also run at 30, with generation on top."},
    {"Pulsa %s para \"%s\"... (%d s, Supr = ninguna)", "Press %s for \"%s\"... (%d s, Del = none)"},
    {"un botón del mando", "a controller button"}, {"una tecla o un botón del ratón", "a key or a mouse button"},
    {"Haz clic en una asignación y pulsa la tecla, botón del ratón o del mando nuevo. Si ya estaba en uso, se intercambian.",
     "Click a binding and press the new key, mouse button or controller button. If it was in use, they swap."},
    {"Restaurar teclado", "Reset keyboard"}, {"Restaurar mando", "Reset controller"},
    {"   Mando conectado: %s", "   Controller connected: %s"}, {"   (ningún mando detectado)", "   (no controller detected)"},
    {"Cambiar de personaje como en Xenoblade 3 (mantén L2 y pulsa L1 / R1: el de arriba / abajo; F2)",
     "Switch characters like in Xenoblade 3 (hold L2 and press L1 / R1: the one above / below; F2)"},
    {"Pasa el control al siguiente miembro del grupo, en el campo y en combate. Cada uno se queda\n"
     "donde está y conserva sus recargas de artes; el anterior pasa a la IA. No funciona en menús ni\n"
     "escenas, y en combate salta a los miembros caídos.",
     "Hands control to the next party member, in the field and in battle. Each stays where it is\n"
     "and keeps its art cooldowns; the previous one goes to the AI. Not in menus or scenes,\n"
     "and in battle it skips downed members."},
    {"Automático (el mando conectado; sin mando, teclado)", "Automatic (the connected controller; without one, keyboard)"},
    {"Teclado (las teclas asignadas)", "Keyboard (the bound keys)"}, {"Iconos de botones", "Button icons"},
    {"Falta el pack: descarga \"%s\" de GameBanana y pon su carpeta romfs en mods\\%s",
     "Pack missing: download \"%s\" from GameBanana and put its romfs folder in mods\\%s"},
    {"Xbox / PlayStation: packs de iconos de CompC (GameBanana). Teclado: el juego dibuja en cada\n"
     "icono la tecla asignada a ese botón (también ratón). Se aplica al iniciar el juego.",
     "Xbox / PlayStation: CompC's icon packs (GameBanana). Keyboard: the game draws on each icon\n"
     "the key bound to that button (mouse too). Applied when the game starts."},
    {"Manteniendo el botón derecho", "Holding the right button"}, {"Siempre", "Always"},
    {"Cámara con el ratón", "Mouse camera"},
    {"Siempre: el ratón mueve la cámara directamente y el cursor queda oculto dentro del juego.\n"
     "F1 (menú) o Alt+Tab lo liberan; al mover el ratón sobre el juego vuelve a capturarse.",
     "Always: the mouse moves the camera directly and the cursor is hidden in the game.\n"
     "F1 (menu) or Alt+Tab release it; moving the mouse over the game captures it again."},
    {"Sensibilidad", "Sensitivity"}, {"Invertir eje Y", "Invert Y axis"}, {"Zona muerta de los sticks (%)", "Stick dead zone (%)"},
    {"Para un mando con drift (el personaje o la cámara se mueven solos): esa parte del recorrido\n"
     "del stick cuenta como centrada. 0 = sin cambios (el juego ya tiene la suya).",
     "For a controller with drift (the character or camera move by themselves): that part of the\n"
     "stick's travel counts as centred. 0 = unchanged (the game has its own)."},
    {"Artes 1-8 y talento: en combate seleccionan el arte de ese hueco de la paleta y lo usan.",
     "Arts 1-8 and talent: in battle they select the art in that palette slot and use it."},
    {"Acción (botón de Switch)", "Action (Switch button)"}, {"Tecla", "Key"}, {"Tecla 2", "Key 2"}, {"Mando", "Controller"},
    {"(stick del mando)", "(controller stick)"}, {"(cruceta + A)", "(d-pad + A)"},
    {"Pon cada mod en su carpeta: mods\\<nombre>\\romfs\\... (la misma estructura que en Ryujinx / yuzu). "
     "Sus archivos reemplazan o añaden archivos del juego. Se aplican en orden alfabético: si dos mods "
     "cambian el mismo archivo, gana el último. Los mods de código (exefs, parches .pchtxt / IPS) no "
     "funcionan en esta versión recompilada.",
     "Put each mod in its own folder: mods\\<name>\\romfs\\... (the same layout as Ryujinx / yuzu). "
     "Its files replace or add game files. They apply in alphabetical order: if two mods change the "
     "same file, the last one wins. Code mods (exefs, .pchtxt / IPS patches) don't work in this "
     "recompiled version."},
    {"Abrir carpeta de mods", "Open mods folder"}, {"Actualizar lista", "Refresh list"}, {"No hay mods instalados.", "No mods installed."},
    {"Volumen", "Volume"}, {"Sonido de los vídeos", "Video sound"}, {"Idioma del juego", "Game language"}, {"Idioma", "Language"},
    {"Textos del juego. Solo los idiomas que incluya tu copia; si uno falla, vuelve a Inglés.",
     "The game's text. Only the languages your copy includes; if one fails, it goes back to English."},
    {"Recompilación nativa para PC · configuración", "Native PC recompilation · settings"},
    {"Gráficos", "Graphics"}, {"Controles", "Controls"}, {"Audio e idioma", "Audio and language"},
    {"Faltan los archivos del juego en targets/ (exefs y romfs de tu copia).", "The game files are missing in targets/ (exefs and romfs of your copy)."},
    {"Falta build/xcde.exe (compila el juego primero).", "build/xcde.exe is missing (build the game first)."},
    {"Listo. Guardado: save/   Ajustes: xcde.ini", "Ready. Saves: save/   Settings: xcde.ini"},
    {"Listo. La primera vez, JUGAR prepara los shaders antes (unos minutos)", "Ready. The first time, PLAY prepares the shaders first (a few minutes)"},
    {"Omitir", "Skip"}, {"Detener", "Stop"},
    {"Precompilación detenida (se puede seguir otro día: lo ya hecho se conserva)", "Precompilation stopped (it can go on another day: what's done is kept)"},
    {"JUGAR", "PLAY"}, {"Precompilar shaders", "Precompile shaders"},
    {"Traduce y compila todos los shaders del juego por adelantado, para que la primera visita\n"
     "a una zona no se detenga a compilarlos. Solo hace falta una vez (o tras actualizar el juego).",
     "Translates and compiles all of the game's shaders ahead of time, so the first visit to an\n"
     "area doesn't stop to compile them. Only needed once (or after updating the game)."},
    {"Guardar ajustes", "Save settings"}, {"Ajustes guardados en xcde.ini", "Settings saved to xcde.ini"},
    {"No se pudo escribir xcde.ini", "Could not write xcde.ini"}, {"Crear reporte", "Create report"},
    {"Junta en un .zip los logs de las dos últimas partidas, xcde.ini y el último informe de\n"
     "cierre inesperado (sin partidas guardadas ni archivos del juego), para adjuntarlo a un reporte.",
     "Puts in a .zip the logs of the last two sessions, xcde.ini and the latest crash report\n"
     "(no saves or game files), to attach to a bug report."},
    {"Salir", "Quit"}, {"No se pudo iniciar build/xcde.exe (error ", "Could not start build/xcde.exe (error "},
    {"Juego iniciado. Registro: build/last_game.log", "Game started. Log: build/last_game.log"},
    {"No se pudo iniciar la precompilación (error ", "Could not start the precompilation (error "},
    {"Buscando shaders en los archivos del juego...", "Looking for shaders in the game files..."},
    {"Preparando shaders antes de la primera partida (solo esta vez)", "Preparing shaders before the first game (only this time)"},
    {"Precompilando shaders", "Precompiling shaders"}, {"Variantes conocidas (semilla)…", "Known variants (seed)…"},
    {"Archivos %llu de %llu · programas de shaders listos %llu de %llu", "Files %llu of %llu · shader programs ready %llu of %llu"},
    {"El controlador gráfico falló en un shader: se salta y se sigue (intento ", "The graphics driver failed on a shader: it is skipped (attempt "},
    {"Shaders precompilados", "Shaders precompiled"},
    {"La precompilación falló (registro: build/last_precompile.log)", "Precompilation failed (log: build/last_precompile.log)"},
    {"No hay nada que reportar todavía (ni logs ni xcde.ini)", "Nothing to report yet (no logs or xcde.ini)"},
    {"No se pudo crear el reporte (falta tar.exe de Windows)", "Could not create the report (Windows' tar.exe is missing)"},
    {"No se pudo crear el reporte", "Could not create the report"}, {"Reporte creado: ", "Report created: "},
    {" (adjúntalo al reportar el problema)", " (attach it to the bug report)"},
    {"Idioma del launcher", "Launcher language"},
};
static const char *T(const char *es) {
    if (g.ui_lang != 1) return es;
    for (const auto &e : k_en) if (!strcmp(e[0], es)) return e[1];
    return es;
}
// (combo lists: each element through T)
struct TList { const char *p[48]; };
static TList TL(const char *const *a, int n) { TList t{}; for (int i = 0; i < n && i < 48; i++) t.p[i] = T(a[i]); return t; }
static std::string g_root;  // repository root (where targets/, build/, xcde.ini live)

static std::string path(const char *rel) { return g_root + "/" + rel; }
struct Mod { std::string name; bool on = true; bool present = true; };
static std::vector<Mod> g_mods;  // (mods tab; xcde.ini mod_<name>)

static int find(const char *const *list, int n, const char *v) {
    for (int i = 0; i < n; i++) if (!strcmp(list[i], v)) return i;
    return -1;
}

static void load() {
    for (int i = 0; i < NPAD; i++) g.pad[i] = k_def_pad[i];
    for (int i = 0; i < NACT; i++) g.key[i] = k_def_key[i], g.key2[i] = k_def_key2[i];
    FILE *f = fopen(path("xcde.ini").c_str(), "r");
    if (!f) return;
    int layout = 1;  // (bind_layout: an older file's keyboard lines are the old layout - the new defaults replace them)
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        if (!memcmp(k, "\xEF\xBB\xBF", 3)) k += 3;  // (a UTF-8 BOM: the file was saved from Notepad)
        while (*k == ' ' || *k == '\t') k++;
        for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (char *e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'); ) *--e = 0;
        int i;
        if (!strcmp(k, "fullscreen")) g.fullscreen = atoi(v) != 0;
        else if (!strcmp(k, "window") && (i = find(k_wins, 5, v)) >= 0) g.win = i;
        else if (!strcmp(k, "resolution") && (i = find(k_res_ini, 7, v)) >= 0) g.res = i;
        else if (!strcmp(k, "upscaler")) g.upscaler = !strcmp(v, "fsr") ? 1 : !strcmp(v, "fsr3") ? 2 : !strcmp(v, "dlss") ? 3 : 0;
        else if (!strcmp(k, "upscale_quality") && (i = find(k_uq_ini, 5, v)) >= 0) g.uquality = i;
        else if (!strcmp(k, "frame_gen")) g.frame_gen = atoi(v) != 0;
        else if (!strcmp(k, "antialias")) g.smaa = !strcmp(v, "smaa");
        else if (!strcmp(k, "depth_of_field")) g.dof = atoi(v) != 0;
        else if (!strcmp(k, "game_blur")) g.game_blur = atoi(v) != 0;
        else if (!strcmp(k, "vsync")) g.vsync = atoi(v) != 0;
        else if (!strcmp(k, "cutscenes")) { g.cutscenes = !strcmp(v, "30") || !strcmp(v, "30fg") ? 1 : 0; g.cut_fg = !strcmp(v, "30fg"); }
        else if (!strcmp(k, "party_switch")) g.party_switch = atoi(v) != 0;
        else if (!strcmp(k, "show_fps")) g.show_fps = atoi(v) != 0;
        else if (!strcmp(k, "fps")) { g.fps = 0; for (int j = 0; j < 6; j++) if (!strcmp(v, k_fps_ini[j])) g.fps = j; }
        else if (!strcmp(k, "volume")) g.volume = atoi(v);
        else if (!strcmp(k, "movie_audio")) g.movie_audio = atoi(v) != 0;
        else if (!strcmp(k, "language") && (i = find(k_lang_ini, 7, v)) >= 0) g.lang = i;
        else if (!strncmp(k, "mod_", 4)) g_mods.push_back(Mod{k + 4, atoi(v) != 0, false});
        else if (!strncmp(k, "bind_pad_", 9) && (i = find(k_ini, NPAD, k + 9)) >= 0) {
            int h = find(k_pad_ini, PADB_COUNT, v);
            if (h >= 0) g.pad[i] = h;
        } else if (!strncmp(k, "bind_key_", 9) && (i = find(k_ini, NACT, k + 9)) >= 0) {
            g.key[i] = key_from_ini(v);
        } else if (!strncmp(k, "bind_key2_", 10) && (i = find(k_ini, NACT, k + 10)) >= 0) {
            g.key2[i] = key_from_ini(v);
        } else if (!strcmp(k, "bind_layout")) layout = atoi(v);
        else if (!strcmp(k, "mouse_look")) g.mouse_look = std::clamp(atoi(v), 0, 2);
        else if (!strcmp(k, "mouse_sens")) g.mouse_sens = std::clamp(atoi(v), 1, 100);
        else if (!strcmp(k, "stick_deadzone")) g.stick_deadzone = std::clamp(atoi(v), 0, 30);
        else if (!strcmp(k, "launcher_lang")) g.ui_lang = !strcmp(v, "es") ? 0 : 1;
        else if (!strcmp(k, "mouse_invert")) g.mouse_invert = atoi(v) != 0;
        else if (!strcmp(k, "button_icons") && (i = find(k_icon_ini, 5, v)) >= 0) g.icons = i;
    }
    fclose(f);
    if (layout < 2) for (int i = 0; i < NACT; i++) g.key[i] = k_def_key[i], g.key2[i] = k_def_key2[i];
}

static bool save() {
    FILE *f = fopen(path("xcde.ini").c_str(), "w");
    if (!f) return false;
    fprintf(f, "# Monado settings (written by the launcher)\n");
    fprintf(f, "fullscreen = %d\nwindow = %s\nresolution = %s\nupscaler = %s\nantialias = %s\nshow_fps = %d\n", g.fullscreen,
            k_wins[g.win], k_res_ini[g.res], k_ups_ini[g.upscaler], g.smaa ? "smaa" : "off", g.show_fps);
    fprintf(f, "upscale_quality = %s\nframe_gen = %d\n", k_uq_ini[g.uquality], g.frame_gen);
    fprintf(f, "depth_of_field = %d\n", g.dof);
    fprintf(f, "game_blur = %d\n", g.game_blur);
    fprintf(f, "vsync = %d\n", g.vsync);
    fprintf(f, "cutscenes = %s\n", g.cut_fg ? "30fg" : g.cutscenes ? "30" : "same");
    fprintf(f, "party_switch = %d\n", g.party_switch);
    fprintf(f, "volume = %d\nmovie_audio = %d\nlanguage = %s\nfps = %s\n", g.volume, g.movie_audio, k_lang_ini[g.lang], k_fps_ini[g.fps]);
    for (const Mod &m : g_mods) if (!is_icon_mod(m.name)) fprintf(f, "mod_%s = %d", m.name.c_str(), m.on), fputc(10, f);
    int ic = icons_effective(g.icons);
    fprintf(f, "button_icons = %s\nmod_%s = %d\nmod_%s = %d\nmod_icons_keyboard = %d\n", k_icon_ini[g.icons], k_icon_mod[0], ic == 2,
            k_icon_mod[1], ic == 3, ic == 4);
    for (int i = 0; i < NPAD; i++) fprintf(f, "bind_pad_%s = %s\n", k_ini[i], k_pad_ini[g.pad[i]]);
    fprintf(f, "bind_layout = 2\n");
    for (int i = 0; i < NACT; i++) {
        fprintf(f, "bind_key_%s = %s\n", k_ini[i], key_ini_name(g.key[i]));
        fprintf(f, "bind_key2_%s = %s\n", k_ini[i], key_ini_name(g.key2[i]));
    }
    fprintf(f, "mouse_look = %d\nmouse_sens = %d\nmouse_invert = %d\n", g.mouse_look, g.mouse_sens, g.mouse_invert);
    fprintf(f, "stick_deadzone = %d\n", g.stick_deadzone);
    fprintf(f, "launcher_lang = %s\n", g.ui_lang ? "en" : "es");
    fclose(f);
    return true;
}

static bool exists(const char *rel) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(path(rel).c_str(), &pi);
}

static std::string g_status;
static bool g_launched_ok;  // (launch() started the game: the launcher closes)
static float g_scale = 1;

static void launch() {
    if (!save()) { g_status = T("No se pudo escribir xcde.ini"); return; }
#ifdef _WIN32
    std::string exe = path("build/xcde.exe");
    std::string cmd = "\"" + exe + "\" targets";
    // the game's DLLs (SDL3, Vulkan loader, FFmpeg, opus) come from MSYS2
    const char *old = getenv("PATH");
    std::string p = std::string("C:\\msys64\\ucrt64\\bin;") + (old ? old : "");
    SetEnvironmentVariableA("PATH", p.c_str());
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    std::string log = path("build/last_game.log");
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE h = CreateFileA(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = h;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessA(exe.c_str(), buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, g_root.c_str(), &si, &pi);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) { g_status = std::string(T("No se pudo iniciar build/xcde.exe (error ")) + std::to_string(GetLastError()) + ")"; return; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_status = T("Juego iniciado. Registro: build/last_game.log");
    g_launched_ok = true;
#endif
}

// ------------------------------------------------------------------ shader precompilation (T187)
// "Precompilar shaders" runs `build/xcde.exe --precompile targets` (runtime/nvn/vulkan/precompile.c): it translates every
// shader in the game's files into cache/spirv, so the first visit to an area doesn't stop to compile. It reports
// progress in cache/precompile_progress.txt and writes cache/precompile_done.txt at the end. JUGAR runs it first
// when it was never done (once, a few minutes; "Omitir" skips it).
#ifdef _WIN32
static HANDLE g_pc_proc;
#endif
static bool g_pc_then_play, g_pc_play_now;
static float g_pc_frac;
static std::string g_pc_text;

// [XCDE] T211: done for THIS build - precompile_done.txt names the build/xcde.exe (size, time) it was made with; a new
// build translates shaders differently, so JUGAR precompiles again (unchanged shaders are cache hits: quick)
static bool pc_done() {
    FILE *f = fopen(path("cache/precompile_done.txt").c_str(), "r");
    if (!f) return false;
    char line[256];
    long long sz = -1, mt = -1;
    while (fgets(line, sizeof line, f)) sscanf(line, "exe %lld %lld", &sz, &mt);
    fclose(f);
    struct stat es;
    if (stat(path("build/xcde.exe").c_str(), &es) != 0) return true;
    return sz == (long long)es.st_size && mt == (long long)es.st_mtime;
}
static bool pc_running() {
#ifdef _WIN32
    return g_pc_proc != NULL;
#else
    return false;
#endif
}

static void pc_start(bool then_play) {
#ifdef _WIN32
    if (g_pc_proc) return;
    std::string exe = path("build/xcde.exe");
    std::string cmd = "\"" + exe + "\" --precompile targets";
    const char *old = getenv("PATH");
    std::string p = std::string("C:\\msys64\\ucrt64\\bin;") + (old ? old : "");
    SetEnvironmentVariableA("PATH", p.c_str());
    CreateDirectoryA(path("cache").c_str(), NULL);
    DeleteFileA(path("cache/precompile_progress.txt").c_str());
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    std::string log = path("build/last_precompile.log");
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE h = CreateFileA(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = h;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessA(exe.c_str(), buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL,
                             g_root.c_str(), &si, &pi);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) { g_status = std::string(T("No se pudo iniciar la precompilación (error ")) + std::to_string(GetLastError()) + ")"; return; }
    CloseHandle(pi.hThread);
    g_pc_proc = pi.hProcess;
    g_pc_then_play = then_play;
    g_pc_frac = 0;
    g_pc_text = T("Buscando shaders en los archivos del juego...");
    g_status = then_play ? T("Preparando shaders antes de la primera partida (solo esta vez)") : T("Precompilando shaders");
#endif
}

static void pc_stop() {
#ifdef _WIN32
    if (!g_pc_proc) return;
    TerminateProcess(g_pc_proc, 1);
    WaitForSingleObject(g_pc_proc, 2000);
    CloseHandle(g_pc_proc);
    g_pc_proc = NULL;
#endif
}

// (every frame while it runs) progress from the file; at the end, the result and JUGAR if it was waiting
static void pc_poll() {
#ifdef _WIN32
    if (!g_pc_proc) return;
    static Uint64 last;
    Uint64 now = SDL_GetTicks();
    if (now - last < 200) return;
    last = now;
    FILE *f = fopen(path("cache/precompile_progress.txt").c_str(), "r");
    if (f) {
        char phase[16] = "";
        unsigned long long fd = 0, ft = 0, found = 0, done = 0;
        if (fscanf(f, "%15s %llu %llu %llu %llu", phase, &fd, &ft, &found, &done) == 5 && ft) {
            // reading the files is ~1/5 of the time, building the programs found the rest (it follows the reading)
            float fs = (float)fd / (float)ft, fc = found ? (float)done / (float)found : 0.0f;
            g_pc_frac = 0.15f * fs + 0.85f * fs * fc;
            char t[160];
            if (!strcmp(phase, "seed")) snprintf(t, sizeof t, "%s", T("Variantes conocidas (semilla)…"));  // [XCDE] T211: the seed pass
            else snprintf(t, sizeof t, T("Archivos %llu de %llu · programas de shaders listos %llu de %llu"), fd, ft, done, found);
            g_pc_text = t;
        }
        fclose(f);
    }
    if (WaitForSingleObject(g_pc_proc, 0) != WAIT_OBJECT_0) return;
    DWORD code = 1;
    GetExitCodeProcess(g_pc_proc, &code);
    CloseHandle(g_pc_proc);
    g_pc_proc = NULL;
    // [XCDE] T240: a run that died while the driver was building (cache/precompile_inflight.bin left behind) starts
    // again by itself - the new run skips what was being built (precompile.c crash-safe) - up to 3 times
    static int retries;
    DWORD att = GetFileAttributesA(path("cache/precompile_inflight.bin").c_str());
    if (!(code == 0 && pc_done()) && att != INVALID_FILE_ATTRIBUTES && retries < 3) {
        retries++;
        bool then_play = g_pc_then_play;
        pc_start(then_play);
        g_status = std::string(T("El controlador gráfico falló en un shader: se salta y se sigue (intento ")) + std::to_string(retries + 1) + ")";
        return;
    }
    retries = 0;
    g_status = code == 0 && pc_done() ? T("Shaders precompilados") : T("La precompilación falló (registro: build/last_precompile.log)");
    if (g_pc_then_play) g_pc_play_now = true;
#endif
}

// ------------------------------------------------------------------ remapping capture
static int cap_dev = -1, cap_slot = -1;  // dev 0 keyboard, 1 controller, 2 keyboard (second key)
static Uint64 cap_until, cap_since;

// a key / mouse input for the slot being captured: one already in use elsewhere (either column) takes the slot's old one
static void capture_key(int k) {
    int *t = cap_dev == 2 ? g.key2 : g.key, *o = cap_dev == 2 ? g.key : g.key2;
    for (int i = 0; i < NACT; i++) {
        if (t[i] == k && i != cap_slot) t[i] = t[cap_slot];
        if (o[i] == k) o[i] = t[cap_slot];
    }
    t[cap_slot] = k;
    cap_dev = -1;
}

static bool capture_event(const SDL_Event &e) {
    if (cap_dev < 0) return false;
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.scancode == SDL_SCANCODE_DELETE && cap_dev != 1) { capture_key(0); return true; }  // (Supr: none)
        if (e.key.scancode == SDL_SCANCODE_ESCAPE && SDL_GetTicks() - cap_since < 700) { cap_dev = -1; return true; }
        if (cap_dev != 1) capture_key(e.key.scancode);
        else if (e.key.scancode == SDL_SCANCODE_ESCAPE) cap_dev = -1;
        return true;
    }
    // mouse buttons / wheel (not the click that started the capture)
    if (cap_dev != 1 && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && SDL_GetTicks() - cap_since > 250) {
        capture_key(MOUSE_BASE + (e.button.button < 6 ? e.button.button : 5));
        return true;
    }
    if (cap_dev != 1 && e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.y) {
        capture_key(e.wheel.y > 0 ? M_WUP : M_WDOWN);
        return true;
    }
    if (cap_dev == 1 && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        int h = e.gbutton.button;
        for (int i = 0; i < NPAD; i++) if (g.pad[i] == h && i != cap_slot) g.pad[i] = g.pad[cap_slot];
        g.pad[cap_slot] = h;
        cap_dev = -1;
        return true;
    }
    if (cap_dev == 1 && e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.value > 20000 &&
        (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
        int h = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PADB_LT : PADB_RT;
        for (int i = 0; i < NPAD; i++) if (g.pad[i] == h && i != cap_slot) g.pad[i] = g.pad[cap_slot];
        g.pad[cap_slot] = h;
        cap_dev = -1;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ UI
static void tab_graphics() {
    ImGui::SeparatorText(T("Pantalla"));
    ImGui::Checkbox(T("Pantalla completa"), &g.fullscreen);
    ImGui::SetNextItemWidth(260 * g_scale);
    ImGui::Combo(T("Tamaño de la ventana"), &g.win, k_wins, 5);
    ImGui::SeparatorText(T("Resolución y escalado"));
    ImGui::SetNextItemWidth(360 * g_scale);
    ImGui::Combo(T("Resolución interna"), &g.res, TL(k_res_ui, 7).p, 7);
    ImGui::TextDisabled(T("El juego original renderiza a 720p. Más resolución = imagen más nítida y más carga para la GPU."));
    const char *ups[] = {"Bilineal (suave)", "AMD FSR 1.0 (nítido)", "AMD FSR 3.1 (temporal, cualquier GPU)",
                         "NVIDIA DLSS (temporal, GPU RTX)"};
    ImGui::SetNextItemWidth(320 * g_scale);
    ImGui::Combo(T("Escalado / antialiasing"), &g.upscaler, TL(ups, 4).p, 4);
    ImGui::SetItemTooltip(T("FSR 1.0: escalado espacial, solo cuando la resolución interna es menor que la ventana.\n"
                          "FSR 3.1 y DLSS: reconstrucción temporal - renderizan a menos resolución (según la calidad)\n"
                          "y reconstruyen la imagen de la ventana con varios frames: más FPS y bordes sin dientes.\n"
                          "Sustituyen al antialiasing temporal del juego. DLSS necesita una GPU NVIDIA RTX; si no\n"
                          "está disponible se usa FSR 3.1."));
    if (g.upscaler >= 2) {
        const char *uq[] = {"Nativo (solo antialiasing: DLAA / FSR Native AA)", "Calidad (67 %)", "Equilibrado (59 %)",
                            "Rendimiento (50 %)", "Ultra rendimiento (33 %)"};
        ImGui::SetNextItemWidth(320 * g_scale);
        ImGui::Combo(T("Calidad del escalado"), &g.uquality, TL(uq, 5).p, 5);
        ImGui::SetItemTooltip(T("Con 'Resolución interna' en 'Igual que la ventana': la resolución a la que se renderiza\n"
                              "antes de reconstruir (porcentaje del lado de la ventana)."));
        ImGui::Checkbox(T("Generación de frames (FSR 3)"), &g.frame_gen);
        ImGui::SetItemTooltip(T("Inserta un frame interpolado entre cada dos del juego en todo el juego: el doble de imágenes\n"
                              "por segundo (p. ej. FPS 30 -> 60 en pantalla). La de AMD: funciona con FSR 3.1 y con DLSS.\n"
                              "Añade algo de latencia. Se aplica al iniciar el juego."));
        ImGui::BeginDisabled(g.frame_gen);
        ImGui::Checkbox(T("Generación de frames solo en cinemáticas"), &g.cut_fg);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(T("El juego va a tus FPS sin generación de frames; en las cinemáticas (donde el juego anima a\n"
                              "los personajes a 30) baja a 30 parejos y la generación las lleva a 60 en pantalla.\n"
                              "Pensado para jugar a 60 en un monitor de 60 Hz. Se aplica al iniciar el juego."));
    } else {
        ImGui::TextDisabled(T("Se aplica cuando la resolución interna es menor que la ventana."));
    }
    ImGui::SeparatorText(T("Calidad"));
    ImGui::Checkbox(T("Antialiasing SMAA"), &g.smaa);
    ImGui::Checkbox(T("Profundidad de campo (desenfoque del juego)"), &g.dof);
    ImGui::SetItemTooltip(T("El desenfoque de lo cercano y lo lejano que el juego aplica también en Switch.\n"
                          "Desactivado: toda la escena nítida (y algo menos de trabajo para la GPU)."));
    ImGui::Checkbox(T("Antialiasing y desenfoque de movimiento del juego"), &g.game_blur);
    ImGui::SetItemTooltip(T("El antialiasing temporal (TMAA), el desenfoque de movimiento y el del pelo que el juego\n"
                          "aplica para los 720p de la Switch: a 1080p o más suavizan toda la imagen.\n"
                          "Desactivado: imagen nítida (combínalo con SMAA para los bordes)."));
    ImGui::Checkbox(T("Mostrar FPS en la barra de título"), &g.show_fps);
    ImGui::SeparatorText(T("Velocidad"));
    static const char *fps_ui[] = {"30 (como en Switch)", "60", "120", "144", "165", "Sin límite"};
    ImGui::Combo("FPS", &g.fps, TL(fps_ui, 6).p, 6);
    ImGui::SetItemTooltip(T("Más de 30: la velocidad del mundo, el reloj y las escenas no cambian (probado a 60 y 120);\n"
                          "la cámara queda un poco más lejos. El V-Sync forzado en el panel de NVIDIA / AMD o el límite\n"
                          "de RivaTuner también limitan los FPS: déjalos en 'configuración de la aplicación' / 0."));
    ImGui::Checkbox(T("V-Sync (con 30 / 60 FPS)"), &g.vsync);
    ImGui::SetItemTooltip(T("Cada imagen se muestra exactamente una vez, sincronizada con la pantalla: movimiento\n"
                          "fluido (sin imágenes repetidas o saltadas). Con más de 60 FPS o sin límite no se aplica."));
    static const char *cut_ui[] = {"Como el resto del juego", "30 estables (como en Switch)"};
    ImGui::SetNextItemWidth(320 * g_scale);
    ImGui::Combo(T("Cinemáticas"), &g.cutscenes, TL(cut_ui, 2).p, 2);
    ImGui::SetItemTooltip(T("En las cinemáticas el juego anima a los personajes a 30 aunque juegues a más FPS: con la\n"
                          "cámara a 60 encima se notan saltos (p. ej. el pelo de Dickson).\n"
                          "30 estables: las cinemáticas van a 30 parejos, cámara y personajes juntos (con V-Sync en un\n"
                          "monitor de 60 Hz cada imagen dura exactamente dos refrescos).\n"
                          "Con 'Generación de frames solo en cinemáticas' también van a 30, con generación encima."));
}

static void tab_controls() {
    if (cap_dev >= 0) {
        int left = (int)((cap_until - SDL_GetTicks()) / 1000) + 1;
        if (SDL_GetTicks() > cap_until) cap_dev = -1;
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), T("Pulsa %s para \"%s\"... (%d s, Supr = ninguna)"),
                           cap_dev == 1 ? T("un botón del mando") : T("una tecla o un botón del ratón"), T(k_names[cap_slot]), left);
    } else {
        ImGui::TextDisabled(T("Haz clic en una asignación y pulsa la tecla, botón del ratón o del mando nuevo. Si ya estaba en uso, se intercambian."));
    }
    if (ImGui::Button(T("Restaurar teclado"))) for (int i = 0; i < NACT; i++) g.key[i] = k_def_key[i], g.key2[i] = k_def_key2[i];
    ImGui::SameLine();
    if (ImGui::Button(T("Restaurar mando"))) for (int i = 0; i < NPAD; i++) g.pad[i] = k_def_pad[i];
    ImGui::SameLine();
    int npads = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&npads);
    if (npads > 0) ImGui::Text(T("   Mando conectado: %s"), SDL_GetGamepadNameForID(ids[0]));
    else ImGui::TextDisabled(T("   (ningún mando detectado)"));
    SDL_free(ids);
    ImGui::Checkbox(T("Cambiar de personaje como en Xenoblade 3 (mantén L2 y pulsa L1 / R1: el de arriba / abajo; F2)"), &g.party_switch);
    ImGui::SetItemTooltip(T("Pasa el control al siguiente miembro del grupo, en el campo y en combate. Cada uno se queda\n"
                          "donde está y conserva sus recargas de artes; el anterior pasa a la IA. No funciona en menús ni\n"
                          "escenas, y en combate salta a los miembros caídos."));
    {
        static const char *ic_ui[] = {"Automático (el mando conectado; sin mando, teclado)", "Switch", "Xbox", "PlayStation",
                                      "Teclado (las teclas asignadas)"};
        ImGui::SetNextItemWidth(340 * g_scale);
        ImGui::Combo(T("Iconos de botones"), &g.icons, TL(ic_ui, 5).p, 5);
        int ic = icons_effective(g.icons);
        if ((ic == 2 || ic == 3) && !exists((std::string("mods/") + k_icon_mod[ic - 2] + "/romfs").c_str()))  // (4: keyboard, no pack)
            ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), T("Falta el pack: descarga \"%s\" de GameBanana y pon su carpeta romfs en mods\\%s"),
                               ic == 2 ? "Xbox control layout" : "PS5 DualSense control layout", k_icon_mod[ic - 2]);
        ImGui::SetItemTooltip(T("Xbox / PlayStation: packs de iconos de CompC (GameBanana). Teclado: el juego dibuja en cada\n"
                              "icono la tecla asignada a ese botón (también ratón). Se aplica al iniciar el juego."));
    }
    {
        static const char *ml_ui[] = {"No", "Manteniendo el botón derecho", "Siempre"};
        ImGui::SetNextItemWidth(260 * g_scale);
        ImGui::Combo(T("Cámara con el ratón"), &g.mouse_look, TL(ml_ui, 3).p, 3);
        ImGui::SetItemTooltip(T("Siempre: el ratón mueve la cámara directamente y el cursor queda oculto dentro del juego.\n"
                              "F1 (menú) o Alt+Tab lo liberan; al mover el ratón sobre el juego vuelve a capturarse."));
    }
    ImGui::BeginDisabled(!g.mouse_look);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140 * g_scale);
    ImGui::SliderInt(T("Sensibilidad"), &g.mouse_sens, 1, 100);
    ImGui::SameLine();
    ImGui::Checkbox(T("Invertir eje Y"), &g.mouse_invert);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(140 * g_scale);
    ImGui::SliderInt(T("Zona muerta de los sticks (%)"), &g.stick_deadzone, 0, 30);
    ImGui::SetItemTooltip(T("Para un mando con drift (el personaje o la cámara se mueven solos): esa parte del recorrido\n"
                          "del stick cuenta como centrada. 0 = sin cambios (el juego ya tiene la suya)."));
    ImGui::TextDisabled(T("Artes 1-8 y talento: en combate seleccionan el arte de ese hueco de la paleta y lo usan."));
    if (ImGui::BeginTable("binds", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn(T("Acción (botón de Switch)"), ImGuiTableColumnFlags_WidthFixed, 220 * g_scale);
        ImGui::TableSetupColumn(T("Tecla"), ImGuiTableColumnFlags_WidthFixed, 130 * g_scale);
        ImGui::TableSetupColumn(T("Tecla 2"), ImGuiTableColumnFlags_WidthFixed, 130 * g_scale);
        ImGui::TableSetupColumn(T("Mando"));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (int i = 0; i < NACT; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(T(k_names[i]));
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            char lbl[96];
            for (int c = 0; c < 2; c++) {
                int dev = c ? 2 : 0, k = c ? g.key2[i] : g.key[i];
                snprintf(lbl, sizeof lbl, "%s##k%d", cap_dev == dev && cap_slot == i ? "..." : key_ui_name(k), c);
                if (ImGui::Button(lbl, ImVec2(120 * g_scale, 0))) { cap_dev = dev; cap_slot = i; cap_since = SDL_GetTicks(); cap_until = cap_since + 5000; }
                ImGui::TableNextColumn();
            }
            if (i < NPAD) {
                snprintf(lbl, sizeof lbl, "%s##p", cap_dev == 1 && cap_slot == i ? "..." : T(k_pad_label[g.pad[i]]));
                if (ImGui::Button(lbl, ImVec2(-1, 0))) { cap_dev = 1; cap_slot = i; cap_since = SDL_GetTicks(); cap_until = cap_since + 5000; }
            } else if (i < NSTICK) {
                ImGui::TextDisabled(T("(stick del mando)"));
            } else if (i < NACT - 2) {
                ImGui::TextDisabled(T("(cruceta + A)"));
            } else {
                ImGui::TextDisabled(i == NACT - 2 ? "(L2 + L1)" : "(L2 + R1)");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// Mods: every folder in mods/ with a romfs/ inside (runtime/services/romfs_virtual.c applies them alphabetically,
// a later one wins for the same file); xcde.ini mod_<folder> = 0/1, on by default.
static void scan_mods() {
    std::vector<Mod> found;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path("mods\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            std::string rf = path((std::string("mods\\") + fd.cFileName + "\\romfs").c_str());
            DWORD a = GetFileAttributesA(rf.c_str());
            if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) continue;
            Mod m{fd.cFileName, true};
            for (const Mod &o : g_mods) if (o.name == m.name) m.on = o.on;
            found.push_back(m);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(found.begin(), found.end(), [](const Mod &a, const Mod &b) { return a.name < b.name; });
    for (const Mod &o : g_mods) {  // (keep the settings of folders not present now)
        bool here = false;
        for (const Mod &m : found) here |= m.name == o.name;
        if (!here) { Mod k = o; k.present = false; found.push_back(k); }
    }
    g_mods = found;
}

// [XCDE] "Crear reporte": reports\xcde_report_<date>.zip with what a bug report needs - the last two game logs (this
// run's and the previous one, build\lasrecomp*.log), xcde.ini and the newest crash report (crash\, text + minidump).
// No saves or game files. Zipped by the tar.exe that Windows 10+ ships; the folder opens when it is done.
static void make_report() {
    std::vector<std::string> files;
    for (const char *f : {"build/lasrecomp.log", "build/lasrecomp.prev.log", "xcde.ini"})
        if (GetFileAttributesA(path(f).c_str()) != INVALID_FILE_ATTRIBUTES) files.push_back(f);
    WIN32_FIND_DATAA fd;
    std::string newest;
    HANDLE h = FindFirstFileA(path("crash/crash_*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do if (newest.empty() || strcmp(fd.cFileName, newest.c_str()) > 0) newest = fd.cFileName; while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!newest.empty()) {  // (crash_<stamp>.txt sorts by date)
        files.push_back("crash/" + newest);
        std::string dmp = "crash/" + newest.substr(0, newest.size() - 4) + ".dmp";
        if (GetFileAttributesA(path(dmp.c_str()).c_str()) != INVALID_FILE_ATTRIBUTES) files.push_back(dmp);
    }
    if (files.empty()) { g_status = T("No hay nada que reportar todavía (ni logs ni xcde.ini)"); return; }
    CreateDirectoryA(path("reports").c_str(), NULL);
    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", localtime(&now));
    std::string zip = std::string("reports/xcde_report_") + stamp + ".zip";
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, sizeof sys);
    std::string tar = std::string(sys) + "\\tar.exe";
    std::string cmd = "\"" + tar + "\" -a -c -f \"" + zip + "\"";
    for (const std::string &f : files) cmd += " \"" + f + "\"";
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(tar.c_str(), buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_root.c_str(), &si, &pi)) {
        g_status = T("No se pudo crear el reporte (falta tar.exe de Windows)");
        return;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code) { g_status = T("No se pudo crear el reporte"); return; }
    g_status = std::string(T("Reporte creado: ")) + zip + T(" (adjúntalo al reportar el problema)");
    ShellExecuteA(NULL, "open", path("reports").c_str(), NULL, NULL, SW_SHOWNORMAL);
}

static void tab_mods() {
    static bool scanned;
    if (!scanned) { scanned = true; scan_mods(); }
    ImGui::TextWrapped(T("Pon cada mod en su carpeta: mods\\<nombre>\\romfs\\... (la misma estructura que en Ryujinx / yuzu). "
                       "Sus archivos reemplazan o añaden archivos del juego. Se aplican en orden alfabético: si dos mods "
                       "cambian el mismo archivo, gana el último. Los mods de código (exefs, parches .pchtxt / IPS) no "
                       "funcionan en esta versión recompilada."));
    if (ImGui::Button(T("Abrir carpeta de mods"))) {
        CreateDirectoryA(path("mods").c_str(), NULL);
        ShellExecuteA(NULL, "open", path("mods").c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Actualizar lista"))) scan_mods();
    ImGui::Separator();
    int shown = 0;
    for (Mod &m : g_mods) {
        if (!m.present || is_icon_mod(m.name)) continue;
        ImGui::Checkbox(m.name.c_str(), &m.on);
        shown++;
    }
    if (!shown) ImGui::TextDisabled(T("No hay mods instalados."));
}

static void tab_audio() {
    ImGui::SeparatorText(T("Audio"));
    ImGui::SetNextItemWidth(300 * g_scale);
    ImGui::SliderInt(T("Volumen"), &g.volume, 0, 200, "%d%%");
    ImGui::Checkbox(T("Sonido de los vídeos"), &g.movie_audio);
    ImGui::SeparatorText(T("Idioma del juego"));
    ImGui::SetNextItemWidth(260 * g_scale);
    ImGui::Combo(T("Idioma"), &g.lang, TL(k_lang_ui, 7).p, 7);
    ImGui::TextDisabled(T("Textos del juego. Solo los idiomas que incluya tu copia; si uno falla, vuelve a Inglés."));
}

// [XCDE] start-up trace (build/launcher.log) and a visible error instead of a silent exit: a PC where the launcher "did
// nothing" (no window, no message) had no way to tell why.
static FILE *g_llog;
static void llog(const char *fmt, ...) {
    if (!g_llog) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_llog, fmt, ap);
    va_end(ap);
    fputc('\n', g_llog);
    fflush(g_llog);
}
static int fail(const char *what) {
    char msg[1024];
    snprintf(msg, sizeof msg, "%s\n\n%s\n\n(build\\launcher.log)", what, SDL_GetError());
    llog("ERROR: %s: %s", what, SDL_GetError());
    MessageBoxA(NULL, msg, "Monado II - Launcher", MB_OK | MB_ICONERROR);
    return 1;
}
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e) {
    char msg[256];
    snprintf(msg, sizeof msg, "The launcher crashed (exception %#lx at %p).\n\n(build\\launcher.log)",
             e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    llog("CRASH: exception %#lx at %p", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    MessageBoxA(NULL, msg, "Monado II - Launcher", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv) {
    // root: the folder above the launcher's build/ folder (or the working directory)
    const char *base = SDL_GetBasePath();
    g_root = base ? base : ".";
    while (!g_root.empty() && (g_root.back() == '/' || g_root.back() == '\\')) g_root.pop_back();
    if (g_root.size() > 6 && (g_root.compare(g_root.size() - 6, 6, "\\build") == 0 || g_root.compare(g_root.size() - 6, 6, "/build") == 0))
        g_root.resize(g_root.size() - 6);
    // --shot <file.bmp> [tab] [precompile]: render a few frames of that tab, save the window as a BMP and quit (tests)
    const char *shot = argc > 2 && !strcmp(argv[1], "--shot") ? argv[2] : NULL;
    int shot_tab = argc > 3 ? atoi(argv[3]) : 0, frame_no = 0;
    bool shot_pc = shot && argc > 4 && !strcmp(argv[4], "precompile");  // (... <tab> precompile: the bar after ~5 s of a run)
    g_llog = fopen((g_root + "\\build\\launcher.log").c_str(), "w");
    SetUnhandledExceptionFilter(crash_filter);
    llog("launcher start, root %s", g_root.c_str());
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        llog("SDL_Init with gamepads failed (%s): video only", SDL_GetError());
        if (!SDL_Init(SDL_INIT_VIDEO)) return fail("Could not start SDL (video).");
    }
    SDL_Window *win = SDL_CreateWindow("Monado II - Launcher", 980, 680, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!win) return fail("Could not create the launcher window.");
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    if (!ren) {  // (an old or missing graphics driver: the CPU renderer still shows the launcher)
        llog("GPU renderer failed (%s): software renderer", SDL_GetError());
        ren = SDL_CreateRenderer(win, "software");
    }
    if (!ren) return fail("Could not create a renderer for the launcher window (graphics driver?).");
    llog("renderer %s", SDL_GetRendererName(ren));
    SDL_SetRenderVSync(ren, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    float scale = SDL_GetWindowDisplayScale(win);
    if (scale <= 0) scale = 1;
    g_scale = scale;
    SDL_SetWindowSize(win, (int)(980 * scale), (int)(680 * scale));
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (!shot) SDL_ShowWindow(win);
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * scale)) io.Fonts->AddFontDefault();
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 0; st.FrameRounding = 4; st.FramePadding = ImVec2(8, 5); st.ItemSpacing = ImVec2(10, 8);
    st.ScaleAllSizes(scale);
    st.Colors[ImGuiCol_Button] = ImVec4(0.18f, 0.35f, 0.55f, 1);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.47f, 0.72f, 1);
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);
    load();
    bool have_game = exists("targets/exefs/main") || exists("targets/exefs");
    bool have_exe = exists("build/xcde.exe");
    bool run = true, launched = false;
    if (shot_pc) pc_start(false);
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (capture_event(e)) continue;
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) run = false;
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(e.gdevice.which);
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("main", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1, 1), "Xenoblade Chronicles: Definitive Edition");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("%s", T("Recompilación nativa para PC · configuración"));
        {   // [XCDE] the launcher's own language (Spanish / English), saved with the settings
            static const char *ll[] = {"Español", "English"};
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 330 * scale);
            ImGui::SetNextItemWidth(150 * scale);
            ImGui::Combo(T("Idioma del launcher"), &g.ui_lang, ll, 2);
        }
        float bottom = ImGui::GetFrameHeightWithSpacing() * (pc_running() ? 3.3f : 2.2f);
        ImGui::BeginChild("tabs", ImVec2(0, -bottom));
        if (ImGui::BeginTabBar("t")) {
            ImGuiTabItemFlags f0 = shot && shot_tab == 0 ? ImGuiTabItemFlags_SetSelected : 0, f1 = shot && shot_tab == 1 ? ImGuiTabItemFlags_SetSelected : 0,
                              f2 = shot && shot_tab == 2 ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(T("Gráficos"), NULL, f0)) { tab_graphics(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(T("Controles"), NULL, f1)) { tab_controls(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(T("Audio e idioma"), NULL, f2)) { tab_audio(); ImGui::EndTabItem(); }
            ImGuiTabItemFlags f3 = shot && shot_tab == 3 ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(T("Mods"), NULL, f3)) { tab_mods(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::Separator();
        if (!have_game) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", T("Faltan los archivos del juego en targets/ (exefs y romfs de tu copia)."));
        else if (!have_exe) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", T("Falta build/xcde.exe (compila el juego primero)."));
        else ImGui::TextDisabled(T("%s"), g_status.empty() ? (pc_done() ? T("Listo. Guardado: save/   Ajustes: xcde.ini")
                                                                   : T("Listo. La primera vez, JUGAR prepara los shaders antes (unos minutos)"))
                                                       : g_status.c_str());
        pc_poll();
        if (pc_running()) {
            ImGui::ProgressBar(g_pc_frac, ImVec2(-150 * scale, 0), g_pc_text.c_str());
            ImGui::SameLine();
            if (ImGui::Button(g_pc_then_play ? T("Omitir") : T("Detener"), ImVec2(-1, 0))) {
                pc_stop();
                g_status = T("Precompilación detenida (se puede seguir otro día: lo ya hecho se conserva)");
                if (g_pc_then_play) g_pc_play_now = true;
            }
        }
        if (g_pc_play_now) {
            g_pc_play_now = false;
            launch();
            launched = g_launched_ok;
            if (launched) run = false;
        }
        ImGui::BeginDisabled(!have_game || !have_exe || launched || pc_running());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.3f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.7f, 0.4f, 1));
        if (ImGui::Button(T("JUGAR"), ImVec2(200 * scale, 0))) {
            if (!pc_done()) pc_start(true);
            else { launch(); launched = g_launched_ok; if (launched) run = false; }
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button(T("Precompilar shaders"))) pc_start(false);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(T("Traduce y compila todos los shaders del juego por adelantado, para que la primera visita\n"
                              "a una zona no se detenga a compilarlos. Solo hace falta una vez (o tras actualizar el juego)."));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T("Guardar ajustes"))) g_status = save() ? T("Ajustes guardados en xcde.ini") : T("No se pudo escribir xcde.ini");
        ImGui::SameLine();
        if (ImGui::Button(T("Crear reporte"))) make_report();
        ImGui::SetItemTooltip(T("Junta en un .zip los logs de las dos últimas partidas, xcde.ini y el último informe de\n"
                              "cierre inesperado (sin partidas guardadas ni archivos del juego), para adjuntarlo a un reporte."));
        ImGui::SameLine();
        if (ImGui::Button(T("Salir"))) run = false;
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 18, 20, 26, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        if (shot && ++frame_no == (shot_pc ? 300 : 5)) {
            SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
            if (sf) { SDL_SaveBMP(sf, shot); SDL_DestroySurface(sf); }
            run = false;
        }
        SDL_RenderPresent(ren);
    }
    if (!launched) pc_stop();  // (closing the launcher stops a precompilation; what it wrote is kept)
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
