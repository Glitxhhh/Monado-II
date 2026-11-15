"""Production Windows optional feedback capability and explicit authored GPU oracle."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_renderer_support import (PORT, compiler_command, compiler_environment,
                                     graphics_prefix, native_flags, binary_name)


@unittest.skipUnless(sys.platform == "win32", "Native Windows Vulkan contract")
class WindowsFeedbackDevice(unittest.TestCase):
    def test_actual_instance_provider_extension_union_and_validation_retry(self):
        compiler, prefix = compiler_command(), graphics_prefix()
        if not compiler or not prefix:
            self.skipTest("Pinned native graphics compiler/dependencies required")
        production = (PORT / "runtime/nvn/vulkan/device.c").read_text(encoding="utf8")
        begin = production.index("static void create_instance(void)")
        end = production.index("\nstatic bool has_ext", begin)
        template = (PORT / "tests/native/test_instance_extensions_windows.c").read_text(encoding="utf8")
        with tempfile.TemporaryDirectory(prefix="monado-instance-provider-") as temporary:
            source = Path(temporary) / "instance.c"
            source.write_text(template.replace("// PRODUCTION_CREATE_INSTANCE", production[begin:end]), encoding="utf8")
            image = Path(temporary) / binary_name("instance")
            result = subprocess.run(compiler + native_flags() + ["-I", str(prefix / "include"), str(source), "-o", str(image)],
                env=compiler_environment(), capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(image)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS production instance complete", result.stdout)

    def test_production_optional_feature(self):
        compiler, prefix = compiler_command(), graphics_prefix()
        if not compiler or not prefix:
            self.skipTest("Pinned native graphics compiler/dependencies required")
        with tempfile.TemporaryDirectory(prefix="monado-feedback-device-") as temporary:
            image = Path(temporary) / binary_name("feature")
            command = compiler + native_flags() + ["-I", str(PORT / "runtime"), "-I", str(prefix / "include"),
                str(PORT / "tests/native/test_feedback_device_windows.c"), "-o", str(image)]
            result = subprocess.run(command, env=compiler_environment(), capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(image)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS native feedback capability", result.stdout)

    def test_actual_provider_surface_initialization_order(self):
        compiler, prefix = compiler_command(), graphics_prefix()
        if not compiler or not prefix:
            self.skipTest("Pinned native graphics compiler/dependencies required")
        production = (PORT / "runtime/nvn/vulkan/device.c").read_text(encoding="utf8")
        begin = production.index("    create_instance();", production.index("void ui_window_init(void)"))
        end = production.index("    create_swapchain();", begin) + len("    create_swapchain();")
        template = (PORT / "tests/native/test_dlss_device_order_windows.c").read_text(encoding="utf8")
        with tempfile.TemporaryDirectory(prefix="monado-provider-device-") as temporary:
            source = Path(temporary) / "order.c"
            source.write_text(template.replace("// PRODUCTION_DEVICE_INITIALIZATION", production[begin:end]), encoding="utf8")
            image = Path(temporary) / binary_name("order")
            result = subprocess.run(compiler + native_flags() + ["-I", str(prefix / "include"), str(source), "-o", str(image)],
                env=compiler_environment(), capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(image)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS production selected-adapter", result.stdout)

    @unittest.skipUnless(os.environ.get("MONADO_TEST_VULKAN_GPU") == "1", "Explicit authored real Vulkan GPU oracle")
    def test_actual_gpu_feedback_pixels(self):
        compiler, prefix = compiler_command(), graphics_prefix()
        if not compiler or not prefix:
            self.skipTest("Pinned native graphics compiler/dependencies required")
        with tempfile.TemporaryDirectory(prefix="monado-feedback-gpu-") as temporary:
            image = Path(temporary) / binary_name("feedback-gpu")
            # Pinned Zig selects its C++ driver explicitly; an external Clang
            # selected by MONADO_TEST_CC accepts -x c++ and links libc++ below.
            driver = compiler[:-1] + ["c++"] if compiler[-1] == "cc" else compiler + ["-x", "c++", "-lstdc++"]
            command = driver + ["-std=gnu++20", "-O1", "-UNDEBUG", "-march=x86_64_v2", "-ffp-contract=off",
                "-fno-fast-math", "-fno-strict-aliasing", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                "-I", str(PORT / "runtime"), "-I", str(prefix / "include"), str(PORT / "tests/native/test_feedback_gpu_windows.cpp"),
                str(prefix / "lib/libshaderc_shared.dll.a"), str(prefix / "lib/libvulkan-1.dll.a"), "-o", str(image)]
            result = subprocess.run(command, env=compiler_environment(), capture_output=True, text=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            environment = compiler_environment()
            environment["PATH"] = str(prefix / "bin") + os.pathsep + environment.get("PATH", "")
            result = subprocess.run([str(image)], env=environment, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS authored Vulkan feedback", result.stdout)
