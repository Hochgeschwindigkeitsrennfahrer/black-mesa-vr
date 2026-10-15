#!/usr/bin/env python3
"""Projection check for the Black Mesa VR fake-runtime tests (VRRuntimeBackend=fake, MirrorBothEyes=true):
does each eye image use the projection the OpenXR helper would submit for it?
Ported from css-vr/tools/css-projcheck.py; the declared tangents come from the fake helper's log line
("fakehelper eye N raw l r t b ... uv ..."), i.e. the helper's submit FOV and contain-blit, mapped onto the
whole eye texture.

  bm-projcheck.py pose "<hmd line>" <left|right> <dpitch> <dyaw> <droll>
      prints the fake.txt hmd line turned by those Source angles about that eye (the eye stays put: no parallax)
  bm-projcheck.py fit <left|right> <bmvr_log.txt> <base.png> "<base hmd>" <turned.png> "<hmd>" [...]
      [--declared-scale SX SY]

Shots are screenshots of the game window with MirrorBothEyes (left eye = left half, right eye = right half, each
the whole eye texture stretched). The tangents the whole texture covers come from the mod's own log line
("eye N raw l r t b ... uv uMin-uMax vMin-vMax"): the submitted bounds uv cover the declared l..r / t..b. Without
parallax each pixel of a turned shot shows a direction that maps to a computable pixel of the base shot; the fit
gives the real horizontal/vertical scale of the image against the declared tangents (1.000 = correct; Black Mesa
VR's squash would read vertical ~0.71). Exit 1 when either is off by more than 2 %. Same method as
xash3d-fwgs/utils/fakexr/fakexr-projcheck.py.
"""
import re
import sys

import numpy as np
from PIL import Image

N = 256           # analysis size (longer side)
EYE_OFFSET = 0.0315  # half the fake helper's default ipd 0.063, metres along the head's right axis


def source_vectors(pitch, yaw, roll):
    """Source AngleVectors (as FakeBackend's MakePose): forward, right, up in Source axes (x fwd, y left, z up)."""
    sp, cp = np.sin(np.radians(pitch)), np.cos(np.radians(pitch))
    sy, cy = np.sin(np.radians(yaw)), np.cos(np.radians(yaw))
    sr, cr = np.sin(np.radians(roll)), np.cos(np.radians(roll))
    fwd = np.array([cp * cy, cp * sy, -sp])
    right = np.array([-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp])
    up = np.array([cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp])
    return fwd, right, up


def ovr(v):
    """Source -> OpenVR axes: (x, y, z) -> (-y, z, -x)"""
    return np.array([-v[1], v[2], -v[0]])


def hmd_rotation(angles):
    """the hmd pose's rotation in OpenVR axes (columns right, up, back), as MakePose builds it"""
    fwd, right, up = source_vectors(*angles)
    return np.stack([ovr(right), ovr(up), ovr(-fwd)], 1)


def parse_hmd(line):
    v = [float(x) for x in line.split()[-6:]]
    return np.array(v[:3]), v[3:]


def eye_position(pos, angles, eye):
    _, right, _ = source_vectors(*angles)
    return pos + (EYE_OFFSET if eye == "right" else -EYE_OFFSET) * right


def cmd_pose(args):
    line, eye = args[0], args[1]
    dp, dy, dr = map(float, args[2:5])
    pos, ang = parse_hmd(line)
    eye_pos = eye_position(pos, ang, eye)
    new_ang = [ang[0] + dp, ang[1] + dy, ang[2] + dr]
    _, right, _ = source_vectors(*new_ang)
    new_pos = eye_pos - (EYE_OFFSET if eye == "right" else -EYE_OFFSET) * right
    print("hmd %.5f %.5f %.5f %.3f %.3f %.3f" % (*new_pos, *new_ang))


def declared_tangents(log, eye):
    """tangent extents (left, right, up, down; up positive) of the whole eye texture, from the mod's log"""
    pat = re.compile(r"eye (\d) raw l (\S+) r (\S+) t (\S+) b (\S+) .* uv (\S+)-(\S+) (\S+)-(\S+)")
    found = None
    for line in open(log, errors="replace"):
        m = pat.search(line)
        if m and int(m.group(1)) == (1 if eye == "right" else 0):
            found = [float(x) for x in m.groups()[1:]]
    if not found:
        sys.exit(f"no 'eye N raw ... uv ...' line for the {eye} eye in {log}")
    l, r, t, b, u0, u1, v0, v1 = found
    x = lambda u: l + (u - u0) / (u1 - u0) * (r - l)
    y_down = lambda v: t + (v - v0) / (v1 - v0) * (b - t)  # OpenVR: t < 0 is up, v = 0 is the top row
    return x(0.0), x(1.0), -y_down(0.0), -y_down(1.0)


