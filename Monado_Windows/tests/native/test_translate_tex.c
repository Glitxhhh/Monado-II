// T052 (tests first): Maxwell texture-gather instructions in the GLSL translator. Programs are synthesized from field
// encoders (no game shaders): NVN header 0x30 + SPH 0x50 + code (groups of 4: scheduling word + 3 instructions).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/nvn/maxwell/shader.h"

ShaderInfo *maxwell_translate(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key);
void maxwell_free(ShaderInfo *s);

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

#define EXIT_W 0xe30000000007000full
#define NOP_W 0x50b0000000070f00ull

// TLD4S: gpr0 dest, gpr8 x, gpr20 y, gpr28 second dest, texture index, flags
static uint64_t tld4s(int d0, int x, int y, int d1, int tex, int comp, int fp16, int dc, int aoffi, int nodep) {
    return 0xdf00000000000000ull | ((uint64_t)tex << 36) | ((uint64_t)d1 << 28) | ((uint64_t)y << 20) | (7ull << 16) |
           ((uint64_t)x << 8) | (uint64_t)d0 | ((uint64_t)nodep << 49) | ((uint64_t)dc << 50) | ((uint64_t)aoffi << 51) |
           ((uint64_t)comp << 52) | ((uint64_t)fp16 << 55);
}


// TLD4 (full gather form): 0xc838 | component << 56 | offset mode << 54 (0 none, 1 AOFFI, 2 PTP) | dc << 50
static uint64_t tld4(int d, int x, int p, int tex, int comp, int offmode, int dc, int target, int mask) {
    return 0xc838000000000000ull | ((uint64_t)comp << 56) | ((uint64_t)offmode << 54) | ((uint64_t)dc << 50) |
           ((uint64_t)tex << 36) | ((uint64_t)mask << 31) | ((uint64_t)target << 29) | ((uint64_t)p << 20) | (7ull << 16) |
           ((uint64_t)x << 8) | (uint64_t)d;
}


// Surface ops (SULD 0xeb0x / SUST 0xeb2x): bit 51 always set, bit 52 = data mode (D: size in bits 20..22: 0 8b, 1 16b,
// 2 32b, 3 64b, 4 128b; P: component mask in bits 20..23), image type bits 33..35 (3 = 2D), bound (bit 28 = 0),
// handle index bits 36..48, coordinates gpr8, data gpr0.
static uint64_t surf(uint64_t base, int d, int x, int tex, int type, int dmode, int field) {
    return base | (8ull << 48) | ((uint64_t)dmode << 52) | ((uint64_t)tex << 36) | ((uint64_t)type << 33) |
           ((uint64_t)field << 20) | (7ull << 16) | ((uint64_t)x << 8) | (uint64_t)d;
}
#define SULD_(d, x, tex, type, dmode, field) surf(0xeb00000000000000ull, d, x, tex, type, dmode, field)
#define SUST_(d, x, tex, type, dmode, field) surf(0xeb20000000000000ull, d, x, tex, type, dmode, field)

static ShaderInfo *translate(uint64_t instr) {
    uint8_t blob[0x80 + 8 * 16];
    memset(blob, 0, sizeof blob);
    uint64_t *code = (uint64_t *)(blob + 0x80);
    code[0] = 0; code[1] = instr; code[2] = EXIT_W; code[3] = NOP_W;
    ShaderKey key; memset(&key, 0, sizeof key);
    key.glsl = 1;
    return maxwell_translate(blob, sizeof blob, STAGE_FRAGMENT, &key);
}

static void test_gather_components(void) {
    for (int comp = 0; comp < 4; comp++) {
        ShaderInfo *s = translate(tld4s(0, 8, 9, 4, 3, comp, 0, 0, 0, 0));
        CHECK(s && s->msl);
        if (!s || !s->msl) { if (s) printf("  error: %s\n", s->error); if (s) maxwell_free(s); continue; }
        char want[64];
        snprintf(want, sizeof want, ", %d)", comp);
        CHECK(strstr(s->msl, "textureGather(t") != NULL);
        CHECK(strstr(s->msl, want) != NULL);
        CHECK(s->ntex == 1);
        CHECK(s->error[0] == 0);
        // second destination holds components 2 and 3: r4 / r5
        CHECK(strstr(s->msl, "r4 = ") != NULL && strstr(s->msl, "r5 = ") != NULL);
        CHECK(strstr(s->msl, "r0 = ") != NULL && strstr(s->msl, "r1 = ") != NULL);
        maxwell_free(s);
    }
}

