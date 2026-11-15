#!/usr/bin/env python3
"""Closed private runtime graph using source-built GNU 2.39 media/SDL libraries.

Original package sysroots remain immutable. Only reachable libraries enter the
new graph; host driver/desktop/C++/glibc boundaries remain those of the reviewed
base policy. No host package, input or user profile is modified.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location('_portable_' + name, ROOT / 'scripts' / (name + '.py'))
    obj = importlib.util.module_from_spec(spec); spec.loader.exec_module(obj); return obj


def sha(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def policy_sha(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def verify_marker(port, sysroot, marker):
    port, sysroot = Path(port), Path(sysroot)
    provenance = marker.get('portable_baseline', {})
    expected = {'portable-baseline-linux.lock.json': sha(port / 'portable-baseline-linux.lock.json'),
                'scripts/setup_portable_runtime_graph.py': sha(port / 'scripts/setup_portable_runtime_graph.py'),
                'scripts/build_portable_dependencies.py': sha(port / 'scripts/build_portable_dependencies.py'),
                'scripts/setup_portable_baseline.py': sha(port / 'scripts/setup_portable_baseline.py')}
    if provenance.get('recipes') != expected or provenance.get('target_glibc') != '2.39':
        raise ValueError('portable runtime recipe/lock identity changed')
    policy = marker.get('runtime_policy', {})
    original = json.loads((port / 'graphics-deps-linux.lock.json').read_text())['runtime']
    if (policy.get('host_sonames') != original['host_sonames'] or policy.get('root_sonames') != original['root_sonames']
            or marker.get('runtime_policy_sha256') != policy_sha(policy) or policy.get('minimum_glibc') != '2.39'):
        raise ValueError('portable runtime boundary differs from reviewed source policy')
    if set(marker.get('owned_runtime_files', {})) != set(policy.get('owned_sonames', {})):
        raise ValueError('portable private runtime inventory is incomplete')
    for name, row in {**marker['owned_runtime_files'], **marker.get('license_files', {})}.items():
        p = sysroot / row['path']
        if p.resolve() != p or not p.is_file() or not p.is_relative_to(sysroot) or p.stat().st_size != row['bytes'] or sha(p) != row['sha256']:
            raise ValueError('portable runtime file changed: ' + name)
    return policy


def build_graph(base, *, jobs=4):
    # Explicit local dependency setup is a build operation. Reuse only a stable
    # source-built provider; retain old/failed outputs when rebuilding recipes.
    import sys
    sys.path.insert(0, str(ROOT / 'scripts'))
    try:
        import build_portable_dependencies as portable
        try:
            component_path, component = portable.verify_existing()
        except (OSError, ValueError, RuntimeError):
            portable.baseline.prepare()
            portable.build(ROOT / '.deps/portable/build' / ('deps-' + uuid.uuid4().hex), jobs)
            component_path, component = portable.verify_existing()
    finally:
        sys.path.pop(0)
    graph = module('linux_runtime_graph')
    lock_path = ROOT / 'graphics-deps-linux.lock.json'
    lock = json.loads(lock_path.read_text())
    original = lock['runtime']
    prefix = component_path.parent / 'prefix'
    overrides = {}
    for row in component['outputs'].values():
        info = graph.inspect_elf(row['path'])
        overrides[info['soname']] = Path(row['path'])
    chosen = {}; pending = list(original['root_sonames'])
    while pending:
        name = pending.pop()
        if name in chosen:
            continue
        if name in original['host_sonames']:
            continue
        provider = overrides.get(name)
        if provider is None:
            if name not in original['owned_sonames']:
                raise ValueError('unclassified portable dependency: ' + name)
            provider = (Path(base) / original['owned_sonames'][name]['path']).resolve(strict=True)
        info = graph.inspect_elf(provider)
        portable.elf_floor(provider)
        chosen[name] = (provider, info)
        pending.extend(info['needed'])
    recipes = {name: sha(ROOT / name) for name in ('portable-baseline-linux.lock.json',
        'scripts/setup_portable_runtime_graph.py', 'scripts/build_portable_dependencies.py', 'scripts/setup_portable_baseline.py')}
    binding = {'base_lock': sha(lock_path), 'component_receipt': sha(component_path), 'recipes': recipes}
    identity = policy_sha(binding)
    parent = ROOT / '.deps/graphics/sysroots'
    output = parent / ('portable-' + identity[:16])
    if output.is_symlink():
        raise ValueError('portable sysroot cannot be a symlink')
    if output.exists():
        verify_marker(ROOT, output, json.loads((output / '.monado-deps.json').read_text()))
        return output
    with tempfile.TemporaryDirectory(prefix='portable-stage-', dir=parent) as directory:
        stage = Path(directory) / 'root'; stage.mkdir()
        include = stage / 'usr/include'; include.parent.mkdir()
        shutil.copytree(Path(base) / 'usr/include', include, symlinks=True)
        shutil.copytree(prefix / 'include', include, symlinks=True, dirs_exist_ok=True)
        lib = stage / 'usr/lib/x86_64-linux-gnu'; lib.mkdir(parents=True)
        pc = lib / 'pkgconfig'
        shutil.copytree(Path(base) / 'usr/lib/x86_64-linux-gnu/pkgconfig', pc, symlinks=True)
        for source in (prefix / 'lib/pkgconfig').glob('*.pc'):
            text = source.read_text().replace(str(prefix), '/usr').replace('libdir=${prefix}/lib\n', 'libdir=${prefix}/lib/x86_64-linux-gnu\n')
            (pc / source.name).write_text(text)
        owned = {}; files = {}
        for name, (source, info) in chosen.items():
            destination = lib / source.name
            if destination.exists():
                raise ValueError('portable library basename collision')
            shutil.copyfile(source, destination)
            for alias in {name, name.split('.so')[0] + '.so'} - {source.name}:
                (lib / alias).symlink_to(source.name)
            relative = destination.relative_to(stage).as_posix()
            owned[name] = {'path': relative, 'package': 'source-portable' if name in overrides else original['owned_sonames'][name]['package']}
            files[name] = {'path': relative, 'bytes': destination.stat().st_size, 'sha256': sha(destination)}
        # Retain the exact source licenses for our source-built replacements,
        # alongside the pinned package notices for every remaining private lib.
        docs = stage / 'usr/share/doc'; docs.mkdir(parents=True)
        sources = Path(component['source_root'])
        notices = {'libsdl3-0': sources / 'SDL3-3.4.8/LICENSE.txt'}
        for name, (provider, info) in chosen.items():
            if name in overrides:
                continue
            package = original['owned_sonames'][name]['package']
            notices[package] = Path(base) / 'usr/share/doc' / package / 'copyright'
        license_files = {}
        for package, notice in notices.items():
            if not notice.is_file():
                raise ValueError('private runtime license missing: ' + str(notice))
            target = docs / package / 'copyright'; target.parent.mkdir()
            shutil.copyfile(notice, target)
            license_files['notice:' + package] = {'path': str(target.relative_to(stage)), 'bytes': target.stat().st_size, 'sha256': sha(target)}
        ff_docs = docs / 'monado-ffmpeg'; ff_docs.mkdir()
        for source in sorted((sources / 'ffmpeg-8.0.1').glob('COPYING*')):
            target = ff_docs / source.name; shutil.copyfile(source, target)
            license_files['ffmpeg:' + source.name] = {'path': str(target.relative_to(stage)), 'bytes': target.stat().st_size, 'sha256': sha(target)}
        for name in ('LICENSE.md', 'CREDITS', 'Changelog'):
            source = sources / 'ffmpeg-8.0.1' / name
            if source.is_file():
                target = ff_docs / name; shutil.copyfile(source, target)
                license_files['ffmpeg:' + name] = {'path': str(target.relative_to(stage)), 'bytes': target.stat().st_size, 'sha256': sha(target)}
        policy = {**original, 'owned_sonames': dict(sorted(owned.items())), 'minimum_glibc': '2.39',
                  'library_dirs': ['usr/lib/x86_64-linux-gnu'], 'scope': 'Source-built SDL/media plus pinned reachable private libraries; host platform/driver interfaces preserved.'}
        audit = graph.audit_closure([stage / owned[n]['path'] for n in policy['root_sonames']], stage, policy, {})
        errors = [e for e in audit['errors'] if e['kind'] != 'host_missing']
        if errors:
            raise ValueError('portable private closure failed: ' + json.dumps(errors[:8]))
        marker = {'lock_sha256': sha(lock_path), 'packages': lock['packages'], 'runtime_policy': policy,
            'runtime_policy_sha256': policy_sha(policy), 'owned_runtime_files': files, 'license_files': license_files,
            'required_host_interfaces': sorted({e['needed'] for e in audit['edges'] if e['needed'] in policy['host_sonames']}),
            'runtime_scope': policy['scope'], 'portable_baseline': {'recipes': recipes, 'target_glibc': '2.39',
                'component_receipt_sha256': sha(component_path), 'source_build': component}}
        (stage / '.monado-deps.json').write_text(json.dumps(marker, indent=2) + '\n')
        stage.rename(output)
    verify_marker(ROOT, output, marker)
    return output
