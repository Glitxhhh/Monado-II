"""Symbolize / disassemble RVAs of a built exe with its PDB (dbghelp via ctypes) and capstone.
usage: python tools/exe_sym.py build/xcde.exe <rva hex> [<rva hex> ...] [--dis <bytes>]"""
import ctypes, ctypes.wintypes as w, struct, sys, os

def pe_sections(data):
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    n = struct.unpack_from('<H', data, pe + 6)[0]
    opt = struct.unpack_from('<H', data, pe + 20)[0]
    base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
    secs = []
    for i in range(n):
        o = pe + 24 + opt + i * 40
        name = data[o:o + 8].rstrip(b'\0').decode()
        vsize, va, rsize, raw = struct.unpack_from('<IIII', data, o + 8)
        secs.append((name, va, vsize, raw, rsize))
    return base, secs

class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', w.ULONG), ('TypeIndex', w.ULONG), ('Reserved', ctypes.c_uint64 * 2), ('Index', w.ULONG),
                ('Size', w.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', w.ULONG), ('Value', ctypes.c_uint64),
                ('Address', ctypes.c_uint64), ('Register', w.ULONG), ('Scope', w.ULONG), ('Tag', w.ULONG),
                ('NameLen', w.ULONG), ('MaxNameLen', w.ULONG), ('Name', ctypes.c_char * 512)]

def main():
    exe = sys.argv[1]
    args = sys.argv[2:]
    dis = 0
    if '--dis' in args:
        i = args.index('--dis'); dis = int(args[i + 1], 0); args = args[:i] + args[i + 2:]
    data = open(exe, 'rb').read()
    base, secs = pe_sections(data)
    dh = ctypes.WinDLL('dbghelp')
    h = w.HANDLE(0x1234)
    dh.SymSetOptions(0x2 | 0x4)  # UNDNAME | DEFERRED_LOADS
    dh.SymInitializeW(h, ctypes.c_wchar_p(os.path.dirname(os.path.abspath(exe))), False)
    dh.SymLoadModuleExW.restype = ctypes.c_uint64
    mb = dh.SymLoadModuleExW(h, None, ctypes.c_wchar_p(os.path.abspath(exe)), None, ctypes.c_uint64(base), 0, None, 0)
    for a in args:
        rva = int(a, 16)
        si = SYMBOL_INFO(); si.SizeOfStruct = 88; si.MaxNameLen = 500  # sizeof(SYMBOL_INFO)
        disp = ctypes.c_uint64()
        ok = dh.SymFromAddr(h, ctypes.c_uint64(mb + rva), ctypes.byref(disp), ctypes.byref(si))
        print(f'{rva:x}: ' + (f'{si.Name.decode()} +{disp.value:#x} (size {si.Size:#x})' if ok else 'no symbol'))
        if dis:
            import capstone
            for name, va, vs, raw, rs in secs:
                if va <= rva < va + vs:
                    code = data[raw + rva - va: raw + rva - va + dis]
                    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
                    for ins in md.disasm(code, base + rva):
                        print(f'   {ins.address - base:x}  {ins.mnemonic} {ins.op_str}')
main()
