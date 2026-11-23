# vpk.py <x_dir.vpk> <regex>: list files.  vpk.py <x_dir.vpk> <a,b,c> <outdir>: extract them (VPK v1/v2, multi-archive).
# Run with python3 -I, from outside the extract dir.
import struct, os, sys
def read_dir(path):
    f=open(path,'rb'); d=f.read()
    sig,ver,tree=struct.unpack_from('<III',d,0)
    assert sig==0x55aa1234
    hdr=28 if ver==2 else 12
    p=hdr; ents={}
    def cs():
        nonlocal p
        e=d.index(b'\0',p); s=d[p:e].decode('latin1'); p=e+1; return s
    while True:
        ext=cs()
        if not ext: break
        while True:
            pth=cs()
            if not pth: break
            while True:
                nm=cs()
                if not nm: break
                crc,pre,ai,off,ln,term=struct.unpack_from('<IHHIIH',d,p); p+=18
                predata=d[p:p+pre]; p+=pre
                full=(pth+'/' if pth.strip()!='' else '')+nm+'.'+ext
                ents[full.lower()]=(ai,off,ln,predata)
    return ents, hdr+tree, d
def get(path, ents, dirend, d, name):
    ai,off,ln,pre=ents[name.lower()]
    if ai==0x7fff:
        data=d[dirend+off:dirend+off+ln]
    else:
        base=path.replace('_dir.vpk','_%03d.vpk'%ai)
        with open(base,'rb') as f: f.seek(off); data=f.read(ln)
    return pre+data
if __name__=='__main__':
    path=sys.argv[1]; ents,de,d=read_dir(path)
    if len(sys.argv)==3:
        import re
        for k in sorted(ents):
            if re.search(sys.argv[2],k): print(k)
    else:
        out=sys.argv[3]
        for n in sys.argv[2].split(','):
            data=get(path,ents,de,d,n)
            o=os.path.join(out,n); os.makedirs(os.path.dirname(o),exist_ok=True); open(o,'wb').write(data)
