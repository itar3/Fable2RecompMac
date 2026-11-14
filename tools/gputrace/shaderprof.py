"""GPU time per shader from a Metal System Trace + 'Metal GPU Counters' recording.

Build with -DFABLE2_GPU_LABELS=ON (shader entry points become p/v + guest hash),
record while pipelines get built (e.g. toggle fable2_debug_nan_paint right after
the recording starts), then export two tables and run:
  xctrace export --input X.trace --xpath '/trace-toc/run/data/table[@schema="metal-shader-profiler-shader-list"]' --output list.xml
  xctrace export --input X.trace --xpath '/trace-toc/run/data/table[@schema="gpu-shader-profiler-interval"]' --output pcs.xml
  python3 shaderprof.py list.xml pcs.xml Fragment|Vertex
"""
import sys, bisect, collections, xml.etree.ElementTree as ET

def rows(path):
    ids = {}
    for ev, row in ET.iterparse(path):
        if row.tag != 'row': continue
        out = []
        for c in row:
            if 'ref' in c.attrib: out.append(ids[c.attrib['ref']])
            else:
                v = (c.attrib.get('fmt'), c.text)
                if 'id' in c.attrib: ids[c.attrib['id']] = v
                for cc in c:
                    if 'id' in cc.attrib: ids[cc.attrib['id']] = (cc.attrib.get('fmt'), cc.text)
                out.append(v)
        yield out
        row.clear()

ranges = []
for r in rows(sys.argv[1]):
    try: ranges.append((int(r[5][1]), int(r[6][1]), r[1][0].split(' (')[0], r[7][0]))
    except Exception: pass
ranges.sort(); starts = [x[0] for x in ranges]
byname = collections.Counter(); bytype = collections.Counter(); total = 0
for r in rows(sys.argv[2]):
    try: pc = int(r[2][1]); d = int(r[1][1])
    except Exception: continue
    total += d
    i = bisect.bisect_right(starts, pc) - 1
    if i >= 0 and ranges[i][0] <= pc < ranges[i][1]:
        byname[(ranges[i][2], ranges[i][3])] += d; bytype[ranges[i][3]] += d
    else: bytype['unmapped'] += d
print({k: round(100.0 * v / total, 1) for k, v in bytype.items()})
for (n, t), d in byname.most_common(200):
    if t == sys.argv[3]: print('%6.2f%%  %s' % (100.0 * d / total, n))
