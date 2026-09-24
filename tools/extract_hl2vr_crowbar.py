#!/usr/bin/env python3
"""Extract HL2VR vr_crowbar models/materials from hl2vr.vpk (directory VPK v2)."""
import os
import struct
import sys

VPK = r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life 2 VR\hlvr\hl2vr.vpk"


def read_cstr(buf: bytes, i: int):
    e = buf.find(b"\x00", i)
    if e < 0:
        raise ValueError("unterminated string at %d" % i)
    return buf[i:e].decode("ascii", "replace"), e + 1


def parse_vpk(path: str):
    with open(path, "rb") as f:
        data = f.read()
    sig, ver, tree = struct.unpack_from("<III", data, 0)
    if sig != 0x55AA1234:
        raise SystemExit("not a VPK")
    off = 12
    if ver == 2:
        off = 28
    tree_end = off + tree
    i = off
    files = []
    while i < tree_end:
        ext, i = read_cstr(data, i)
        if ext == "":
            break
        while i < tree_end:
            folder, i = read_cstr(data, i)
            if folder == "":
                break
            while i < tree_end:
                name, i = read_cstr(data, i)
                if name == "":
                    break
                crc, preload, archive, offset, length, term = struct.unpack_from(
                    "<IHHIIH", data, i
                )
                i += 18
                preload_data = data[i : i + preload]
                i += preload
                if folder == " ":
                    rel = "%s.%s" % (name, ext)
                else:
                    rel = "%s/%s.%s" % (folder, name, ext)
                files.append(
                    {
                        "rel": rel.replace("\\", "/"),
                        "archive": archive,
                        "offset": offset,
                        "length": length,
                        "preload": preload_data,
                    }
                )
    data_start = tree_end
    return data, data_start, files


def extract_bytes(data, data_start, entry):
    body = entry["preload"]
    if entry["length"] > len(entry["preload"]):
        start = data_start + entry["offset"]
        rest = data[start : start + entry["length"] - len(entry["preload"])]
        body = entry["preload"] + rest
    return body


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "VR",
        "hl2vr_crowbar",
    )
    data, data_start, files = parse_vpk(VPK)
    print("vpk files:", len(files), "data_start", data_start)
    keep = []
    for e in files:
        r = e["rel"].lower()
        if "crowbar" in r or r.startswith("models/weapons/vr_crowbar") or r.startswith(
            "materials/models/weapons/vr_crowbar"
        ):
            keep.append(e)
            print(" ", e["rel"], e["length"])
    if not keep:
        print("no crowbar files")
        for e in files:
            if "weapons/vr_" in e["rel"].lower() and e["rel"].lower().endswith(".mdl"):
                print(" mdl", e["rel"])
        return
    os.makedirs(out_dir, exist_ok=True)
    for e in keep:
        dest = os.path.join(out_dir, e["rel"].replace("/", os.sep))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        blob = extract_bytes(data, data_start, e)
        with open(dest, "wb") as f:
            f.write(blob)
        print("wrote", dest, len(blob))


if __name__ == "__main__":
    main()
