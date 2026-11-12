import sys,collections,xml.etree.ElementTree as ET
def load(path, want):
    ids={}
    def val(e):
        if 'ref' in e.attrib: return ids[e.attrib['ref']]
        v=(e.attrib.get('fmt'), e.text)
        if 'id' in e.attrib: ids[e.attrib['id']]=v
        for c in e: val(c)
        return v
    for ev,row in ET.iterparse(path):
        if row.tag!='row': continue
        cols=[val(c) for c in row]; row.clear()
        yield cols
spill=collections.Counter(); spill_n=collections.Counter(); sizes=collections.Counter()
for c in load(sys.argv[1],None):
    enc=int(c[2][1]); b=int(c[3][1]); spill[enc]+=b; spill_n[enc]+=1; sizes[b]+=1
frag={}
for c in load(sys.argv[2],None):
    try:
        if 'Fable2' in (c[10][0] or '') and c[2][0]=='Fragment':
            enc=int(c[16][1]); frag[enc]=frag.get(enc,0)+int(c[1][1])
    except Exception: pass
print('spill sizes (bytes:count):',sizes.most_common(8))
tot=sum(frag.values()); 
big=[e for e,d in frag.items() if d>5e6]; small=[e for e,d in frag.items() if d<=5e6]
def s(es): return sum(spill_n[e] for e in es), sum(1 for e in es if spill_n[e])
print('encoders matched with spills:',sum(1 for e in frag if e in spill_n),'of',len(frag),' spill encoders total',len(spill_n))
print('big passes', len(big), 'spill events,encoders with spills', s(big))
print('small passes', len(small), 'spill events,encoders with spills', s(small))
