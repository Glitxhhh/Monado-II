#!/usr/bin/env python3
"""[XCDE] LINUX-103: pinned local CMake/Ninja, no pip or system installation."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT=Path(__file__).resolve().parents[1]
LOCK=ROOT/'build-tools-linux.lock.json'

def digest(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def setup():
    rows=json.loads(LOCK.read_text())['tools'];cache=ROOT/'.deps/build-tools'
    if cache.is_symlink():raise ValueError('build-tool cache must be ordinary')
    (cache/'downloads').mkdir(parents=True,exist_ok=True)
    packages=cache/'packages';packages.mkdir(exist_ok=True);result={}
    for row in rows:
        archive=cache/'downloads'/row['filename']
        if archive.is_symlink():raise ValueError('tool archive must be ordinary')
        if not archive.exists():
            fd,tmp=tempfile.mkstemp(prefix='.download-',dir=archive.parent);os.close(fd)
            try:
                with urllib.request.urlopen(row['url'],timeout=120) as src,open(tmp,'wb') as dst:
                    remaining=row['bytes']
                    while remaining:
                        data=src.read(min(1048576,remaining))
                        if not data:raise ValueError('truncated build tool')
                        dst.write(data);remaining-=len(data)
                    if src.read(1):raise ValueError('oversized build tool')
                if digest(tmp)!=row['sha256']:raise ValueError('build tool SHA256 mismatch')
                os.replace(tmp,archive)
            finally:
                if os.path.exists(tmp):os.unlink(tmp)
        if archive.stat().st_size!=row['bytes'] or digest(archive)!=row['sha256']:
            raise ValueError('cached build tool differs from lock')
        target=packages/(row['name']+'-'+row['version']);receipt=target/'.monado-tool.json'
        if target.is_symlink():raise ValueError('build tool directory must be ordinary')
        if not target.exists():
            work=Path(tempfile.mkdtemp(prefix='.unpack-',dir=packages))
            try:
                total=0;files={}
                with zipfile.ZipFile(archive) as z:
                    for entry in z.infolist():
                        parts=PurePosixPath(entry.filename)
                        mode=entry.external_attr>>16;total+=entry.file_size
                        if parts.is_absolute() or '..' in parts.parts or '\\' in entry.filename or stat.S_ISLNK(mode) or total>256*1024*1024:
                            raise ValueError('unsafe build-tool archive')
                        p=work/entry.filename
                        if entry.is_dir():p.mkdir(parents=True,exist_ok=True);continue
                        p.parent.mkdir(parents=True,exist_ok=True)
                        with z.open(entry) as src,p.open('xb') as dst:shutil.copyfileobj(src,dst)
                        p.chmod(0o755 if mode&0o111 else 0o644);files[entry.filename]=digest(p)
                (work/'.monado-tool.json').write_text(json.dumps({'archive_sha256':row['sha256'],'files':files},sort_keys=True)+'\n')
                os.rename(work,target)
            finally:
                if work.exists():shutil.rmtree(work)
        if receipt.is_symlink() or not receipt.is_file():raise ValueError('unowned build-tool directory')
        saved=json.loads(receipt.read_text())
        if saved.get('archive_sha256')!=row['sha256']:raise ValueError('tool receipt mismatch')
        # The archive is hash-locked; a mutable cache receipt is not authority
        # to omit or bless executable files. Revalidate against archive bytes.
        with zipfile.ZipFile(archive) as z:
            for entry in z.infolist():
                if entry.is_dir():continue
                rel=entry.filename;p=target/rel
                sha=hashlib.sha256(z.read(entry)).hexdigest()
                if p.is_symlink() or not p.is_file() or p.stat().st_size!=entry.file_size or digest(p)!=sha:
                    raise ValueError('build tool changed: '+rel)
        exe=target/row['binary']
        p=subprocess.run([str(exe),'--version'],stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=15,
                         env={'PATH':'/usr/bin:/bin','LANG':'C.UTF-8'})
        if p.returncode or row['version'] not in p.stdout:raise ValueError('build tool version check failed')
        result[row['name']]={'path':str(exe),'version':row['version'],'sha256':digest(exe),'archive_sha256':row['sha256']}
    (cache/'active-tools.json').write_text(json.dumps(result,indent=2)+'\n');return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.parse_args()
    try:print(json.dumps(setup(),indent=2))
    except (OSError,ValueError,subprocess.SubprocessError) as e:p.exit(1,str(e)+'\n')
