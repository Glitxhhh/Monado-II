"""Exact source/notice qualification for a public experimental setup artifact.

This checks the redistributed component bytes against their corresponding source
companions. It does not publish anything or assert gameplay/image-quality success.
"""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import re
import sys

PORT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PORT.parent/'tools'))
sys.path.insert(0,str(PORT/'tools'))

def identity(raw):
    return {'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}

def public_source(root,manifest_path,version):
    from publication_guard import verify_tree
    problems=verify_tree(Path(root))
    if problems:raise ValueError('Public Windows source failed publication policy: '+'; '.join(problems))
    if not isinstance(version,str) or not re.fullmatch(r'\d+\.\d+\.\d+',version):
        raise ValueError('An independent numeric Windows release version is required')
    declaration=json.loads(Path(manifest_path).read_bytes())
    manifest=json.loads((Path(root)/'publication-manifest.json').read_bytes())
    if (declaration.get('version')!=version or manifest.get('release_version')!=version or
            manifest.get('windows_implementation')!='native-windows' or
            manifest.get('platform')!='windows' or manifest.get('source_only') is not True or
            manifest.get('release_channel')!='experimental'):
        raise ValueError('Public setup must embed the exact versioned experimental Windows release source')
    return declaration

def qualify(members,*,version,native_report,python_report,frontend,relink_output):
    import windows_release_sources as native
    import windows_python_release_sources as python
    table={name:identity(member.read()) for name,member in members.items()}
    static={item['dependency_path']:{key:item[key] for key in ('bytes','sha256')}
            for item in frontend['static_libraries']}
    if set(static)!={'lib/libSDL3.a','lib/libiconv.a'} or frontend.get('input_hashes_rechecked') is not True:
        raise ValueError('Public frontend lacks its exact retained static-library/source qualification')
    sealed=native.seal_relink(Path(native_report),Path(frontend['relink_recipe_path']).parent,Path(relink_output))
    native_value=native.verify_report(sealed,payload_files=table,release_version=version,
        static_files=static,relink_root=Path(frontend['relink_recipe_path']).parent)
    python_value=python.verify_report(Path(python_report),payload_files=table,release_version=version)
    notices={}
    for report,path in ((native_value,Path(sealed)),(python_value,Path(python_report))):
        for target,item in report['notices'].items():
            if (not target.startswith(('licenses/native/','licenses/python/')) or
                    any(p in ('','.','..') for p in target.split('/')) or '\\' in target):
                raise ValueError('Invalid release notice target')
            source=path.parent/item['path'];raw=source.read_bytes()
            if identity(raw)!={k:item[k] for k in ('bytes','sha256')}:
                raise ValueError('Qualified dependency notice changed')
            if target in notices and notices[target]!=raw:raise ValueError('Conflicting dependency notices')
            notices[target]=raw
    if not notices:raise ValueError('Public setup has no verified third-party notices')
    notice_index=(
        'Monado II - Windows Experimental '+version+'\n\n'
        'Bundled Python tools and native development/runtime dependencies retain\n'
        'their individual licenses below. Corresponding source, package build\n'
        'recipes, and native frontend relink objects accompany the setup EXE in\n'
        'the same release. No game code, game files, saves, private graphics SDK\n'
        'binaries or prepared model assets are included.\n\n'+
        '\n'.join(sorted(notices))+'\n').encode('utf8')
    notices['licenses/README.txt']=notice_index
    result={'schema':1,'release_version':version,'native_source_report':str(sealed),
        'native_source_report_sha256':hashlib.sha256(Path(sealed).read_bytes()).hexdigest(),
        'python_source_report':str(python_report),
        'python_source_report_sha256':hashlib.sha256(Path(python_report).read_bytes()).hexdigest(),
        'native_payload_subset_sha256':native_value['binary_inputs_sha256'],
        'python_payload_subset_sha256':python_value['binary_inputs_sha256'],
        'notice_files':{name:identity(raw) for name,raw in notices.items()},
        'static_inputs':static,'frontend_relink_verified':True,'gameplay_qualification':False}
    return result,notices
