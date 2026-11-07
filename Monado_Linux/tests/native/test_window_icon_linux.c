// [XCDE] LINUX-016: actual rt_window_icon path/PNG/API component verification.
// runtime/paths.c is a separate translation unit compiled with its window-icon
// call renamed to icon_spy. This fixture still calls the real SDL function;
// only plat_exe_path is supplied, so neither selected application is executed.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include "../../runtime/paths.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL line%d: %s (SDL: %s; errno: %s)\n", __LINE__, #value, SDL_GetError(), strerror(errno)); abort(); } } while (0)
static const char *selected_executable, *capture_path;
static unsigned calls;
static int width, height;
static uint64_t alpha_zero, alpha_full, alpha_partial, rgba_bytes;
static unsigned alpha_min = 255, alpha_max;
static bool real_api_result;

bool plat_exe_path(char *out, size_t size) {
    size_t length = strlen(selected_executable);
    if (length >= size) return false;
    memcpy(out, selected_executable, length + 1);
    return true;
}

bool plat_remove_tree(const char *path) {
    (void)path;
    CHECK(!"Icon checks must never call deletion helpers");
    return false;
}

bool icon_spy(SDL_Window *window, SDL_Surface *surface) {
    CHECK(window && surface && calls == 0);
    calls++;
    SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    CHECK(rgba && rgba->w > 0 && rgba->h > 0);
    CHECK(SDL_LockSurface(rgba));
    width = rgba->w; height = rgba->h;
    FILE *file = fopen(capture_path, "wbx");
    CHECK(file);
    for (int y = 0; y < height; y++) {
        const unsigned char *row = (unsigned char *)rgba->pixels + y * rgba->pitch;
        size_t bytes = (size_t)width * 4;
        CHECK(fwrite(row, 1, bytes, file) == bytes);
        rgba_bytes += bytes;
        for (int x = 0; x < width; x++) {
            unsigned alpha = row[x * 4 + 3];
            alpha_zero += alpha == 0;
            alpha_full += alpha == 255;
            alpha_partial += alpha != 0 && alpha != 255;
            if (alpha < alpha_min) alpha_min = alpha;
            if (alpha > alpha_max) alpha_max = alpha;
        }
    }
    CHECK(fclose(file) == 0);
    SDL_UnlockSurface(rgba);
    SDL_DestroySurface(rgba);
    // The dummy driver may reject an OS icon. The meaningful check here is
    // the exact decoded pixels presented to SDL, not compositor acceptance.
    real_api_result = SDL_SetWindowIcon(window, surface);
    fprintf(stderr, "real SDL_SetWindowIcon returned%d (%s)\n", real_api_result, SDL_GetError());
    return real_api_result;
}

static void json_string(FILE *file, const char *text) {
    CHECK(fputc('"', file) != EOF);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '"' || *p == '\\') CHECK(fputc('\\', file) != EOF);
        if (*p < 32) CHECK(fprintf(file, "\\u%04x", *p) > 0);
        else CHECK(fputc(*p, file) != EOF);
    }
    CHECK(fputc('"', file) != EOF);
}

int main(int argc, char **argv) {
    CHECK(argc == 8);
    selected_executable = argv[1]; capture_path = argv[4];
    const char *input = argv[2], *state = argv[3], *expected_resource = argv[6];
    unsigned expected_calls = (unsigned)atoi(argv[7]);
    CHECK(expected_calls <= 1);
    alarm(15);
    CHECK(unsetenv("LARECOMP_SAVE_DIR") == 0);
    char error[512], cwd[PATH_MAX], resource[PATH_MAX] = {0};
    CHECK(rt_paths_init_linux(input, state, error, sizeof error));
    CHECK(getcwd(cwd, sizeof cwd) && !strcmp(cwd, state));
    CHECK(!strcmp(rt_data_root(), state) && !strcmp(rt_input_root(), input));
    bool found = rt_resource("icon.png", NULL, resource, sizeof resource);
    if (!strcmp(expected_resource, "-")) CHECK(!found);
    else CHECK(found && !strcmp(resource, expected_resource));
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    CHECK(SDL_GetCurrentVideoDriver() && !strcmp(SDL_GetCurrentVideoDriver(), "dummy"));
    SDL_Window *window = SDL_CreateWindow("Monado icon component", 64, 64, SDL_WINDOW_HIDDEN);
    CHECK(window && (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN));
    rt_window_icon(window);
    CHECK(calls == expected_calls);
    if (!calls) CHECK(access(capture_path, F_OK) != 0);
    FILE *metadata = fopen(argv[5], "wbx");
    CHECK(metadata);
    CHECK(fprintf(metadata, "{\n  \"resource_found\": %s,\n  \"resolved_resource\": ", found ? "true" : "false") > 0);
    json_string(metadata, resource);
    CHECK(fprintf(metadata,
        ",\n  \"calls\": %u,\n  \"width\": %d,\n  \"height\": %d,\n  \"rgba_bytes\": %llu,"
        "\n  \"alpha_zero\": %llu,\n  \"alpha_full\": %llu,\n  \"alpha_partial\": %llu,"
        "\n  \"alpha_min\": %u,\n  \"alpha_max\": %u,\n  \"real_api_result\": %s,"
        "\n  \"video_driver\": \"dummy\",\n  \"hidden_window\": true,\n  \"cwd_matches_state\": true\n}\n",
        calls, width, height, (unsigned long long)rgba_bytes, (unsigned long long)alpha_zero,
        (unsigned long long)alpha_full, (unsigned long long)alpha_partial,
        calls ? alpha_min : 0, alpha_max, real_api_result ? "true" : "false") > 0);
    CHECK(fclose(metadata) == 0);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("PASS actual rt_window_icon: %u calls, %dx%d RGBA, hidden dummy window\n", calls, width, height);
    return 0;
}
