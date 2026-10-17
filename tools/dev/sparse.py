"""sparse.py need READLOG | apply BLOB IDX: a sparse copy of the disc ($OUTBREAK_WORK/f1disc).

The NETBIO archives are huge; only the parts the game has read are filled in. `need` lists the
parts a run's read log (OUTBREAK_READLOG) touched that aren't there yet (merged, 2 KB aligned);
`apply` writes a fetched blob (from fetch_ranges.py) into place."""
import sys,os,json
W=os.environ.get('OUTBREAK_WORK', os.path.expanduser('~/outbreak-work')); D=W+'/f1disc'; H=W+'/f1disc.have.json'
have=json.load(open(H)) if os.path.exists(H) else {n:[[0,16<<20]] for n in ('NETBIO00.DAT','NETBIO01.DAT','NETBIO02.DAT')}
def sub(a,b,ranges):
    out=[[a,b]]
    for x,y in ranges:
        nxt=[]
        for p,q in out:
            if y<=p or x>=q: nxt.append([p,q]); continue
            if p<x: nxt.append([p,x])
            if y<q: nxt.append([y,q])
        out=nxt
    return out
def merge(r,gap=0):
    r=sorted(r); m=[]
    for a,b in r:
        if m and a<=m[-1][1]+gap: m[-1][1]=max(m[-1][1],b)
        else: m.append([a,b])
    return m
if sys.argv[1]=='need':
    want={}
    for l in open(sys.argv[2]):
        n,o,s=l.split(); 
        if not n.startswith('NETBIO'): continue
        o,s=int(o),int(s); a=o&~2047; b=(o+s+2047)&~2047
        for p,q in sub(a,b,have.get(n,[])): want.setdefault(n,[]).append([p,q])
    tot=0
    for n in want:
        want[n]=merge(want[n],65536)
        for a,b in want[n]: print(n,a,b-a); tot+=b-a
    print('total',tot,file=sys.stderr)
elif sys.argv[1]=='apply':   # apply blob + index
    blob=open(sys.argv[2],'rb'); 
    for l in open(sys.argv[3]):
        n,o,s=l.split(); o,s=int(o),int(s)
        d=blob.read(s); assert len(d)==s
        with open(D+'/'+n,'r+b') as f: f.seek(o); f.write(d)
        have.setdefault(n,[]).append([o,o+s]); have[n]=merge(have[n])
    json.dump(have,open(H,'w'))
    print({n:sum(b-a for a,b in r) for n,r in have.items()})
