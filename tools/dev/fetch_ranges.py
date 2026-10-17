"""fetch_ranges.py DISC.iso REQ OUT.gz: on the machine with the disc, cut the byte ranges REQ lists
("FILE offset size" per line, files in the image's root) into OUT.gz and an index OUT.gz.idx.
Runs anywhere with Python 3; reads only what is asked."""
import struct,sys,gzip
iso,req,out=sys.argv[1:4]
f=open(iso,'rb'); f.seek(16*2048); pvd=f.read(2048); root=pvd[156:190]
lba,size=struct.unpack('<I',root[2:6])[0],struct.unpack('<I',root[10:14])[0]
f.seek(lba*2048); d=f.read(size); i=0; ent={}
while i<len(d):
    l=d[i]
    if l==0: i=(i//2048+1)*2048; continue
    r=d[i:i+l]; ent[r[33:33+r[32]].decode('ascii','replace').split(';')[0]]=struct.unpack('<I',r[2:6])[0]*2048; i+=l
o=gzip.open(out,'wb',compresslevel=1); idx=open(out+'.idx','w'); t=0
for l in open(req):
    n,a,s=l.split(); a,s=int(a),int(s); f.seek(ent[n]+a); b=f.read(s); o.write(b); idx.write('%s %d %d\n'%(n,a,len(b))); t+=len(b)
o.close(); print('bytes',t)
