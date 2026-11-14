#!/usr/bin/env python3
"""Finds recompiled functions that share non-volatile GPRs (r14-r31) with their callers:
live-in (read before written on some path from the entry, not counting spills to the stack) or
written without being saved (the caller sees the change). Input: generated code WITHOUT
non_volatile_as_local (registers as ctx.rNN, save/restore helper calls visible).
usage: nvshare.py GENERATED_DIR [--list]"""
import re, sys, glob, collections
REG = re.compile(r'(?<![A-Za-z0-9_])ctx\.r(1[24-9]|2[0-9]|3[01])\.(?:u64|s64|u32|s32|u16|s16|u8|s8)')
ASSIGN = re.compile(r'^ctx\.r(1[24-9]|2[0-9]|3[01])\.(?:u64|s64|u32|s32) = (.*);$')
SPILL = re.compile(r'^REX_STORE_U64\(ctx\.r(?:1|12)\.u32 \+ -\d+, ctx\.r(1[4-9]|2[0-9]|3[01])\.u64\);$')
UNSPILL = re.compile(r'^ctx\.r(1[4-9]|2[0-9]|3[01])\.u64 = REX_LOAD_U64\(ctx\.r(?:1|12)\.u32 \+ -\d+\);$')
SAVE = re.compile(r'^__savegprlr_(\d+)\(ctx, base\);$')
REST = re.compile(r'^__restgprlr_(\d+)\(ctx, base\);$')
LABEL = re.compile(r'^(loc_[0-9A-Fa-f]+):$')
GOTO = re.compile(r'goto (loc_[0-9A-Fa-f]+);')
FUNC = re.compile(r'^DEFINE_REX_FUNC\(([A-Za-z0-9_]+)\) \{$')

def analyse(name, body):
    blocks = []  # each: dict(use,defs,succ(labels),fall(bool))
    index = {}
    cur = dict(use=set(), defs=set(), succ=[], fall=True, label=None)
    blocks.append(cur)
    saved, written, in_switch, dead = set(), set(), False, False
    helper = False; ninstr = sum(1 for x in body if x.strip().startswith('// '))
    def new_block(label=None):
        nonlocal cur, dead
        cur = dict(use=set(), defs=set(), succ=[], fall=True, label=label)
        blocks.append(cur)
        if label: index[label] = len(blocks) - 1
        dead = False
    for raw in body:
        line = raw.strip()
        if not line or line.startswith('//') or line.startswith('PPC') or line.startswith('uint32_t ea') or line == 'REX_FUNC_PROLOGUE();':
            continue
        m = LABEL.match(line)
        if m:
            prev = cur
            new_block(m.group(1))
            continue
        if dead and not in_switch:
            # unreachable until the next label; still record helper saves for completeness
            continue
        if line.startswith('switch ('):
            in_switch = True
            for r in REG.findall(line):
                if int(r) not in cur['defs']: cur['use'].add(int(r))
            continue
        if in_switch:
            if line == '}':
                in_switch = False
                cur['fall'] = False
                dead = True
                continue
            g = GOTO.search(line)
            if g: cur['succ'].append(g.group(1))
            continue
        m = SAVE.match(line)
        if m:
            saved |= set(range(int(m.group(1)), 32)); helper = True; continue
        if REST.match(line):
            continue
        m = SPILL.match(line)
        if m:
            saved.add(int(m.group(1))); continue
        if UNSPILL.match(line):
            continue  # restoring the caller's value
        cond = line.startswith('if (')
        g = GOTO.search(line)
        # reads / writes
        m = ASSIGN.match(line)
        reads = [int(r) for r in REG.findall(line)]
        w = None
        if m:
            w = int(m.group(1))
            rhs = m.group(2)
            if rhs == 'ctx.r%d.u64' % w:      # mr rN,rN padding
                reads, w = [], None
            else:
                reads = [int(r) for r in REG.findall(rhs)]
        for r in reads:
            if r not in cur['defs']: cur['use'].add(r)
        if w is not None:
            cur['defs'].add(w); written.add(w)
        if g:
            cur['succ'].append(g.group(1))
            if not cond:
                cur['fall'] = False; dead = True
            else:
                new_block()   # a branch ends the block: later writes must not hide reads at its target
        elif line == 'return;':
            cur['fall'] = False; dead = True
    n = len(blocks)
    succ = []
    for i, b in enumerate(blocks):
        s = [index[l] for l in b['succ'] if l in index]
        if b['fall'] and i + 1 < n: s.append(i + 1)
        succ.append(s)
    live_in = [set(b['use']) for b in blocks]
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            out = set()
            for s in succ[i]: out |= live_in[s]
            new = blocks[i]['use'] | (out - blocks[i]['defs'])
            if new != live_in[i]:
                live_in[i] = new; changed = True
    return live_in[0] - {12}, written - saved - {12}, 12 in live_in[0], helper, ninstr

def main():
    d = sys.argv[1]
    res = {}
    for path in sorted(glob.glob(d + '/*_recomp.*.cpp')):
        name, body = None, []
        for raw in open(path, errors='replace'):
            m = FUNC.match(raw.rstrip('\n'))
            if m:
                name, body = m.group(1), []
                continue
            if name is not None:
                if raw.startswith('}'):
                    res[name] = analyse(name, body); name = None
                else:
                    body.append(raw)
    fmt = lambda x: ','.join('r%d' % r for r in sorted(x)) or '-'
    r12in = {k for k, v in res.items() if v[2]}
    cats = collections.defaultdict(list)
    for k, (li, un, r12, helper, n) in res.items():
        if r12:
            if li or (un - {31}): cats['A funclet sharing registers (needs share_registers)'].append(k)
            else: cats['B funclet using only its frame pointer (needs r12 only)'].append(k)
        elif li or un:
            if helper or n > 400: cats['C normal function, maybe-uninitialised path (not sharing)'].append(k)
            else: cats['D no r12, small, no helper saves (look at these)'].append(k)
    print(f'{len(res)} functions; r12 live-in: {len(r12in)}')
    for c in sorted(cats):
        print(f'  {c}: {len(cats[c])}')
    if '--list' in sys.argv:
        for c in sorted(cats):
            for k in sorted(cats[c]):
                li, un, r12, helper, n = res[k]
                print(c[0], k, 'live-in:', fmt(li), 'unsaved:', fmt(un), 'instr:', n, 'helper' if helper else '')
main()
