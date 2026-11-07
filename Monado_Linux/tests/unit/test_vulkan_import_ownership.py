"""[XCDE] LINUX-005: guard renderer ownership without executing game code.

The guest submit-side fence scan and renderer executor walk similar CmdRec
loops. Heap import retirement must never enter the fence scan: in asynchronous
mode it runs concurrently with Vulkan recording. This source-level guard covers
that integration constraint; native host-import tests cover Vulkan behavior.
"""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "runtime/nvn/vulkan/exec.c"


def code_only(source):
    # Preserve offsets while excluding comments and literal contents from matches.
    literals = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(literals, lambda match: " " * len(match.group()), source, flags=re.S)


def function_span(code, name):
    declaration = re.search(r"\b" + name + r"\s*\([^;{}]*\)\s*\{", code)
    if declaration is None:
        raise AssertionError(f"missing function definition: {name}")
    start = declaration.end()
    depth = 1
    for pos in range(start, len(code)):
        if code[pos] == "{":
            depth += 1
        elif code[pos] == "}":
            depth -= 1
            if depth == 0:
                return declaration.start(), start, pos
    raise AssertionError(f"unterminated function: {name}")


def check_import_owner(source):
    code = code_only(source)
    owner_definition, owner_start, owner_end = function_span(code, "exec_list")
    helper_definition, _, _ = function_span(code, "gmem_prepare_command")
    calls = [match.start() for match in re.finditer(r"\bgmem_prepare_command\s*\(", code)
             if match.start() != helper_definition]
    if len(calls) != 1 or not owner_start <= calls[0] < owner_end:
        raise AssertionError("heap import retirement must occur only in exec_list, never guest-side scan_fences")
    record = code.index("CmdRec", owner_start, owner_end)
    if calls[0] > record:
        raise AssertionError("heap import retirement must precede per-command state")
    return code, (owner_definition, owner_start, owner_end)


class VulkanImportOwnership(unittest.TestCase):
    def test_retirement_stays_on_executor_command_boundary(self):
        check_import_owner(SOURCE.read_text())

    def test_guest_fence_scan_regression_is_detected(self):
        source = SOURCE.read_text()
        _, body, _ = function_span(code_only(source), "scan_fences")
        # Reproduce the reviewed bug: the same call accidentally added to both
        # command walkers. No Vulkan driver calls or game code are run here.
        broken = source[:body] + "\n    gmem_prepare_command();\n" + source[body:]
        with self.assertRaisesRegex(AssertionError, "never guest-side scan_fences"):
            check_import_owner(broken)

    def test_retirement_waits_before_release_and_reimport(self):
        code, _ = check_import_owner(SOURCE.read_text())
        _, start, end = function_span(code, "gmem_prepare_command")
        body = code[start:end]
        self.assertLess(body.index("renderer_flush("), body.index("vk_host_import_destroy("))
        self.assertLess(body.index("vk_host_import_destroy("), body.index("gmem_import("))
        # Descriptor callers can already hold a VkCommandBuffer: a flush here
        # would make that handle's recording generation stale.
        _, start, end = function_span(code, "gmem_info")
        self.assertNotRegex(code[start:end], r"\b(?:renderer_flush|vk_host_import_destroy)\s*\(")

    def test_nested_lists_do_not_retain_vulkan_recording_locals(self):
        code, (_, start, end) = check_import_owner(SOURCE.read_text())
        body = code[start:end]
        prefix = body[:body.index("switch (r->op)")]
        call_branch = body[body.index("case CMD_CALL:"):body.index("case CMD_BIND_PROGRAM:")]
        self.assertRegex(call_branch, r"\bexec_list\s*\(")
        for region in (prefix, call_branch):
            self.assertNotRegex(region, r"\b(?:VkCommandBuffer|VkDescriptorSet|vk_cmd|vk_alloc_set|stage_set)\b")


if __name__ == "__main__":
    unittest.main()
