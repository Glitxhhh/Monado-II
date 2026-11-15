"""Native Windows ordinary-file leases for portable setup and reviewed updates.

Directory leases hold EVERY existing ancestor without FILE_SHARE_DELETE. Never
call Path.resolve() before entry: a reparse point must be rejected, not resolved.
Reads also deny leaf writes/deletes and validate bounded handle identities.

Atomic replacement is ONLY for caller-recognized, current-user-owned mutable
metadata, with a mandatory expected SHA256 and a named cooperating-writer mutex.
Win32 has no expected-file-ID rename CAS: hostile same-user leaf substitution in
the final close/rename window is not a security boundary claimed by this module.
No recursive removal or unknown-file cleanup operation is provided.
"""
from __future__ import annotations
import contextlib
import ctypes as C
from ctypes import wintypes as W
from dataclasses import dataclass
import hashlib
import ntpath
import os
from pathlib import Path
import re
import uuid

WINDOWS = os.name == "nt"
MAX_MEMORY_READ = 64 * 1024 * 1024
MAX_DEPTH = 256
_READ_ATTR, _READ_CONTROL, _DELETE = 0x80, 0x20000, 0x10000
_SHARE_READ, _SHARE_WRITE = 1, 2
_OPEN, _NEW = 3, 1
_REPARSE, _DIRECTORY = 0x400, 0x10
_BACKUP, _NOFOLLOW = 0x02000000, 0x00200000
_INVALID = C.c_void_p(-1).value

class FileGuardError(OSError):
    """An ordinary-file, ownership, identity or sharing boundary was rejected."""

class _FileTime(C.Structure):
    _fields_ = [("low", W.DWORD), ("high", W.DWORD)]
class _FileInfo(C.Structure):
    _fields_ = [("attrs", W.DWORD), ("created", _FileTime), ("accessed", _FileTime),
                ("written", _FileTime), ("volume", W.DWORD), ("size_high", W.DWORD),
                ("size_low", W.DWORD), ("links", W.DWORD), ("index_high", W.DWORD), ("index_low", W.DWORD)]

if WINDOWS:
    K = C.WinDLL("kernel32", use_last_error=True)
    A = C.WinDLL("advapi32", use_last_error=True)
    def _typed(dll, name, result, *arguments):
        function = getattr(dll, name); function.restype = result; function.argtypes = list(arguments); return function
    _typed(K, "CreateFileW", W.HANDLE, W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE)
    _typed(K, "CloseHandle", W.BOOL, W.HANDLE)
    _typed(K, "GetFileInformationByHandle", W.BOOL, W.HANDLE, C.POINTER(_FileInfo))
    _typed(K, "GetFileType", W.DWORD, W.HANDLE)
    _typed(K, "GetFullPathNameW", W.DWORD, W.LPCWSTR, W.DWORD, W.LPWSTR, C.c_void_p)
    _typed(K, "CreateDirectoryW", W.BOOL, W.LPCWSTR, C.c_void_p)
    _typed(K, "WriteFile", W.BOOL, W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p)
    _typed(K, "FlushFileBuffers", W.BOOL, W.HANDLE)
    _typed(K, "MoveFileExW", W.BOOL, W.LPCWSTR, W.LPCWSTR, W.DWORD)
    _typed(K, "SetFileInformationByHandle", W.BOOL, W.HANDLE, C.c_int, C.c_void_p, W.DWORD)
    _typed(K, "GetCurrentProcess", W.HANDLE)
    _typed(K, "LocalFree", C.c_void_p, C.c_void_p)
    _typed(K, "CreateMutexW", W.HANDLE, C.c_void_p, W.BOOL, W.LPCWSTR)
    _typed(K, "WaitForSingleObject", W.DWORD, W.HANDLE, W.DWORD)
    _typed(K, "ReleaseMutex", W.BOOL, W.HANDLE)
    _typed(A, "OpenProcessToken", W.BOOL, W.HANDLE, W.DWORD, C.POINTER(W.HANDLE))
    _typed(A, "GetTokenInformation", W.BOOL, W.HANDLE, C.c_int, C.c_void_p, W.DWORD, C.POINTER(W.DWORD))
    _typed(A, "GetSecurityInfo", W.DWORD, W.HANDLE, C.c_int, W.DWORD, C.POINTER(C.c_void_p),
           C.c_void_p, C.c_void_p, C.c_void_p, C.POINTER(C.c_void_p))
    _typed(A, "IsValidSid", W.BOOL, C.c_void_p)
    _typed(A, "GetLengthSid", W.DWORD, C.c_void_p)
    _typed(A, "EqualSid", W.BOOL, C.c_void_p, C.c_void_p)

