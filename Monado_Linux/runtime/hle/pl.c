// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Peiikai (LASRecomp)
// Copyright (c) 2026 Monado contributors
// Monado II publication-only font loader. The original development sources stay
// unchanged. This override accepts user-supplied plain TTF/OTF/TTC files only;
// proprietary font containers, deobfuscation and embedded key material are absent.
// Font rendering/glyph coverage still depends on the supplied compatible font.
// See the root LICENSE and NOTICE for attribution and third-party terms.
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../hle.h"

typedef struct { uint64_t addr, size; } Font;
static Font fonts[2];  // Standard substitute; optional user-supplied button font.
static int g_fonts_loaded;

enum { FONT_MAX_BYTES = 64 * 1024 * 1024 };

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static void put32(uint8_t *p, uint32_t value) {
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
    p[3] = value;
}

static int plain_extension(const char *path) {
    const char *extension = strrchr(path, '.');
    return extension && (!strcasecmp(extension, ".ttf") ||
                         !strcasecmp(extension, ".otf") ||
                         !strcasecmp(extension, ".ttc"));
}

static uint8_t *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
    long end = ftell(file);
    if (end < 12 || end > FONT_MAX_BYTES || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return NULL;
    }
    size_t size = (size_t)end;
    uint8_t *data = malloc(size);
    if (!data) { fclose(file); return NULL; }
    int error = fread(data, 1, size, file) != size;
    if (fclose(file)) error = 1;
    if (error) { free(data); return NULL; }
    *length = size;
    return data;
}

static int sfnt_tag(const uint8_t *data) {
    return be32(data) == 0x00010000u || !memcmp(data, "OTTO", 4) ||
           !memcmp(data, "true", 4);
}

// Table offsets in a collection are absolute within the original collection.
// Validate all ranges before allocation/copy; this is a container check, not a
// replacement for the font engine's outline/cmap/glyph validation.
static int sfnt_directory(const uint8_t *data, size_t length, size_t face,
                          size_t prefix, uint32_t *count, size_t *directory_size) {
    if (face > length || length - face < 12 || !sfnt_tag(data + face)) return 0;
    uint32_t tables = (uint32_t)data[face + 4] << 8 | data[face + 5];
    size_t size = 12 + (size_t)tables * 16;
    if (!tables || size > length - face) return 0;
    int have_data = 0;
    for (uint32_t i = 0; i < tables; i++) {
        const uint8_t *entry = data + face + 12 + (size_t)i * 16;
        size_t offset = be32(entry + 8), bytes = be32(entry + 12);
        if (offset > length || bytes > length - offset) return 0;
        if (!bytes) continue;
        if ((offset & 3) || offset < prefix ||
            (offset < face + size && offset + bytes > face)) return 0;
        have_data = 1;
    }
    if (!have_data) return 0;
    *count = tables;
    *directory_size = size;
    return 1;
}

static uint32_t sfnt_checksum(const uint8_t *data, size_t length) {
    uint32_t sum = 0;
    for (size_t offset = 0; offset < length; offset += 4) sum += be32(data + offset);
    return sum;
}

