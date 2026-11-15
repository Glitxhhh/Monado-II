"""SDK callback failures invalidate an otherwise successful pixel/exit result."""
import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('authored_dlss_oracle',Path(__file__).resolve().parents[1]/'run_windows_dlss_sdk.py')
oracle=importlib.util.module_from_spec(spec);spec.loader.exec_module(oracle)

class CallbackOracle(unittest.TestCase):
    def test_silent_sdk_success_is_not_a_validation_layer_claim(self):
        self.assertEqual(oracle.sdk_diagnostics('[dlss] Streamline loaded\n',mask_format='r16'),
                         {'sdk_errors':0,'undefined_format_errors':0,'cleanup_errors':0})
    def test_real_callback_error_rejects_even_successful_pixel_exit(self):
        text='PASS actual production DLSS SDK: pixels\n[dlss] [streamline][error] vulkan.cpp:3077[isFormatSupported] Cannot have undefined format\n'
        with self.assertRaisesRegex(ValueError,'pixel-only'):oracle.sdk_diagnostics(text,mask_format='r16')
    def test_cleanup_failure_after_pixels_is_fatal(self):
        with self.assertRaisesRegex(ValueError,'cleanup'):
            oracle.sdk_diagnostics('PASS actual production DLSS SDK: pixels\n[dlss] resource reset failed: 25\n',mask_format='r16')
    def test_explicit_negative_requires_exact_four_errors_per_each_of_three_frames(self):
        line='[dlss] [streamline][error] vulkan.cpp:3077[isFormatSupported] Cannot have undefined format\n'
        self.assertEqual(oracle.sdk_diagnostics(line*12,mask_format='r8',expect_r8_errors=True)['undefined_format_errors'],12)
        for text in (line*8,line*16,line*12+'[dlss] [streamline][error] unexpected SDK error\n',line*12+'[dlss] resource reset failed: 25\n'):
            with self.subTest(text=text[-80:]),self.assertRaises(ValueError):oracle.sdk_diagnostics(text,mask_format='r8',expect_r8_errors=True)

if __name__=='__main__':unittest.main()