def _require_windows():
    if not WINDOWS:
        raise OSError("Native Windows file guards require Win32; there is no production fallback")

def _failure(action, path, error=None):
    error = C.get_last_error() if error is None else error
    raise FileGuardError(error, f"{action}: {C.FormatError(error).strip()}", os.fspath(path))

def _component(name):
    if not name or name in (".", "..") or name[-1:] in (" ", "."):
        raise ValueError("Ambiguous Windows path component")
    if any(ord(c) < 32 or c in '<>:"/\\|?*' for c in name):
        raise ValueError("Invalid Windows path component")
    if re.fullmatch(r"CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³]", name.split(".", 1)[0], re.I):
        raise ValueError("Windows device names are not regular paths")
    name.encode("utf-16-le", "strict")
    return name

def canonical_path(path):
    _require_windows()
    value = os.fspath(path)
    if not isinstance(value, str) or not value or value.startswith(("\\\\?\\", "\\\\.\\")):
        raise ValueError("Expected an ordinary absolute Unicode Windows path")
    value = value.replace("/", "\\")
    drive, tail = ntpath.splitdrive(value)
    if not drive or not tail.startswith("\\"):
        raise ValueError("Absolute drive or UNC path required")
    if drive.startswith("\\\\"):
        parts = drive[2:].split("\\")
        if len(parts) != 2: raise ValueError("UNC server/share required")
        for name in parts: _component(name)
    elif not re.fullmatch(r"[A-Za-z]:", drive):
        raise ValueError("Unsupported Windows path namespace")
    parts = tail[1:].split("\\")
    if parts and parts[-1] == "": parts.pop()
    for name in parts: _component(name)
    if len(parts) > MAX_DEPTH: raise ValueError("Path exceeds guard depth")
    length = K.GetFullPathNameW(value, 0, None, None)
    if not length or length > 32767: _failure("canonical path", value)
    output = C.create_unicode_buffer(length + 1)
    got = K.GetFullPathNameW(value, len(output), output, None)
    if not got or got >= len(output): _failure("canonical path", value)
    result = output.value
    return result if result == drive + "\\" else result.rstrip("\\")

def _native(path):
    return "\\\\?\\UNC\\" + path[2:] if path.startswith("\\\\") else "\\\\?\\" + path

def _chain(path):
    drive, tail = ntpath.splitdrive(path)
    current = drive + "\\"; yield current
    for name in filter(None, tail.split("\\")):
        current = ntpath.join(current, name); yield current

@dataclass(frozen=True)
class Identity:
    volume: int
    file_id: int
    size: int
    written: int
    links: int
    attributes: int

def _info(handle, path, directory):
    info = _FileInfo()
    if not K.GetFileInformationByHandle(handle, C.byref(info)): _failure("file identity", path)
    if K.GetFileType(handle) != 1 or info.attrs & _REPARSE or bool(info.attrs & _DIRECTORY) != directory:
        raise FileGuardError(f"Nonordinary/reparse path rejected: {path}")
    if not directory and info.links != 1:
        raise FileGuardError(f"Hard-linked file rejected: {path}")
    return Identity(info.volume, (info.index_high << 32) | info.index_low,
                    (info.size_high << 32) | info.size_low, (info.written.high << 32) | info.written.low,
                    info.links, info.attrs)

def _user_sid():
    token = W.HANDLE()
    if not A.OpenProcessToken(K.GetCurrentProcess(), 8, C.byref(token)): _failure("process token", "TokenUser")
    try:
        size = W.DWORD()
        A.GetTokenInformation(token, 1, None, 0, C.byref(size))
        if not size.value or size.value > 65536: raise FileGuardError("Unbounded TokenUser identity")
        buffer = C.create_string_buffer(size.value)
        if not A.GetTokenInformation(token, 1, buffer, size, C.byref(size)): _failure("TokenUser", "process")
        sid = C.cast(buffer, C.POINTER(C.c_void_p))[0]
        if not A.IsValidSid(sid): raise FileGuardError("Invalid current-user SID")
        length = A.GetLengthSid(sid)
        if not 8 <= length <= 68: raise FileGuardError("Unbounded current-user SID")
        return C.string_at(sid, length)
    finally: K.CloseHandle(token)

def _owned(handle, path):
    sid, descriptor = C.c_void_p(), C.c_void_p()
    status = A.GetSecurityInfo(handle, 1, 1, C.byref(sid), None, None, None, C.byref(descriptor))
    if status: _failure("owner query", path, status)
    try:
        user = C.create_string_buffer(_user_sid())
        if not sid or not A.IsValidSid(sid) or not A.EqualSid(sid, user):
            raise FileGuardError(f"Current TokenUser does not own path: {path}")
    finally:
        if descriptor: K.LocalFree(descriptor)

