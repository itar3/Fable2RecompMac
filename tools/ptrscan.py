#!/usr/bin/env python3
"""Find code addresses referenced from data that codegen didn't turn into
functions (vtable slots and other function pointers), and print config
entries for them.

Usage: ptrscan.py <image.bin> <repo>  -> writes candidates.toml next to image."""
import bisect, glob, os, re, struct, sys

IMAGE_BASE = 0x82000000
CODE_BASE, CODE_END = 0x82170000, 0x832CA03C

img_path, repo = sys.argv[1], sys.argv[2]
img = open(img_path, "rb").read()
gen = os.path.join(repo, "generated")


def word(a):
    o = a - IMAGE_BASE
    return struct.unpack(">I", img[o:o + 4])[0]


# Known function entries and where each generated body actually ends.
known = sorted(int(a, 16) for a in re.findall(
    r"\{ (0x[0-9A-F]{8}), \w+ \}", open(os.path.join(gen, "Fable2_init.cpp")).read()))
known_set = set(known)
ends = {}
for f in glob.glob(os.path.join(gen, "Fable2_recomp.*.cpp")):
    name, addr, last = None, 0, None
    for line in open(f):
        m = re.match(r"DEFINE_REX_FUNC\((?:sub_)?(\w+)\)", line)
        if m:
            if name is not None and last is not None:
                ends[name] = last + 4
            name = m.group(1)
            hm = re.fullmatch(r"[0-9A-F]{8}", name)
            addr, last = (int(name, 16) if hm else None), None
            continue
        if name is None or addr is None:
            continue
        m = re.match(r"loc_([0-9A-F]{8}):", line)
        if m:
            addr = int(m.group(1), 16)
        elif line.startswith("\t// "):
            last = addr
            addr += 4
    if name is not None and last is not None:
        ends[name] = last + 4

# Covered ranges [start, end) for every sub_XXXXXXXX we could measure.
covered = sorted((int(n, 16), e) for n, e in ends.items() if re.fullmatch(r"[0-9A-F]{8}", n))
cov_starts = [s for s, _ in covered]


def inside_known(a):
    i = bisect.bisect_right(cov_starts, a) - 1
    return i >= 0 and covered[i][0] <= a < covered[i][1]


# Scan everything outside the code section for big-endian pointers into code.
cands = set()
for off in range(0, len(img) - 3, 4):
    a = IMAGE_BASE + off
    if CODE_BASE <= a < CODE_END:
        continue
    t = struct.unpack(">I", img[off:off + 4])[0]
    if not (CODE_BASE <= t < CODE_END) or t & 3 or t in known_set:
        continue
    if inside_known(t):
        continue
    if word(t) == 0:  # padding, not code
        continue
    cands.add(t)

data_cands = len(cands)

# Scan code for address materialisation: lis rD,hi then (within a few
# instructions) addi rX,rD,lo, giving a code address -- function pointers
# stored into tables at runtime never appear in the image's data.
for a in range(CODE_BASE, CODE_END - 4, 4):
    ins = word(a)
    if ins >> 26 != 15 or (ins >> 16) & 31 != 0:  # lis = addis rD,0,imm
        continue
    rd, hi = (ins >> 21) & 31, ins & 0xFFFF
    for b in range(a + 4, min(a + 4 * 8, CODE_END), 4):
        j = word(b)
        op = j >> 26
        if op == 14 and (j >> 16) & 31 == rd:  # addi rX,rD,lo
            lo = j & 0xFFFF
            t = ((hi << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
            if (CODE_BASE <= t < CODE_END and not t & 3 and t not in known_set
                    and not inside_known(t) and word(t) != 0):
                cands.add(t)
            break
        if (j >> 21) & 31 == rd and op not in (36, 37, 38, 44, 45, 47, 32, 34, 40, 42):
            break  # rD overwritten by something other than a load/store
print(f"from data pointers: {data_cands}, from lis/addi in code: {len(cands) - data_cands}")

# Size each candidate up to the next entry (known or candidate), minus padding.
entries = sorted(known_set | cands)
out = []
for t in sorted(cands):
    nxt = entries[bisect.bisect_right(entries, t)]
    end = nxt
    while end - 4 > t and word(end - 4) == 0:
        end -= 4
    size = end - t
    if size > 0x2000:
        continue  # too big to trust; leave for manual review
    out.append((t, size))

path = os.path.join(os.path.dirname(img_path), "candidates.toml")
with open(path, "w") as f:
    for t, s in out:
        f.write(f"0x{t:08X} = {{ size = 0x{s:X} }}\n")
print(f"known functions: {len(known)}, measured: {len(covered)}")
print(f"pointer targets outside known code: {len(cands)}, emitted: {len(out)} -> {path}")
for t in (0x82967560, 0x829675B0, 0x82267CC8, 0x822675A8, 0x82E960C0, 0x82452EC0):
    print(f"  check 0x{t:08X}: {'found' if t in cands else ('known' if t in known_set else 'MISSED')}")
