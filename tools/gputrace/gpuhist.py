import sys,collections,xml.etree.ElementTree as ET
ids={}
def val(e):
    if 'ref' in e.attrib: return ids[e.attrib['ref']]
    v=(e.attrib.get('fmt'), e.text)
    if 'id' in e.attrib: ids[e.attrib['id']]=v
    for c in e: val(c)
    return v
durs=[]
for ev,row in ET.iterparse(sys.argv[1]):
    if row.tag!='row': continue
    cols=[val(c) for c in row]
    try:
        dur=int(cols[1][1]); chan=cols[2][0]; proc=cols[10][0] or ''
    except Exception: row.clear(); continue
    if 'Fable2' in proc and chan=='Fragment': durs.append(dur)
    row.clear()
durs.sort(); tot=sum(durs)
bins=[(0,20e3),(20e3,50e3),(50e3,100e3),(100e3,200e3),(200e3,500e3),(500e3,1e6),(1e6,2e6),(2e6,5e6),(5e6,1e9)]
for lo,hi in bins:
    s=[d for d in durs if lo<=d<hi]
    print(f'{lo/1e3:6.0f}-{hi/1e3:6.0f} us: n={len(s):6d} time={sum(s)/1e6:8.1f} ms ({100*sum(s)/tot:4.1f}%)')
print('n',len(durs),'median us',durs[len(durs)//2]/1e3)