def _open(path, *, directory, owner=False, access=_READ_ATTR, disposition=_OPEN, share=3):
    handle = K.CreateFileW(_native(path), access | (_READ_CONTROL if owner else 0), share, None,
                           disposition, _NOFOLLOW | (_BACKUP if directory else 0), None)
    if handle == _INVALID: _failure("open", path)
    try:
        identity = _info(handle, path, directory)
        if owner: _owned(handle, path)
        return handle, identity
    except BaseException:
        K.CloseHandle(handle); raise

@contextlib.contextmanager
def _mutex(path, timeout_ms=30000):
    if not isinstance(timeout_ms,int) or not 0 <= timeout_ms <= 30000: raise ValueError("Mutex timeout must be 0..30000 ms")
    digest = hashlib.sha256(_user_sid() + path.casefold().encode("utf-16-le")).hexdigest()
    handle = K.CreateMutexW(None, False, "Local\\MonadoFileGuard-" + digest)
    if not handle: _failure("path mutex", path)
    acquired = False
    try:
        status = K.WaitForSingleObject(handle, timeout_ms)
        if status not in (0, 0x80):
            if status == 258: raise TimeoutError(f"File guard writer is busy: {path}")
            _failure("wait path mutex", path)
        acquired = True; yield
    finally:
        if acquired: K.ReleaseMutex(handle)
        K.CloseHandle(handle)

