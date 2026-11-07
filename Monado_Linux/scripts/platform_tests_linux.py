#!/usr/bin/env python3
"""[XCDE] LINUX-003: reproducible Linux platform/fiber component tests, never a game run.

Uses the project-local Clang wrapper and graphics sysroot. Every invocation gets
fresh ignored artifacts; commands, exact environments, hashes and results are
retained in receipt.json. No downloads, package installation or game input.
"""
import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import signal
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parent.parent
GROUPS = ("context", "fibers", "platform", "condvar", "kernel", "watchdog", "guard", "boot", "fatal-notice")
SYNTHETIC_ROOT = 'synthetic input with spaces, "quotes" and unicode é'


class TestFailure(Exception):
    pass


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def sources():
    """Follow local includes, including fixtures that include kernel.c directly."""
    names = ["scripts/platform_tests_linux.py", "graphics-deps-linux.lock.json",
             "runtime/host/main_boot.c", "runtime/diag/watchdog.c", "runtime/platform/linux.c",
             "runtime/paths.c", "runtime/platform/linux_state_guard.c",
             "runtime/diag/diag.c", "runtime/diag/sampler.c", "runtime/hle/fibers.c",
             "runtime/hle/fiber_context_linux.c"]
    names += ["tests/native/" + name for name in (
        "test_fiber_context_linux.c", "test_fibers.c", "test_platform_linux.c",
        "test_condvar.c", "test_kernel_fibers_linux.c", "test_watchdog_linux.c",
        "test_boot_linux_support.c", "test_state_guard_linux.c", "test_fatal_notice_linux.c")]
    pending = [ROOT / name for name in names]
    found = {}
    while pending:
        path = pending.pop().resolve()
        name = str(path.relative_to(ROOT))
        if name in found:
            continue
        found[name] = digest(path)
        if path.suffix in (".c", ".h"):
            for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(), re.M):
                for target in (path.parent / include, ROOT / "runtime" / include):
                    if target.is_file():
                        pending.append(target)
                        break
                else:
                    raise TestFailure(f"unresolved local include {include!r} in {name}")
    return dict(sorted(found.items()))


def owned_children():
    """Only descendants of this runner, including orphans adopted as subreaper."""
    pending = [os.getpid()]
    found = set()
    while pending:
        pid = pending.pop()
        try:
            children = Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
        except FileNotFoundError:
            continue
        for child in map(int, children):
            if child not in found:
                found.add(child)
                pending.append(child)
    return found


def kill_owned():
    children = owned_children()
    groups = set()
    for pid in children:
        try:
            group = os.getpgid(pid)
            # Never signal a group outside our owned descendant set.
            if group in children:
                groups.add(group)
        except ProcessLookupError:
            pass
    for group in groups:
        try:
            os.killpg(group, signal.SIGKILL)
        except ProcessLookupError:
            pass
    for pid in children:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    return sorted(children)


def reap_owned():
    reaped = []
    while True:
        try:
            pid, status = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            return reaped
        if not pid:
            return reaped
        reaped.append({"pid": pid, "wait_status": status})


