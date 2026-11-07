#!/usr/bin/env python3
"""Generate docs/hle_audit.md (Art. VI, FR-065): every host-side (HLE) piece of the runtime with its class.

  python tools/hle_audit.py            write docs/hle_audit.md
  python tools/hle_audit.py --check    exit 1 if the committed file is out of date (used by the unit tests / CI)

Inventory (scanned from runtime/): SVCs (kernel.c svc_impl), IPC services (SERVICE(...) and the commands their default
branch reports through diag_unimplemented), and HLE functions (HLE_FUNC(...)). Class: games/xcde/hle_classes.tsv
(kind<TAB>name or prefix*<TAB>real|simplified|stub<TAB>note); anything not listed is "unreviewed" -- the audit says so
rather than guessing. Observed gaps come from docs/evidence/M3/hle_coverage.tsv (XCDE_HLE_COVERAGE=<file> run).
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'docs', 'hle_audit.md')
MANIFEST = os.path.join(ROOT, 'games', 'xcde', 'hle_classes.tsv')
COVERAGE = os.path.join(ROOT, 'docs', 'evidence', 'M3', 'hle_coverage.tsv')
CLASSES = ('real', 'simplified', 'stub')


def read(path):
    with open(path, encoding='utf8') as f:
        return f.read()


def match_brace(s, i):
    """s[i] == '{': index just past the matching '}' (strings and comments skipped)."""
    depth, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n and s[i] != q:
                i += 2 if s[i] == '\\' else 1
        elif s.startswith('//', i):
            i = s.find('\n', i)
            if i < 0:
                return n
        elif s.startswith('/*', i):
            i = s.find('*/', i) + 1
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def demangle(sym):
    """Itanium names of the simple nested form _ZN2nn2os4NameE...; anything else is returned as is."""
    m = re.match(r'_ZN((?:\d+[A-Za-z_]\w*)+)E', sym)
    if not m:
        return sym
    return _split_nested(m.group(1))


def _split_nested(body):
    parts = []
    while body:
        m = re.match(r'(\d+)', body)
        if not m:
            break
        n = int(m.group(1))
        parts.append(body[len(m.group(1)):len(m.group(1)) + n])
        body = body[len(m.group(1)) + n:]
    return '::'.join(parts)


def runtime_files():
    for d, _, fs in os.walk(os.path.join(ROOT, 'runtime')):
        for f in sorted(fs):
            if f.endswith('.c'):
                yield os.path.join(d, f)


def scan():
    svcs, services, funcs = [], [], []
    k = read(os.path.join(ROOT, 'runtime', 'kernel.c'))
    m0 = re.search(r'static void svc_impl\([^)]*\)\s*\{', k)  # (the definition, not the forward declaration)
    a = m0.start()
    b = match_brace(k, m0.end() - 1)
    body = k[a:b]
    # a "case 0x..:" label (or several) followed by its first line of code / comment
    for m in re.finditer(r'((?:case 0x[0-9A-Fa-f]+:\s*)+)([^\n]*)', body):
        ids = re.findall(r'0x([0-9A-Fa-f]+)', m.group(1))
        rest = m.group(2)
        cm = re.search(r'//\s*(.*)$', rest)
        text = cm.group(1).strip() if cm else ''
        trivial = bool(re.fullmatch(r'\s*x\[0\] = R_OK;\s*(break;)?\s*(//.*)?', rest))
        for i in ids:
            svcs.append((int(i, 16), text, trivial))
    for p in runtime_files():
        s = read(p)
        rel = os.path.relpath(p, ROOT).replace('\\', '/')
        for m in re.finditer(r'\bHLE_FUNC\("([^"]+)",\s*(\w+)\)\s*\{', s):
            end = match_brace(s, m.end() - 1)
            code = s[m.end():end - 1]
            stripped = re.sub(r'\(void\)\s*c;|c->x\[0\]\s*=\s*0;|\s+', '', re.sub(r'//.*', '', code))
            funcs.append((m.group(1), rel, len(code.strip()), stripped == ''))
        for m in re.finditer(r'\bSERVICE\("([^"]+)",\s*(\w+)\)\s*\{', s):
            end = match_brace(s, m.end() - 1)
            code = s[m.end():end - 1]
            services.append((m.group(1), rel, code.count('diag_unimplemented(')))
    return sorted(set(svcs)), sorted(set(services)), sorted(set(funcs))


def load_manifest():
    rules = []
    if os.path.exists(MANIFEST):
        for line in read(MANIFEST).splitlines():
            if not line.strip() or line.startswith('#'):
                continue
            kind, name, cls, note = (line.split('\t') + ['', '', '', ''])[:4]
            assert cls in CLASSES, f'{MANIFEST}: bad class {cls!r} in {line!r}'
            rules.append((kind, name, cls, note))
    return rules


def classify(rules, kind, name, auto_stub=False):
    for k, n, cls, note in rules:
        if k == kind and (name == n or (n.endswith('*') and name.startswith(n[:-1]))):
            return cls, note
    if auto_stub:
        return 'stub', '(automatic: the body does nothing)'
    return 'unreviewed', ''


def load_coverage():
    gaps = []
    if os.path.exists(COVERAGE):
        for line in read(COVERAGE).splitlines():
            if line.strip():
                kind, name, n = (line.split('\t') + ['0'])[:3]
                gaps.append((kind, name, n))
    return gaps


def render():
    svcs, services, funcs = scan()
    rules = load_manifest()
    gaps = load_coverage()
    out = ['# HLE audit (generated)', '',
           '> Generated by `python tools/hle_audit.py` from `runtime/` and `games/xcde/hle_classes.tsv`. **Do not edit by hand.**',
           '> Classes (Art. VI): `real` (faithful behaviour), `simplified` (approximates or cuts), `stub` (returns success / zeros),',
           '> `unreviewed` (not classified yet: the audit doesn\'t guess). Strict mode: `XCDE_STRICT=1`.', '']
    counts = {}

    def row(kind, name, where, cls, note):
        counts[(kind, cls)] = counts.get((kind, cls), 0) + 1
        return f'| `{name}` | {where} | {cls} | {note} |'

    out += ['## SVC (`runtime/kernel.c`)', '', '| SVC | Description | Class | Note |', '|---|---|---|---|']
    for i, text, trivial in svcs:
        cls, note = classify(rules, 'svc', f'0x{i:02x}', trivial)
        counts[('svc', cls)] = counts.get(('svc', cls), 0) + 1
        out.append(f'| 0x{i:02x} | {text} | {cls} | {note} |')
    out += ['', '## IPC services (`SERVICE(...)`)', '', '| Service | File | Class | Commands falling into `diag_unimplemented` / note |', '|---|---|---|---|']
    for name, rel, nun in services:
        cls, note = classify(rules, 'service', name)
        extra = f'{nun} default branch(es)' + (f'; {note}' if note else '')
        out.append(row('service', name, rel, cls, extra))
    out += ['', '## HLE functions (`HLE_FUNC(...)`)', '', '| Symbol | File | Class | Note |', '|---|---|---|---|']
    for sym, rel, size, empty in funcs:
        cls, note = classify(rules, 'func', sym, empty)
        label = demangle(sym)
        out.append(row('func', label if label != sym else sym, rel, cls, note))
    out += ['', '## Gaps observed at run time', '']
    if gaps:
        out += ['Recorded with `XCDE_HLE_COVERAGE` (`docs/evidence/M3/hle_coverage.tsv`): commands / imports / services the game asked for and',
                'the runtime does not implement (in normal mode they return success; with `XCDE_STRICT=1` they stop the game).', '',
                '| Kind | Item | Count |', '|---|---|---|']
        out += [f'| {k} | {n} | {c} |' for k, n, c in gaps]
    else:
        out.append('No coverage data yet.')
    out += ['', '## Summary', '', '| Kind | real | simplified | stub | unreviewed |', '|---|---|---|---|---|']
    for kind in ('svc', 'service', 'func'):
        c = [counts.get((kind, k), 0) for k in CLASSES + ('unreviewed',)]
        out.append(f'| {kind} | {c[0]} | {c[1]} | {c[2]} | {c[3]} |')
    return '\n'.join(out) + '\n'


def main():
    text = render()
    if '--check' in sys.argv:
        cur = read(OUT).replace('\r\n', '\n') if os.path.exists(OUT) else ''
        if cur != text:
            print('docs/hle_audit.md is out of date: run python tools/hle_audit.py', file=sys.stderr)
            sys.exit(1)
        return
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'w', encoding='utf8', newline='\n') as f:
        f.write(text)
    print('wrote', OUT)


if __name__ == '__main__':
    main()
