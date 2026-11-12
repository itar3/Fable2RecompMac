import sys,xml.etree.ElementTree as ET
ids={}
def val(e):
    if 'ref' in e.attrib: return ids[e.attrib['ref']]
    v=(e.attrib.get('fmt'), e.text)
    if 'id' in e.attrib: ids[e.attrib['id']]=v
    for c in e: val(c)
    return v
rows=[]
for ev,row in ET.iterparse(sys.argv[1]):
    if row.tag!='row': continue
    cols=[val(c) for c in row]
    try:
        if 'Fable2' in (cols[10][0] or '') and cols[2][0]=='Fragment': rows.append((int(cols[1][1]),[c[0] for c in cols]))
    except Exception: pass
    row.clear()
rows.sort(key=lambda r:-r[0])
for d,c in rows[:6]: print(d/1e6,'ms |', ' | '.join(str(x) for x in c))
