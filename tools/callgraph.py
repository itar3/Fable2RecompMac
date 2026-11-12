import re,sys,glob,pickle
funcs={}   # addr -> dict(callees=set, imps=set, lines=int)
cur=None
fre=re.compile(r'^DEFINE_REX_FUNC\((?:sub_)?([0-9A-Fa-f]{8}|\w+)\)')
cre=re.compile(r'\bsub_([0-9A-F]{8})\(ctx')
ire=re.compile(r'__imp__(\w+)\(ctx')
for fn in glob.glob(sys.argv[1]+'/Fable2_recomp.*.cpp'):
    for line in open(fn,errors='ignore'):
        m=fre.match(line)
        if m:
            name=m.group(1)
            try: cur=int(name,16)
            except: cur=name
            funcs[cur]={'c':set(),'i':set(),'n':0}; continue
        if cur is None: continue
        f=funcs[cur]; f['n']+=1
        for a in cre.findall(line): f['c'].add(int(a,16))
        for a in ire.findall(line): f['i'].add(a)
pickle.dump(funcs,open(sys.argv[2],'wb'))
print(len(funcs))
