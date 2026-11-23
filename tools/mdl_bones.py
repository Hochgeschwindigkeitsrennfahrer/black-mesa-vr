# mdl_bones.py <model path without extension> (needs .mdl + .vvd): bones, textures, sequences, body parts -> models ->
# meshes, and per mesh the vertex count per bone set (rigid part = one bone).
import struct, sys, collections
def cstr(d,o):
    e=d.index(b'\0',o); return d[o:e].decode('latin1')
def load(base):
    d=open(base+'.mdl','rb').read(); v=open(base+'.vvd','rb').read()
    I=lambda o: struct.unpack_from('<i',d,o)[0]
    ver=I(4); nb,bi=I(156),I(160)
    bones=[]
    for i in range(nb):
        o=bi+i*216; bones.append((cstr(d,o+I(o)), I(o+4)))
    ntex,ti=I(204),I(208)
    tex=[cstr(d,ti+i*64+I(ti+i*64)) for i in range(ntex)]
    nseq,si=I(188),I(192)
    # seqdesc size probe
    seqs=[]
    for sz in (212,):
        seqs=[cstr(d,si+i*sz+I(si+i*sz+4)) for i in range(nseq)]
    nbp,bpi=I(232),I(236)
    # vvd
    VI=lambda o: struct.unpack_from('<i',v,o)[0]
    nfix,fixs,vds=VI(48),VI(52),VI(56)
    def vert(i):
        o=vds+i*48
        w=struct.unpack_from('<3f',v,o); b=struct.unpack_from('<3B',v,o+12); n=v[o+15]
        return [(b[k],w[k]) for k in range(n) if w[k]>0]
    order=None
    if nfix:
        order=[]
        for k in range(nfix):
            lod,src,num=struct.unpack_from('<3i',v,fixs+k*12)
            if lod>=0: order.extend(range(src,src+num))
    vx=lambda i: vert(order[i] if order else i)
    parts=[]
    for p in range(nbp):
        o=bpi+p*16; pname=cstr(d,o+I(o)); nm,base,mi=I(o+4),I(o+8),I(o+12)
        models=[]
        for m in range(nm):
            mo=o+mi+m*148; mname=cstr(d,mo)[:64]
            nmesh,meshi,nverts,vidx=I(mo+72),I(mo+76),I(mo+80),I(mo+84)
            meshes=[]
            for k in range(nmesh):
                mmo=mo+meshi+k*116
                mat,mnv,mvo=I(mmo),I(mmo+8),I(mmo+12)
                start=vidx//48+mvo
                usage=collections.Counter(); multi=collections.Counter()
                for q in range(start,start+mnv):
                    bw=vx(q); bs=frozenset(b for b,_ in bw)
                    for b in bs: usage[b]+=1
                    multi[bs]+=1
                meshes.append((tex[mat] if mat<len(tex) else mat,mnv,usage,multi))
            models.append((mname,nverts,meshes))
        parts.append((pname,base,models))
    return ver,bones,tex,seqs,parts
if __name__=='__main__':
    ver,bones,tex,seqs,parts=load(sys.argv[1])
    print('version',ver,'bones',len(bones))
    for i,(n,p) in enumerate(bones): print(f'  bone {i:3d} {n:40s} parent {p}')
    print('textures',tex)
    print('sequences',seqs)
    for pname,base,models in parts:
        print(f'BODYPART {pname} base={base}')
        for mname,nv,meshes in models:
            print(f'  model {mname!r} verts={nv}')
            for mat,mnv,usage,multi in meshes:
                print(f'    mesh mat={mat} verts={mnv}')
                # group by bone-set: show top sets
                for bs,c in multi.most_common(25):
                    print(f'      {c:6d} verts on {sorted(bones[b][0] for b in bs)}')
