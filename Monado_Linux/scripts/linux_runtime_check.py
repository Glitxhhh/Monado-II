#!/usr/bin/env python3
"""[XCDE] LINUX-042: checked Linux dependency loading, never game execution.

The system ELF loader is invoked only with --list. It does not run constructors,
application entry points, game code, renderers or vendor initialization. A pass
establishes current loadability and the declared dependency boundary, not gameplay
or GPU support. Inherited loader variables never influence these checks.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HOST_PATH = '/usr/sbin:/sbin:/usr/bin:/bin'
MAX_OUTPUT = 128 * 1024
_GNU_VERSIONS = frozenset(('GLIBCXX_3.4.32', 'CXXABI_1.3.9'))
_graph_module = None


class RuntimeDependencyError(ValueError):
    pass


def graph_module():
    global _graph_module
    if _graph_module is None:
        path = Path(__file__).with_name('linux_runtime_graph.py')
        spec = importlib.util.spec_from_file_location('_monado_linux_runtime_graph', path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        _graph_module = module
    return _graph_module


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def clean_environment(library_path=None):
    result = {'PATH': HOST_PATH, 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'}
    if library_path is not None:
        result['LD_LIBRARY_PATH'] = str(library_path)
    return result


def native_file(path, *, host=False):
    path = Path(path).absolute()
    if '..' in path.parts:
        raise RuntimeDependencyError('runtime path contains parent traversal')
    target = path.resolve(strict=True)
    if not host and target != path:
        raise RuntimeDependencyError('runtime artifact must not be a symlink: ' + str(path))
    before = target.stat()
    if not stat.S_ISREG(before.st_mode) or not os.access(target, os.R_OK):
        raise RuntimeDependencyError('runtime artifact must be a readable ordinary file: ' + str(target))
    with target.open('rb') as stream:
        header = stream.read(64)
    if (len(header) < 64 or header[:6] != b'\x7fELF\x02\x01'
            or int.from_bytes(header[18:20], 'little') != 62):
        raise RuntimeDependencyError('runtime artifact is not little-endian Linux x86-64 ELF: ' + str(target))
    return target


def _tool(name):
    path = shutil.which(name, path=HOST_PATH)
    if not path:
        raise RuntimeDependencyError('Missing host runtime inspection tool: ' + name)
    target = Path(path).resolve(strict=True)
    if not target.is_file() or not os.access(target, os.X_OK):
        raise RuntimeDependencyError('Invalid host runtime inspection tool: ' + name)
    return str(target)


def _run(argv, *, runner=None, timeout=30, max_output=MAX_OUTPUT):
    result = (runner or subprocess.run)(list(map(str, argv)),
        env=clean_environment(), capture_output=True, text=True, timeout=timeout,
        stdin=subprocess.DEVNULL)
    if len(result.stdout or '') > max_output or len(result.stderr or '') > max_output:
        raise RuntimeDependencyError('runtime inspection output exceeds its bound')
    return result


def host_library_paths(*, names=(), runner=None):
    """Use the trusted host cache, not user environment or directory searches."""
    result = _run([_tool('ldconfig'), '-p'], runner=runner, max_output=8 * 1024 * 1024)
    if result.returncode:
        raise RuntimeDependencyError('Cannot inspect the host dynamic-library cache: ' + result.stderr[-2000:])
    paths = {}
    for line in result.stdout.splitlines():
        match = re.fullmatch(r'\s*([^\s/]+)\s+\(([^)]*)\)\s+=>\s+(/[^\n]+)', line)
        if not match or 'x86-64' not in match[2]:
            continue
        # Preserve loader-cache precedence. A later alternate provider must not
        # silently hide an incompatible library that the real loader selects.
        paths.setdefault(match[1], Path(match[3]))
    for name in names:
        if name in paths or not re.fullmatch(r'[A-Za-z0-9_+.-]+\.so(?:\.[A-Za-z0-9_+.-]+)*', name):
            continue
        for directory in ('/usr/lib/x86_64-linux-gnu', '/lib/x86_64-linux-gnu',
                          '/usr/lib64', '/lib64', '/usr/lib', '/lib'):
            candidate = Path(directory) / name
            if candidate.is_file():
                try:
                    paths[name] = native_file(candidate, host=True)
                    break
                except (OSError, ValueError):
                    pass
    return paths


def trusted_loader(interpreter=None):
    candidates = ([Path(interpreter)] if interpreter else []) + [
        Path('/lib64/ld-linux-x86-64.so.2'),
        Path('/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2'),
        Path('/usr/lib/ld-linux-x86-64.so.2'),
        Path('/usr/lib64/ld-linux-x86-64.so.2'),
        Path('/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2')]
    host_roots = tuple(Path(p) for p in ('/lib', '/lib64', '/usr/lib', '/usr/lib64'))
    for path in candidates:
        if not path.is_absolute() or path.name != 'ld-linux-x86-64.so.2':
            continue
        try:
            target = path.resolve(strict=True)
            if not any(target.is_relative_to(root.resolve()) for root in host_roots if root.exists()):
                continue
            native_file(target, host=True)
            if os.access(target, os.X_OK):
                return target
        except (OSError, ValueError):
            continue
    raise RuntimeDependencyError('A native glibc x86-64 system ELF loader is required')


def discover_gnu_cxx(*, required_versions=_GNU_VERSIONS, host_paths=None):
    paths = host_library_paths(names=('libstdc++.so.6',)) if host_paths is None else host_paths
    selected = paths.get('libstdc++.so.6')
    if selected is None:
        raise RuntimeDependencyError('A compatible native GNU C++ runtime (libstdc++.so.6) is required; '
                                     'install your distribution\'s runtime package')
    try:
        selected = native_file(selected, host=True)
        info = graph_module().inspect_elf(selected)
    except (OSError, ValueError, RuntimeError) as error:
        raise RuntimeDependencyError('Invalid native GNU C++ runtime: ' + str(error)) from error
    if info.get('soname') != 'libstdc++.so.6':
        raise RuntimeDependencyError('GNU C++ runtime must provide SONAME libstdc++.so.6')
    missing = set(required_versions) - set(info.get('defined_versions', ()))
    if missing:
        raise RuntimeDependencyError('GNU C++ runtime is missing required ABI versions: ' + ', '.join(sorted(missing)))
    return selected


def library_directories(sysroot, policy=None):
    sysroot = Path(sysroot).resolve(strict=True)
    names = (policy or {}).get('library_dirs', ['usr/lib/x86_64-linux-gnu'])
    if not isinstance(names, list) or not names:
        raise RuntimeDependencyError('Runtime policy must declare its private library directories')
    directories = []
    for name in names:
        if not isinstance(name, str) or not name or Path(name).is_absolute() or '..' in Path(name).parts:
            raise RuntimeDependencyError('Invalid private runtime library directory')
        path = sysroot / name
        if ':' in str(path) or any(ord(char) < 32 for char in str(path)):
            raise RuntimeDependencyError('Linux runtime library paths cannot contain colons or control characters')
        if path.resolve() != path or not path.is_dir():
            raise RuntimeDependencyError('Private runtime library directory is missing or linked: ' + str(path))
        if path not in directories:
            directories.append(path)
    return directories


def library_path(sysroot, policy=None):
    return ':'.join(map(str, library_directories(sysroot, policy)))


def policy_digest(policy):
    return hashlib.sha256(json.dumps(policy, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def load_policy(port, sysroot, *, expected_lock=None, expected_policy=None):
    port, sysroot = Path(port).resolve(strict=True), Path(sysroot).resolve(strict=True)
    if not sysroot.is_relative_to(port / '.deps/graphics/sysroots'):
        raise RuntimeDependencyError('Runtime sysroot must belong to its build port')
    marker = json.loads((sysroot / '.monado-deps.json').read_text())
    lock_tag = marker.get('lock_sha256')
    if expected_lock is not None and lock_tag != expected_lock:
        raise RuntimeDependencyError('Runtime sysroot differs from the selected build receipt')
    if expected_lock is None:
        # Setup/probe checks today's source. Preserved artifacts instead use the
        # immutable policy recorded in their old sysroot and bound build receipt.
        lock_path = port / 'graphics-deps-linux.lock.json'
        lock = json.loads(lock_path.read_text())
        if digest(lock_path) != lock_tag:
            raise RuntimeDependencyError('Runtime sysroot does not match the dependency lock')
        policy = lock.get('runtime')
    else:
        policy = marker.get('runtime_policy')
    marked = marker.get('runtime_policy_sha256')
    if policy is None:
        if marked is not None or expected_policy is not None:
            raise RuntimeDependencyError('Receipted runtime policy is missing')
        return None, lock_tag
    if not isinstance(policy, dict) or policy.get('schema') != 1:
        raise RuntimeDependencyError('Unsupported runtime dependency policy')
    actual_policy = policy_digest(policy)
    if marked != actual_policy or expected_policy is not None and expected_policy != actual_policy:
        raise RuntimeDependencyError('Runtime dependency policy differs from its successful receipt')
    return policy, lock_tag



def require_receipt_validation(receipt, marker):
    proof = receipt.get('runtime_validation') or {}
    if marker.get('runtime_policy_sha256') is not None:
        if (not isinstance(proof, dict) or proof.get('schema') != 1 or proof.get('status') != 'passed'
                or proof.get('policy') != 'closed-owned-with-explicit-host-boundaries'
                or any(not isinstance(proof.get(key), str) or not re.fullmatch(r'[0-9a-f]{64}', proof[key])
                       for key in ('runtime_policy_sha256', 'owned_runtime_files_sha256'))):
            raise RuntimeDependencyError('Current runtime build receipt lacks its successful policy/file validation')
    return proof


def _describe(errors):
    parts = []
    for error in errors[:24]:
        if isinstance(error, dict):
            parts.append(json.dumps(error, sort_keys=True))
        else:
            parts.append(str(error))
    if len(errors) > 24:
        parts.append(str(len(errors) - 24) + ' further dependency errors')
    return '\n'.join(parts)


def check_artifacts(artifacts, sysroot, policy=None, *, host_paths=None, runner=None, expected_files=None):
    """Validate current owned/host ABI boundaries and list every real ELF root."""
    roots = list(dict.fromkeys(native_file(path) for path in artifacts))
    if not roots:
        raise RuntimeDependencyError('No runtime artifacts selected for validation')
    sysroot = Path(sysroot).resolve(strict=True)
    selected_path = library_path(sysroot, policy)
    graph = graph_module()
    audit = None
    if policy is not None:
        discovered = host_library_paths(names=policy.get('host_sonames', {}), runner=runner) if host_paths is None else host_paths
        explicit = {name: Path(discovered[name]).resolve(strict=True)
                    for name in policy.get('host_sonames', {}) if name in discovered}
        for name, target in explicit.items():
            if target.is_relative_to(sysroot) or any(target.is_relative_to(root.parent) for root in roots):
                raise RuntimeDependencyError('Native host interface cannot come from private runtime/output: ' + name)
        audit = graph.audit_closure(roots, sysroot, policy, explicit)
        if not audit.get('passed'):
            raise RuntimeDependencyError('Linux runtime dependency/ABI validation failed:\n' +
                _describe(audit.get('errors', ['unknown dependency graph failure'])))
    manifest_hash = None
    manifest = {}
    if policy is not None:
        marker = json.loads((sysroot / '.monado-deps.json').read_text())
        manifest = marker.get('owned_runtime_files')
        if not isinstance(manifest, dict) or set(manifest) != set(policy.get('owned_sonames', {})):
            raise RuntimeDependencyError('Private runtime file inventory differs from its declared policy')
        manifest_hash = policy_digest(manifest)
        if expected_files is not None and expected_files != manifest_hash:
            raise RuntimeDependencyError('Private runtime file inventory differs from the selected build receipt')
    listings, resolved = [], {}
    for root in roots:
        info = graph.inspect_elf(root)
        loader = trusted_loader(info.get('interpreter'))
        command = [str(loader), '--library-path', selected_path, '--list', str(root)]
        result = _run(command, runner=runner)
        output = (result.stdout or '') + (result.stderr or '')
        if result.returncode or 'not found' in output or 'no version information available' in output:
            raise RuntimeDependencyError('Linux runtime loader check failed for ' + root.name + ':\n' +
                output[-8000:] + '\nRebuild from current Monado II source to prepare compatible private dependencies. '
                'This check did not start the game or change saves.')
        for line in output.splitlines():
            match = re.fullmatch(r'\s*(\S+)\s+=>\s+(.+?)\s+\(0x[0-9a-fA-F]+\)\s*', line)
            if match:
                name, target = match[1], Path(match[2]).resolve(strict=True)
                if name in resolved and resolved[name] != target:
                    raise RuntimeDependencyError('Native artifacts resolve inconsistent providers for ' + name)
                resolved[name] = target
        listings.append({'path': str(root), 'sha256': digest(root), 'argv': command,
                         'exit': result.returncode, 'output_sha256': hashlib.sha256(output.encode()).hexdigest(),
                         'resolved_libraries': len(re.findall(r'=> /', output))})
    if policy is not None:
        # The cache is a discovery aid. Re-audit the providers that the real
        # loader actually selected, including CPU hwcap/cache precedence.
        for name, target in resolved.items():
            if name in policy['owned_sonames']:
                expected = (sysroot / policy['owned_sonames'][name]['path']).resolve(strict=True)
                if target != expected:
                    raise RuntimeDependencyError('Loader bypassed the private owned provider for ' + name)
            elif name in policy['host_sonames']:
                if target.is_relative_to(sysroot) or any(target.is_relative_to(root.parent) for root in roots):
                    raise RuntimeDependencyError('Loader replaced a native host interface with private runtime/output: ' + name)
                explicit[name] = target
        actual_roots = [*roots, *[path for name, path in resolved.items() if name in policy['owned_sonames']]]
        audit = graph.audit_closure(actual_roots, sysroot, policy, explicit)
        if not audit.get('passed'):
            raise RuntimeDependencyError('Actual loaded provider ABI validation failed:\n' + _describe(audit.get('errors', [])))
        objects = {item['path']: item for item in audit.get('objects', ())}
        for name, provider in policy['owned_sonames'].items():
            expected = manifest.get(name, {})
            selected = (sysroot / provider['path']).resolve(strict=True)
            if expected.get('path') != provider['path']:
                raise RuntimeDependencyError('Private runtime inventory path changed: ' + name)
            item = objects.get(str(selected))
            if item is not None and (item['sha256'] != expected.get('sha256') or item['bytes'] != expected.get('bytes')):
                raise RuntimeDependencyError('Private runtime file changed after setup: ' + name)
    return {'schema': 1, 'status': 'passed', 'policy': 'closed-owned-with-explicit-host-boundaries' if policy else 'legacy-loader-list-only',
            'library_path': selected_path, 'artifacts': listings,
            'inspected_objects': len((audit or {}).get('objects', ())),
            'owned_objects': sum(Path(item['path']).is_relative_to(sysroot) for item in (audit or {}).get('objects', ())),
            'runtime_policy_sha256': policy_digest(policy) if policy else None,
            'owned_runtime_files_sha256': manifest_hash,
            'scope': 'Static declared dependency/version graph and trusted system loader --list; no constructors, game, UI or GPU initialization',
            'limitations': 'Host driver/session implementations and dlopen plugins are outside the pinned owned graph; no gameplay or GPU qualification'}


def check_receipted(binary, port):
    binary, port = native_file(binary), Path(port).resolve(strict=True)
    receipt = json.loads((binary.parent / 'receipt.json').read_text())
    if receipt.get('status') != 'passed' or receipt.get('executable', {}).get('sha256') != digest(binary):
        raise RuntimeDependencyError('Runtime executable does not match a successful build receipt')
    if receipt.get('executable', {}).get('path') != str(binary):
        raise RuntimeDependencyError('Runtime executable was relocated from its build receipt')
    sysroot = Path(receipt.get('dependencies', {}).get('sysroot', ''))
    policy, _ = load_policy(port, sysroot,
        expected_lock=receipt.get('sources_before', {}).get('graphics-deps-linux.lock.json'),
        expected_policy=receipt.get('runtime_validation', {}).get('runtime_policy_sha256'))
    proof = require_receipt_validation(receipt, json.loads((sysroot / '.monado-deps.json').read_text()))
    companions = []
    for name, expected in receipt.get('runtime_resources', {}).items():
        if not isinstance(name, str) or Path(name).name != name or name in ('.', '..'):
            raise RuntimeDependencyError('Invalid receipted runtime companion name')
        path = binary.parent / name
        if (path.resolve() != path or not path.is_file() or expected.get('path') != str(path)
                or expected.get('bytes') != path.stat().st_size or expected.get('sha256') != digest(path)):
            raise RuntimeDependencyError('Runtime companion differs from its build receipt: ' + name)
        if '.so' in name:
            companions.append(path)
    return check_artifacts([binary, *companions], sysroot, policy,
        expected_files=proof.get('owned_runtime_files_sha256'))


def probe(port=ROOT, *, output=None):
    """Compile an authored no-as-needed root probe, then only list its loading."""
    port = Path(port).resolve(strict=True)
    sysroot = Path((port / '.deps/graphics/active-sysroot.txt').read_text().strip())
    policy, lock_sha = load_policy(port, sysroot)
    if policy is None:
        raise RuntimeDependencyError('The dependency preflight requires a current explicit runtime policy')
    names = policy.get('root_sonames')
    if not isinstance(names, list) or not names:
        raise RuntimeDependencyError('Runtime dependency roots are missing')
    directories = library_directories(sysroot, policy)
    owned = policy.get('owned_sonames', {})
    libraries = []
    for name in names:
        metadata = owned.get(name)
        if not isinstance(metadata, dict) or not isinstance(metadata.get('path'), str):
            raise RuntimeDependencyError('Runtime root is not owned: ' + str(name))
        path = sysroot / metadata['path']
        target = path.resolve(strict=True)
        if not target.is_relative_to(sysroot):
            raise RuntimeDependencyError('Runtime root escapes its sysroot')
        libraries.append(target)
    temporary = None
    if output is None:
        temporary = tempfile.TemporaryDirectory(prefix='runtime-probe-', dir=port / '.deps/graphics')
        output = Path(temporary.name)
    else:
        output = Path(output).absolute()
        if output.resolve() != output or not output.is_relative_to(port):
            raise RuntimeDependencyError('Runtime probe output must be a new ordinary build-port child')
        output.mkdir(parents=True, exist_ok=False)
    try:
        source, binary = output / 'runtime-probe.c', output / 'runtime-probe'
        source.write_text('/* Authored load-only dependency probe; never executed. */\nint main(void) { return 0; }\n')
        compiler = port / '.deps/bin/clang-cc'
        if compiler.resolve() != compiler or not compiler.is_file() or not os.access(compiler, os.X_OK):
            raise RuntimeDependencyError('Missing ordinary port-local C compiler wrapper')
        command = [str(compiler), '-O0', '-UNDEBUG', '-march=x86_64_v2', str(source), '-Wl,--no-as-needed',
                   *map(str, libraries), '-Wl,--as-needed', *['-L' + str(path) for path in directories],
                   '-Xlinker', '-rpath', '-Xlinker', library_path(sysroot, policy), '-o', str(binary)]
        result = _run(command, timeout=120)
        if result.returncode:
            raise RuntimeDependencyError('Authored runtime dependency probe could not link:\n' + (result.stderr or '')[-8000:])
        checked = check_artifacts([binary], sysroot, policy)
        checked.update(probe_source_sha256=digest(source), dependency_lock_sha256=lock_sha,
                       compiler=str(compiler), compiler_sha256=digest(compiler), compile_argv=command)
        return checked
    finally:
        if temporary is not None:
            temporary.cleanup()


def dependency_plan(port=ROOT):
    path = Path(port) / 'graphics-deps-linux.lock.json'
    if not path.is_file():
        return {'lock': str(path), 'status': 'missing source lock'}
    lock = json.loads(path.read_text())
    policy = lock.get('runtime') or {}
    return {'lock': str(path), 'lock_sha256': digest(path),
            'policy_sha256': policy_digest(policy) if policy else None,
            'owned_sonames': sorted(policy.get('owned_sonames', {})),
            'host_interfaces': policy.get('host_sonames', {}),
            'scope': 'Read-only declared source policy, not a host/runtime compatibility result'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', action='store_true', required=True)
    parser.add_argument('--port', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args(argv)
    try:
        print(json.dumps(probe(args.port, output=args.output), indent=2))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
