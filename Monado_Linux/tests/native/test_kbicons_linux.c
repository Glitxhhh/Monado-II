// [XCDE] LINUX-004: procedural icon fixture only; no game atlas or input.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
static bool hide_fonts;
static FILE *fixture_fopen(const char *path, const char *mode);
#define fopen fixture_fopen
#include "../../runtime/services/kbicons.c"
#undef fopen

int16_t g_bind_key[BIND_KEY_COUNT], g_bind_key2[BIND_KEY_COUNT];
int g_mouse_look;
static FILE *fixture_fopen(const char *path, const char *mode) {
    if (hide_fonts && (!strncmp(path, "C:/Windows/Fonts/", 17) || !strncmp(path, "/usr/share/fonts/", 17))) return NULL;
    return fopen(path, mode);
}
static void put32(unsigned char *p, uint32_t value) { memcpy(p, &value, 4); }
int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (!strcmp(argv[1], "font")) {
        CHECK(font_load());
        CHECK(stbtt_FindGlyphIndex(&g_font, 'W') > 0);
        puts("Linux keyboard system font: OK");
        return 0;
    }
    hide_fonts = !strcmp(argv[1], "fallback");
    if (!hide_fonts) {
        // Isolate cache creation from font discovery, including in the old-source reproducer.
        size_t font_size;
        g_font_data = read_all("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", &font_size);
        CHECK(g_font_data && font_size > 0 && stbtt_InitFont(&g_font, g_font_data, 0));
    }
    CHECK(mkdir("menu", 0700) == 0 && mkdir("menu/image", 0700) == 0);
    const char *path = "menu/image/mnu001_btn.wilay";
    unsigned char input[8192] = {0}, pixels[8 * 8 * 4];
    memset(pixels, 255, sizeof pixels);
    size_t block_size;
    unsigned char *block = encode_bc3(pixels, 8, 8, &block_size);
    CHECK(block && block_size <= 0x400 - 0x28);
    memcpy(input, "LAHD", 4);
    put32(input + 0x24, 0x40);
    put32(input + 0x40, 8); put32(input + 0x44, 1);
    put32(input + 0x4c, 0x100); put32(input + 0x50, 0x400);
    memcpy(input + 0x140, block, block_size); free(block);
    uint32_t footer[10] = {512, 0, 8, 8, 1, 1, 68, 1, 0, 0};
    memcpy(footer + 9, "LBIM", 4);
    memcpy(input + 0x140 + 0x400 - sizeof footer, footer, sizeof footer);
    FILE *f = fopen(path, "wx"); CHECK(f);
    CHECK(fwrite(input, 1, sizeof input, f) == sizeof input && fclose(f) == 0);
    uint64_t length = 0;
    unsigned char *result = kbicons_make(path, &length);
    if (hide_fonts) { CHECK(!result); CHECK(access("cache", F_OK) != 0); }
    else {
        CHECK(result && length == sizeof input);
        DIR *d = opendir("cache/kbicons"); CHECK(d);
        int files = 0; struct dirent *entry;
        while ((entry = readdir(d))) if (entry->d_name[0] != '.') { CHECK(!strstr(entry->d_name, ".tmp")); files++; }
        closedir(d); CHECK(files == 1);
        uint64_t again_length = 0;
        unsigned char *again = kbicons_make(path, &again_length);
        CHECK(again && again_length == length && !memcmp(again, result, length));
        free(again); free(result);
    }
    unsigned char after[sizeof input];
    f = fopen(path, "rb"); CHECK(f);
    CHECK(fread(after, 1, sizeof after, f) == sizeof after && fclose(f) == 0);
    CHECK(!memcmp(after, input, sizeof input));
    puts(hide_fonts ? "Missing keyboard font preserves original atlas: OK" : "Linux keyboard cache creation/reuse/input preservation: OK");
    return 0;
}
