"""Owned suspended-start native preparation children; no shell or ambient DLL path."""
from __future__ import annotations
import ctypes as C
from ctypes import wintypes as W
import os, subprocess, time
from pathlib import Path

class Cancelled(RuntimeError): pass

def run(argv, *, cwd, log, environment, timeout, checkpoint=lambda:None,output=None,max_log_bytes=64*1024*1024):
    """Assign the suspended child to a kill-on-close job before its first thread runs."""
    if os.name!='nt': raise OSError('Native Windows process supervision required')
    # Capture children intentionally use relative shim/capture paths. Unlike
    # compilation, moving their cwd to a short directory would change inputs
    # and state. Refuse this optional attempt before a log or child is created.
    if len(str(cwd).encode('utf-16-le'))//2+1>=260:
        raise ValueError('Optional native FSR4 preparation needs a working directory shorter than 260 characters; choose a shorter installation location. Existing work and selected models are preserved')
    import msvcrt
    K=C.WinDLL('kernel32',use_last_error=True)
    class SI(C.Structure):
        _fields_=[('cb',W.DWORD),('reserved',W.LPWSTR),('desktop',W.LPWSTR),('title',W.LPWSTR),
            ('x',W.DWORD),('y',W.DWORD),('xs',W.DWORD),('ys',W.DWORD),('xc',W.DWORD),('yc',W.DWORD),
            ('fill',W.DWORD),('flags',W.DWORD),('show',W.WORD),('reserved2',W.WORD),('bytes',C.c_void_p),
            ('stdin',W.HANDLE),('stdout',W.HANDLE),('stderr',W.HANDLE)]
    class SX(C.Structure): _fields_=[('si',SI),('attributes',C.c_void_p)]
    class PI(C.Structure): _fields_=[('process',W.HANDLE),('thread',W.HANDLE),('pid',W.DWORD),('tid',W.DWORD)]
    class Basic(C.Structure):
        _fields_=[('pt',C.c_int64),('jt',C.c_int64),('flags',W.DWORD),('min',C.c_size_t),('max',C.c_size_t),
                  ('count',W.DWORD),('affinity',C.c_size_t),('priority',W.DWORD),('schedule',W.DWORD)]
    class Limits(C.Structure):
        _fields_=[('basic',Basic),('io',C.c_uint64*6),('pm',C.c_size_t),('jm',C.c_size_t),('pp',C.c_size_t),('pj',C.c_size_t)]
    def api(name,args,result=W.BOOL):
        f=getattr(K,name);f.argtypes=args;f.restype=result;return f
    close=api('CloseHandle',[W.HANDLE]); createjob=api('CreateJobObjectW',[C.c_void_p,W.LPCWSTR],W.HANDLE)
    setjob=api('SetInformationJobObject',[W.HANDLE,C.c_int,C.c_void_p,W.DWORD])
    assign=api('AssignProcessToJobObject',[W.HANDLE,W.HANDLE]);resume=api('ResumeThread',[W.HANDLE],W.DWORD)
    wait=api('WaitForSingleObject',[W.HANDLE,W.DWORD],W.DWORD)
    exitcode=api('GetExitCodeProcess',[W.HANDLE,C.POINTER(W.DWORD)])
    terminate=api('TerminateProcess',[W.HANDLE,W.UINT]); inherit=api('SetHandleInformation',[W.HANDLE,W.DWORD,W.DWORD])
    init=api('InitializeProcThreadAttributeList',[C.c_void_p,W.DWORD,W.DWORD,C.POINTER(C.c_size_t)])
    update=api('UpdateProcThreadAttribute',[C.c_void_p,W.DWORD,C.c_size_t,C.c_void_p,C.c_size_t,C.c_void_p,C.c_void_p])
    delete=api('DeleteProcThreadAttributeList',[C.c_void_p],None)
    create=api('CreateProcessW',[W.LPCWSTR,W.LPWSTR,C.c_void_p,C.c_void_p,W.BOOL,W.DWORD,C.c_void_p,W.LPCWSTR,C.POINTER(SX),C.POINTER(PI)])
    job=createjob(None,None);pi=PI();attributes=None;initialized=False;started=time.monotonic()
    try:
        if not job: raise C.WinError(C.get_last_error())
        limits=Limits();limits.basic.flags=0x2000
        if not setjob(job,9,C.byref(limits),C.sizeof(limits)):raise C.WinError(C.get_last_error())
        if type(max_log_bytes) is not int or not 1<=max_log_bytes<=64*1024*1024:raise ValueError('Invalid child log bound')
        with open(log,'xb',buffering=0) as logfile,open(log,'rb',buffering=0) as observer,open(os.devnull,'rb',buffering=0) as null:
            identity=os.fstat(logfile.fileno());observed=os.fstat(observer.fileno())
            if (identity.st_dev,identity.st_ino)!=(observed.st_dev,observed.st_ino):raise OSError('Created child log identity changed')
            pending=b''
            def drain(final=False):
                nonlocal pending
                if os.fstat(logfile.fileno()).st_size>max_log_bytes:raise RuntimeError('Preparation child exceeded log size limit')
                while True:
                    block=observer.read(65536)
                    if not block:break
                    if output is None:continue
                    pending+=block
                    while b'\n' in pending or len(pending)>16384:
                        end=pending.find(b'\n',0,16385)
                        end=end+1 if end>=0 else 16384
                        line,pending=pending[:end],pending[end:];output(line.decode('utf8',errors='replace').rstrip('\r\n'))
                if final and pending and output is not None:output(pending.decode('utf8',errors='replace'));pending=b''
            handles=(W.HANDLE*2)(msvcrt.get_osfhandle(null.fileno()),msvcrt.get_osfhandle(logfile.fileno()))
            for handle in handles:
                if not inherit(handle,1,1):raise C.WinError(C.get_last_error())
            size=C.c_size_t();init(None,1,0,C.byref(size));attributes=C.create_string_buffer(size.value)
            if not init(attributes,1,0,C.byref(size)):raise C.WinError(C.get_last_error())
            initialized=True
            if not update(attributes,0,0x20002,handles,C.sizeof(handles),None,None):raise C.WinError(C.get_last_error())
            startup=SX();startup.si.cb=C.sizeof(startup);startup.si.flags=0x100
            startup.si.stdin=handles[0];startup.si.stdout=startup.si.stderr=handles[1];startup.attributes=C.cast(attributes,C.c_void_p)
            env='\0'.join(k+'='+v for k,v in sorted(environment.items(),key=lambda x:x[0].upper()))+'\0\0'
            envbuffer=C.create_unicode_buffer(env);command=C.create_unicode_buffer(subprocess.list2cmdline([str(v) for v in argv]))
            checkpoint()
            if not create(str(argv[0]),command,None,None,True,0x08000000|0x4|0x400|0x80000,envbuffer,str(cwd),C.byref(startup),C.byref(pi)):raise C.WinError(C.get_last_error())
            if not assign(job,pi.process):raise C.WinError(C.get_last_error())
            if resume(pi.thread)==0xffffffff:raise C.WinError(C.get_last_error())
            close(pi.thread);pi.thread=None
            while wait(pi.process,100)==0x102:
                drain()
                checkpoint()
                if time.monotonic()-started>timeout:raise TimeoutError('Preparation child timed out')
            drain(final=True)
            code=W.DWORD()
            if not exitcode(pi.process,C.byref(code)):raise C.WinError(C.get_last_error())
            return {'argv':[str(v) for v in argv],'exit':code.value,'seconds':time.monotonic()-started,'log':str(log)}
    finally:
        if pi.thread:close(pi.thread)
        if pi.process:
            if wait(pi.process,0)==0x102:terminate(pi.process,130)
            if job:close(job);job=None
            wait(pi.process,10000);close(pi.process)
        if initialized:delete(attributes)
        if job:close(job)
