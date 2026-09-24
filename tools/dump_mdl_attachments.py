#!/usr/bin/env python3
"""Dump Source studiohdr attachments from a .mdl."""
import struct
import sys

MDL = sys.argv[1] if len(sys.argv) > 1 else r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life 2 VR\hlvr\models\weapons\vr_crowbar.mdl"


def cstr(buf, off):
    e = buf.find(b"\x00", off)
    return buf[off:e].decode("ascii", "replace")


with open(MDL, "rb") as f:
    data = f.read()

ident, version, checksum = struct.unpack_from("<III", data, 0)
name = data[12:76].split(b"\x00", 1)[0].decode("ascii", "replace")
length = struct.unpack_from("<I", data, 76)[0]
print("id", ident, "ver", version, "name", name, "len", length, "file", len(data))

# studiohdr_t after name[64]+length:
# eyeposition Vector (12), illum (12), hull_min (12), hull_max (12), view_bbmin (12), view_bbmax (12)
# flags, numbones, boneindex, numbonecontrollers, bonecontrollerindex
# numhitboxsets, hitboxsetindex
# numlocalanim, localanimindex, numlocalseq, localseqindex
# activitylistversion, eventsindexed
# numtextures, textureindex, numcdtextures, cdtextureindex
# numskinref, numskinfamilies, skinindex
# numbodyparts, bodypartindex
# numlocalattachments, localattachmentindex
base = 80  # after length
# 6 vectors * 12 = 72
base += 72  # 152
flags, numbones, boneindex = struct.unpack_from("<iii", data, base)
print("flags", hex(flags), "bones", numbones, "boneindex", boneindex)

# Walk fields from numbones to attachments. Offsets from studio.h after boneindex:
off = base + 12  # after flags,numbones,boneindex
numbonecontrollers, bonecontrollerindex = struct.unpack_from("<ii", data, off)
off += 8
numhitboxsets, hitboxsetindex = struct.unpack_from("<ii", data, off)
off += 8
numlocalanim, localanimindex = struct.unpack_from("<ii", data, off)
off += 8
numlocalseq, localseqindex = struct.unpack_from("<ii", data, off)
off += 8
activitylistversion, eventsindexed = struct.unpack_from("<ii", data, off)
off += 8
numtextures, textureindex = struct.unpack_from("<ii", data, off)
off += 8
numcdtextures, cdtextureindex = struct.unpack_from("<ii", data, off)
off += 8
numskinref, numskinfamilies, skinindex = struct.unpack_from("<iii", data, off)
off += 12
numbodyparts, bodypartindex = struct.unpack_from("<ii", data, off)
off += 8
numlocalattachments, localattachmentindex = struct.unpack_from("<ii", data, off)
print("attachments", numlocalattachments, "index", localattachmentindex)
print("textures", numtextures, "bodyparts", numbodyparts)

# mstudioattachment_t: sznameindex, flags, localbone, matrix3x4 (12 floats), unused[8]
# size = 4+4+4+48+32 = 92
ATT_SIZE = 92
for i in range(numlocalattachments):
    a = localattachmentindex + i * ATT_SIZE
    sznameindex, flags, localbone = struct.unpack_from("<iii", data, a)
    mat = struct.unpack_from("<12f", data, a + 12)
    aname = cstr(data, a + sznameindex)
    origin = (mat[3], mat[7], mat[11])
    fwd = (mat[0], mat[4], mat[8])
    left = (mat[1], mat[5], mat[9])
    up = (mat[2], mat[6], mat[10])
    print("att[%d] %s bone=%d flags=%d origin=%s" % (i, aname, localbone, flags, origin))
    print("  fwd", fwd)
    print("  left", left)
    print("  up", up)
    print("  mat", mat)

# mstudiobone_t is large; dump name + pos for grip/top bones
# Common v48 bone: sznameindex, parent, ... pos at a known offset.
# Source mstudiobone_t starts: sznameindex, parent, bonecontroller[6], pos Vector, quat, rot, posscale, rotscale, poseToBone matrix3x4, ...
print("--- bones of interest ---")
# sizeof mstudiobone_t v48 is typically 216
BONE_SIZE = 216
for i in range(numbones):
    b = boneindex + i * BONE_SIZE
    sznameindex, parent = struct.unpack_from("<ii", data, b)
    bname = cstr(data, b + sznameindex)
    # pos is after sznameindex(4)+parent(4)+bonecontroller[6](24) = 32
    pos = struct.unpack_from("<3f", data, b + 32)
    if i in (0, 1, 37, 38, 39, 40) or "grip" in bname.lower() or "top" in bname.lower() or "crow" in bname.lower() or parent < 0:
        print("bone[%d] %s parent=%d pos=%s" % (i, bname, parent, pos))

# textures
# mstudiotexture_t: sznameindex, flags, used, unused1, material, clientmaterial, unused[10]
# often 64 bytes
print("--- textures ---")
for texsz in (64, 16, 32):
    names = []
    for i in range(min(numtextures, 8)):
        t = textureindex + i * texsz
        if t + 4 > len(data):
            break
        sz = struct.unpack_from("<i", data, t)[0]
        n = cstr(data, t + sz) if 0 < t + sz < len(data) else "?"
        names.append(n)
    print("texsz", texsz, names)

# mstudiobodyparts_t: sznameindex, nummodels, base, modelindex
print("--- bodyparts ---")
for i in range(numbodyparts):
    bp = bodypartindex + i * 16
    sz, nmodels, base, mindex = struct.unpack_from("<iiii", data, bp)
    print("bodypart", cstr(data, bp + sz), "models", nmodels, "modelindex", mindex)
    # mstudiomodel_t: name[64], type, boundingradius, nummeshes, meshindex, numvertices, vertexindex, tangentsindex, numattachments, attachmentindex, numeyeballs, eyeballindex, vertexdata, unused[8]
    # name is 64 bytes at start
    for mi in range(nmodels):
        md = bp + mindex + mi * 148  # guess; print name anyway
        mname = data[md:md+64].split(b"\x00", 1)[0].decode("ascii", "replace")
        nummeshes = struct.unpack_from("<i", data, md + 64 + 4 + 4)[0]
        print("  model", mname, "nummeshes_guess", nummeshes)
