// [XCDE] In-game settings overlay (runtime/ui/menu.h; Windows / Vulkan): F1 opens and closes it (Esc closes), the
// launcher's options over the running game. Dear ImGui drawn by its Vulkan backend into the window image after the
// game image (device.c calls menu_render inside that rendering). Events arrive on the main thread and are queued for
// the render thread, which owns the ImGui context; while the menu is open the game sees no keyboard / mouse / pad
// input. Options that the renderer reads every frame apply at once (resolution, upscaler and its quality, SMAA, depth
// of field, volume, FPS in the title, V-Sync); the others are saved to xcde.ini and apply at the next start
// (marked "al reiniciar"). "Guardar" writes xcde.ini (settings_save) for the launcher and the next start.
#include <SDL3/SDL.h>
#include <math.h>
#include <mutex>
#include <string.h>

#include "imgui.h"
#include "imgui_impl_vulkan.h"

extern "C" {
#include "../nvn/vulkan/vk.h"
#include "../settings.h"
#include "menu.h"
bool vk_imgui_backend_init(void);
void input_reset(void);
void vk_request_swapchain_recreate(void);
bool dlss_available(void);
}

static std::mutex q_lock;
static SDL_Event q[256];
static int q_n;
static volatile bool open_, want_window;
static bool fs_applied = false, fs_init;

static bool is_input(Uint32 t) {
    switch (t) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return true;
    default: return false;
    }
}

extern "C" bool menu_handle_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&
        (e->key.scancode == SDL_SCANCODE_F1 || (open_ && e->key.scancode == SDL_SCANCODE_ESCAPE))) {
        open_ = !open_ && e->key.scancode == SDL_SCANCODE_F1;
        input_reset();  // (keys held when the menu opened / closed are released for the game)
        return true;
    }
    if (!open_ || !is_input(e->type)) return false;
    std::lock_guard<std::mutex> g(q_lock);
    if (q_n < 256) q[q_n++] = *e;
    return true;
}

extern "C" bool menu_is_open(void) { return open_; }

extern "C" void menu_apply_window_requests(void *sdl_window) {
    if (!fs_init) { fs_init = true; fs_applied = g_settings.fullscreen; }
    if (!want_window || !sdl_window) return;
    want_window = false;
    if (fs_applied != g_settings.fullscreen) {
        SDL_SetWindowFullscreen((SDL_Window *)sdl_window, g_settings.fullscreen);
        fs_applied = g_settings.fullscreen;
    }
}

extern "C" void menu_test_open(void) { open_ = true; }

static ImGuiKey key_of(SDL_Scancode s) {
    switch (s) {
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    default: return ImGuiKey_None;
    }
}
static ImGuiKey pad_key(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    default: return ImGuiKey_None;
    }
}

// Events queued by the main thread -> ImGui IO (render thread). Mouse positions: window points -> pixels.
static void feed(ImGuiIO &io, uint32_t w, uint32_t h) {
    SDL_Event ev[256];
    int n;
    {
        std::lock_guard<std::mutex> g(q_lock);
        n = q_n;
        memcpy(ev, q, sizeof(SDL_Event) * n);
        q_n = 0;
    }
    for (int i = 0; i < n; i++) {
        const SDL_Event &e = ev[i];
        float sx = 1, sy = 1;
        SDL_Window *win = SDL_GetWindowFromID(e.motion.windowID);
        if (win) {
            int ww, wh;
            SDL_GetWindowSize(win, &ww, &wh);
            if (ww > 0 && wh > 0) { sx = (float)w / ww; sy = (float)h / wh; }
        }
        switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION: io.AddMousePosEvent(e.motion.x * sx, e.motion.y * sy); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            io.AddMousePosEvent(e.button.x * sx, e.button.y * sy);
            io.AddMouseButtonEvent(b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: io.AddMouseWheelEvent(e.wheel.x, e.wheel.y); break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
            ImGuiKey k = key_of(e.key.scancode);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_EVENT_KEY_DOWN);
            break;
        }
        case SDL_EVENT_TEXT_INPUT: io.AddInputCharactersUTF8(e.text.text); break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            ImGuiKey k = pad_key(e.gbutton.button);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) open_ = false;  // (B: back to the game)
            break;
        }
        default: break;
        }
    }
}

