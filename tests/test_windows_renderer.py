"""Native portable renderer contracts use only authored bytes and mock resources."""
from pathlib import Path
import subprocess
import tempfile
import os
import re
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parent))
from native_renderer_support import ROOT, PORT, compiler_command, compiler_environment, binary_name, native_flags, graphics_prefix

class RendererNativeContracts(unittest.TestCase):
    def test_windows_fg_input_qualification(self):
        if PORT.name!="Monado_Windows":
            self.skipTest("Native Windows FidelityFX adapter")
        source=(PORT/"runtime/nvn/vulkan/temporal_fg.inc").read_text(encoding="utf8")
        body="\n".join(extract(source,name) for name in ["tmp_frame_gen_wanted","fg_reset","tmp_fg_inputs_ready","fg_dispatch_cb","fg_frame","tmp_fg_idle","tmp_fg_before_present"])
        result=self.graphics_fixture("fg-qualification","test_fg_qualification_windows.c",{"fg_qualification_source.h":body})
        self.assertIn("PASS actual native FG",result)

    def test_windows_fg_presentation_drain_and_context_lifetime(self):
        if PORT.name!="Monado_Windows":self.skipTest("Native Windows FidelityFX adapter")
        source=(PORT/"runtime/nvn/vulkan/temporal_fg.inc").read_text(encoding="utf8")
        body="\n".join(extract(source,name) for name in ["tmp_fg_drain","fg_context"])
        result=self.graphics_fixture("fg-lifecycle","test_fg_lifecycle_windows.c",{"fg_lifecycle_source.h":body})
        self.assertIn("PASS native SDK presentation drain/context ownership",result)

    def test_windows_provider_output_counter(self):
        if PORT.name!="Monado_Windows":
            self.skipTest("Native Windows SDK counter")
        result=self.graphics_fixture("fps-counter","test_fps_provider_counter_windows.c")
        self.assertIn("PASS provider accepted-total",result)

    def test_windows_repeat_policy(self):
        if PORT.name != "Monado_Windows":
            self.skipTest("Native Windows composition policy")
        compiler=compiler_command()
        if not compiler:
            self.skipTest("Clang or local pinned Zig is required")
        with tempfile.TemporaryDirectory(prefix="monado-repeat-") as temporary:
            binary=Path(temporary)/binary_name("repeat-policy")
            args=compiler+native_flags()+["-I",str(PORT/"runtime"),
                str(PORT/"tests/native/test_repeat_present_windows.c"),"-o",str(binary)]
            result=subprocess.run(args,env=compiler_environment(),capture_output=True,text=True,timeout=120)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("policy15cases",result.stdout)

    def test_scaled_gather_translator(self):
        compiler = compiler_command()
        if not compiler:
            self.skipTest("Clang or local pinned Zig is required")
        with tempfile.TemporaryDirectory(prefix="monado-gather-") as temporary:
            binary = Path(temporary) / binary_name("gather")
            args = compiler + native_flags() + ["-I", str(PORT / "runtime"),
                str(ROOT / "tests/native/test_scaled_gather.c"),
                str(PORT / "runtime/nvn/maxwell/translate.c"), "-o", str(binary)]
            result = subprocess.run(args, env=compiler_environment(), capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("contracts: PASS", result.stdout)

    def test_production_sampler_cache(self):
        compiler = compiler_command()
        if not compiler:
            self.skipTest("Clang or local pinned Zig is required")
        source = (PORT / "runtime/nvn/vulkan/textures.c").read_text()
        begin = source.index("// [XCDE] LINUX-103: intern semantic copies,")
        end = source.index("VkSampler vk_default_sampler", begin)
        with tempfile.TemporaryDirectory(prefix="monado-sampler-cache-") as temporary:
            folder = Path(temporary)
            (folder / "sampler_cache_source.h").write_text(source[begin:end])
            binary = folder / binary_name("sampler-cache")
            args = compiler + native_flags() + ["-I", str(PORT / "runtime"), "-I", str(folder),
                str(ROOT / "tests/native/test_sampler_cache.c"), "-o", str(binary)]
            result = subprocess.run(args, env=compiler_environment(), capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("contracts: PASS", result.stdout)

    def graphics_fixture(self, name, source, generated=None, shaderc=False):
        prefix = graphics_prefix()
        if sys.platform != "win32" or not prefix:
            self.skipTest("Checked native Windows graphics dependencies are required")
        compiler = compiler_command()
        if not compiler:
            self.skipTest("Clang or local pinned Zig is required")
        with tempfile.TemporaryDirectory(prefix="monado-renderer-") as temporary:
            folder = Path(temporary)
            for filename, body in (generated or {}).items():
                (folder / filename).write_bytes(body.encode("utf8"))
            if generated and name == "feedback":
                fixture = folder / "feedback.c"
                fixture.write_bytes(source.encode("utf8"))
            else:
                fixture = PORT / "tests/native" / source
            binary = folder / binary_name(name)
            args = compiler + native_flags() + ["-w", "-I", str(PORT / "runtime"), "-I", str(PORT / "third_party"),
                "-I", str(prefix / "include"), "-I", str(folder), str(fixture), "-o", str(binary)]
            if shaderc:
                args += ['-DMONADO_TEMPORAL_SHADERS_SOURCE="' + (PORT / 'runtime/nvn/vulkan/shaders.c').as_posix() + '"',
                         str(prefix / "lib/libshaderc_shared.dll.a"), str(prefix / "lib/libvulkan-1.dll.a")]
            result = subprocess.run(args, env=compiler_environment(), capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            environment = compiler_environment()
            environment["PATH"] = str(prefix / "bin") + os.pathsep + environment.get("PATH", "")
            result = subprocess.run([str(binary)], env=environment, capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout

    def test_feedback_and_zero_sharpening(self):
        source = (PORT / "runtime/nvn/vulkan/exec.c").read_text(encoding="utf8")
        template = (PORT / "tests/native/test_renderer_feedback_windows.c").read_text(encoding="utf8")
        names = ["feedback_load_clear_ready", "feedback_layout", "feedback_sample_layout", "vk_feedback_boundary",
                 "snapshots_invalidate", "feedback_written", "vk_feedback_texture_gone", "shader_texture_object",
                 "is_attachment", "snapshot_image", "view_range", "feedback_overlaps", "feedback_color_mask",
                 "feedback_color_written", "resolve_feedback"]
        declarations = """static TexBackend *pass_feedback[8];static unsigned pass_nfeedback,draw_feedback_mask;static bool pass_feedback_clear_only;
static NvnTexture *feedback_tex[9];static TexBackend *feedback_copy[9];static int nfeedback;
static NvnTexture *snap_tex[8];static bool snap_ok[8];static const char *snap_why[8],*snap_reason="?";
static uint64_t feedback_visibility=1;
typedef struct {NvnTexture *key;TexBackend copy;VkImage source;uint64_t source_identity,visibility,frame;bool read_only_valid;}Snap;
static Snap snaps[16];static int nsnaps;static void feedback_written(NvnTexture *);
"""
        generated = template.replace("// GENERATED_PRODUCTION_FUNCTIONS", declarations + "\n".join(extract(source, n) for n in names))
        generated = generated.replace("// GENERATED_SHARPEN_FUNCTION", extract(source, "fsr1_sharpen"))
        self.assertIn("PASS production feedback", self.graphics_fixture("feedback", generated, {"marker.h": ""}))

    def test_temporal_history_and_world_cameras(self):
        source = (PORT / "runtime/nvn/vulkan/temporal.c").read_text(encoding="utf8")
        header = """#include "nvn/vulkan/temporal.h"
#include "nvn/vulkan/temporal_camera.h"
#include "settings.h"
bool dlss_available(void);void dlss_reset(void);bool diag_deterministic(void);uint64_t renderer_present_count(void);
static bool ffx_load(void){return false;}
static bool fsr4_frame_available(void){return false;}
const char *tmp_fsr4_status(void){return "fixture provider unavailable";}
static void fsr4_reset(void){}
static void fg_reset(void){} // SDK-owned FG boundary is qualified in its separate production fixture
static TmpCamera previous;static uint32_t hist_rw,hist_rh,hist_ow,hist_oh;
static int hist_upscaler,hist_quality,hist_provider;static uint64_t hist_frame,frames,t_last;
static int active_provider=-1,active_upscaler=-1,active_quality=-1;
static bool effective_linear;
static uint32_t jit_index,jit_phases=8;static uint64_t snap_frame=~0ull,game_vel_frame=~0ull,fsr_ring[4];
static unsigned fsr_slot;static TexBackend *game_vel;
"""
        names = ["history_invalidate", "env_int", "tmp_enabled", "halton", "tmp_jitter", "tmp_reset", "tmp_set_velocity",
                 "tmp_forget_texture", "tmp_hud_started", "mat_from_rows", "mat_mul", "mat_inv", "history_prepare", "history_commit"]
        header += "\n".join(extract(source, name) for name in names)
        self.assertIn("36 cases passed", self.graphics_fixture("temporal-history", "test_temporal_history_windows.c", {"temporal_history_source.h": header}))

    def test_actual_temporal_shaderc_and_reflection(self):
        self.assertIn("10 cases passed", self.graphics_fixture("temporal-shaders", "test_temporal_shaders_windows.c", shaderc=True))


def extract(source, name):
    match = re.search(r"^([^\n;]*\b" + re.escape(name) + r"\([^;]*?\)\s*\{)", source, re.M)
    if not match:
        raise AssertionError("Missing production function " + name)
    begin = match.start()
    opening = source.index("{", begin)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end] + "\n"
