import pickle,sys,collections
F={k:v for k,v in pickle.load(open(sys.argv[1],'rb')).items() if isinstance(k,int)}
LO,HI=int(sys.argv[2],16),int(sys.argv[3],16)
inr=lambda a: LO<=a<HI
callers=collections.defaultdict(set)
for a,f in F.items():
    for c in f['c']: callers[c].add(a)
reg=[a for a in F if inr(a)]
api={a:[c for c in callers[a] if not inr(c)] for a in reg}
api={a:v for a,v in api.items() if v}
extcallers=set(c for v in api.values() for c in v)
print('functions in D3D block:',len(reg),' lines:',sum(F[a]['n'] for a in reg))
print('entry points called from outside:',len(api))
print('distinct outside callers:',len(extcallers))
# entry points by number of callers
for a,v in sorted(api.items(),key=lambda x:-len(x[1]))[:60]:
    print(hex(a),'callers',len(v),'lines',F[a]['n'],'imps',sorted(F[a]['i'])[:3])
# where do callers live (1MB buckets)
bk=collections.Counter((c>>20)<<20 for c in extcallers)
print({hex(k):v for k,v in sorted(bk.items())})