// [XCDE] the menu's language follows the launcher's (xcde.ini launcher_lang; g_settings.ui_lang 1 = English): the
// Spanish text is the key, T() gives the English one.
static const char *const k_en[][2] = {
    {"Igual que la ventana (recomendado)", "Match the window (recommended)"}, {"1x - 720p (como Switch)", "1x - 720p (like Switch)"},
    {"75 % de la ventana", "75 % of the window"}, {"50 % de la ventana", "50 % of the window"},
    {"Dinámica (mantiene 60 FPS)", "Dynamic (holds 60 FPS)"}, {"Opciones  (F1 / Esc para volver al juego)", "Options  (F1 / Esc to go back to the game)"},
    {"Gráficos", "Graphics"}, {"Pantalla", "Display"}, {"Pantalla completa", "Fullscreen"},
    {"Resolución y escalado", "Resolution and scaling"}, {"Resolución interna", "Internal resolution"},
    {"Bilineal (suave)", "Bilinear (soft)"}, {"AMD FSR 1.0 (nítido)", "AMD FSR 1.0 (sharp)"}, {"Escalado / antialiasing", "Upscaling / anti-aliasing"},
    {"DLSS se carga al iniciar el juego: hasta reiniciar se usa FSR 3.1.", "DLSS loads when the game starts: FSR 3.1 is used until a restart."},
    {"Nativo (solo antialiasing)", "Native (anti-aliasing only)"}, {"Calidad (67 %)", "Quality (67 %)"}, {"Equilibrado (59 %)", "Balanced (59 %)"},
    {"Rendimiento (50 %)", "Performance (50 %)"}, {"Ultra rendimiento (33 %)", "Ultra performance (33 %)"}, {"Calidad del escalado", "Upscaling quality"},
    {"Generación de frames (FSR 3; activarla por primera vez: al reiniciar)", "Frame generation (FSR 3; the first time it is enabled: after a restart)"},
    {"Calidad", "Quality"}, {"Antialiasing SMAA", "SMAA anti-aliasing"}, {"(no se usa con FSR 3.1 / DLSS)", "(not used with FSR 3.1 / DLSS)"},
    {"Profundidad de campo", "Depth of field"},
    {"Antialiasing y desenfoque de movimiento del juego (al reiniciar)", "The game's anti-aliasing and motion blur (after a restart)"},
    {"Mostrar FPS en la barra de título", "Show FPS in the title bar"}, {"Velocidad", "Speed"}, {"30 (como en Switch)", "30 (like Switch)"},
    {"Sin límite", "Unlimited"}, {"FPS (al reiniciar)", "FPS (after a restart)"}, {"V-Sync (con 30 / 60 FPS)", "V-Sync (at 30 / 60 FPS)"},
    {"Volumen", "Volume"}, {"Audio de los vídeos (al reiniciar)", "Video sound (after a restart)"}, {"Controles", "Controls"},
    {"La asignación de teclas y botones se cambia en el launcher (Play Monado.bat), pestaña Controles.",
     "Keys and buttons are remapped in the launcher, Controls tab."},
    {"Guardar", "Save"}, {"Volver al juego", "Back to the game"}, {"Guardado en xcde.ini", "Saved to xcde.ini"},
};
static const char *T(const char *es) {
    if (g_settings.ui_lang != 1) return es;
    for (const auto &e : k_en) if (!strcmp(e[0], es)) return e[1];
    return es;
}
struct TList { const char *p[8]; };
static TList TL(const char *const *a, int n) { TList t{}; for (int i = 0; i < n && i < 8; i++) t.p[i] = T(a[i]); return t; }

static const char *const k_res_ui[] = {"Igual que la ventana (recomendado)", "1x - 720p (como Switch)", "2x - 1440p", "3x - 2160p",
                                       "75 % de la ventana", "50 % de la ventana", "Dinámica (mantiene 60 FPS)"};
static const int k_res_mode[] = {RES_MATCH, 1, 2, 3, RES_Q75, RES_Q50, RES_DYNAMIC};

