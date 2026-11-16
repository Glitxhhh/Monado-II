#!/usr/bin/env python3
"""Build a project-local, GNU 2.39 CPython for the standalone setup AppImage.

The normal game source workflow still accepts its existing CPython 3.14. This
recipe is a packaging tool: no system install, pip packages or game data. It
binds the Python source, baseline sysroot, compiler and all packaged ELF files.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import signal
import stat
import subprocess
import tarfile
import tempfile
import textwrap
import time
import urllib.request

import setup_portable_baseline as baseline
from build_portable_dependencies import build_environment, elf_floor, phase_failure, wait_for_build

ROOT = baseline.ROOT
LOCK = ROOT / 'python-runtime-linux.lock.json'
ENVIRONMENT_RECIPE = Path(__file__).with_name('build_portable_dependencies.py')
# Same standard native stores as tools/builder_appimage/python_entry.c. The
# packaging probe runs before that wrapper exists; OpenSSL's compiled Ubuntu
# defaults alone do not locate Fedora's system trust store.
SYSTEM_CA_FILES = ('/etc/ssl/certs/ca-certificates.crt',
    '/etc/pki/tls/certs/ca-bundle.crt', '/etc/ssl/cert.pem',
    '/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem')
LIBS = {'libz.so.1':'zlib1g', 'libexpat.so.1':'libexpat1',
    'libssl.so.3':'libssl3t64', 'libcrypto.so.3':'libssl3t64',
    'libffi.so.8':'libffi8', 'libsqlite3.so.0':'libsqlite3-0',
    'libbz2.so.1.0':'libbz2-1.0', 'liblzma.so.5':'liblzma5',
    'libzstd.so.1':'libzstd1', 'libuuid.so.1':'libuuid1'}
GLIBC_SONAMES = {'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2',
    'librt.so.1', 'libmvec.so.1', 'libanl.so.1', 'ld-linux-x86-64.so.2'}
COMPILERS = ('.deps/bin/clang-cc', '.deps/bin/clang-cxx',
             '.venv/lib/python3.14/site-packages/ziglang/zig')
REQUIRED_MODULES = ('_asyncio', '_bisect', '_blake2', '_bz2', '_codecs_cn',
    '_codecs_hk', '_codecs_iso2022', '_codecs_jp', '_codecs_kr', '_codecs_tw',
    '_csv', '_ctypes', '_decimal', '_elementtree', '_hashlib', '_heapq', '_hmac',
    '_interpchannels', '_interpqueues', '_interpreters', '_json', '_lsprof',
    '_lzma', '_md5', '_multibytecodec', '_multiprocessing', '_pickle',
    '_posixshmem', '_posixsubprocess', '_queue', '_random', '_remote_debugging',
    '_sha1', '_sha2', '_sha3', '_socket', '_sqlite3', '_ssl', '_statistics',
    '_struct', '_uuid', '_zoneinfo', '_zstd', 'array', 'binascii', 'cmath',
    'fcntl', 'grp', 'math', 'mmap', 'pyexpat', 'resource', 'select', 'syslog',
    'termios', 'unicodedata', 'zlib')

def encoded(x): return (json.dumps(x, indent=2, sort_keys=True)+'\n').encode()


def system_ca_file():
    for name in SYSTEM_CA_FILES:
        path = Path(name)
        try:
            if path.is_file() and os.access(path, os.R_OK):
                return str(path)
        except OSError:
            continue
    raise ValueError('No readable standard system CA trust store found. Install or repair '
                     'your distribution\'s ca-certificates package before building portable Python.')


def prepare_build_libraries(directory, libdir):
    """Give CPython's build-time extension checks their target non-glibc libs.

    A successful link is followed by imports during make, before installation.
    Fedora's bzip2 SONAME differs from Noble's, so host resolution alone cannot
    import every target extension. Never expose the complete target library
    directory (and its old libc/loader) to native compiler/tool children.
    """
    directory, libdir = Path(directory), Path(libdir)
    if set(LIBS) & GLIBC_SONAMES or any(name.startswith('ld-') or '/' in name for name in LIBS):
        raise ValueError('Portable Python build libraries must exclude glibc and loaders')
    directory.mkdir(mode=0o700)
    rows = {}
    for name in LIBS:
        source = (libdir / name).resolve(strict=True)
        if not source.is_relative_to(libdir.resolve()) or not source.is_file():
            raise ValueError('Portable Python build library escaped its pinned sysroot: ' + name)
        before = {'bytes': source.stat().st_size, 'sha256': baseline.sha(source)}
        target = directory / name
        shutil.copyfile(source, target)
        rows[name] = {**before, 'source': str(source), 'glibc_max': elf_floor(target)}
        if target.stat().st_size != before['bytes'] or baseline.sha(target) != before['sha256']:
            raise ValueError('Portable Python build library copy differs: ' + name)
    verify_build_libraries(directory, rows)
    return rows


def verify_build_libraries(directory, rows):
    directory = Path(directory)
    if set(rows) != set(LIBS) or {path.name for path in directory.iterdir()} != set(LIBS):
        raise ValueError('Portable Python build library inventory changed')
    for name, row in rows.items():
        path = directory / name
        if (path.is_symlink() or not path.is_file() or path.stat().st_nlink != 1
                or path.stat().st_size != row['bytes'] or baseline.sha(path) != row['sha256']):
            raise ValueError('Portable Python build library changed: ' + name)


def apache_license_text(source):
    """Retain the complete operative Apache terms from the pinned Python source."""
    text = (Path(source) / 'Doc/license.rst').read_text()
    section = text.split('\nOpenSSL\n-------\n', 1)
    if len(section) != 2:
        raise ValueError('Pinned Python OpenSSL license section changed')
    section = section[1].split('\n\nexpat\n-----\n', 1)
    if len(section) != 2:
        raise ValueError('Pinned Python OpenSSL license boundary changed')
    section = section[0]
    beginning = re.search(r'(?m)^[ \t]*Apache License\n[ \t]*Version 2\.0, January 2004\n', section)
    if beginning is None or section.count('END OF TERMS AND CONDITIONS') != 1:
        raise ValueError('Pinned Python Apache license text is incomplete')
    # Keep any upstream appendix with the operative terms, through the checked
    # next-section boundary. The full source license document is retained too.
    terms = textwrap.dedent(section[beginning.start():]).strip() + '\n'
    if any(not re.search(r'(?m)^\s*' + str(number) + r'\. ', terms) for number in range(1, 10)):
        raise ValueError('Pinned Python Apache license sections are incomplete')
    return terms

def download():
    row = json.loads(LOCK.read_text())
    if (row.get('schema') != 1 or row.get('version') != '3.14.4' or
        row.get('url') != 'https://www.python.org/ftp/python/3.14.4/Python-3.14.4.tar.xz'):
        raise ValueError('Unreviewed Python source identity')
    cache = ROOT / '.deps/portable/cache'; cache.mkdir(parents=True, exist_ok=True)
    target = baseline.ordinary(cache / row['filename'])
    if not target.exists():
        part = target.with_suffix('.partial')
        fd = os.open(part, os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW, 0o600)
        try:
            with os.fdopen(fd,'wb') as dest, urllib.request.urlopen(row['url'],timeout=60) as source:
                total=0
                while block:=source.read(1024*1024):
                    total+=len(block)
                    if total>row['bytes']: raise ValueError('Python source exceeds locked size')
                    dest.write(block)
            if part.stat().st_size!=row['bytes'] or baseline.sha(part)!=row['sha256']:
                raise ValueError('Python source identity differs')
            part.rename(target)
        except BaseException:
            part.unlink(missing_ok=True); raise
    if target.stat().st_size!=row['bytes'] or baseline.sha(target)!=row['sha256']:
        raise ValueError('Cached Python source identity differs')
    return target

def download_development():
    lock=json.loads(LOCK.read_text()); cache=ROOT/'.deps/portable/cache'
    cache.mkdir(parents=True,exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        return list(pool.map(lambda row: baseline.fetch(row,cache),lock['development_packages']))

def verify_existing():
    marker=baseline.ordinary(ROOT/'.deps/portable/active-python.json')
    value=json.loads(marker.read_text()); receipt_path=baseline.ordinary(value['receipt'])
    if not receipt_path.is_relative_to(ROOT/'.deps/portable/python') or baseline.sha(receipt_path)!=value['sha256']:
        raise ValueError('Portable Python receipt differs')
    receipt=json.loads(receipt_path.read_text())
    if (receipt.get('status')!='passed' or receipt.get('lock_sha256')!=baseline.sha(LOCK)
        or receipt.get('recipe_sha256')!=baseline.sha(__file__)
        or receipt.get('baseline_lock_sha256')!=baseline.sha(baseline.LOCK)
        or receipt.get('environment_recipe_sha256')!=baseline.sha(ENVIRONMENT_RECIPE)):
        raise ValueError('Portable Python recipe or baseline differs')
    if set(receipt.get('compiler',{})) != set(COMPILERS):
        raise ValueError('Portable Python compiler identity missing')
    for name,expected in receipt['compiler'].items():
        if baseline.sha(ROOT/name)!=expected: raise ValueError('Portable Python compiler changed')
    prefix=baseline.ordinary(receipt_path.parent/'prefix', directory=True)
    for name,row in receipt['outputs'].items():
        p=baseline.ordinary(prefix/name)
        if p.stat().st_size!=row['bytes'] or baseline.sha(p)!=row['sha256']:
            raise ValueError('Portable Python output changed: '+name)
    return receipt_path, receipt

def compiler_entry(directory, name, compiler):
    """Autoconf treats CC/CXX as shell commands, even with argv-based configure."""
    entry = directory / name
    entry.write_text('#!/bin/sh\nexec ' + shlex.quote(str(compiler)) + ' "$@"\n')
    entry.chmod(0o700)
    return entry


def run_owned_process(argv, *, cwd, env, stdout, timeout=1200, progress=None):
    """A canceled make/configure must not outlive its private source tree."""
    process=subprocess.Popen(list(map(str,argv)),cwd=cwd,env=env,
        stdin=subprocess.DEVNULL,stdout=stdout,stderr=subprocess.STDOUT,
        start_new_session=True)
    try:
        return (wait_for_build(process, progress, timeout=timeout) if progress
                else process.wait(timeout=timeout))
    except BaseException:
        try:
            os.killpg(process.pid,signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        finally:
            # A parent can exit on TERM while a compiler descendant ignores
            # it. Finish this exact owned group even after the parent exits.
            try:
                os.killpg(process.pid,signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        raise


def build(output, jobs=4):
    out=baseline.ordinary(output,directory=True)
    if out.exists() or not out.is_relative_to(ROOT/'.deps/portable/python'):
        raise ValueError('Fresh .deps/portable/python output required')
    # This is the explicit source-build operation, which already downloads the
    # pinned Python and development archives. A fresh launcher-only workspace
    # need not have run the separate portable graphics recipe first. Prepare
    # missing, hash-checked baseline inputs here; verify_existing stays offline.
    prepared=baseline.prepare(); archive=download(); development=download_development()
    out.mkdir(mode=0o700,parents=True)
    # Autoconf/make command strings cannot safely carry installation paths
    # containing whitespace. Source, build, sysroot and installation all stay
    # in one private physical /tmp directory; TMPDIR is intentionally ignored.
    with tempfile.TemporaryDirectory(prefix='monado-python-', dir='/tmp') as temporary:
        workspace=baseline.ordinary(Path(temporary),directory=True)
        info=workspace.stat()
        if (any(c.isspace() for c in str(workspace)) or info.st_uid!=os.geteuid()
            or stat.S_IMODE(info.st_mode)!=0o700):
            raise ValueError('Portable Python requires a private space-free build directory')
        return build_private(out,workspace,prepared,archive,development,jobs)


def build_private(out, workspace, prepared, archive, development, jobs):
    trust_store = system_ca_file()
    logs=workspace/'logs'; logs.mkdir()
    with tarfile.open(archive) as tar:
        tar.extractall(workspace,filter=baseline.archive_filter)
    source=workspace/'Python-3.14.4'; prefix=workspace/'prefix'; work=workspace/'build'; work.mkdir()
    # Zig rejects implicit build-clock macros even with SOURCE_DATE_EPOCH.
    # Pin CPython's optional display metadata to its upstream release date.
    buildinfo=source/'Modules/getbuildinfo.c'; original=buildinfo.read_text()
    if original.count('#define DATE __DATE__')!=1 or original.count('#define TIME __TIME__')!=1:
        raise ValueError('CPython reproducible build metadata source changed')
    buildinfo.write_text(original.replace('#define DATE __DATE__','#define DATE "Apr  7 2026"').replace('#define TIME __TIME__','#define TIME "00:00:00"'))
    # Keep the accepted portable runtime sysroot immutable. Python needs four
    # additional development packages; extract them only into this fresh copy.
    sysroot=workspace/'sysroot'; shutil.copytree(prepared['sysroot'],sysroot,symlinks=True)
    for package in development:
        process=subprocess.Popen(['/usr/bin/dpkg-deb','--fsys-tarfile',package],stdout=subprocess.PIPE)
        try:
            with tarfile.open(fileobj=process.stdout,mode='r|') as tar:
                tar.extractall(sysroot,filter=baseline.archive_filter)
            if process.wait()!=0: raise ValueError('Python development package extraction failed')
        finally:
            process.stdout.close()
            if process.poll() is None: process.kill();process.wait()
    libdir=sysroot/'usr/lib/x86_64-linux-gnu'
    build_libdir=workspace/'build-libraries'
    build_libraries=prepare_build_libraries(build_libdir,libdir)
    tools=workspace/'tools'; tools.mkdir()
    cc=compiler_entry(tools,'cc',ROOT/'.deps/bin/clang-cc')
    cxx=compiler_entry(tools,'cxx',ROOT/'.deps/bin/clang-cxx')
    flags='-target x86_64-linux-gnu.2.39 -march=x86_64_v2 -fPIC -O2 -ffp-contract=off -fno-strict-aliasing'
    # [XCDE] LINUX-111: the host pkg-config wrapper must resolve its host helper,
    # never an executable from the target development sysroot.
    env=build_environment(sysroot)
    env.update({
        'LD_LIBRARY_PATH':str(build_libdir),
        'CC':str(cc),'CXX':str(cxx),
        'CFLAGS':flags,'CPPFLAGS':'-I'+str(sysroot/'usr/include')+' -I'+str(sysroot/'usr/include/x86_64-linux-gnu'),
        'LDFLAGS':'-target x86_64-linux-gnu.2.39 -L'+str(libdir),
        'PKG_CONFIG':'/usr/bin/pkg-config','PYTHONDONTWRITEBYTECODE':'1',
        'SOURCE_DATE_EPOCH':'1775520000'}) # upstream release date, reproducible build metadata
    receipt={'schema':1,'status':'running','version':'3.14.4','minimum_glibc':'2.39',
        'lock_sha256':baseline.sha(LOCK),'recipe_sha256':baseline.sha(__file__),
        'baseline_lock_sha256':baseline.sha(baseline.LOCK),
        'environment_recipe_sha256':baseline.sha(ENVIRONMENT_RECIPE),
        'runtime_certificate_store':trust_store,'commands':[], 'outputs':{}}
    receipt['build_libraries']={'directory':str(build_libdir),'files':build_libraries,
        'scope':'Private copies of the reviewed non-glibc runtime dependencies only; native host libc/loader unchanged'}
    receipt['compiler']={name:baseline.sha(ROOT/name) for name in COMPILERS}
    receipt['source_archive']={'bytes':archive.stat().st_size,'sha256':baseline.sha(archive)}
    receipt['baseline_source_manifest_sha256']=prepared.get('source_manifest_sha256')
    receipt['private_build']={'directory':str(workspace),'mode':0o700,'tmpdir_environment_ignored':True,
        'compiler_entries':{p.name:baseline.sha(p) for p in (cc,cxx)}}
    receipt['build_environment']=env
    receipt['source_patches']={'Modules/getbuildinfo.c':{'before_sha256':hashlib.sha256(original.encode()).hexdigest(),'after_sha256':baseline.sha(buildinfo)}}
    def save(): (out/'receipt.json').write_bytes(encoded(receipt))
    def run(name,argv,cwd=work,env_override=None):
        start=time.monotonic(); code=None; failure=None
        verify_build_libraries(build_libdir,build_libraries)
        print('python-'+name+': starting; log: '+str(out/'logs'/(name+'.log')),flush=True)
        try:
            with (logs/(name+'.log')).open('wb') as log:
                code=run_owned_process(argv,cwd=cwd,env=env_override or env,stdout=log,
                    progress='python-'+name)
        except BaseException as error:
            failure=type(error).__name__+': '+str(error)
            raise
        finally:
            row={'name':name,'argv':list(map(str,argv)),'cwd':str(cwd),
                'exit':code,'seconds':time.monotonic()-start,'log':str(out/'logs'/(name+'.log'))}
            if failure: row['error']=failure
            receipt['commands'].append(row)
            save()
        if code:
            raise phase_failure('python-'+name,logs/(name+'.log'),code,
                display_path=out/'logs'/(name+'.log'))
        print('python-'+name+': completed ('+format(time.monotonic()-start,'.1f')+'s)',flush=True)
    try:
        save()
        run('configure',[source/'configure','--prefix='+str(prefix),'--without-ensurepip','--disable-test-modules'])
        make=sysroot/'usr/bin/make'
        run('compile',[make,'-j'+str(jobs)])
        run('install',[make,'install'])
        # Copy only reviewed non-glibc dependencies. The host owns libc/loader.
        for soname,package in LIBS.items():
            shutil.copy2(build_libdir/soname,prefix/'lib'/soname)
        notices=prefix/'share/doc/monado-ii'; notices.mkdir(parents=True,exist_ok=True)
        # Upstream LICENSE covers Python itself, while Doc/license.rst and
        # embedded HACL headers carry additional incorporated-code notices.
        # Keep complete texts, plus the PSF-required brief modification summary.
        texts=[(source/'LICENSE').read_text(),
            'Monado II packaging changes: private GNU 2.39 build; optional build-date metadata pinned to the upstream release date. No interpreter semantic changes.\n',
            (source/'Doc/license.rst').read_text(),(source/'Modules/expat/COPYING').read_text()]
        for header in sorted((source/'Modules/_hacl').rglob('*')):
            if header.suffix not in ('.c','.h') or not header.is_file(): continue
            text=header.read_text()
            if text.startswith('/*'):
                comment=text[:text.find('*/')+2]
                if 'Copyright' in comment and comment not in texts: texts.append(comment)
        (notices/'Python-copyright.txt').write_text('\n\n'.join(texts)+'\n')
        for package in sorted(set(LIBS.values())):
            shutil.copy2(sysroot/'usr/share/doc'/package/'copyright',notices/(package+'-copyright.txt'))
        # Fedora has no Debian common-licenses directory. The hash-locked
        # Python source already contains these complete Apache operative terms.
        (notices/'Apache-2.0.txt').write_text(apache_license_text(source))
        receipt['license_sources'] = {'Apache-2.0.txt': {
            'path': 'Python-3.14.4/Doc/license.rst', 'source_sha256': baseline.sha(source/'Doc/license.rst'),
            'section': 'OpenSSL / Apache License 2.0, sections 1 through 9',
            'sha256': baseline.sha(notices/'Apache-2.0.txt')}}
        shutil.copy2(source/'LICENSE',prefix/'lib/python3.14/LICENSE.txt')
        # No runtime kind marker yet: the packaging wrapper is installed later.
        probe='''import importlib,ssl,sqlite3,sys; from compression import zstd; assert sys.version_info[:3]==(3,14,4); assert sqlite3.connect(':memory:').execute('select 42').fetchone()==(42,); assert zstd.decompress(zstd.compress(b'portable'))==b'portable'; assert ssl.create_default_context().get_ca_certs(); modules=''' + repr(REQUIRED_MODULES) + '''; [importlib.import_module(name) for name in modules]; print('portable Python '+str(len(modules))+' imports/TLS/SQLite/zstd: PASS')'''
        runtime_env={'PATH':'/usr/bin:/bin','LANG':'C.UTF-8','LC_ALL':'C.UTF-8','PYTHONDONTWRITEBYTECODE':'1',
            'LD_LIBRARY_PATH':str(prefix/'lib'),'SSL_CERT_FILE':trust_store}
        python=prefix/'bin/python3.14'
        run('probe-current',[python,'-I','-B','-c',probe],env_override=runtime_env)
        run('probe-noble',[libdir/'ld-linux-x86-64.so.2','--library-path',str(prefix/'lib')+':'+str(libdir),python,'-I','-B','-c',probe],env_override=runtime_env)
        shutil.copytree(prefix,out/'prefix',symlinks=True)
        prefix=out/'prefix'; python=prefix/'bin/python3.14'
        runtime_env['LD_LIBRARY_PATH']=str(prefix/'lib')
        run('probe-relocated-current',[python,'-I','-B','-c',probe],env_override=runtime_env)
        run('probe-relocated-noble',[libdir/'ld-linux-x86-64.so.2','--library-path',str(prefix/'lib')+':'+str(libdir),python,'-I','-B','-c',probe],env_override=runtime_env)
        for p in sorted(prefix.rglob('*')):
            if p.is_symlink() or not p.is_file(): continue
            data=p.read_bytes()
            row={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
            if data.startswith(b'\x7fELF'): row['glibc_max']=elf_floor(p)
            if p.suffix!='.pyc': receipt['outputs'][p.relative_to(prefix).as_posix()]=row
        if baseline.sha(__file__)!=receipt['recipe_sha256']:
            raise ValueError('Python recipe changed during build; preserve output and rebuild from stable source')
        if baseline.sha(ENVIRONMENT_RECIPE)!=receipt['environment_recipe_sha256']:
            raise ValueError('Python build environment recipe changed during build')
        if any(baseline.sha(ROOT/name)!=expected for name,expected in receipt['compiler'].items()):
            raise ValueError('Python compiler changed during build')
        if baseline.sha(LOCK)!=receipt['lock_sha256'] or baseline.sha(baseline.LOCK)!=receipt['baseline_lock_sha256']:
            raise ValueError('Python source/baseline lock changed during build')
        receipt['status']='passed'; save()
        marker=ROOT/'.deps/portable/active-python.json'
        tmp=marker.with_suffix('.partial'); tmp.write_bytes(encoded({'receipt':str(out/'receipt.json'),'sha256':baseline.sha(out/'receipt.json')}));tmp.replace(marker)
    except BaseException as error:
        receipt.update(status='failed',error=str(error));raise
    finally:
        shutil.copytree(logs,out/'logs')
        if (work/'config.log').is_file(): shutil.copy2(work/'config.log',out/'logs/config.log')
        save()
    return receipt

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--download',action='store_true');ap.add_argument('--output',type=Path);ap.add_argument('--jobs',type=int,choices=range(1,9),default=4)
    args=ap.parse_args()
    def interrupted(_signal, _frame): raise InterruptedError('Portable Python build canceled')
    signal.signal(signal.SIGTERM,interrupted)
    if args.download: print(download());print(download_development())
    elif args.output: print(json.dumps(build(args.output,args.jobs),indent=2))
    else:
        path,receipt=verify_existing()
        print(json.dumps({'receipt':str(path),'receipt_sha256':baseline.sha(path),
            'prefix':str(path.parent/'prefix'),'python':str(path.parent/'prefix/bin/python3.14'),
            'minimum_glibc':receipt['minimum_glibc'],'version':receipt['version']},sort_keys=True))
if __name__=='__main__': main()
