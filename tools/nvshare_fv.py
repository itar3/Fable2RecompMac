#!/usr/bin/env python3
"""Like tools/nvshare.py, for the non-volatile FP (f14-f31) and vector (v14-v31, v64-v127) registers:
functions that read one before writing it (live-in) or write one without saving it.
usage: nvshare_fv.py GENERATED_DIR(without non_volatile_as_local)"""
import re, sys, glob, collections
def nv_f(n): return 14 <= n <= 31
def nv_v(n): return 14 <= n <= 31 or 64 <= n <= 127
TOK = re.compile(r'(?<![A-Za-z0-9_])ctx\.([fv])(\d+)\.')
F_ASSIGN = re.compile(r'^ctx\.f(\d+)\.(?:f64|u64|s64|u32|s32|f32) = (.*);$')
V_STORE = re.compile(r'^simde_mm_store[u]?_(?:si128|ps)\((?:\(simde__m128i\*\))?ctx\.v(\d+)\.[a-z0-9]+, (.*)\);$')
V_ASSIGN = re.compile(r'^ctx\.v(\d+)(?:\.[a-z0-9]+(?:\[\d+\])?)? = (.*);$')
F_SPILL = re.compile(r'^REX_STORE_U64\(ctx\.r(?:1|12)\.u32 \+ -\d+, ctx\.f(\d+)\.u64\);$')
F_UNSPILL = re.compile(r'^ctx\.f(\d+)\.u64 = REX_LOAD_U64\(ctx\.r(?:1|12)\.u32 \+ -\d+\);$')
V_MEMSTORE = re.compile(r'^simde_mm_store[u]?_(?:si128|ps)\(\(simde__m128i\*\)REX_RAW_ADDR\(ea\), .*ctx\.v(\d+)\.[a-z0-9]+\)')
EA = re.compile(r'^ea = (.*);$')
SAVEF = re.compile(r'^__savefpr_(\d+)\(ctx, base\);$')
SAVEV = re.compile(r'^__savevmx_(\d+)\(ctx, base\);$')
REST = re.compile(r'^__rest(?:fpr|vmx|gprlr)_(\d+)\(ctx, base\);$')
LABEL = re.compile(r'^(loc_[0-9A-Fa-f]+):$')
GOTO = re.compile(r'goto (loc_[0-9A-Fa-f]+);')
FUNC = re.compile(r'^DEFINE_REX_FUNC\(([A-Za-z0-9_]+)\) \{$')
def key(c, n): return (c, n)
def is_nv(k): return nv_f(k[1]) if k[0] == 'f' else nv_v(k[1])
def analyse(body):
    blocks = [dict(use=set(), defs=set(), succ=[], fall=True)]; index = {}
    cur = blocks[0]; saved = set(); written = set(); in_switch = False; dead = False; ea_stack = False
    for raw in body:
        line = raw.strip()
        if not line or line.startswith('//') or line.startswith('PPC') or line == 'REX_FUNC_PROLOGUE();': continue
        m = LABEL.match(line)
        if m:
            cur = dict(use=set(), defs=set(), succ=[], fall=True); blocks.append(cur); index[m.group(1)] = len(blocks) - 1; dead = False; continue
        if dead and not in_switch: continue
        if line.startswith('switch ('): in_switch = True; continue
        if in_switch:
            if line == '}': in_switch = False; cur['fall'] = False; dead = True; continue
            g = GOTO.search(line)
            if g: cur['succ'].append(g.group(1))
            continue
        m = SAVEF.match(line)
        if m: saved |= {('f', i) for i in range(int(m.group(1)), 32)}; continue
        m = SAVEV.match(line)
        if m:
            nn = int(m.group(1)); saved |= {('v', i) for i in range(nn, 32 if nn < 32 else 128)}; continue
        if REST.match(line): continue
        m = EA.match(line)
        if m: ea_stack = 'ctx.r1.' in m.group(1); continue
        m = V_MEMSTORE.match(line)
        if m and ea_stack and nv_v(int(m.group(1))) and ('v', int(m.group(1))) not in written:
            saved.add(('v', int(m.group(1)))); continue
        m = F_SPILL.match(line)
        if m: saved.add(('f', int(m.group(1)))); continue
        if F_UNSPILL.match(line): continue
        w = None; rhs = line
        m = F_ASSIGN.match(line)
        if m: w = ('f', int(m.group(1))); rhs = m.group(2)
        else:
            m = V_STORE.match(line) or V_ASSIGN.match(line)
            if m: w = ('v', int(m.group(1))); rhs = m.group(2)
        reads = [(c, int(n)) for c, n in TOK.findall(rhs)]
        for k in reads:
            if is_nv(k) and k not in cur['defs']: cur['use'].add(k)
        if w is not None and is_nv(w):
            cur['defs'].add(w); written.add(w)
        g = GOTO.search(line)
        if g:
            cur['succ'].append(g.group(1))
            if not line.startswith('if ('): cur['fall'] = False; dead = True
            else: cur = dict(use=set(), defs=set(), succ=[], fall=True); blocks.append(cur)   # a branch ends the block
        elif line == 'return;': cur['fall'] = False; dead = True
    n = len(blocks); succ = []
    for i, b in enumerate(blocks):
        s = [index[l] for l in b['succ'] if l in index]
        if b['fall'] and i + 1 < n: s.append(i + 1)
        succ.append(s)
    live = [set(b['use']) for b in blocks]; changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            out = set()
            for s in succ[i]: out |= live[s]
            new = blocks[i]['use'] | (out - blocks[i]['defs'])
            if new != live[i]: live[i] = new; changed = True
    return live[0], written - saved, saved