class Runner:
    def __init__(self, output, compiler, sysroot, selected):
        self.output, self.compiler, self.sysroot = output, compiler, sysroot
        self.build = output / "build"
        self.build.mkdir()
        for name in ("tmp", "data", "config", "cache"):
            (output / name).mkdir()
        # Do not inherit user runtime controls, preload libraries or credentials.
        self.env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8", "TZ": "UTC",
                    "TMPDIR": str(output / "tmp"), "XDG_DATA_HOME": str(output / "data"),
                    "XDG_CONFIG_HOME": str(output / "config"), "XDG_CACHE_HOME": str(output / "cache"),
                    "LARECOMP_DATA_ROOT": str(output / "data"), "XCDE_STALL": "0"}
        # Zig spells named CPU baselines differently from GCC/direct Clang.
        baseline = "x86_64_v2" if Path(self.compiler).resolve() == (ROOT / ".deps/bin/clang-cc").resolve() else "x86-64-v2"
        self.flags = ["-O1", "-UNDEBUG", "-march=" + baseline, "-ffp-contract=off", "-fno-strict-aliasing", "-pthread", "-Iruntime"]
        libdir = sysroot / "usr/lib/x86_64-linux-gnu"
        self.sdl = libdir / "libSDL3.so"
        self.sdl_flags = ["-I" + str(sysroot / "usr/include")]
        self.sdl_link = [str(self.sdl), "-Wl,-rpath," + str(libdir), "-ldl"]
        self.receipt = {"schema": 1, "scope": "synthetic Linux components only; no game input or gameplay",
                        "started_utc": datetime.now(timezone.utc).isoformat(), "status": "running",
                        "source_root": str(ROOT), "output": str(output), "groups": selected,
                        "host": {"uname": list(platform.uname()), "libc": platform.libc_ver(),
                                 "python": sys.version, "runner_argv": sys.argv},
                        "sources_before": sources(), "commands": [], "artifacts": {},
                        "dependencies": {"compiler": str(compiler), "compiler_sha256": digest(compiler),
                                         "sysroot": str(sysroot), "sdl3": str(self.sdl.resolve()),
                                         "sdl3_sha256": digest(self.sdl)},
                        "core_limit": [0, 0], "child_subreaper": True}
        self.save()

    def save(self):
        target = self.output / "receipt.json"
        temporary = self.output / "receipt.json.tmp"
        temporary.write_text(json.dumps(self.receipt, indent=2) + "\n")
        temporary.replace(target)

    def run(self, name, argv, *, cwd=None, env=None, expected=0, contains=None,
            timeout=60, attempts=None, artifact=None):
        if cwd is None:
            cwd = self.output / name
            cwd.mkdir()
        command_env = self.env | (env or {})
        argv = list(map(str, argv))
        stdout, stderr = self.output / (name + ".stdout.log"), self.output / (name + ".stderr.log")
        record = {"name": name, "argv": argv, "cwd": str(cwd), "env": command_env,
                  "timeout_seconds": timeout, "expected_exit": expected,
                  "capture": "child pipes; parent persists output after execution",
                  "expected_text": contains or {}, "stdout": str(stdout), "stderr": str(stderr)}
        self.receipt["commands"].append(record)
        self.save()
        print(f"{name}: running", flush=True)
        started = time.monotonic()
        reason = None
        # Writable regular-file stdio outside state would legitimately fail the
        # inherited-FD audit. Pipes preserve the real guard; only this parent
        # persists the logs. communicate drains both streams without deadlock.
        process = subprocess.Popen(argv, cwd=cwd, env=command_env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, stdin=subprocess.DEVNULL,
                                   start_new_session=True)
        record["pid"] = process.pid
        try:
            out, err = process.communicate(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
            reason = "timeout" if isinstance(error, subprocess.TimeoutExpired) else "interrupted"
            record["killed_owned_pids"] = kill_owned()
            out, err = process.communicate(timeout=5)
        stdout.write_bytes(out)
        stderr.write_bytes(err)
        # Tests create separate process groups. Subreaping keeps even those
        # descendants owned and removable if a failing test exits prematurely.
        leftover = owned_children()
        if leftover:
            record["leftover_owned_pids"] = sorted(leftover)
            record.setdefault("killed_owned_pids", []).extend(kill_owned())
            for _ in range(100):
                record.setdefault("reaped_descendants", []).extend(reap_owned())
                if not owned_children():
                    break
                kill_owned()
                time.sleep(0.01)
            reason = reason or "test left descendant processes"
        record.update(exit=process.returncode, seconds=time.monotonic() - started)
        record["stdout_sha256"] = digest(stdout)
        record["stderr_sha256"] = digest(stderr)
        errors = []
        if reason:
            errors.append(reason)
        if process.returncode != expected:
            errors.append(f"exit {process.returncode}, expected {expected}")
        for stream, snippet in (contains or {}).items():
            path = stdout if stream == "stdout" else stderr
            if snippet not in path.read_text(errors="replace"):
                errors.append(f"missing {stream} text {snippet!r}")
        if attempts is not None:
            log = Path(command_env["TEST_BOOT_LOG"])
            actual = []
            if log.is_file():
                for line in log.read_text().splitlines():
                    match = re.fullmatch(r"attempt (-?\d+) pid (\d+)", line)
                    if not match:
                        errors.append(f"malformed retry evidence: {line!r}")
                    else:
                        actual.append([int(match[1]), int(match[2])])
            record["boot_attempts"] = actual
            record["expected_attempts"] = attempts
            if actual != [[attempt, process.pid] for attempt in attempts]:
                errors.append("reservation attempts or same-PID re-exec evidence differ")
        if artifact and not errors:
            self.receipt["artifacts"][str(artifact.relative_to(self.output))] = digest(artifact)
        record["passed"] = not errors
        record["errors"] = errors
        self.save()
        print(f"{name}: {'FAIL' if errors else 'PASS'} ({record['seconds']:.2f}s)", flush=True)
        if errors:
            raise TestFailure(f"{name}: {'; '.join(errors)}; see {stderr}")

    def compile(self, name, inputs, *, sdl=False, extra=(), obj=False):
        target = self.build / (name + (".o" if obj else ""))
        argv = [self.compiler, *self.flags, *(self.sdl_flags if sdl else []), *extra,
                *inputs, *(["-c"] if obj else []), "-o", target]
        if not obj:
            argv += [*(self.sdl_link if sdl else []), "-lm"]
        self.run("compile-" + name, argv, cwd=ROOT, timeout=120, artifact=target)
        return target

    def finish(self, error=None):
        self.receipt["sources_after"] = sources()
        changed = self.receipt["sources_before"] != self.receipt["sources_after"]
        self.receipt["sources_changed_during_run"] = changed
        if changed:
            error = (error + "; " if error else "") + "source files changed during run; rerun from stable source"
        self.receipt["finished_utc"] = datetime.now(timezone.utc).isoformat()
        self.receipt["status"] = "failed" if error else "passed"
        self.receipt["error"] = error
        self.save()
        return error


def suite(runner, selected):
    r = runner
    r.run("compiler-version", [r.compiler, "--version"], cwd=ROOT, timeout=30)
    fibers = ["runtime/hle/fibers.c", "runtime/hle/fiber_context_linux.c"]
    platform_sources = ["runtime/platform/linux.c", "runtime/paths.c"]
    kernel = [*fibers, *platform_sources, "runtime/diag/diag.c", "runtime/diag/sampler.c"]
    if "context" in selected:
        exe = r.compile("test_context", ["tests/native/test_fiber_context_linux.c", fibers[1]])
        r.run("context", [exe])
    if "fibers" in selected:
        exe = r.compile("test_fibers", ["tests/native/test_fibers.c", *fibers])
        r.run("fibers", [exe])
    if "platform" in selected:
        exe = r.compile("test_platform", ["tests/native/test_platform_linux.c", *platform_sources],
                        sdl=True, extra=["-Wall", "-Wextra", "-Werror"])
        r.run("platform", [exe], contains={"stdout": "platform Linux: OK (0 failures)"})
    if "fatal-notice" in selected:
        # [XCDE] LINUX-007: compile the real function; only SDL's modal call is
        # mocked. No SDL library/display is used by the retained section.
        exe = r.compile("test_fatal_notice", ["tests/native/test_fatal_notice_linux.c"],
                        extra=[*r.sdl_flags, "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-ldl"])
        for value, calls in (("unset", "1"), ("0", "1"), ("1", "0"),
                             ("01", "1"), ("true", "1"), ("", "1")):
            name = "fatal-notice-" + (value or "empty")
            log = r.output / name / "fatal.log"
            r.run(name, [exe, value, log, calls], expected=37, artifact=log)
            if log.read_text() != "synthetic fatal details remain in the log\n":
                raise TestFailure(name + ": fatal log was not preserved")
    if "condvar" in selected:
        # This inherited test includes all kernel.c but calls only wait/mutex
        # helpers. Discard unused SVC/renderer entry points rather than inventing
        # a renderer implementation to satisfy their unrelated references.
        exe = r.compile("test_condvar", ["tests/native/test_condvar.c", *kernel], sdl=True,
                        extra=["-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"])
        r.run("condvar", [exe], env={"XCDE_DETERMINISTIC": "0"}, contains={"stdout": "OK"})
    if "kernel" in selected:
        exe = r.compile("test_kernel_fibers", ["tests/native/test_kernel_fibers_linux.c", *kernel], sdl=True)
        for value in ("0", "1"):
            r.run("kernel-" + value, [exe], env={"XCDE_DETERMINISTIC": value}, contains={"stdout": "OK"})
    if "watchdog" in selected:
        # Compile the unchanged production interface as well as the counting adapter fixture.
        r.compile("watchdog-production", ["runtime/diag/watchdog.c"], obj=True)
        obj = r.compile("watchdog-fixture", ["runtime/diag/watchdog.c"], obj=True,
                        extra=["-Dpthread_create=test_pthread_create"])
        exe = r.compile("test_watchdog", ["tests/native/test_watchdog_linux.c", obj])
        r.run("watchdog-disabled", [exe, "disabled"], contains={"stdout": "watchdog disabled: OK"})
        r.run("watchdog-reports", [exe], contains={"stdout": "watchdog loading/progress/stall reports: OK"})
        for index, value in enumerate(("-1", "bad", "2x", "4294967296")):
            r.run(f"watchdog-invalid-{index}", [exe, "invalid"], env={"XCDE_STALL": value},
                  expected=-signal.SIGABRT, contains={"stderr": "XCDE_STALL must specify an interval"})
    if "guard" in selected:
        exe = r.compile("test_state_guard", ["tests/native/test_state_guard_linux.c",
                                             "runtime/platform/linux_state_guard.c"],
                        extra=["-Wall", "-Wextra", "-Werror"])
        for mode in ("baseline", "normal", "inherited"):
            path = r.output / ("guard-" + mode)
            path.mkdir()
            state = path / "state"; state.mkdir()
            input_file = path / "synthetic-input"; input_file.write_bytes(b"sentinel")
            r.run("guard-" + mode, [exe, mode, state, input_file], cwd=path,
                  contains={"stdout": "negative control: unguarded input writable" if mode == "baseline"
                            else "state guard: contents/create/truncate/remove/rename/link, threads and exec OK"})
            if input_file.read_bytes() != b"sentinel" or (path / "synthetic-input-new").exists():
                raise TestFailure("state-guard fixture modified synthetic input")
    if "boot" in selected:
        r.compile("main-boot-production", ["runtime/host/main_boot.c"], sdl=True, obj=True, extra=["-DXCDE_GFX"])
        obj = r.compile("main-boot-fixture", ["runtime/host/main_boot.c"], sdl=True, obj=True,
                        extra=["-DXCDE_GFX", "-Dplat_reserve_guest=test_plat_reserve_guest",
                               "-Dplat_init=test_plat_init", "-Dplat_exec=test_plat_exec"])
        exe = r.compile("test_boot", ["tests/native/test_boot_linux_support.c", *platform_sources,
                                      "runtime/platform/linux_state_guard.c", obj],
                        sdl=True, extra=["-DXCDE_GFX"])

        def boot(name, args=None, failures="0", expected=37, attempts=None, env=None, contains=None,
                 external_save=False, real_logging=False):
            name = "boot-" + name
            path = r.output / name
            path.mkdir()
            input_dir = path / SYNTHETIC_ROOT
            (input_dir / "exefs").mkdir(parents=True)
            (input_dir / "romfs").mkdir()
            original = input_dir / "romfs/original"; original.write_bytes(b"sentinel")
            state = path / "state"
            fixture_env = {"TEST_BOOT_LOG": str(path / "attempts.log"), "TEST_RESERVE_FAILURES": failures,
                           "TEST_BOOT_SENTINEL": "environment preserved",
                           "TEST_BOOT_EXPECTED_INPUT": str(input_dir),
                           "TEST_BOOT_EXPECTED_STATE": str(state)} | (env or {})
            if external_save:
                fixture_env["LARECOMP_SAVE_DIR"] = str(input_dir / "romfs")
            if real_logging:
                fixture_env["TEST_REAL_PLAT_INIT"] = "1"
            if args is None:
                args = ["--state-root", "state", "--precompile", SYNTHETIC_ROOT]
            r.run(name, [exe, *args], cwd=path, env=fixture_env, expected=expected,
                  attempts=attempts if attempts is not None else [0], contains=contains)
            if original.read_bytes() != b"sentinel":
                raise TestFailure(name + ": synthetic input was changed")
            generated = state / "cache/synthetic-precompile"
            if expected == 37:
                if generated.read_bytes() != b"synthetic cache\n":
                    raise TestFailure(name + ": isolated synthetic precompile output missing")
            elif generated.exists():
                raise TestFailure(name + ": callback ran despite rejected startup")
            if real_logging:
                logfile = state / "logs/monado.log"
                if "synthetic precompile paths/guard verified; argv/environment preserved" not in logfile.read_text():
                    raise TestFailure(name + ": real platform logging did not reach isolated log")
                unexpected = [p for p in path.rglob("*.log") if p not in (logfile, path / "attempts.log")]
                if unexpected:
                    raise TestFailure(name + ": unexpected log paths " + str(unexpected))

        success = {"stdout": "synthetic precompile paths/guard verified; argv/environment preserved"}
        boot("direct", contains=success)
        boot("retry", failures="2", attempts=[0, 1, 2], contains=success)
        boot("environment-state", args=["--precompile", SYNTHETIC_ROOT],
             env={"MONADO_STATE_ROOT": "state"}, contains=success)
        boot("real-platform-log", real_logging=True, contains={"stderr": "state write guard active"})
        boot("limit", failures="99", expected=1, attempts=list(range(9)),
             contains={"stderr": "FATAL: guest address range"})
        for index, (value, attempt) in enumerate((("bad", 0), ("-1", -1), ("9", 9))):
            boot(f"invalid-retry-{index}", failures="99", expected=1, attempts=[attempt],
                 env={"XCDE_RELAUNCH": value}, contains={"stderr": "FATAL: invalid XCDE_RELAUNCH retry count"})
        boot("exec-failure", failures="1", expected=1, env={"TEST_EXEC_FAILURE": "1"},
             contains={"stderr": "cannot re-execute after reservation failure"})
        boot("usage", args=[], expected=2, contains={"stderr": "usage: xcde_boot"})
        boot("missing-state", args=["--precompile", SYNTHETIC_ROOT], expected=1,
             contains={"stderr": "an explicit nonempty state root is required"})
        boot("missing-state-argument", args=["--state-root"], expected=2,
             contains={"stderr": "--state-root requires a directory"})
        boot("overlap", args=["--state-root", SYNTHETIC_ROOT, "--precompile", SYNTHETIC_ROOT], expected=1,
             contains={"stderr": "input and state roots must be disjoint"})
        boot("external-save", external_save=True, expected=1,
             contains={"stderr": "LARECOMP_SAVE_DIR must be a child of the isolated state root"})
        for index, value in enumerate(("-1", "bad", "2x", "4294967296", "0")):
            boot(f"invalid-watchdog-{index}", args=["--state-root", "state", SYNTHETIC_ROOT], expected=-signal.SIGABRT,
                 env={"TEST_BOOT_WATCHDOG": "1", "XCDE_WATCHDOG": value},
                 contains={"stderr": "XCDE_WATCHDOG must specify a positive interval"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="absent directory under this port's runs/ or build/")
    parser.add_argument("--cc", type=Path, default=ROOT / ".deps/bin/clang-cc", help="one compiler executable")
    parser.add_argument("--sysroot", type=Path, help="defaults to .deps/graphics/active-sysroot.txt")
    parser.add_argument("--only", nargs="+", choices=GROUPS, help="run only selected focused groups")
    args = parser.parse_args()
    if sys.platform != "linux" or platform.machine() != "x86_64":
        parser.error("these focused tests require native Linux x86-64")
    os.umask(0o077)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0):  # PR_SET_CHILD_SUBREAPER, for owned test descendants.
        parser.error(f"cannot become child subreaper: errno {ctypes.get_errno()}")
    compiler = args.cc.resolve()
    if not compiler.is_file() or not os.access(compiler, os.X_OK):
        parser.error(f"compiler unavailable: {compiler}")
    sysroot = (args.sysroot or Path((ROOT / ".deps/graphics/active-sysroot.txt").read_text().strip())).resolve()
    if not (sysroot / "usr/include/SDL3/SDL.h").is_file() or not (sysroot / "usr/lib/x86_64-linux-gnu/libSDL3.so").is_file():
        parser.error("SDL3 headers/library missing; set up the locked local graphics dependencies first")
    if args.output:
        output = args.output.absolute()
        if output.is_symlink() or output.resolve() != output or not any(
                output.is_relative_to(ROOT / name) for name in ("runs", "build")):
            parser.error("--output must be a new non-symlink directory under Monado_Linux/runs or build")
        if output.exists():
            parser.error("--output already exists; choose a fresh directory to preserve its evidence")
        output.mkdir(parents=True, exist_ok=False)
    else:
        if (ROOT / "runs").is_symlink():
            parser.error("the Linux runs directory must not be a symlink")
        (ROOT / "runs").mkdir(exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="linux-platform.", dir=ROOT / "runs"))
    print(f"Focused Linux component artifacts: {output}", flush=True)
    runner = Runner(output, compiler, sysroot, args.only or list(GROUPS))
    error = None
    try:
        suite(runner, args.only or GROUPS)
    except (TestFailure, OSError, subprocess.SubprocessError, KeyboardInterrupt) as failure:
        error = str(failure) or type(failure).__name__
    error = runner.finish(error)
    print(("FAIL: " + error if error else "PASS: focused Linux components; gameplay not covered") +
          f"\nReceipt: {output / 'receipt.json'}", flush=True)
    return 1 if error else 0


if __name__ == "__main__":
    sys.exit(main())
