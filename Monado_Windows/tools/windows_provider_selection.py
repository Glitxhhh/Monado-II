#!/usr/bin/env python3
"""Stage explicitly supplied local Windows providers without loading their code.

Only the Vulkan FidelityFX API and the four Streamline DLSS modules used by this
runtime are selected. This is local packaging/integrity evidence, not GPU or SDK
ABI qualification. No download, agreement acceptance, driver change or mutation
of an existing runtime is performed.
"""
from __future__ import annotations

from contextlib import ExitStack, contextmanager
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
MAX_DLL = 512 * 1024 * 1024
MAX_LICENSE = 1024 * 1024
MAX_TOTAL = 1024 * 1024 * 1024
REQUIRED = {
    'fidelityfx': ('amd_fidelityfx_vk.dll',),
    'streamline': ('sl.interposer.dll', 'sl.common.dll', 'sl.dlss.dll', 'nvngx_dlss.dll'),
}
EXPORTS = {
    'amd_fidelityfx_vk.dll': {'ffxCreateContext', 'ffxDestroyContext', 'ffxDispatch', 'ffxQuery', 'ffxConfigure'},
    'sl.interposer.dll': {'slInit', 'slGetFeatureRequirements', 'slSetVulkanInfo', 'slEvaluateFeature',
        'slGetNewFrameToken', 'slSetConstants', 'slSetTagForFrame', 'slFreeResources', 'slShutdown', 'vkGetDeviceProcAddr'},
}
# Known redistributable runtime dependencies only. Anything else needs an
# explicit reviewed recipe, rather than recursively copying arbitrary DLLs.
REDISTRIBUTABLE = {'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'msvcp140_1.dll',
    'msvcp140_2.dll', 'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll', 'concrt140.dll'}
SYSTEM = {'kernel32.dll', 'kernelbase.dll', 'ntdll.dll', 'msvcrt.dll', 'ucrtbase.dll', 'advapi32.dll',
    'bcrypt.dll', 'bcryptprimitives.dll', 'crypt32.dll', 'cryptbase.dll', 'cryptsp.dll', 'wintrust.dll',
    'user32.dll', 'gdi32.dll', 'gdi32full.dll', 'ole32.dll', 'oleaut32.dll', 'combase.dll', 'shell32.dll',
    'shlwapi.dll', 'version.dll', 'winmm.dll', 'ws2_32.dll', 'winhttp.dll', 'wininet.dll', 'wtsapi32.dll',
    'secur32.dll', 'sspicli.dll', 'cfgmgr32.dll', 'setupapi.dll', 'imm32.dll', 'dwmapi.dll', 'dxgi.dll',
    'd3d11.dll', 'd3d12.dll', 'd3dcompiler_47.dll', 'opengl32.dll', 'rpcrt4.dll', 'powrprof.dll',
    'dbghelp.dll', 'dbgcore.dll', 'psapi.dll', 'iphlpapi.dll', 'win32u.dll', 'normaliz.dll', 'ntmarta.dll'}
# Vulkan is already part of the checked native build's dependency closure.
HOST = {'vulkan-1.dll'}
LAYOUTS = ('', 'bin', 'bin/x64', 'bin/x64/production')
LICENSES = ('LICENSE', 'LICENSE.txt', 'LICENSE.md', 'license.txt')
HASH = re.compile(r'[0-9a-f]{64}\Z')


def module(path, name):
    loaded = sys.modules.get(name)
    if loaded:
        if Path(loaded.__file__) != path:
            raise ValueError('A provider verifier module is already loaded from a different source')
        return loaded
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result


def dependencies():
    scope = hashlib.sha256(str(ROOT).casefold().encode('utf-8')).hexdigest()[:16]
    guard = module(ROOT.parent / 'tools/windows_file_guard.py', 'monado_provider_file_guard_' + scope)
    image = module(ROOT / 'tools/pe_symbols_windows.py', 'monado_provider_pe_' + scope).Image
    return guard, image


def encoded(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + '\n').encode('utf-8')


