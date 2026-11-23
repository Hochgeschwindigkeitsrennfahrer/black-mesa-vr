import struct, sys, os
# vpk_entities.py <maps_dir.vpk> <path inside vpk, e.g. maps/bm_c2a5a.bsp>: print the BSP entity lump
d = sys.argv[1]; want = sys.argv[2]
f = open(d, 'rb'); data = f.read()
sig, ver, treesz = struct.unpack_from('<III', data, 0)
hdr = 12 if ver == 1 else 28
p = hdr
def rs():
    global p
    e = data.index(b'\0', p); s = data[p:e].decode('latin1'); p = e + 1; return s
found = None
while True:
    ext = rs()
    if not ext: break
    while True:
        path = rs()
        if not path: break
        while True:
            name = rs()
            if not name: break
            crc, pre, arc, off, ln, term = struct.unpack_from('<IHHIIH', data, p); p += 18
            preload = data[p:p+pre]; p += pre
            full = f"{path}/{name}.{ext}"
            if full.lower().endswith(want.lower()):
                found = (arc, off, ln, preload)
arc, off, ln, preload = found
base = d[:-len('_dir.vpk')]
if arc == 0x7fff:
    blob = preload + data[hdr + treesz + off: hdr + treesz + off + ln]
else:
    with open(f"{base}_{arc:03d}.vpk", 'rb') as a: a.seek(off); blob = preload + a.read(ln)
# BSP lump 0 = entities
lofs, llen = struct.unpack_from('<ii', blob, 8)
sys.stdout.write(blob[lofs:lofs+llen].decode('latin1'))