// Extract collection face zero into its own ordinary sfnt. No source bytes are
// altered; table data is copied and offsets/checksum adjustment are rebuilt.
static uint8_t *ttc_face0(const uint8_t *data, size_t length, size_t *output_length) {
    if (length < 16 || memcmp(data, "ttcf", 4)) return NULL;
    uint32_t version = be32(data + 4), faces = be32(data + 8);
    if ((version != 0x00010000u && version != 0x00020000u) || !faces ||
        faces > (length - 12) / 4) return NULL;
    size_t prefix = 12 + (size_t)faces * 4;
    if (version == 0x00020000u) {
        if (length - prefix < 12) return NULL;
        prefix += 12;
    }
    size_t face = be32(data + 12), directory_size;
    uint32_t tables;
    if (face < prefix || !sfnt_directory(data, length, face, prefix,
                                        &tables, &directory_size)) return NULL;
    size_t total = directory_size;
    for (uint32_t i = 0; i < tables; i++) {
        const uint8_t *entry = data + face + 12 + (size_t)i * 16;
        size_t padded = ((size_t)be32(entry + 12) + 3) & ~(size_t)3;
        if (padded > FONT_MAX_BYTES - total) return NULL;
        total += padded;
    }
    uint8_t *result = calloc(1, total);
    if (!result) return NULL;
    memcpy(result, data + face, 12);
    size_t position = directory_size, head = 0;
    for (uint32_t i = 0; i < tables; i++) {
        const uint8_t *entry = data + face + 12 + (size_t)i * 16;
        size_t offset = be32(entry + 8), bytes = be32(entry + 12);
        memcpy(result + 12 + (size_t)i * 16, entry, 8);
        put32(result + 12 + (size_t)i * 16 + 8, (uint32_t)position);
        put32(result + 12 + (size_t)i * 16 + 12, (uint32_t)bytes);
        memcpy(result + position, data + offset, bytes);
        if (!memcmp(entry, "head", 4) && bytes >= 12) head = position;
        position += (bytes + 3) & ~(size_t)3;
    }
    if (head) {
        put32(result + head + 8, 0);
        // OpenType's documented whole-font checksum adjustment, not a decoder.
        put32(result + head + 8, 0xB1B0AFBAu - sfnt_checksum(result, total));
    }
    *output_length = total;
    return result;
}

static int load_one(const char *path, Font *font) {
    if (!path || !plain_extension(path)) return 0;
    size_t length;
    uint8_t *data = read_file(path, &length);
    if (!data) return 0;
    if (!memcmp(data, "ttcf", 4)) {
        size_t extracted_length;
        uint8_t *extracted = ttc_face0(data, length, &extracted_length);
        free(data);
        if (!extracted) {
            LOG("[pl] %s: invalid plain font collection\n", path);
            return 0;
        }
        data = extracted;
        length = extracted_length;
    } else {
        uint32_t tables;
        size_t directory_size;
        if (!sfnt_directory(data, length, 0, 0, &tables, &directory_size)) {
            LOG("[pl] %s: invalid plain TTF/OTF font\n", path);
            free(data);
            return 0;
        }
    }
    void *mapped = hle_alloc(length + 16);
    if (!mapped) { free(data); return 0; }
    memcpy(mapped, data, length);
    free(data);
    font->addr = (uint64_t)(uintptr_t)mapped;
    font->size = length;
    LOG("[pl] system font %s (%zu bytes)\n", path, length);
    return 1;
}

RT_ON_RESET(pl_reset) {
    g_fonts_loaded = 0;
    memset(fonts, 0, sizeof fonts);  // Font buffers belonged to the HLE heap.
}

static void load_fonts(void) {
    if (g_fonts_loaded) return;
    const char *environment = getenv("LARECOMP_FONT");
    const char *standard[] = {
        environment,
        "fonts/nintendo_udsg-r_std_003.ttf", "fonts/nintendo_udsgr_std_003.ttf",
        "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",
        "C:/Windows/Fonts/YuGothR.ttc", "C:/Windows/Fonts/YuGothM.ttc",
        "/System/Library/Fonts/Supplemental/Arial.ttf", "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"
    };
    for (size_t i = 0; i < sizeof standard / sizeof *standard && !fonts[0].addr; i++)
        if (standard[i]) load_one(standard[i], &fonts[0]);
    if (!fonts[0].addr) FATAL("no usable plain TTF/OTF/TTC system font substitute\n");
    load_one("fonts/nintendo_ext_003.ttf", &fonts[1]);
    g_fonts_loaded = 1;
}

// SharedFontType0–4 retain the Standard substitute. Type5 uses the optional
// button font when supplied, otherwise the same real Standard font mapping.
static const Font *font_for(uint64_t type) {
    load_fonts();
    return type == 5 && fonts[1].addr ? &fonts[1] : &fonts[0];
}

HLE_FUNC("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", pl_get_load_state) {
    (void)font_for(c->x[0]);  // Never report loaded before a real mapping exists.
    c->x[0] = 1;
}
HLE_FUNC("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", pl_get_address) {
    c->x[0] = font_for(c->x[0])->addr;
}
HLE_FUNC("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", pl_get_size) {
    c->x[0] = font_for(c->x[0])->size;
}
