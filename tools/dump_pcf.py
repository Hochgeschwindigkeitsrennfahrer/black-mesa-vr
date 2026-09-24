"""Dump Source PCF (binary DMX encoding 2 / pcf 1)."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

# Source 2013 DmxAttributeType_t
TYPES = {
    1: "element",
    2: "int",
    3: "float",
    4: "bool",
    5: "string",
    6: "binary",
    7: "time",
    8: "color",
    9: "vector2",
    10: "vector3",
    11: "vector4",
    12: "qangle",
    13: "quaternion",
    14: "vmatrix",
}
ARRAY_BASE = 15  # AT_ELEMENT_ARRAY


def read_cstr(data: bytes, pos: int) -> tuple[str, int]:
    end = data.index(b"\x00", pos)
    return data[pos:end].decode("latin-1", errors="replace"), end + 1


def parse_pcf(data: bytes):
    gt = data.index(b">")
    pos = gt + 1
    if data[pos] == 0x0A:
        pos += 1
    if data[pos] == 0x00:
        pos += 1
    n_strings = struct.unpack_from("<h", data, pos)[0]
    pos += 2
    strings = []
    for _ in range(n_strings):
        s, pos = read_cstr(data, pos)
        strings.append(s)

    def sidx(i: int) -> str:
        if 0 <= i < len(strings):
            return strings[i]
        return f"?{i}"

    n_elems = struct.unpack_from("<i", data, pos)[0]
    pos += 4
    elems = []
    for _ in range(n_elems):
        t = struct.unpack_from("<h", data, pos)[0]
        pos += 2
        name, pos = read_cstr(data, pos)
        pos += 16
        elems.append({"type": sidx(t), "name": name, "attrs": {}})

    def read_val(pos: int, t: int, from_array: bool = False):
        if t == 1:
            idx = struct.unpack_from("<i", data, pos)[0]
            return idx, pos + 4
        if t == 2:
            return struct.unpack_from("<i", data, pos)[0], pos + 4
        if t == 3:
            return struct.unpack_from("<f", data, pos)[0], pos + 4
        if t == 4:
            return bool(data[pos]), pos + 1
        if t == 5:
            # encoding < 4: inline string
            return read_cstr(data, pos)
        if t == 6:
            n = struct.unpack_from("<i", data, pos)[0]
            return data[pos + 4 : pos + 4 + n], pos + 4 + n
        if t == 7:
            return struct.unpack_from("<i", data, pos)[0], pos + 4
        if t == 8:
            return tuple(data[pos : pos + 4]), pos + 4
        if t == 9:
            return struct.unpack_from("<ff", data, pos), pos + 8
        if t in (10, 12):
            return struct.unpack_from("<fff", data, pos), pos + 12
        if t in (11, 13):
            return struct.unpack_from("<ffff", data, pos), pos + 16
        if t == 14:
            return "matrix", pos + 64
        raise ValueError(f"bad type {t} at {pos}")

    for el in elems:
        n_attr = struct.unpack_from("<i", data, pos)[0]
        pos += 4
        for _ in range(n_attr):
            ni = struct.unpack_from("<h", data, pos)[0]
            at = data[pos + 2]
            pos += 3
            name = sidx(ni)
            if at >= ARRAY_BASE:
                base = at - ARRAY_BASE + 1
                count = struct.unpack_from("<i", data, pos)[0]
                pos += 4
                items = []
                for _i in range(count):
                    item, pos = read_val(pos, base, True)
                    items.append(item)
                el["attrs"][name] = items
            else:
                val, pos = read_val(pos, at)
                el["attrs"][name] = val

    return elems


def resolve(elems, val):
    if isinstance(val, int) and 0 <= val < len(elems):
        return elems[val]
    return val


def dump_system(elems, el) -> None:
    print(f"\n=== {el['name']} ({el['type']}) ===")
    attrs = el["attrs"]
    for key in (
        "material",
        "view model effect",
        "children",
        "initializers",
        "operators",
        "emitters",
        "renderers",
        "constraints",
        "forces",
    ):
        if key not in attrs:
            continue
        val = attrs[key]
        if key in ("material", "view model effect"):
            print(f"  {key}: {val}")
            continue
        print(f"  {key}:")
        if not isinstance(val, list):
            print(f"    {val}")
            continue
        for item in val:
            child = resolve(elems, item)
            if not isinstance(child, dict):
                print(f"    {item}")
                continue
            fn = child["attrs"].get("functionName", "")
            extra = []
            for k, v in child["attrs"].items():
                if k == "functionName":
                    continue
                kl = k.lower()
                if any(
                    s in kl
                    for s in (
                        "control",
                        "trace",
                        "los",
                        "distance",
                        "offset",
                        "material",
                        "orient",
                        "view",
                        "camera",
                        "radius",
                    )
                ):
                    extra.append(f"{k}={v}")
            print(f"    [{child['name']}] {fn}")
            if extra:
                print("      " + "; ".join(extra[:12]))


def main() -> int:
    elems = parse_pcf(Path(sys.argv[1]).read_bytes())
    needle = sys.argv[2].lower() if len(sys.argv) > 2 else "gluon_beam"
    print(f"{len(elems)} elements")
    for el in elems:
        if el["type"] == "DmeParticleSystemDefinition" and needle in el["name"].lower():
            dump_system(elems, el)
    return 0


if __name__ == "__main__":
    sys.exit(main())