res = {}
HELPER = re.compile(r'^__(save|rest)')
for path in sorted(glob.glob(sys.argv[1] + '/*_recomp.*.cpp')):
    name, body = None, []
    for raw in open(path, errors='replace'):
        m = FUNC.match(raw.rstrip('\n'))
        if m: name, body = m.group(1), []; continue
        if name is not None:
            if raw.startswith('}'):
                if not HELPER.match(name): res[name] = analyse(body)
                name = None
            else: body.append(raw)
fmt = lambda x: ','.join('%s%d' % k for k in sorted(x)) or '-'
live_unsaved = {k: v for k, v in res.items() if (v[0] - v[2])}
live_saved = {k: v for k, v in res.items() if v[0] and not (v[0] - v[2])}
unsaved = {k: v for k, v in res.items() if v[1]}
print(f'{len(res)} functions; live-in FP/vector not saved by the function: {len(live_unsaved)}; live-in but saved (maybe-uninitialised): {len(live_saved)}; unsaved writes: {len(unsaved)}')
for c in 'fv':
    a = sum(1 for v in live_unsaved.values() if any(k[0] == c for k in v[0] - v[2])); b = sum(1 for v in unsaved.values() if any(k[0] == c for k in v[1]))
    print(f'  class {c}: live-in not saved {a}, unsaved writes {b}')
regs = collections.Counter(k for v in unsaved.values() for k in v[1])
print('  most often written unsaved:', ', '.join('%s%d x%d' % (k[0], k[1], n) for k, n in regs.most_common(14)))
regs = collections.Counter(k for v in live_unsaved.values() for k in v[0] - v[2])
print('  most often live-in:', ', '.join('%s%d x%d' % (k[0], k[1], n) for k, n in regs.most_common(14)))
for title, d in (('LIVE-IN, NOT SAVED', live_unsaved), ('UNSAVED WRITES', unsaved)):
    print('##', title)
    for k in sorted(d)[:14]:
        li, un, sv = d[k]
        print('  ', k, 'live-in:', fmt(li - sv)[:70], '| unsaved writes:', fmt(un)[:90])
