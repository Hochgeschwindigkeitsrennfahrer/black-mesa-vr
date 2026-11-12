import struct, re, sys
class PE:
    def __init__(s, path):
        s.d = open(path,'rb').read(); d=s.d
        e = struct.unpack_from('<I', d, 0x3C)[0]
        nsec = struct.unpack_from('<H', d, e+6)[0]; opt = struct.unpack_from('<H', d, e+20)[0]
        s.base = struct.unpack_from('<I', d, e+24+28)[0]
        s.secs=[]
        o = e+24+opt
        for i in range(nsec):
            name=d[o:o+8].rstrip(b'\0').decode(); vs,va,rs,ra=struct.unpack_from('<IIII', d, o+8)
            s.secs.append((name,va,vs,ra,rs)); o+=40
    def text(s):
        return [x for x in s.secs if x[0]=='.text'][0]
    def rva2off(s, rva):
        for n,va,vs,ra,rs in s.secs:
            if va<=rva<va+max(vs,rs): return rva-va+ra
    def off2rva(s, off):
        for n,va,vs,ra,rs in s.secs:
            if ra<=off<ra+rs: return off-ra+va
    def at(s, rva, n): o=s.rva2off(rva); return s.d[o:o+n]
    def find(s, hexpat, sec='.text'):
        h = hexpat.replace(' ','')
        toks=[h[i:i+2] for i in range(0,len(h),2)]
        rx=b''.join(b'.' if t in ('??','?') else re.escape(bytes([int(t,16)])) for t in toks)
        n,va,vs,ra,rs=[x for x in s.secs if x[0]==sec][0]
        return [m.start()+va for m in re.finditer(rx, s.d[ra:ra+rs], re.S)]
def norm(p):  # "55 8B EC ? ?" or "558BEC????" -> "558BEC????"
    toks=p.split() if ' ' in p.strip() else [p[i:i+2] for i in range(0,len(p),2)]
    return ''.join('??' if t in ('?','??') else t for t in toks)

def relocs(pe):
    if hasattr(pe,'_rel'): return pe._rel
    d=pe.d; e=struct.unpack_from('<I', d, 0x3C)[0]
    rva,size=struct.unpack_from('<II', d, e+24+96+5*8)
    o=pe.rva2off(rva); end=o+size; out=[]
    while o<end:
        page,bs=struct.unpack_from('<II', d, o)
        if bs==0: break
        for i in range((bs-8)//2):
            w=struct.unpack_from('<H', d, o+8+2*i)[0]
            if w>>12==3: out.append(page+(w&0xFFF))
        o+=bs
    pe._rel=sorted(out); return pe._rel
def refs(pe, start, length):
    import bisect
    r=relocs(pe); i=bisect.bisect_left(r,start); out=[]
    while i<len(r) and r[i]<start+length:
        out.append((r[i], struct.unpack_from('<I', pe.d, pe.rva2off(r[i]))[0]-pe.base)); i+=1
    return out
def is_start(pe, rva):
    b=pe.at(rva-1,4); return b[0] in (0xCC,0xC3,0x90) or b[0]==0xC2

import bisect
import capstone
_md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def masked(pe, rva, n):
    """bytes at rva..rva+n; relocated dwords and relative branch/call targets masked (False)"""
    raw = bytearray(pe.at(rva, n)); mask = [True] * n
    rs = relocs(pe); i = bisect.bisect_left(rs, rva - 3)
    while i < len(rs) and rs[i] < rva + n:
        for k in range(4):
            j = rs[i] - rva + k
            if 0 <= j < n: mask[j] = False
        i += 1
    for ins in _md.disasm(bytes(raw), rva):
        if ins.mnemonic.startswith('j') or ins.mnemonic == 'call':
            if len(ins.bytes) >= 5 and ins.bytes[0] in (0xE8, 0xE9): k = ins.address - rva + 1; w = 4
            elif len(ins.bytes) == 6 and ins.bytes[0] == 0x0F: k = ins.address - rva + 2; w = 4
            elif len(ins.bytes) == 2 and (0x70 <= ins.bytes[0] <= 0x7F or ins.bytes[0] == 0xEB): k = ins.address - rva + 1; w = 1
            else: continue
            for j in range(k, min(n, k + w)): mask[j] = False
    return bytes(raw), mask


def search(pe, raw, mask):
    """rvas in .text where raw matches with mask"""
    rx = b''.join(re.escape(bytes([b])) if m else b'.' for b, m in zip(raw, mask))
    n, va, vs, ra, rs = pe.text()
    return [m.start() + va for m in re.finditer(rx, pe.d[ra:ra + rs], re.S)]