def load(png, eye, hmd, tan):
    img = Image.open(png).convert("L")
    w, h = img.size
    img = img.crop((0, 0, w // 2, h) if eye == "left" else (w // 2, 0, w, h))
    w, h = img.size
    scale = N / max(w, h)
    img = img.resize((max(1, round(w * scale)), max(1, round(h * scale))), Image.BOX)
    pos, ang = parse_hmd(hmd)
    return {"img": np.asarray(img, np.float32) / 255.0, "R": hmd_rotation(ang),
            "eye": eye_position(pos, ang, eye), "tan": tan, "name": png}


def actual_tan(t, sx, sy):
    tl, tr, tu, td = t
    cx, cy = (tl + tr) / 2, (tu + td) / 2
    return cx + (tl - cx) * sx, cx + (tr - cx) * sx, cy + (tu - cy) * sy, cy + (td - cy) * sy


def residual(a, b, sx, sy):
    ha, wa = a["img"].shape
    hb, wb = b["img"].shape
    al, ar, au, ad = actual_tan(a["tan"], sx, sy)
    bl, br, bu, bd = actual_tan(b["tan"], sx, sy)
    u = (np.arange(wb) + 0.5) / wb
    v = (np.arange(hb) + 0.5) / hb
    tx = bl + u[None, :] * (br - bl)
    ty = bu - v[:, None] * (bu - bd)
    d = np.stack([np.broadcast_to(tx, (hb, wb)), np.broadcast_to(ty, (hb, wb)), -np.ones((hb, wb))], -1)
    d = d @ (a["R"].T @ b["R"]).T
    front = d[..., 2] < -1e-6
    z = np.where(front, -d[..., 2], 1.0)
    ua = (d[..., 0] / z - al) / (ar - al) * wa - 0.5
    va = (au - d[..., 1] / z) / (au - ad) * ha - 0.5
    ok = front & (ua >= 0) & (ua <= wa - 1.001) & (va >= 0) & (va <= ha - 1.001)
    if ok.mean() < 0.25:
        return np.inf
    x0 = np.floor(ua[ok]).astype(int)
    y0 = np.floor(va[ok]).astype(int)
    fx = ua[ok] - x0
    fy = va[ok] - y0
    img = a["img"]
    s = (img[y0, x0] * (1 - fx) * (1 - fy) + img[y0, x0 + 1] * fx * (1 - fy) +
         img[y0 + 1, x0] * (1 - fx) * fy + img[y0 + 1, x0 + 1] * fx * fy)
    return float(np.abs(s - b["img"][ok]).mean())


def cmd_fit(args):
    declared = (1.0, 1.0)
    if "--declared-scale" in args:
        i = args.index("--declared-scale")
        declared = (float(args[i + 1]), float(args[i + 2]))
        args = args[:i] + args[i + 3:]
    eye, log, rest = args[0], args[1], args[2:]
    if len(rest) < 4 or len(rest) % 2:
        sys.exit(__doc__)
    tan = actual_tan(declared_tangents(log, eye), *declared)
    shots = [load(rest[i], eye, rest[i + 1], tan) for i in range(0, len(rest), 2)]
    for s in shots:
        if np.abs(s["eye"] - shots[0]["eye"]).max() > 1e-3:
            sys.exit(f"{s['name']}: the {eye} eye moved; turn with 'pose'")
    base, turned = shots[0], shots[1:]
    # a view that did not change at all means the scene is not live (menu, loading screen, stale image), not a
    # projection error: say so instead of fitting noise
    for t in turned:
        if t["img"].shape == base["img"].shape and float(np.abs(t["img"] - base["img"]).mean()) * 255 < 0.5:
            print(f"projection check: CANNOT TELL, {t['name'].rsplit('/', 1)[-1]} is the same image as the base "
                  "(the view did not turn: not in a map, paused, or a stale image)")
            sys.exit(2)
    total = lambda sx, sy: sum(residual(base, t, sx, sy) for t in turned)

    best = (np.inf, 1.0, 1.0)
    for step, span, centre in ((0.05, 0.4, False), (0.01, 0.05, True), (0.002, 0.01, True)):
        cx, cy = (best[1], best[2]) if centre else (1.0, 1.0)
        for sx in np.arange(cx - span, cx + span + 1e-9, step):
            for sy in np.arange(cy - span, cy + span + 1e-9, step):
                e = total(sx, sy)
                if e < best[0]:
                    best = (e, sx, sy)
    err0 = total(1.0, 1.0)
    _, sx, sy = best
    tl, tr, tu, td = tan
    print(f"projection check ({eye} eye, texture tangents l {tl:.3f} r {tr:.3f} u {tu:.3f} d {td:.3f}): "
          f"horizontal scale {sx:.3f}, vertical {sy:.3f} (1.000 = as declared); mismatch {err0 * 255:.1f} at the "
          f"declared projection, {best[0] * 255:.1f} at the best fit")
    for t in turned:
        print(f"  {t['name'].rsplit('/', 1)[-1]}: mismatch {residual(base, t, 1.0, 1.0) * 255:.1f} declared, "
              f"{residual(base, t, sx, sy) * 255:.1f} fitted")
    if abs(sx - 1) > 0.02 or abs(sy - 1) > 0.02:
        print("projection check: FAILED (the image does not use the projection the mod declares)")
        sys.exit(1)
    print("projection check: OK")


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("pose", "fit"):
        sys.exit(__doc__)
    (cmd_pose if sys.argv[1] == "pose" else cmd_fit)(sys.argv[2:])