def identity(data):
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def system_import(name):
    key = name.lower()
    return key in SYSTEM or key in HOST or key.startswith(('api-ms-win-', 'ext-ms-win-'))


def system_directory():
    import ctypes as C
    from ctypes import wintypes as W
    kernel=C.WinDLL('kernel32',use_last_error=True)
    kernel.GetSystemDirectoryW.argtypes=[W.LPWSTR,W.UINT];kernel.GetSystemDirectoryW.restype=W.UINT
    value=C.create_unicode_buffer(32768);length=kernel.GetSystemDirectoryW(value,len(value))
    if not length or length>=len(value):raise OSError(C.get_last_error(),'Cannot identify the native Windows system directory')
    return Path(value.value)


def system_owner(stream):
    """The separately scoped installed CRT must remain owned by the OS/admins."""
    import ctypes as C
    from ctypes import wintypes as W
    import msvcrt
    advapi=C.WinDLL('advapi32',use_last_error=True);kernel=C.WinDLL('kernel32',use_last_error=True)
    advapi.GetSecurityInfo.argtypes=[W.HANDLE,C.c_int,W.DWORD,C.POINTER(C.c_void_p),C.c_void_p,C.c_void_p,C.c_void_p,C.POINTER(C.c_void_p)]
    advapi.GetSecurityInfo.restype=W.DWORD
    advapi.ConvertSidToStringSidW.argtypes=[C.c_void_p,C.POINTER(W.LPWSTR)];advapi.ConvertSidToStringSidW.restype=W.BOOL
    kernel.LocalFree.argtypes=[C.c_void_p];kernel.LocalFree.restype=C.c_void_p
    owner=C.c_void_p();descriptor=C.c_void_p();text=W.LPWSTR()
    result=advapi.GetSecurityInfo(msvcrt.get_osfhandle(stream.fileno()),1,1,C.byref(owner),None,None,None,C.byref(descriptor))
    try:
        if result:raise OSError(result,'Cannot inspect installed Microsoft runtime ownership')
        if not advapi.ConvertSidToStringSidW(owner,C.byref(text)):raise OSError(C.get_last_error(),'Cannot identify installed runtime owner')
        sid=text.value
        if sid not in {'S-1-5-18','S-1-5-32-544','S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464'}:
            raise ValueError('Installed Microsoft runtime is not owned by Windows/TrustedInstaller/Administrators')
        return sid
    finally:
        if text:kernel.LocalFree(C.cast(text,C.c_void_p))
        if descriptor:kernel.LocalFree(descriptor)


def same_source(before,after):
    return (isinstance(before,dict) and isinstance(after,dict) and
            Path(before.get('root',''))==Path(after.get('root','')) and
            {k:v for k,v in before.items() if k not in ('root','system')}==
            {k:v for k,v in after.items() if k not in ('root','system')})