static void test_fp16_pack(void) {
    ShaderInfo *s = translate(tld4s(0, 8, 9, 4, 3, 1, 1, 0, 0, 0));
    CHECK(s && s->msl);
    if (s && s->msl) {
        CHECK(strstr(s->msl, "packHalf2x16") != NULL);
        CHECK(strstr(s->msl, "r5 = ") == NULL);  // packed pairs: d0 and d1 only
    }
    if (s) maxwell_free(s);
}

static void test_nodep_flag_is_ignored(void) {
    ShaderInfo *a = translate(tld4s(0, 8, 9, 4, 3, 0, 0, 0, 0, 0));
    ShaderInfo *b = translate(tld4s(0, 8, 9, 4, 3, 0, 0, 0, 0, 1));
    CHECK(a && b && a->msl && b->msl);
    if (a && b && a->msl && b->msl) CHECK(strcmp(a->msl, b->msl) == 0);
    if (a) maxwell_free(a);
    if (b) maxwell_free(b);
}

static void test_unsupported_variants_fail_cleanly(void) {
    ShaderInfo *s = translate(tld4s(0, 8, 9, 4, 3, 0, 0, 1, 0, 0));  // depth compare: not implemented yet
    CHECK(s != NULL);
    if (s) { CHECK(s->msl == NULL); CHECK(strstr(s->error, "tld4s") != NULL); maxwell_free(s); }
}

static void test_txq_still_decodes(void) {  // TXQ is 0xdf48...: it must not be taken for TLD4S
    ShaderInfo *s = translate(0xdf48000000070f00ull | (3ull << 36) | (1ull << 22));
    CHECK(s != NULL);
    if (s) { CHECK(strstr(s->error, "tld4s") == NULL); maxwell_free(s); }
}


static void test_tld4_components(void) {
    for (int comp = 0; comp < 4; comp++) {
        ShaderInfo *s = translate(tld4(0, 34, 8, 22, comp, 0, 0, 1, 15));
        CHECK(s && s->msl);
        if (!s || !s->msl) { if (s) { printf("  error: %s\n", s->error); maxwell_free(s); } continue; }
        char want[32];
        snprintf(want, sizeof want, ", %d)", comp);
        CHECK(strstr(s->msl, "textureGather(t") != NULL);
        CHECK(strstr(s->msl, want) != NULL);
        CHECK(strstr(s->msl, "r0 = ") && strstr(s->msl, "r1 = ") && strstr(s->msl, "r2 = ") && strstr(s->msl, "r3 = "));
        maxwell_free(s);
    }
}

static void test_tld4_mask_selects_components(void) {
    ShaderInfo *s = translate(tld4(4, 34, 8, 22, 0, 0, 0, 1, 5));  // mask 0b0101: components 0 and 2 -> r4, r5
    CHECK(s && s->msl);
    if (s && s->msl) CHECK(strstr(s->msl, "r4 = ") && strstr(s->msl, "r5 = ") && !strstr(s->msl, "r6 = "));
    if (s) maxwell_free(s);
}

static void test_tld4_offsets_and_ptp(void) {
    ShaderInfo *a = translate(tld4(0, 34, 8, 22, 0, 1, 0, 1, 15));  // AOFFI: offset in gpr20
    CHECK(a && a->msl);
    if (a && a->msl) CHECK(strstr(a->msl, "<< 28") != NULL);
    if (a) maxwell_free(a);
    ShaderInfo *b = translate(tld4(0, 34, 8, 22, 0, 2, 0, 1, 15));  // PTP: translated without the per-texel offsets
    CHECK(b && b->msl);
    if (b && !b->msl) printf("  error: %s\n", b->error);
    if (b) maxwell_free(b);
}

static void test_tld4_depth_compare(void) {
    ShaderInfo *s = translate(tld4(0, 34, 8, 22, 0, 0, 1, 1, 15));
    CHECK(s && s->msl);
    if (s && s->msl) { CHECK(strstr(s->msl, "textureGather(t") != NULL); CHECK(s->tex[0].shadow); }
    if (s) maxwell_free(s);
}

static void test_tld4_rejects_3d_and_tex_is_untouched(void) {
    ShaderInfo *s = translate(tld4(0, 34, 8, 22, 0, 0, 0, 2, 15));
    CHECK(s != NULL);
    if (s) { CHECK(s->msl == NULL); CHECK(strstr(s->error, "tld4") != NULL); maxwell_free(s); }
    // a plain TEX (0xc038, 2D) must still be a texture() lookup
    uint64_t tex = 0xc038000000000000ull | (22ull << 36) | (15ull << 31) | (1ull << 29) | (7ull << 16) | (34ull << 8);
    ShaderInfo *t = translate(tex);
    CHECK(t && t->msl);
    if (t && t->msl) CHECK(strstr(t->msl, "textureGather") == NULL);
    if (t) maxwell_free(t);
}