static void ui(void) {
    static bool saved_note;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x * 0.5f, vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.y * 0.95f, vp->Size.y * 0.8f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin(T("Opciones  (F1 / Esc para volver al juego)"), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem(T("Gráficos"))) {
            ImGui::SeparatorText(T("Pantalla"));
            if (ImGui::Checkbox(T("Pantalla completa"), &g_settings.fullscreen)) want_window = true;
            ImGui::SeparatorText(T("Resolución y escalado"));
            int ri = 0;
            for (int i = 0; i < 7; i++) if (k_res_mode[i] == g_settings.res_mode) ri = i;
            if (ImGui::Combo(T("Resolución interna"), &ri, TL(k_res_ui, 7).p, 7)) g_settings.res_mode = k_res_mode[ri];
            static const char *ups[] = {"Bilineal (suave)", "AMD FSR 1.0 (nítido)", "AMD FSR 3.1 (temporal)", "NVIDIA DLSS (temporal, RTX)"};
            static const int upv[] = {UPSCALE_BILINEAR, UPSCALE_FSR1, UPSCALE_FSR3, UPSCALE_DLSS};
            int ui_ = 0;
            for (int i = 0; i < 4; i++) if (upv[i] == g_settings.upscaler) ui_ = i;
            if (ImGui::Combo(T("Escalado / antialiasing"), &ui_, TL(ups, 4).p, 4)) g_settings.upscaler = upv[ui_];
            if (g_settings.upscaler == UPSCALE_DLSS && !dlss_available())
                ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), T("DLSS se carga al iniciar el juego: hasta reiniciar se usa FSR 3.1."));
            if (g_settings.upscaler == UPSCALE_FSR3 || g_settings.upscaler == UPSCALE_DLSS) {
                static const char *uq[] = {"Nativo (solo antialiasing)", "Calidad (67 %)", "Equilibrado (59 %)", "Rendimiento (50 %)",
                                           "Ultra rendimiento (33 %)"};
                ImGui::Combo(T("Calidad del escalado"), &g_settings.upscale_quality, TL(uq, 5).p, 5);
                ImGui::BeginDisabled(g_settings.upscaler != UPSCALE_FSR3);
                ImGui::Checkbox(T("Generación de frames (FSR 3; activarla por primera vez: al reiniciar)"), &g_settings.frame_gen);
                ImGui::EndDisabled();
            }
            ImGui::SeparatorText(T("Calidad"));
            bool smaa = g_settings.antialias == AA_SMAA;
            if (ImGui::Checkbox(T("Antialiasing SMAA"), &smaa)) g_settings.antialias = smaa ? AA_SMAA : AA_OFF;
            if (g_settings.upscaler == UPSCALE_FSR3 || g_settings.upscaler == UPSCALE_DLSS) ImGui::SameLine(), ImGui::TextDisabled(T("(no se usa con FSR 3.1 / DLSS)"));
            bool dof = !g_settings.dof_off;
            if (ImGui::Checkbox(T("Profundidad de campo"), &dof)) g_settings.dof_off = !dof;
            ImGui::Checkbox(T("Antialiasing y desenfoque de movimiento del juego (al reiniciar)"), &g_settings.game_blur);
            ImGui::Checkbox(T("Mostrar FPS en la barra de título"), &g_settings.show_fps);
            ImGui::SeparatorText(T("Velocidad"));
            static const int fpsv[] = {30, 60, 120, 144, 165, 0};
            static const char *fpsu[] = {"30 (como en Switch)", "60", "120", "144", "165", "Sin límite"};
            int fi = 0;
            for (int i = 0; i < 6; i++) if (fpsv[i] == g_settings.fps) fi = i;
            if (ImGui::Combo(T("FPS (al reiniciar)"), &fi, TL(fpsu, 6).p, 6)) g_settings.fps = fpsv[fi];
            if (ImGui::Checkbox(T("V-Sync (con 30 / 60 FPS)"), &g_settings.vsync)) vk_request_swapchain_recreate();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("Audio"))) {
            int vol = (int)lroundf(g_settings.volume * 100);
            if (ImGui::SliderInt(T("Volumen"), &vol, 0, 200, "%d %%")) g_settings.volume = vol / 100.0f;
            ImGui::Checkbox(T("Audio de los vídeos (al reiniciar)"), &g_settings.movie_audio);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("Controles"))) {
            ImGui::TextWrapped(T("La asignación de teclas y botones se cambia en el launcher (Play Monado.bat), pestaña Controles."));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button(T("Guardar"))) { settings_save(); saved_note = true; }
    ImGui::SameLine();
    if (ImGui::Button(T("Volver al juego"))) { open_ = false; input_reset(); }
    if (saved_note) { ImGui::SameLine(); ImGui::TextDisabled(T("Guardado en xcde.ini")); }
    ImGui::End();
}

extern "C" void menu_render(void *cmd, uint32_t w, uint32_t h) {
    static ImGuiContext *ctx;
    static float font_h;
    static uint64_t t_last;
    if (!open_) return;
    if (!ctx) {
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        font_h = fmaxf(16.0f, h / 1080.0f * 22.0f);
        if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", font_h, nullptr, io.Fonts->GetGlyphRangesDefault()))
            io.Fonts->AddFontDefault();
        ImGui::StyleColorsDark();
        ImGui::GetStyle().ScaleAllSizes(font_h / 16.0f);
        if (!vk_imgui_backend_init()) { ImGui::DestroyContext(ctx); ctx = nullptr; open_ = false; return; }
        ImGui_ImplVulkan_CreateFontsTexture();
    }
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    uint64_t now = SDL_GetTicksNS();
    io.DeltaTime = t_last ? (float)((now - t_last) / 1e9) : 1.0f / 60;
    if (io.DeltaTime <= 0) io.DeltaTime = 1.0f / 60;
    t_last = now;
    feed(io, w, h);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    ui();
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);
}
