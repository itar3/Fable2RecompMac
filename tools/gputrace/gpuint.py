import sys,collections,xml.etree.ElementTree as ET
ids={}
def val(e):
    if 'ref' in e.attrib: return ids[e.attrib['ref']]
    v=(e.attrib.get('fmt'), e.text)
    if 'id' in e.attrib: ids[e.attrib['id']]=v
    for c in e: val(c)  # register nested ids
    return v
by_chan=collections.Counter(); n=collections.Counter(); by_label=collections.Counter(); procs=collections.Counter()
t0=None;t1=None
for ev,row in ET.iterparse(sys.argv[1]):
    if row.tag!='row': continue
    cols=[val(c) for c in row]
    # order: start,duration,channel,frame,latency,depth,label,state,uuid,color,process,gpu,...
    try:
        start=int(cols[0][1]); dur=int(cols[1][1]); chan=cols[2][0]; label=cols[6][0] or ''; proc=cols[10][0] or ''
    except Exception: row.clear(); continue
    procs[proc]+=dur
    if 'Fable2' in proc:
        by_chan[chan]+=dur; n[chan]+=1
        by_label[(chan,label.split(' 0x')[0][:60])]+=dur
        t0=start if t0 is None else min(t0,start); t1=max(t1 or 0,start+dur)
    row.clear()
span=(t1-t0)/1e9 if t0 else 0
print('span s',round(span,2)); print({k:round(v/1e9,3) for k,v in procs.most_common(4)})
for c,v in by_chan.most_common(): print(f'{c:12s} {v/1e9:7.3f}s  {100*v/1e9/span:5.1f}% of span  n={n[c]}')
for k,v in by_label.most_common(15): print(f'{v/1e9:7.3f}s', k)