static void test_suld_loads_through_a_storage_image(void) {
    static const struct { int size, nregs; } cases[] = {{4, 1}, {5, 2}, {6, 4}};  // D sizes as ssbu-recomp: 4 R32UI, 5 RG32UI, 6 RGBA32UI
    for (int i = 0; i < 3; i++) {
        ShaderInfo *s = translate(SULD_(0, 4, 258, 3, 1, cases[i].size));
        CHECK(s && s->msl);
        if (!s || !s->msl) { if (s) { printf("  error: %s\n", s->error); maxwell_free(s); } continue; }
        CHECK(strstr(s->msl, "imageLoad(i") != NULL);
        CHECK(strstr(s->msl, "texelFetch") == NULL);
        CHECK(strstr(s->msl, "uniform uimage2D i") != NULL);
        CHECK(s->ntex == 1 && s->tex[0].is_image);
        char r[16];
        for (int k = 0; k < 4; k++) {
            snprintf(r, sizeof r, "r%d = ", k);
            if (k < cases[i].nregs) CHECK(strstr(s->msl, r) != NULL); else CHECK(strstr(s->msl, r) == NULL);
        }
        maxwell_free(s);
    }
}

static void test_suld_p_mode_uses_the_component_mask(void) {
    ShaderInfo *s = translate(SULD_(4, 8, 258, 3, 0, 5));  // mask 0b0101: components x and z -> r4, r5
    CHECK(s && s->msl);
    if (s && s->msl) CHECK(strstr(s->msl, "r4 = ") && strstr(s->msl, "r5 = ") && !strstr(s->msl, "r6 = "));
    if (s) maxwell_free(s);
}

static void test_sust_stores_through_a_storage_image(void) {
    ShaderInfo *s = translate(SUST_(0, 4, 258, 3, 0, 15));  // P mode, all four components from r0..r3, coords r4/r5
    CHECK(s && s->msl);
    if (!s || !s->msl) { if (s) { printf("  error: %s\n", s->error); maxwell_free(s); } return; }
    CHECK(strstr(s->msl, "imageStore(i") != NULL);
    CHECK(strstr(s->msl, "ivec2(int(r4), int(r5))") != NULL);
    CHECK(strstr(s->msl, "uvec4(r0, r1, r2, r3)") != NULL);
    CHECK(strstr(s->msl, "uniform uimage2D i") != NULL && !strstr(s->msl, "readonly"));
    CHECK(strstr(s->msl, "sust dropped") == NULL);
    CHECK(s->tex[0].is_image);
    maxwell_free(s);
    ShaderInfo *t = translate(SUST_(2, 4, 258, 3, 1, 5));  // D mode 64-bit (size 5): two registers
    CHECK(t && t->msl);
    if (t && t->msl) CHECK(strstr(t->msl, "uvec4(r2, r3, 0u, 0u)") != NULL);
    if (t) maxwell_free(t);
}

static void test_surface_ops_fail_cleanly_when_unsupported(void) {
    ShaderInfo *s = translate(SULD_(0, 4, 258, 5, 1, 4));  // 3D image
    CHECK(s != NULL);
    if (s) { CHECK(s->msl == NULL); CHECK(strstr(s->error, "suld") != NULL); maxwell_free(s); }
    ShaderInfo *t = translate(SULD_(0, 4, 258, 3, 1, 0));  // 8-bit raw loads
    CHECK(t != NULL);
    if (t) { CHECK(t->msl == NULL); CHECK(strstr(t->error, "suld") != NULL); maxwell_free(t); }
    ShaderInfo *u = translate(SUST_(0, 4, 258, 5, 1, 4));
    CHECK(u != NULL);
    if (u) { CHECK(u->msl == NULL); CHECK(strstr(u->error, "sust") != NULL); maxwell_free(u); }
}

int main(void) {
    test_gather_components();
    test_fp16_pack();
    test_nodep_flag_is_ignored();
    test_unsupported_variants_fail_cleanly();
    test_txq_still_decodes();
    test_tld4_components();
    test_tld4_mask_selects_components();
    test_tld4_offsets_and_ptp();
    test_tld4_depth_compare();
    test_tld4_rejects_3d_and_tex_is_untouched();
    test_suld_loads_through_a_storage_image();
    test_suld_p_mode_uses_the_component_mask();
    test_sust_stores_through_a_storage_image();
    test_surface_ops_fail_cleanly_when_unsupported();
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
