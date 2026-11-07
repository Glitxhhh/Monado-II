// T127 (test first): LDC.64 c[idx][Ra + imm] into Rd, Rd+1 when Ra == Rd. The second word must come from the address
// the instruction started with, not from the first word just loaded into Ra (exploded meshes in another LAS-based
// port, Let's Go recomp #21). Programs are synthesized from field encoders (no game shaders).
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

// LDC: 0xef90 | size << 48 (4 = 32 bit, 5 = 64 bit) | cbuf index << 36 | offset << 20 | Ra << 8 | Rd
static uint64_t ldc(int d, int a, int idx, int off, int size) {
    return 0xef90000000000000ull | ((uint64_t)size << 48) | ((uint64_t)idx << 36) | ((uint64_t)off << 20) | ((uint64_t)a << 8) | (uint64_t)d;
}

static ShaderInfo *translate(uint64_t instr) {
    uint8_t blob[0x80 + 8 * 16];
    memset(blob, 0, sizeof blob);
    uint64_t *code = (uint64_t *)(blob + 0x80);
    code[0] = 0; code[1] = instr; code[2] = EXIT_W; code[3] = NOP_W;
    ShaderKey key; memset(&key, 0, sizeof key);
    key.glsl = 1;
    return maxwell_translate(blob, sizeof blob, STAGE_FRAGMENT, &key);
}

// the statement that assigns register r (the last one in the program)
static const char *assign_of(const char *src, int r, char *out, size_t n) {
    char pat[16];
    snprintf(pat, sizeof pat, " r%d = ", r);
    const char *p = NULL, *q = src;
    while ((q = strstr(q, pat))) p = q++;
    if (!p) return NULL;
    const char *e = strchr(p, ';');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return out;
}

int main(void) {
    ShaderInfo *s = translate(ldc(4, 4, 3, 0x10, 5));  // LDC.64 R4, c[3][R4 + 0x10]
    CHECK(s && s->msl);
    if (s && s->msl) {
        char a[512];
        CHECK(assign_of(s->msl, 5, a, sizeof a) != NULL);
        // the second word's address must not use r4 (overwritten by the first word)
        if (assign_of(s->msl, 5, a, sizeof a)) {
            const char *rhs = strchr(a, '=');
            CHECK(rhs && !strstr(rhs, "r4"));
            if (rhs && strstr(rhs, "r4")) printf("  r5 statement: %s\n", a);
        }
    } else if (s) printf("  error: %s\n", s->error);
    if (s) maxwell_free(s);

    ShaderInfo *t = translate(ldc(4, 6, 3, 0x10, 5));  // Ra != Rd: unchanged behaviour, both words from r6
    CHECK(t && t->msl);
    if (t && t->msl) {
        char a[512];
        CHECK(assign_of(t->msl, 5, a, sizeof a) && strstr(strchr(a, '='), "r6"));
    }
    if (t) maxwell_free(t);

    // T176: the immediate is signed when added to Ra: c[6][R59 - 0x80] (encoded 0xff80) reads 128 bytes below R59
    ShaderInfo *u = translate(ldc(4, 59, 6, 0xff80, 4));
    CHECK(u && u->msl);
    if (u && u->msl) {
        char a[512];
        CHECK(assign_of(u->msl, 4, a, sizeof a) && strstr(a, "- 128u") && !strstr(a, "65408"));
        if (assign_of(u->msl, 4, a, sizeof a) && !strstr(a, "- 128u")) printf("  r4 statement: %s\n", a);
    }
    if (u) maxwell_free(u);

    printf(fails ? "test_translate_ldc: %d failures\n" : "test_translate_ldc: OK\n", fails);
    return fails != 0;
}