def pe_contract(image, name):
    """Check DLL kind, required real exports, normal and delayed dependency names."""
    pe = image.unpack('<I', 0x3c)[0]
    if not image.unpack('<H', pe + 22)[0] & 0x2000:
        raise ValueError('Provider is not an AMD64 DLL: ' + name)
    required = EXPORTS.get(name, set())
    if not required <= set(image.exports()):
        raise ValueError('Required provider exports are missing: ' + name)
    rva, size = image.directories[0]
    if required:
        entry = image.unpack('<IIHHIIIIIII', image.offset(rva, 40))
        for index in range(entry[7]):
            export = image.string(image.unpack('<I', image.offset(entry[9] + index * 4, 4))[0])
            if export in required:
                ordinal = image.unpack('<H', image.offset(entry[10] + index * 2, 2))[0]
                function = image.unpack('<I', image.offset(entry[8] + ordinal * 4, 4))[0]
                if not function or rva <= function < rva + size:
                    raise ValueError('Required provider export is absent or forwarded: ' + export)
                image.offset(function)
    imports = set(image.imports())
    opt = pe + 24
    if image.unpack('<I', opt + 108)[0] >= 14:
        delayed, length = image.unpack('<II', opt + 112 + 13 * 8)
        if delayed:
            if length < 32 or length > 32 * 4096:
                raise ValueError('Invalid delayed provider import bounds')
            for index in range(length // 32):
                entry = image.unpack('<IIIIIIII', image.offset(delayed + index * 32, 32))
                if not any(entry):
                    break
                if entry[0] != 1:
                    raise ValueError('Provider delay imports require ordinary RVA descriptors')
                library = image.string(entry[1])
                if not re.fullmatch(r'[A-Za-z0-9_.+-]+\.dll', library, re.I):
                    raise ValueError('Unsafe delayed provider dependency')
                imports.add(library)
            else:
                raise ValueError('Unterminated delayed provider imports')
    return imports


def locate(directory, name, *, optional=False, license=False):
    candidates = []
    for layout in LAYOUTS:
        relative = (layout + '/' if layout else '') + name
        try:
            stream = directory.open_regular(relative, max_bytes=MAX_LICENSE if license else MAX_DLL, owner=True)
            with stream:
                pass
        except OSError as error:
            if error.errno in (2, 3):
                continue
            raise
        candidates.append(relative)
    if not candidates:
        if optional:
            return None
        raise ValueError('Local SDK folder is missing required file: ' + name)
    if len(candidates) != 1:
        raise ValueError('Ambiguous local SDK copies; select one ordinary flat provider folder: ' + name)
    return candidates[0]


@contextmanager
def checked(roots, *, expected=None, cancel=lambda: None):
    """Retain all ordinary owned source/ancestor leases throughout a stage."""
    if not isinstance(roots, dict) or set(roots) - REQUIRED.keys():
        raise ValueError('Unknown local provider selection')
    if expected is not None and (not isinstance(expected, dict) or set(expected) != set(roots)):
        raise ValueError('Saved provider selection membership changed')
    guard, Image = dependencies()
    total = 0
    with ExitStack() as leases:
        selection, contents = {}, {}
        for kind, root in sorted(roots.items()):
            cancel()
            path = Path(guard.canonical_path(root))
            directory = leases.enter_context(guard.locked_directory(path, owner=True))
            files, staged, installed, pending, seen = {}, {}, {}, list(REQUIRED[kind]), set()
            while pending:
                cancel()
                name = pending.pop(0).lower()
                if name in seen:
                    continue
                seen.add(name)
                relative = locate(directory, name, optional=name in REDISTRIBUTABLE)
                if relative is None:
                    # Only these named VC redistributables may use the actual
                    # protected System32 installation. Never PATH/SystemRoot,
                    # user ownership exceptions, or arbitrary copied siblings.
                    system=system_directory()
                    try:
                        stream=leases.enter_context(guard.open_regular(system/name,max_bytes=MAX_DLL,owner=False))
                    except OSError as error:
                        if error.errno in (2,3):
                            raise ValueError('Missing Microsoft Visual C++ x64 runtime '+name+'; install the official runtime manually or supply its checked local DLL alongside the SDK') from error
                        raise
                    owner=system_owner(stream);data=stream.read(MAX_DLL+1)
                    imports=pe_contract(Image(system/name),name)
                    installed[name]=dict(identity(data),path=str(system/name),origin='installed-system-runtime',owner_sid=owner)
                    for dependency in sorted(imports):
                        key=dependency.lower()
                        if key in REDISTRIBUTABLE:pending.append(key)
                        elif not system_import(key):raise ValueError('Unreviewed installed Microsoft runtime dependency: '+dependency)
                    continue
                stream = leases.enter_context(directory.open_regular(relative, max_bytes=MAX_DLL, owner=True))
                data = stream.read(MAX_DLL + 1)
                total += len(data)
                if len(data) > MAX_DLL or total > MAX_TOTAL:
                    raise ValueError('Local provider closure exceeds its size bound')
                files[relative] = identity(data)
                staged[name] = relative
                contents[(kind, relative)] = data
                image = Image(path / relative)
                for dependency in sorted(pe_contract(image, name)):
                    key = dependency.lower()
                    if key in REQUIRED[kind] or key in REDISTRIBUTABLE:
                        pending.append(key)
                    elif not system_import(key):
                        raise ValueError('Unreviewed or missing provider dependency: ' + dependency)
            license_path = None
            for name in LICENSES:
                license_path = locate(directory, name, optional=True, license=True)
                if license_path:
                    break
            if not license_path:
                raise ValueError('Local provider SDK must include its LICENSE or LICENSE.txt')
            licenses = [license_path]
            if kind == 'streamline':
                licenses.append(locate(directory, 'nvngx_dlss.license.txt', license=True))
            for relative in licenses:
                stream = leases.enter_context(directory.open_regular(relative, max_bytes=MAX_LICENSE, owner=True))
                data = stream.read(MAX_LICENSE + 1)
                if not data or len(data) > MAX_LICENSE:
                    raise ValueError('Empty or oversized provider license')
                files[relative] = identity(data)
                contents[(kind, relative)] = data
            value = {'root': str(path), 'files': files, 'stage': staged, 'licenses': licenses, 'system': installed}
            if expected is not None:
                prior = expected.get(kind)
                if not same_source(prior,value):
                    raise ValueError('Selected local provider files changed; original runtime and selection are preserved')
            selection[kind] = value
        yield selection, contents


def inspect_selection(roots, *, expected=None, cancel=lambda: None):
    with checked(roots, expected=expected, cancel=cancel) as (selection, _):
        return selection


def stage(output, *, fidelityfx=None, streamline=None, expected=None, cancel=lambda: None, progress=None):
    roots = {kind: path for kind, path in (('fidelityfx', fidelityfx), ('streamline', streamline)) if path is not None}
    if not roots:
        if expected:
            raise ValueError('Saved provider selection requires its explicit local source folders')
        return {'schema': 1, 'platform': 'windows-amd64', 'selection': {}, 'files': {}}
    guard, _ = dependencies()
    output = Path(guard.canonical_path(output))
    with checked(roots, expected=expected, cancel=cancel) as (selection, contents), guard.locked_directory(output, owner=True) as directory:
        plan = {}
        for kind, selected in selection.items():
            if output == Path(selected['root']) or output.is_relative_to(Path(selected['root'])) or Path(selected['root']).is_relative_to(output):
                raise ValueError('Provider input and fresh runtime must be disjoint')
            for name, relative in selected['stage'].items():
                target = 'build/' + name
                data = contents[(kind, relative)]
                if target in plan and plan[target] != data:
                    raise ValueError('Conflicting provider dependency: ' + name)
                plan[target] = data
            for relative in selected['licenses']:
                plan['providers/' + kind + '/' + Path(relative).name] = contents[(kind, relative)]
        # Check every target before writing any: no overwrite, including an
        # unknown/same-name existing DLL with identical contents.
        for target in list(plan) + ['windows-providers.json']:
            cancel()
            try:
                with directory.open_regular(target, max_bytes=MAX_DLL, owner=True):
                    pass
            except OSError as error:
                if error.errno in (2, 3):
                    continue
                raise
            raise ValueError('Provider destination already exists; preserved without overwrite: ' + target)
        for index, (target, data) in enumerate(sorted(plan.items()), 1):
            cancel()
            parent, leaf = target.rsplit('/', 1)
            with directory.child(parent, create=True, owner=True) as child:
                child.write_new(leaf, data, owner=True)
            if progress:
                progress('providers', index, len(plan), 'Staging checked local provider files')
        record = {'schema': 1, 'platform': 'windows-amd64', 'selection': selection,
                  'qualification': 'local-integrity-only', 'files': {name: identity(data) for name, data in sorted(plan.items())}}
        raw = encoded(record)
        cancel()
        directory.atomic_write_new('windows-providers.json', raw, owner=True)
        return dict(record, files=dict(record['files'], **{'windows-providers.json': identity(raw)}))
