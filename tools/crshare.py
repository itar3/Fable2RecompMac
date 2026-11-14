#!/usr/bin/env python3
"""Finds recompiled functions that use condition register fields across a function boundary:
read at entry before being set (the caller's flags), or read after a call before being set again
(the callee's flags; only the volatile fields cr0, cr1, cr5, cr6, cr7 count: cr2-cr4 are preserved
by callees). Input: generated code WITHOUT cr_as_local.  usage: crshare.py GENERATED_DIR"""
import re, sys, glob, collections
TOK = re.compile(r'ctx\.cr([0-7])\.(lt|gt|eq|so|un)\b')
BITS = ('lt', 'gt', 'eq', 'so', 'un')
MFCR = re.compile(r'^\S+ \|?= ctx\.cr[0-7]\.(?:lt|gt|eq|so|un) \? 0x[0-9A-Fa-f]+ : 0;$')
BITSET = re.compile(r'^ctx\.cr([0-7])\.(lt|gt|eq|so|un) = (.*);$')
KILL = re.compile(r'ctx\.cr([0-7])\.(?:compare|setFromMask|setFromMask2)\b|ctx\.cr([0-7])\.compare<')
LABEL = re.compile(r'^(loc_[0-9A-Fa-f]+):$')
GOTO = re.compile(r'goto (loc_[0-9A-Fa-f]+);')
FUNC = re.compile(r'^DEFINE_REX_FUNC\(([A-Za-z0-9_]+)\) \{$')
CALL = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)\(ctx, base\);$|^REX_CALL_INDIRECT_FUNC\(')
VOL = {0, 1, 5, 6, 7}
def analyse(body):
    # statements: ('use', set) / ('kill', set) / ('call', name)
    blocks = [dict(st=[], succ=[], fall=True)]; index = {}; cur = blocks[0]; in_switch = False; dead = False
    for raw in body:
        line = raw.strip()
        if not line or line.startswith('//') or line.startswith('PPC') or line == 'REX_FUNC_PROLOGUE();': continue
        m = LABEL.match(line)
        if m:
            cur = dict(st=[], succ=[], fall=True); blocks.append(cur); index[m.group(1)] = len(blocks) - 1; dead = False; continue
        if dead and not in_switch: continue
        if line.startswith('switch ('): in_switch = True; continue
        if in_switch:
            if line == '}': in_switch = False; cur['fall'] = False; dead = True; continue
            g = GOTO.search(line)
            if g: cur['succ'].append(g.group(1))
            continue
        if MFCR.match(line):   # mfcr: the register saved, not used
            continue
        m = BITSET.match(line)
        if m:
            uses = {(int(a), b) for a, b in TOK.findall(m.group(3))}
            kills = {(int(m.group(1)), m.group(2))}
        else:
            uses = {(int(a), b) for a, b in TOK.findall(line)}
            kills = {(int(a or b), bit) for a, b in KILL.findall(line) for bit in BITS}
        if uses: cur['st'].append(('use', uses))
        if kills: cur['st'].append(('kill', kills))
        m = CALL.match(line)
        if m: cur['st'].append(('call', m.group(1) or 'indirect'))
        g = GOTO.search(line)
        if g:
            cur['succ'].append(g.group(1))
            if not line.startswith('if ('): cur['fall'] = False; dead = True
            else: cur = dict(st=[], succ=[], fall=True); blocks.append(cur)   # a branch ends the block
        elif line == 'return;': cur['fall'] = False; dead = True
    n = len(blocks); succ = []
    for i, b in enumerate(blocks):
        s = [index[l] for l in b['succ'] if l in index]
        if b['fall'] and i + 1 < n: s.append(i + 1)
        succ.append(s)
    live_in = [set() for _ in blocks]; changed = True
    def transfer(b, out, record=None):
        live = set(out)
        for kind, v in reversed(b['st']):
            if kind == 'use': live |= v
            elif kind == 'kill': live -= v
            else:
                vol = frozenset(n for n, _ in live if n in VOL)
                if record is not None and vol: record.append((v, vol))
        return live
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            out = set()
            for s in succ[i]: out |= live_in[s]
            new = transfer(blocks[i], out)
            if new != live_in[i]: live_in[i] = new; changed = True
    after_call = []
    for i in range(n):
        out = set()
        for s in succ[i]: out |= live_in[s]
        transfer(blocks[i], out, after_call)
    return live_in[0], after_call
res = {}
for path in sorted(glob.glob(sys.argv[1] + '/*_recomp.*.cpp')):
    name, body = None, []
    for raw in open(path, errors='replace'):
        m = FUNC.match(raw.rstrip('\n'))
        if m: name, body = m.group(1), []; continue
        if name is not None:
            if raw.startswith('}'): res[name] = analyse(body); name = None
            else: body.append(raw)
entry = {k: {n for n, _ in v[0]} for k, v in res.items() if v[0]}
after = {k: v[1] for k, v in res.items() if v[1]}
print(f'{len(res)} functions; reading flags at entry: {len(entry)}; reading volatile flags after a call: {len(after)}')
c = collections.Counter(t for v in after.values() for t, _ in v)
print('callees whose flags are read:', ', '.join('%s x%d' % x for x in c.most_common(12)))
for k in sorted(entry)[:25]: print('  entry', k, 'cr' + ',cr'.join(str(x) for x in sorted(entry[k])))
for k in sorted(after)[:25]: print('  after-call', k, [(t, 'cr' + ',cr'.join(str(x) for x in sorted(f))) for t, f in after[k][:3]])

# Which of the after-call reads take the callee's flags? If the callee (and everything it calls)
# never writes the field, the caller's own value simply survived the call.
def writes_of(body):
    w = set(); calls = set()
    for raw in body:
        line = raw.strip()
        if not line or line.startswith('//'): continue
        m = BITSET.match(line)
        if m: w.add(int(m.group(1)))
        for a, b in KILL.findall(line): w.add(int(a or b))
        m = CALL.match(line)
        if m: calls.add(m.group(1) or 'indirect')
    return w, calls
bodies = {}
for path in sorted(glob.glob(sys.argv[1] + '/*_recomp.*.cpp')):
    name, body = None, []
    for raw in open(path, errors='replace'):
        m = FUNC.match(raw.rstrip('\n'))
        if m: name, body = m.group(1), []; continue
        if name is not None:
            if raw.startswith('}'): bodies[name] = writes_of(body); name = None
            else: body.append(raw)
import functools
sys.setrecursionlimit(100000)
memo = {}
def closure(fn, depth=0, seen=None):
    """Fields written by fn or anything it calls; None = unknown (indirect call, import, too deep)."""
    if fn in memo: return memo[fn]
    if fn not in bodies or re.match(r'__(save|rest)', fn): return set() if re.match(r'__(save|rest)', fn) else None
    seen = seen or set()
    if fn in seen: return set()
    seen = seen | {fn}
    w, calls = bodies[fn]
    out = set(w)
    for c in calls:
        if c == 'indirect' or depth > 40: memo[fn] = None; return None
        r = closure(c, depth + 1, seen)
        if r is None: memo[fn] = None; return None
        out |= r
    memo[fn] = out
    return out
safe = takes = unknown = 0; examples = []
for caller, lst in after.items():
    for callee, fields in lst:
        r = closure(callee) if callee != 'indirect' else None
        if r is None: unknown += 1; examples.append(('unknown', caller, callee, sorted(fields)))
        elif r & set(fields): takes += 1; examples.append(('CALLEE WRITES IT', caller, callee, sorted(set(fields) & r)))
        else: safe += 1
print(f'after-call reads: {safe} survive the call untouched (safe), {takes} where the callee tree writes the field, {unknown} unknown (indirect/imports)')
for e in [x for x in examples if x[0] == 'CALLEE WRITES IT'][:12]: print('  ', e)
for e in [x for x in examples if x[0] == 'unknown'][:8]: print('  ', e)