class GuardedDirectory:
    def __init__(self, path, owner=False):
        self.path = Path(canonical_path(path)); self._handles = []; self._closed = False
        try:
            paths = list(_chain(str(self.path)))
            for index, entry in enumerate(paths):
                handle, _ = _open(entry, directory=True, owner=owner and index == len(paths)-1)
                self._handles.append(handle)
        except BaseException:
            self.close(); raise
    def close(self):
        while self._handles: K.CloseHandle(self._handles.pop())
        self._closed = True
    def _path(self, relative):
        if self._closed: raise ValueError("Directory lease is closed")
        value = os.fspath(relative)
        if not isinstance(value, str): raise ValueError("Unicode relative name required")
        value = value.replace("/", "\\")
        if ntpath.splitdrive(value)[0] or value.startswith("\\"): raise ValueError("Relative name required")
        for name in value.split("\\"): _component(name)
        return ntpath.join(str(self.path), value)
    @contextlib.contextmanager
    def child(self, relative, create=False, owner=False):
        target = self._path(relative)
        parts = ntpath.relpath(target, str(self.path)).split("\\")
        with contextlib.ExitStack() as stack:
            current = self
            for name in parts:
                path = current._path(name)
                if create and not K.CreateDirectoryW(_native(path), None):
                    error = C.get_last_error()
                    if error != 183: _failure("create directory", path, error)
                current = stack.enter_context(locked_directory(path, owner=owner))
            yield current
    @contextlib.contextmanager
    def open_regular(self, relative, max_bytes=None, owner=False):
        import msvcrt
        path = self._path(relative)
        with locked_directory(ntpath.dirname(path)):
            handle, before = _open(path, directory=False, owner=owner, access=0x80000000 | _READ_ATTR, share=_SHARE_READ)
            try:
                if max_bytes is not None and (not isinstance(max_bytes,int) or max_bytes < 0 or before.size > max_bytes):
                    raise FileGuardError(f"File exceeds bounded read: {path}")
                fd = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY); handle = None
                with os.fdopen(fd, "rb", closefd=True) as stream:
                    yield stream
                    after = _info(msvcrt.get_osfhandle(stream.fileno()), path, False)
                    if after != before: raise FileGuardError(f"File changed during read: {path}")
            finally:
                if handle is not None: K.CloseHandle(handle)
    def read_regular(self, relative, max_bytes=MAX_MEMORY_READ, owner=False):
        if not isinstance(max_bytes,int) or not 0 <= max_bytes <= MAX_MEMORY_READ:
            raise ValueError("Memory read cap must be between 0 and 64 MiB; stream larger files")
        with self.open_regular(relative, max_bytes=max_bytes, owner=owner) as stream:
            data = stream.read(max_bytes + 1)
            if len(data)>max_bytes: raise FileGuardError("Read exceeded cap")
            return data
    def _write_new(self, path, data, owner):
        if not isinstance(data,(bytes,bytearray,memoryview)): raise TypeError("Write requires authored bytes")
        handle, _ = _open(path, directory=False, owner=owner, access=0x40000000 | _READ_ATTR | _DELETE,
                          disposition=_NEW, share=_SHARE_READ)
        success = False
        try:
            view = memoryview(data)
            for offset in range(0,len(view),1024*1024):
                block = bytes(view[offset:offset+1024*1024]); buffer = C.create_string_buffer(block); written = W.DWORD()
                if not K.WriteFile(handle,buffer,len(block),C.byref(written),None) or written.value!=len(block):
                    _failure("write new file",path)
            if not K.FlushFileBuffers(handle): _failure("flush new file",path)
            identity = _info(handle,path,False)
            if owner: _owned(handle,path)
            success = True
        finally:
            if not success:
                # Only the exact exclusively-created handle is eligible for cleanup.
                disposition = C.c_ubyte(True)
                K.SetFileInformationByHandle(handle,4,C.byref(disposition),C.sizeof(disposition))
            K.CloseHandle(handle)
        check, final = _open(path,directory=False,owner=owner,share=_SHARE_READ)
        K.CloseHandle(check)
        if final.volume!=identity.volume or final.file_id!=identity.file_id or final.size!=identity.size:
            raise FileGuardError("Created file identity changed after close")
        return final
    def write_new(self, relative, data, owner=True):
        path = self._path(relative)
        with locked_directory(ntpath.dirname(path)), _mutex(path):
            return self._write_new(path,data,owner)
    def _atomic(self, relative, data, expected_sha256, owner):
        path = self._path(relative)
        with locked_directory(ntpath.dirname(path)) as parent, _mutex(path):
            previous = None
            if expected_sha256 is not None:
                if not owner or not re.fullmatch(r"[0-9a-fA-F]{64}",expected_sha256):
                    raise ValueError("Owned replacement requires expected SHA256")
                with parent.open_regular(ntpath.basename(path),max_bytes=MAX_MEMORY_READ,owner=True) as stream:
                    previous = _info(__import__('msvcrt').get_osfhandle(stream.fileno()),path,False)
                    actual = hashlib.file_digest(stream,'sha256').hexdigest()
                    if actual.lower()!=expected_sha256.lower(): raise FileGuardError("Expected file hash differs")
            temporary = ntpath.join(ntpath.dirname(path),'.monado-write-'+uuid.uuid4().hex+'.tmp')
            identity = parent._write_new(temporary,data,owner)
            # No automatic cleanup by path: failed publication preserves its owned
            # temporary for precise recovery rather than removing a substituted name.
            check, now = _open(temporary,directory=False,owner=owner,share=_SHARE_READ)
            K.CloseHandle(check)
            if now!=identity: raise FileGuardError("Staged file identity changed")
            if previous is not None:
                check, now = _open(path,directory=False,owner=True,share=_SHARE_READ)
                K.CloseHandle(check)
                if now!=previous: raise FileGuardError("Replacement target identity changed")
            flags = 8 | (1 if previous is not None else 0)
            if not K.MoveFileExW(_native(temporary),_native(path),flags): _failure("publish file",path)
            check, final = _open(path,directory=False,owner=owner,share=_SHARE_READ)
            K.CloseHandle(check)
            if final.volume!=identity.volume or final.file_id!=identity.file_id or final.size!=identity.size:
                raise FileGuardError("Published file identity differs")
            return final
    def atomic_write_new(self, relative, data, owner=True):
        return self._atomic(relative,data,None,owner)
    def atomic_replace(self, relative, data, expected_sha256, owner=True):
        if expected_sha256 is None: raise ValueError("Replacement requires expected SHA256")
        return self._atomic(relative,data,expected_sha256,owner)

@contextlib.contextmanager
def locked_directory(path, owner=False):
    guard = GuardedDirectory(path,owner=owner)
    try: yield guard
    finally: guard.close()

@contextlib.contextmanager
def open_regular(path, max_bytes=None, owner=False):
    absolute = canonical_path(path)
    with locked_directory(ntpath.dirname(absolute)) as directory:
        with directory.open_regular(ntpath.basename(absolute),max_bytes=max_bytes,owner=owner) as stream:
            yield stream

def read_regular(path, max_bytes=MAX_MEMORY_READ, owner=False):
    absolute = canonical_path(path)
    with locked_directory(ntpath.dirname(absolute)) as directory:
        return directory.read_regular(ntpath.basename(absolute),max_bytes=max_bytes,owner=owner)


@contextlib.contextmanager
def installation_lease(path, timeout_ms=0):
    """Cooperating installation/build writer lock; caller retains directory guards.

    Zero timeout reports an already active operation immediately. The same
    canonical installation or registry identity must be used by every writer.
    """
    with _mutex(canonical_path(path), timeout_ms=timeout_ms):
        yield
