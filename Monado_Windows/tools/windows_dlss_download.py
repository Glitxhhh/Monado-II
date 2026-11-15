#!/usr/bin/env python3
"""Download the pinned official SDK for explicit license review, never activate it.

Only Download is authorized here. The native frontend must separately display the
exact extracted DLSS license and receive Use before existing provider staging runs.
Files stay private under mutable builder state and never enter a public payload.
"""
from __future__ import annotations
import hashlib
import json
import re
import stat
import time
import urllib.parse
import urllib.request
import uuid
import zipfile

PIN = {
    'version':'2.14.16',
    'url':'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip',
    'bytes':276016170,
    'sha256':'32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b',
}
MEMBERS = {
    'bin/x64/sl.interposer.dll':'sl.interposer.dll',
    'bin/x64/sl.common.dll':'sl.common.dll',
    'bin/x64/sl.dlss.dll':'sl.dlss.dll',
    'bin/x64/nvngx_dlss.dll':'nvngx_dlss.dll',
    'bin/x64/NvLowLatencyVk.dll':'NvLowLatencyVk.dll',
    'bin/x64/nvngx_dlss.license.txt':'nvngx_dlss.license.txt',
    'bin/x64/reflex.license.txt':'reflex.license.txt',
    'license.txt':'LICENSE.txt',
}
MAX_ARCHIVE=512*1024*1024
MAX_DLL=128*1024*1024
MAX_LICENSE=1024*1024


def checked_zip(archive):
    """Validate the entire name table before selecting production files only."""
    entries=archive.infolist()
    if not 1<=len(entries)<=5000 or sum(m.file_size for m in entries)>2*1024**3:
        raise ValueError('Official SDK archive inventory exceeds its bound')
    aliases=set();selected={}
    for member in entries:
        name=member.orig_filename;parts=name.rstrip('/').split('/')
        if (name!=member.filename or not name or len(name)>2048 or '\\' in name or ':' in name or name.startswith('/') or
                any(p in ('','.','..') or p!=p.rstrip(' .') or
                    p.split('.')[0].casefold() in {'con','prn','aux','nul'}|
                    {prefix+str(n) for prefix in ('com','lpt') for n in range(1,10)} or
                    any(ord(c)<32 for c in p) for p in parts) or
                stat.S_ISLNK(member.external_attr>>16) or member.flag_bits&1):
            raise ValueError('Unsafe member in official SDK archive')
        folded=name.rstrip('/').casefold()
        if folded in aliases:raise ValueError('Duplicate or case-aliased SDK member')
        aliases.add(folded)
        if name in MEMBERS:
            limit=MAX_DLL if name.endswith('.dll') else MAX_LICENSE
            if member.is_dir() or not 0<member.file_size<=limit:
                raise ValueError('Missing or oversized SDK provider/license member')
            selected[name]=member
    if set(selected)!=set(MEMBERS):raise ValueError('Official SDK lacks the complete production DLSS files and licenses')
    return selected


def official_response():
    response=urllib.request.urlopen(urllib.request.Request(PIN['url'],headers={'User-Agent':'Monado-II-Windows-Development'}),timeout=15)
    final=urllib.parse.urlsplit(response.geturl())
    if final.scheme!='https' or final.hostname not in {'github.com','release-assets.githubusercontent.com','objects.githubusercontent.com'} or final.username or final.password:
        response.close();raise ValueError('Official SDK download left the checked HTTPS hosts')
    return response


def prepare(state_guard,*,checkpoint=lambda:None,progress=lambda *args:None,archive_source=None,_pin=PIN,_response=official_response):
    """Return a reviewable unselected source; never change registry or runtime.

    archive_source is a guarded local stream for offline inspection/testing.
    _pin/_response are authored-test injection boundaries, never CLI/network input.
    """
    if not 0<_pin['bytes']<=MAX_ARCHIVE or not re.fullmatch(r'[0-9a-f]{64}',_pin['sha256']):
        raise ValueError('Invalid pinned SDK identity')
    leaf='dlss-download-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8]
    with state_guard.child(leaf,create=True,owner=True) as attempt:
        checkpoint();progress('download',0,_pin['bytes'],'Downloading official NVIDIA Streamline '+_pin['version']+' for license review')
        data=bytearray();source=archive_source if archive_source is not None else _response()
        try:
            while True:
                checkpoint();block=source.read(min(1024*1024,_pin['bytes']+1-len(data)))
                if not block:break
                data.extend(block)
                if len(data)>_pin['bytes']:raise ValueError('Official SDK download exceeds pinned size')
                progress('download',len(data),_pin['bytes'],'Downloading official SDK; no provider selected')
        finally:
            if archive_source is None:source.close()
        if len(data)!=_pin['bytes'] or hashlib.sha256(data).hexdigest()!=_pin['sha256']:
            raise ValueError('Official SDK size/SHA256 differs from the pinned release')
        checkpoint();attempt.write_new('official-sdk.zip',data,owner=True);del data
        files={}
        with attempt.open_regular('official-sdk.zip',max_bytes=MAX_ARCHIVE,owner=True) as stream,zipfile.ZipFile(stream) as archive:
            selected=checked_zip(archive)
            with attempt.child('review-sdk',create=True,owner=True) as destination:
                for index,(name,target) in enumerate(MEMBERS.items(),1):
                    checkpoint();content=archive.read(selected[name])
                    if len(content)!=selected[name].file_size:raise ValueError('SDK member length changed')
                    if target.endswith('.txt'):
                        try:content.decode('utf-8-sig')
                        except UnicodeDecodeError:
                            # The pinned Reflex agreement is distributed as
                            # Windows-1252. Validate text without rewriting its
                            # exact licensed bytes or recorded SHA256.
                            if target!='reflex.license.txt':raise
                            content.decode('cp1252')
                        if b'\0' in content:raise ValueError('SDK license contains unsupported text')
                    destination.write_new(target,content,owner=True)
                    files[target]={'bytes':len(content),'sha256':hashlib.sha256(content).hexdigest()}
                    progress('extract',index,len(MEMBERS),'Preparing exact production files and licenses for review')
                checkpoint()
                receipt={'schema':1,'status':'downloaded-unselected','archive':dict(_pin),'files':files,
                    'authorization':'Download only; explicit native license Use is required before staging or execution.'}
                destination.write_new('download-receipt.json',(json.dumps(receipt,indent=2,sort_keys=True)+'\n').encode('utf-8'),owner=True)
                return str(destination.path)
