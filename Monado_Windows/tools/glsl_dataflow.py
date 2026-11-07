"""Symbolic dataflow of translated vertex shaders (build/glsl/<addr>_vs.glsl, from LARECOMP_SAVE_MSL=1):
expression trees for gl_Position (and varyings), so two shaders that must produce bit-identical positions (a depth
pre-pass and the colour pass testing EQUAL, T136) can be compared structurally.

  python tools/glsl_dataflow.py <a_vs.glsl> <b_vs.glsl> [--show]

Straight-line code only (switch/for wrappers are ignored). Mul/add operands are sorted (commutative); fma(a,b,c) keeps
c apart; as_u/as_f are identities. Prints, per gl_Position component, whether both shaders build the same tree."""
import re
import sys


class S:
    __slots__ = ("op", "args")

    def __init__(self, op, *args):
        self.op, self.args = op, args

    def key(self):
        return (self.op,) + tuple(a.key() if isinstance(a, S) else a for a in self.args)

    def __repr__(self):
        return self.op if not self.args else "%s(%s)" % (self.op, ",".join(map(repr, self.args)))

    def __mul__(a, b): return S("mul", *sorted([a, b], key=lambda s: repr(s)))
    __rmul__ = __mul__
    def __add__(a, b): return S("add", *sorted([a, b], key=lambda s: repr(s)))
    __radd__ = __add__
    def __neg__(a): return S("neg", a)
    def __sub__(a, b): return S("sub", a, b)
    def __truediv__(a, b): return S("div", a, b)


class Leaf(S):
    def __init__(self, name): S.__init__(self, name)


class Idx:
    def __init__(self, name): self.name = name
    def __getitem__(self, i):
        n = "%s[%d]" % (self.name, i)
        return Idx(n) if self.name.startswith("cb") and "[" not in self.name else Leaf(n)


def fma(a, b, c): return S("fma", *sorted([a, b], key=lambda s: repr(s)), c)
def func(n): return lambda *a: S(n, *a)


def run(path):
    regs, out = {}, {}
    env = {"as_u": lambda x: x, "as_f": lambda x: x, "fma": fma, "log2": func("log2"), "exp2": func("exp2"),
           "inversesqrt": func("rsq"), "saturate": func("sat"), "sqrt": func("sqrt"), "min": func("min"), "max": func("max"),
           "float": lambda x: x, "int": lambda x: x}
    for line in open(path):
        m = re.match(r"\s*(r\d+|gl_Position\[\d\]|v\d+\[\d\])\s*=\s*(.*);\s*(//.*)?$", line)
        if not m or "pc =" in line or "uint pc" in line:
            continue
        dst, rhs = m.group(1), m.group(2)
        rhs = re.sub(r"(\d+\.\d+)f", r"Leaf('\1')", rhs)
        rhs = re.sub(r"(0x[0-9A-Fa-f]+)u", r"Leaf('\1')", rhs)
        rhs = re.sub(r"\b(a\d+)\b", r"Idx('\1')", rhs)
        rhs = re.sub(r"\b(cb\d+)\b", r"Idx('\1')", rhs)
        rhs = re.sub(r"\b(r\d+)\b", lambda mm: "R['%s']" % mm.group(1), rhs)
        class RD(dict):
            def __missing__(self, k): return Leaf("undef_" + k)
        env["R"] = regs if isinstance(regs, RD) else RD(regs)
        regs = env["R"]
        try:
            v = eval(rhs, {"__builtins__": {}}, dict(env, Idx=Idx, Leaf=Leaf))
        except Exception as e:
            v = Leaf("?" + rhs[:40])
        if dst.startswith("r"):
            regs[dst] = v
        else:
            out[dst] = v
    return out


if __name__ == "__main__":
    a, b = run(sys.argv[1]), run(sys.argv[2])
    for k in sorted(set(a) | set(b)):
        if not k.startswith("gl_Position"):
            continue
        ta, tb = a.get(k), b.get(k)
        same = ta is not None and tb is not None and ta.key() == tb.key()
        print(k, "SAME" if same else "DIFFERENT")
        if "--show" in sys.argv:
            print("  A:", repr(ta)[:600]); print("  B:", repr(tb)[:600])
