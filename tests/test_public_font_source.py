"""Native checks for the publication-only plain-font HLE loader.

All font directory/table bytes below are authored synthetic container fixtures,
not Nintendo fonts or full glyph-rendering fixtures. No font downloads, guest,
window, saves, production HLE heap or proprietary decoder is used.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
CANDIDATES = (
    ROOT / 'publication/overrides/runtime/hle/pl.c',
    ROOT / 'Monado_Linux/runtime/hle/pl.c',
    ROOT / 'Monado_Windows/runtime/hle/pl.c',
    ROOT / 'runtime/hle/pl.c',
)
SOURCE = next((path for path in CANDIDATES if path.is_file()), CANDIDATES[0])
STUB = r'''
#pragma once
#include <stdint.h>
#include <stdio.h>
typedef struct { uint64_t x[32]; } Cpu;
void *hle_alloc(size_t);
void fixture_fatal(void);
#define LOG(...) ((void)0)
#define FATAL(...) fixture_fatal()
#define HLE_FUNC(symbol, name) static void name(Cpu *c)
#define RT_ON_RESET(name) static void name(void)
'''
NATIVE = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void *allocations[128];
static size_t allocations_count;
static int allocation_fail;
static unsigned checks;
#define CHECK(condition) do { checks++; if (!(condition)) { fprintf(stderr,"check failed at %d\n",__LINE__); exit(1); } } while (0)
void *hle_alloc(size_t bytes) {
    if (allocation_fail) return NULL;
    void *memory = malloc(bytes);
    assert(memory && allocations_count < 128);
    allocations[allocations_count++] = memory;
    return memory;
}
void fixture_fatal(void) { fputs("no real usable plain font mapping\n", stderr); exit(73); }

// Restrict the test's font discovery to owned fixtures; never open host fonts.
// The production loader's actual file parser/HLE handlers remain unchanged.
static FILE *fixture_fopen(const char *path, const char *mode) {
    const char *base = getenv("FONT_TEST_DIR");
    size_t n = base ? strlen(base) : 0;
    if ((n && !strncmp(path, base, n) && path[n] == '/') || !strncmp(path,"fonts/",6))
        return fopen(path, mode);
    errno = ENOENT;
    return NULL;
}
#define fopen fixture_fopen
#include "runtime/hle/pl.c"
#undef fopen

static void owned_path(char *path, size_t size, const char *name) {
    assert(snprintf(path, size, "%s/%s", getenv("FONT_TEST_DIR"), name) > 0);
}
static void write_fixture(const char *path, const void *data, size_t bytes) {
    FILE *file = fopen(path,"wb"); assert(file);
    assert(fwrite(data,1,bytes,file) == bytes); assert(!fclose(file));
}
static void clear_heap(void) {
    for (size_t i=0;i<allocations_count;i++) free(allocations[i]);
    allocations_count=0;
    pl_reset();
}
static void sfnt(uint8_t *data, const char *tag) {
    memset(data,0,40);
    if (tag) memcpy(data,tag,4); else put32(data,0x00010000u);
    data[5]=1; data[7]=16;
    memcpy(data+12,"head",4);
    put32(data+16,0x02030405u);
    put32(data+20,28);
    put32(data+24,12);
    for (int i=0;i<12;i++) data[28+i]=(uint8_t)(i+1);
}
static void collection(uint8_t *data) {
    memset(data,0,56);
    memcpy(data,"ttcf",4); put32(data+4,0x00010000u);
    put32(data+8,1); put32(data+12,16);
    sfnt(data+16,NULL);
    put32(data+16+20,44);  // Collection tables use absolute original offsets.
}
int main(int argc, char **argv) {
    assert(getenv("FONT_TEST_DIR"));
    if (argc > 1 && !strcmp(argv[1],"--missing")) {
        unsetenv("LARECOMP_FONT");
        Cpu cpu={0}; pl_get_load_state(&cpu);
        return 99;  // Reporting loaded without a real mapping must be impossible.
    }
    char path[2048], other[2048]; uint8_t original[40], damaged[64], ttc[56];
    owned_path(path,sizeof path,"authored.ttf"); sfnt(original,NULL);
    write_fixture(path,original,sizeof original);
    Font font={0}; CHECK(load_one(path,&font));
    CHECK(font.addr && font.size==40 && !memcmp((void *)(uintptr_t)font.addr,original,40));
    CHECK(allocations_count==1);
    owned_path(other,sizeof other,"authored.OTF"); sfnt(damaged,"OTTO"); write_fixture(other,damaged,40);
    Font otf={0}; CHECK(load_one(other,&otf)); CHECK(otf.size==40);
    sfnt(damaged,"true"); owned_path(other,sizeof other,"legacy.ttf"); write_fixture(other,damaged,40);
    Font legacy={0}; CHECK(load_one(other,&legacy));

    // Failures cannot alter an existing destination or map fabricated font data.
    const Font sentinel={123,456}; Font rejected=sentinel;
    owned_path(other,sizeof other,"not-a-font.ttf"); memset(damaged,0x52,sizeof damaged);
    write_fixture(other,damaged,sizeof damaged); CHECK(!load_one(other,&rejected));
    CHECK(rejected.addr==sentinel.addr && rejected.size==sentinel.size);
    owned_path(other,sizeof other,"wrong-extension.txt"); write_fixture(other,original,40);
    CHECK(!load_one(other,&rejected));
    owned_path(other,sizeof other,"empty.ttf"); write_fixture(other,original,0);
    CHECK(!load_one(other,&rejected));
    owned_path(other,sizeof other,"truncated.ttf"); write_fixture(other,original,20);
    CHECK(!load_one(other,&rejected));
    owned_path(other,sizeof other,"missing.ttf"); CHECK(!load_one(other,&rejected));
    CHECK(!load_one(NULL,&rejected));
    uint32_t table_count; size_t directory;
    memcpy(damaged,original,40); damaged[5]=0;
    CHECK(!sfnt_directory(damaged,40,0,0,&table_count,&directory));
    memcpy(damaged,original,40); put32(damaged+20,0xfffffffcu);
    CHECK(!sfnt_directory(damaged,40,0,0,&table_count,&directory));
    memcpy(damaged,original,40); put32(damaged+24,0xfffffffcu);
    CHECK(!sfnt_directory(damaged,40,0,0,&table_count,&directory));
    memcpy(damaged,original,40); put32(damaged+20,12);
    CHECK(!sfnt_directory(damaged,40,0,0,&table_count,&directory));
    memcpy(damaged,original,40); put32(damaged+20,29); put32(damaged+24,11);
    CHECK(!sfnt_directory(damaged,40,0,0,&table_count,&directory));
    memcpy(damaged,original,40); put32(damaged+24,0);
    CHECK(!sfnt_directory(damaged,40,0,0,&table_count,&directory));
    size_t before=allocations_count; allocation_fail=1;
    CHECK(!load_one(path,&rejected)); CHECK(allocations_count==before);
    allocation_fail=0;
    owned_path(other,sizeof other,"oversize.ttf");
    FILE *large=fopen(other,"wb"); assert(large);
    assert(!ftruncate(fileno(large),(off_t)FONT_MAX_BYTES+1)); assert(!fclose(large));
    CHECK(!load_one(other,&rejected)); assert(!unlink(other));

    // Real TTC extraction rewrites absolute offsets and whole-font adjustment.
    collection(ttc); uint8_t saved[56]; memcpy(saved,ttc,56);
    size_t extracted_size=0; uint8_t *extracted=ttc_face0(ttc,56,&extracted_size);
    CHECK(extracted && extracted_size==40);
    CHECK(!memcmp(ttc,saved,56)); CHECK(be32(extracted+20)==28 && be32(extracted+24)==12);
    CHECK(!memcmp(extracted+28,ttc+44,8)); CHECK(sfnt_checksum(extracted,40)==0xB1B0AFBAu);
    free(extracted);
    owned_path(other,sizeof other,"authored.ttc"); write_fixture(other,ttc,56);
    Font collected={0}; CHECK(load_one(other,&collected)); CHECK(collected.size==40);
    CHECK(sfnt_checksum((const uint8_t *)(uintptr_t)collected.addr,40)==0xB1B0AFBAu);
    CHECK(!ttc_face0(ttc,15,&extracted_size));
    for (unsigned i=0;i<5;i++) {
        memcpy(damaged,ttc,56);
        if (i==0) put32(damaged+4,0x00030000u);
        if (i==1) put32(damaged+8,0);
        if (i==2) put32(damaged+8,0xffffffffu);
        if (i==3) put32(damaged+12,0xfffffffcu);
        if (i==4) put32(damaged+12,0);
        CHECK(!ttc_face0(damaged,56,&extracted_size));
    }
    memcpy(damaged,ttc,56); put32(damaged+36,0xfffffffcu);
    CHECK(!ttc_face0(damaged,56,&extracted_size));
    memcpy(damaged,ttc,56); put32(damaged+40,0xfffffffcu);
    CHECK(!ttc_face0(damaged,56,&extracted_size));

    // Actual HLE handlers/reset use real retained maps and preserve type fallback.
    clear_heap(); assert(!setenv("LARECOMP_FONT",path,1));
    Cpu cpu={0}; pl_get_load_state(&cpu); CHECK(cpu.x[0]==1 && fonts[0].addr && g_fonts_loaded);
    cpu.x[0]=0; pl_get_address(&cpu); CHECK(cpu.x[0]==fonts[0].addr);
    cpu.x[0]=4; pl_get_size(&cpu); CHECK(cpu.x[0]==40);
    CHECK(font_for(5)==&fonts[0]);
    clear_heap(); CHECK(!g_fonts_loaded && !fonts[0].addr && !fonts[1].addr);
    assert(!mkdir("fonts",0700));
    write_fixture("fonts/nintendo_ext_003.ttf",original,40);
    cpu.x[0]=5; pl_get_load_state(&cpu); CHECK(cpu.x[0]==1 && fonts[1].addr);
    CHECK(font_for(5)==&fonts[1] && font_for(0)==&fonts[0] && font_for(4)==&fonts[0]);
    cpu.x[0]=5; pl_get_address(&cpu); CHECK(cpu.x[0]==fonts[1].addr);
    cpu.x[0]=5; pl_get_size(&cpu); CHECK(cpu.x[0]==40);
    clear_heap();
    printf("authored plain-font container/HLE checks: %u passed\n",checks);
    return 0;
}
'''


class PublicFontSource(unittest.TestCase):
    def test_only_plain_loader_is_exported(self):
        source = SOURCE.read_text()
        self.assertIn('publication-only font loader', source)
        self.assertIn('SPDX-License-Identifier: MIT', source)
        self.assertIn('Copyright (c) 2026 Peiikai (LASRecomp)', source)
        self.assertNotIn('bfttf', source.lower())
        self.assertIn('(void)font_for(c->x[0])', source)
        self.assertEqual(source.count('HLE_FUNC('), 3)

    @unittest.skipUnless(shutil.which('cc') and os.name == 'posix', 'native POSIX C compiler required')
    def test_native_container_bounds_mapping_and_missing_font(self):
        self.assertIn('publication-only font loader', SOURCE.read_text())
        self.assertNotIn('bfttf', SOURCE.read_text().lower())
        with tempfile.TemporaryDirectory(prefix='monado-public-font-') as temporary:
            work = Path(temporary)
            source = work / 'runtime/hle/pl.c'
            source.parent.mkdir(parents=True)
            shutil.copyfile(SOURCE, source)
            (work / 'runtime/hle.h').write_text(STUB)
            fixture = work / 'fixture.c'
            fixture.write_text(NATIVE)
            binary = work / 'font-fixture'
            command = [shutil.which('cc'), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                       str(fixture), '-o', str(binary)]
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            good = work / 'good'
            missing = work / 'missing'
            good.mkdir()
            missing.mkdir()
            environment = dict(os.environ, FONT_TEST_DIR=str(good))
            result = subprocess.run([str(binary)], cwd=good, env=environment,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('plain-font container/HLE checks:', result.stdout)
            absent = subprocess.run([str(binary), '--missing'], cwd=missing,
                                    env=dict(environment, FONT_TEST_DIR=str(missing)),
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(absent.returncode, 73, absent.stdout + absent.stderr)
            self.assertIn('no real usable plain font mapping', absent.stderr)


if __name__ == '__main__':
    unittest.main()
